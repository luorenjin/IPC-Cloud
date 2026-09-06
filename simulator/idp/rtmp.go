package idp

import (
	"encoding/binary"
	"fmt"
	"math"
	"net"
	"strings"
	"sync"
	"time"
)

// rtmpClient 最小 RTMP 客户端：握手 → connect → createStream → publish → 发视频。
type rtmpClient struct {
	conn     net.Conn
	wmu      sync.Mutex
	streamID uint32
	ts       uint32
	chunk    uint32
	rdmu     sync.Mutex
	buf      []byte
}

// dialRTMP 解析 rtmp://host:port/app/stream?query 并建立连接完成握手。
// 返回 query 供 publish 拼入流名（ZLM 会将其透传给 on_publish hook）。
func dialRTMP(rawURL string) (*rtmpClient, string, string, string, error) {
	u := strings.TrimPrefix(rawURL, "rtmp://")
	host, rest, ok := strings.Cut(u, "/")
	if !ok {
		return nil, "", "", "", fmt.Errorf("bad rtmp url %s", rawURL)
	}
	if _, _, err := net.SplitHostPort(host); err != nil {
		host += ":1935"
	}
	app, stream, ok2 := strings.Cut(rest, "/")
	if !ok2 {
		return nil, "", "", "", fmt.Errorf("rtmp url missing stream: %s", rawURL)
	}
	// 抽出 query（token 等，publish 时拼回流名）
	var query string
	if i := strings.Index(stream, "?"); i >= 0 {
		query = stream[i+1:]
		stream = stream[:i]
	}
	if i := strings.Index(app, "?"); i >= 0 {
		app = app[:i]
	}
	conn, err := net.DialTimeout("tcp", host, 8*time.Second)
	if err != nil {
		return nil, "", "", "", err
	}
	c := &rtmpClient{conn: conn, streamID: 0, chunk: 128}
	if err := c.handshake(); err != nil {
		conn.Close()
		return nil, "", "", "", err
	}
	return c, app, stream, query, nil
}

func (c *rtmpClient) handshake() error {
	c1 := make([]byte, 1536)
	c1[0] = 0 // time
	binary.BigEndian.PutUint32(c1[4:], 0)
	_, err := c.conn.Write(append([]byte{0x03}, c1...))
	if err != nil {
		return err
	}
	s := make([]byte, 1+1536+1536)
	if _, err := readFull(c.conn, s); err != nil {
		return err
	}
	_, err = c.conn.Write(s[1 : 1+1536]) // C2 = echo S1
	return err
}

// publish 建立推流会话并返回。query（如 token=xx）拼入流名，ZLM 会透传给 hook。
func (c *rtmpClient) publish(app, stream, query string) error {
	tcURL := "rtmp://x/" + app
	// SetChunkSize 4096
	c.sendChunk(2, 1, 0, 4, []byte{0x00, 0x00, 0x10, 0x00})
	c.chunk = 4096
	// connect
	c.sendAMF(20, 0, 0,
		amfString("connect"), amfNumber(1),
		amfObject("app", amfString(app), "tcUrl", amfString(tcURL), "type", amfString("nonprivate")))
	if err := c.waitCommand("_result", 5*time.Second); err != nil {
		return err
	}
	// createStream
	c.sendAMF(20, 0, 0, amfString("createStream"), amfNumber(2), amfNull())
	sid, err := c.waitResultNumber(5 * time.Second)
	if err != nil {
		return err
	}
	c.streamID = uint32(sid)
	if query != "" {
		stream += "?" + query
	}
	// publish
	c.sendAMF(20, c.streamID, 0, amfString("publish"), amfNumber(3), amfNull(), amfString(stream), amfString("live"))
	if err := c.waitCommand("onStatus", 5*time.Second); err != nil {
		return err
	}
	return nil
}

// sendVideo 发一条视频消息（自动按 chunk 分块）。
func (c *rtmpClient) sendVideo(tsMs uint32, payload []byte) error {
	return c.sendChunk(6, 9, tsMs, 0, payload)
}

// sendAudio 发一条音频消息（AAC raw，type 8，独立 csid 保持时间戳状态）。
func (c *rtmpClient) sendAudio(tsMs uint32, payload []byte) error {
	return c.sendChunk(4, 8, tsMs, 0, payload)
}

// sendChunk 发送一条消息（fmt0 首块 + fmt3 后续块）。
func (c *rtmpClient) sendChunk(csid, mtype uint8, ts uint32, msid uint32, body []byte) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	h := make([]byte, 0, 18)
	h = append(h, csid) // fmt=0
	h = append(h, byte(ts>>16), byte(ts>>8), byte(ts))
	h = append(h, byte(len(body)>>16), byte(len(body)>>8), byte(len(body)))
	h = append(h, byte(mtype))
	var lit [4]byte
	binary.LittleEndian.PutUint32(lit[:], msid)
	h = append(h, lit[:]...)
	first := true
	for len(body) > 0 {
		if !first {
			h = append(h, 0xC0|csid) // fmt=3
		}
		n := int(c.chunk)
		if n > len(body) {
			n = len(body)
		}
		h = append(h, body[:n]...)
		body = body[n:]
		first = false
	}
	_, err := c.conn.Write(h)
	return err
}

// sendAMF 发 AMF0 command/data 消息。
func (c *rtmpClient) sendAMF(mtype uint8, msid uint32, ts uint32, parts ...[]byte) {
	var body []byte
	for _, p := range parts {
		body = append(body, p...)
	}
	_ = c.sendChunk(3, mtype, ts, msid, body)
}

// readMessage 读一条消息（消费服务端输出）。
func (c *rtmpClient) readMessage() (csid uint8, mtype uint8, body []byte, err error) {
	c.rdmu.Lock()
	defer c.rdmu.Unlock()
	b1 := make([]byte, 1)
	if _, err = readFull(c.conn, b1); err != nil {
		return
	}
	fmt0 := b1[0] >> 6
	csid = b1[0] & 0x3F
	if csid == 0 {
		ext := make([]byte, 1)
		if _, err = readFull(c.conn, ext); err != nil {
			return
		}
		csid = 64 + ext[0]
	}
	hs := map[byte]int{0: 11, 1: 7, 2: 3, 3: 0}[fmt0]
	hh := make([]byte, hs)
	if hs > 0 {
		if _, err = readFull(c.conn, hh); err != nil {
			return
		}
	}
	var mlen uint32
	if fmt0 <= 1 {
		mlen = uint32(hh[3])<<16 | uint32(hh[4])<<8 | uint32(hh[5])
	}
	if fmt0 == 0 && hh[0] == 0xFF && hh[1] == 0xFF && hh[2] == 0xFF {
		ext := make([]byte, 4)
		if _, err = readFull(c.conn, ext); err != nil {
			return
		}
	}
	mtype = uint8(0)
	if fmt0 == 1 {
		mtype = hh[6] // fmt1: ts-delta(3)+len(3)+type(1)
	} else if fmt0 == 0 {
		mtype = hh[6] // fmt0: ts(3)+len(3)+type(6) — 类型在字节 6（此前误读 7 处的流 ID 首字节）
	}
	// fmt0==2 头中无类型字段（仅 ts-delta），mtype 未知返回 0，由调用方跳过
	body = make([]byte, mlen)
	if _, err = readFull(c.conn, body); err != nil {
		return
	}
	return
}

// readFull 读满指定长度。
func readFull(conn net.Conn, buf []byte) (int, error) {
	total := 0
	for total < len(buf) {
		n, err := conn.Read(buf[total:])
		if err != nil {
			return total, err
		}
		total += n
	}
	return total, nil
}

// waitCommand 等待指定命令消息（跳过其他）。
func (c *rtmpClient) waitCommand(name string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		_ = c.conn.SetReadDeadline(deadline)
		_, mtype, body, err := c.readMessage()
		if err != nil {
			return err
		}
		if mtype != 20 && mtype != 17 {
			continue
		}
		parts := amfParse(body)
		if len(parts) >= 1 && string(parts[0]) == name {
			return nil
		}
	}
	return fmt.Errorf("rtmp wait %s timeout", name)
}

// waitResultNumber 等待 _result 并取第一个数值参数（createStream 返回流 id）。
func (c *rtmpClient) waitResultNumber(timeout time.Duration) (float64, error) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		_ = c.conn.SetReadDeadline(deadline)
		_, mtype, body, err := c.readMessage()
		if err != nil {
			return 0, err
		}
		if mtype != 20 && mtype != 17 {
			continue
		}
		parts := amfParse(body)
		if len(parts) >= 1 && string(parts[0]) == "_result" {
			// 响应形如 (name, txid, null, streamID)：取最后一个 8 字节数值（流 ID）
			for i := len(parts) - 1; i >= 1; i-- {
				if len(parts[i]) == 8 {
					return math.Float64frombits(binary.BigEndian.Uint64(parts[i])), nil
				}
			}
		}
	}
	return 0, fmt.Errorf("rtmp _result timeout")
}

// close 结束会话。
func (c *rtmpClient) close() {
	_ = c.sendChunk(3, 20, c.ts, c.streamID,
		append(append(amfString("FCUnpublish"), amfNumber(4)...), amfNull()...))
	_ = c.conn.Close()
}

// ---------- AMF0 ----------

func amfString(s string) []byte {
	b := []byte{0x02, byte(len(s) >> 8), byte(len(s))}
	return append(b, s...)
}

func amfNumber(f float64) []byte {
	b := make([]byte, 9)
	b[0] = 0x00
	binary.BigEndian.PutUint64(b[1:], math.Float64bits(f))
	return b
}

func amfNull() []byte { return []byte{0x05} }

// amfObject 构造 AMF0 object：key 为 string，value 支持 string / 已编码 []byte。
func amfObject(kv ...any) []byte {
	out := []byte{0x03}
	for i := 0; i+1 < len(kv); i += 2 {
		k, _ := kv[i].(string)
		out = append(out, byte(len(k)>>8), byte(len(k)))
		out = append(out, k...)
		switch v := kv[i+1].(type) {
		case string:
			out = append(out, amfString(v)...)
		case []byte:
			out = append(out, v...)
		}
	}
	return append(out, 0x00, 0x00, 0x09)
}

// amfParse 粗解析顶层值：string→原文，number→8 字节，object 跳过。
func amfParse(body []byte) [][]byte {
	var out [][]byte
	for len(body) > 0 {
		t := body[0]
		body = body[1:]
		switch t {
		case 0x02: // string
			if len(body) < 2 {
				return out
			}
			n := int(binary.BigEndian.Uint16(body))
			body = body[2:]
			if len(body) < n {
				return out
			}
			out = append(out, body[:n])
			body = body[n:]
		case 0x00: // number
			if len(body) < 8 {
				return out
			}
			out = append(out, body[:8])
			body = body[8:]
		case 0x05: // null：占位，后续可能还有 number（如 createStream 的流 ID）
			out = append(out, []byte{0})
		case 0x01: // bool：跳过 1 字节
			if len(body) < 1 {
				return out
			}
			body = body[1:]
		case 0x03: // object：扫到 00 00 09 结束
			depth := 0
			i := 0
			for i+3 <= len(body) {
				if depth == 0 && body[i] == 0x00 && body[i+1] == 0x00 && body[i+2] == 0x09 {
					i += 3
					break
				}
				// 粗略跳过 key/value：这里简化为逐字节找结束标记
				i++
			}
			body = body[i:]
		default:
			return out
		}
	}
	return out
}
