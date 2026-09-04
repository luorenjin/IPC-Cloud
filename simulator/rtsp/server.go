// Package rtsp RTSP 服务端：为 ONVIF/RTSP 直连设备提供拉流源
// （OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN，UDP 与 TCP interleaved）。
package rtsp

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"log"
	"math/rand"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/jetscam/ipccloud/simulator/mediagen"
)

// Track 一个可拉流路径。
type Track struct {
	Path   string // 如 /onvif1
	Source *mediagen.Source
	Width  int
	Height int
}

// Server RTSP 服务端。
type Server struct {
	Addr      string // 监听 :8554
	Advertise string // SDP Content-Base 通告 host（为空时取连接本地地址，避免 "rtsp://:8554/..." 非法 URL）
	Tracks    map[string]*Track

	mu   sync.Mutex
	sess map[string]*session
}

// session 一次 PLAY 会话。
type session struct {
	id     string
	track  *Track
	tcp    bool
	ch     int // interleaved channel
	client *net.TCPAddr
	w      *connWriter
	stop   chan struct{}

	pmu    sync.Mutex
	paused bool
}

// connWriter 单连接互斥写：RTSP 响应与 RTP interleaved 数据共用连接，
// 并发写会把响应插进 $ 分帧中间导致接收端 RTP 错位。
type connWriter struct {
	conn net.Conn
	mu   sync.Mutex
}

func (w *connWriter) write(b []byte) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	_, err := w.conn.Write(b)
	return err
}

// New 构造 RTSP server。
func New(addr string, tracks ...*Track) *Server {
	s := &Server{Addr: addr, Tracks: map[string]*Track{}, sess: map[string]*session{}}
	for _, t := range tracks {
		s.Tracks[t.Path] = t
	}
	return s
}

// ListenAndServe 阻塞服务。
func (s *Server) ListenAndServe() error {
	ln, err := net.Listen("tcp", s.Addr)
	if err != nil {
		return err
	}
	for {
		conn, err := ln.Accept()
		if err != nil {
			return err
		}
		go s.handleConn(conn)
	}
}

// request 简化 RTSP 请求。
type request struct {
	method  string
	path    string
	headers map[string]string
	body    string
}

func (s *Server) handleConn(conn net.Conn) {
	defer conn.Close()
	log.Printf("[rtsp] conn from %s", conn.RemoteAddr())
	var cur *session
	w := &connWriter{conn: conn}
	// pend 已读未消费字节：一次 Read 可能同时含 [请求][interleaved 帧][请求…]，
	// 必须按序逐条消费并保留残余，否则 RTCP 帧被截断丢弃后整条 TCP 流错位。
	pend := make([]byte, 0, 64*1024)
	chunk := make([]byte, 16*1024)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(5 * time.Minute))
		consumed, isBin, req, ok := tryFrame(pend)
		if consumed == 0 {
			n, err := conn.Read(chunk)
			if err != nil || n == 0 {
				log.Printf("[rtsp] conn %s read end: n=%d err=%v", conn.RemoteAddr(), n, err)
				if cur != nil {
					s.closeSession(cur)
				}
				return
			}
			pend = append(pend, chunk[:n]...)
			continue
		}
		pend = pend[consumed:]
		if !ok {
			continue // 无法解析的垃圾字节，已同步丢弃
		}
		if isBin {
			// 客户端 RTCP interleaved 帧，丢弃
			continue
		}
		log.Printf("[rtsp] conn %s req %s %s", conn.RemoteAddr(), req.method, req.path)
		switch req.method {
		case "OPTIONS":
			w.resp(req, 200, map[string]string{
				"Public": "OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER",
			}, "")
		case "DESCRIBE":
			s.handleDescribe(w, req)
		case "SETUP":
			cur = s.handleSetup(w, req)
		case "PLAY":
			sessID := ""
			if cur != nil {
				sessID = cur.id
			}
			// 先回 200 再起推流 goroutine：RTP 数据若先于响应到达，
			// 客户端会将其当作响应流解析导致会话建立失败。
			w.resp(req, 200, map[string]string{
				"Session":  sessID,
				"RTP-Info": s.rtpInfo(s.baseURL(conn), req.path),
			}, "")
			if cur != nil {
				s.startStream(cur)
			}
		case "PAUSE":
			if cur != nil {
				cur.setPaused(true)
			}
			sessID := ""
			if cur != nil {
				sessID = cur.id
			}
			w.resp(req, 200, map[string]string{"Session": sessID}, "")
		case "TEARDOWN":
			if cur != nil {
				w.resp(req, 200, map[string]string{"Session": cur.id}, "")
				s.closeSession(cur)
				cur = nil
			} else {
				w.resp(req, 200, nil, "")
			}
		default:
			w.resp(req, 200, nil, "")
		}
	}
}

// tryFrame 从缓冲尝试解析一条消息。
// consumed=0 表示数据不足需继续读；isBin=true 为 interleaved 二进制帧（已完整读出）；
// ok=false 表示垃圾字节（已随 consumed 丢弃）。请求含 Content-Length 时附带消费 body。
func tryFrame(pend []byte) (consumed int, isBin bool, req request, ok bool) {
	if len(pend) == 0 {
		return 0, false, req, false
	}
	if pend[0] == '$' {
		if len(pend) < 4 {
			return 0, false, req, false
		}
		fl := int(pend[2])<<8 | int(pend[3])
		if fl > 512*1024 { // 客户端方向只有 RTCP，超长帧视为流损坏，整帧丢弃重同步
			return 4 + fl, true, req, true
		}
		if len(pend) < 4+fl {
			return 0, false, req, false
		}
		return 4 + fl, true, req, true
	}
	idx := bytes.Index(pend, []byte("\r\n\r\n"))
	if idx < 0 {
		if len(pend) > 64*1024 { // 无头结构且积压超限，丢弃重同步
			return len(pend), false, req, true
		}
		return 0, false, req, false
	}
	headEnd := idx + 4
	req, ok = parseHead(pend[:idx])
	if !ok {
		return headEnd, false, req, true // 非 RTSP 文本，丢弃
	}
	clen := 0
	if v, has := req.headers["content-length"]; has {
		clen, _ = strconv.Atoi(v)
	}
	if len(pend) < headEnd+clen {
		return 0, false, req, false
	}
	return headEnd + clen, false, req, true
}

// baseURL 构造面向客户端的 rtsp://host:port 基址：Advertise 优先，
// 其次连接本地地址（监听 0.0.0.0 时不可用则回退 127.0.0.1）。
func (s *Server) baseURL(conn net.Conn) string {
	host := s.Advertise
	if host == "" {
		if la, ok := conn.LocalAddr().(*net.TCPAddr); ok && la.IP != nil && !la.IP.IsUnspecified() {
			host = la.IP.String()
		} else {
			host = "127.0.0.1"
		}
	}
	_, port, err := net.SplitHostPort(s.Addr)
	if err != nil {
		port = "554"
	}
	return "rtsp://" + net.JoinHostPort(host, port)
}

func (s *Server) rtpInfo(base, path string) string {
	return fmt.Sprintf("url=%s%s/track0;seq=1;rtptime=0", base, path)
}

func (s *Server) handleDescribe(w *connWriter, req request) {
	t := s.trackOf(req.path)
	if t == nil {
		w.resp(req, 404, nil, "")
		return
	}
	base := s.baseURL(w.conn)
	sps, pps := t.Source.SPS(), t.Source.PPS()
	sprop := base64.StdEncoding.EncodeToString(sps) + "," + base64.StdEncoding.EncodeToString(pps)
	profile := fmt.Sprintf("%02x%02x%02x", sps[1], sps[2], sps[3])
	sdp := fmt.Sprintf("v=0\r\no=- %d %d IN IP4 %s\r\ns=SIM-IPC %s\r\nc=IN IP4 %s\r\nt=0 0\r\n"+
		"m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"+
		"a=fmtp:96 packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s\r\n"+
		"a=control:track0\r\n", time.Now().Unix(), time.Now().Unix(), hostOf(base), t.Path, hostOf(base), profile, sprop)
	w.resp(req, 200, map[string]string{
		"Content-Type": "application/sdp",
		"Content-Base": base + t.Path,
	}, sdp)
}

// hostOf 从 rtsp://host:port 提取 host（SDP c= 行只填 host）。
func hostOf(base string) string {
	u := strings.TrimPrefix(base, "rtsp://")
	if i := strings.LastIndex(u, ":"); i >= 0 {
		return u[:i]
	}
	return u
}

func (s *Server) trackOf(path string) *Track {
	path = strings.TrimSuffix(path, "/track0")
	if t, ok := s.Tracks[path]; ok {
		return t
	}
	// 无路径匹配时返回第一个（兼容直接根路径拉流）
	for _, t := range s.Tracks {
		return t
	}
	return nil
}

func (s *Server) handleSetup(w *connWriter, req request) *session {
	t := s.trackOf(req.path)
	if t == nil {
		w.resp(req, 404, nil, "")
		return nil
	}
	tr := req.headers["transport"]
	sess := &session{id: fmt.Sprintf("%08x", rand.Uint32()), track: t, w: w}
	var respT string
	switch {
	case strings.Contains(tr, "TCP") || strings.Contains(tr, "interleaved"):
		sess.tcp = true
		sess.ch = 0
		if i := strings.Index(tr, "interleaved="); i >= 0 {
			fmt.Sscanf(tr[i+12:], "%d", &sess.ch)
		}
		respT = fmt.Sprintf("RTP/AVP/TCP;unicast;interleaved=%d-%d", sess.ch, sess.ch+1)
	default:
		// UDP：解析 client_port
		cp := 0
		if i := strings.Index(tr, "client_port="); i >= 0 {
			fmt.Sscanf(tr[i+12:], "%d", &cp)
		}
		if cp == 0 {
			w.resp(req, 461, nil, "")
			return nil
		}
		remote := w.conn.RemoteAddr().(*net.TCPAddr)
		sess.client = &net.TCPAddr{IP: remote.IP, Port: cp}
		respT = fmt.Sprintf("RTP/AVP;unicast;client_port=%d-%d;server_port=30010-30011", cp, cp+1)
	}
	s.mu.Lock()
	s.sess[sess.id] = sess
	s.mu.Unlock()
	w.resp(req, 200, map[string]string{"Session": sess.id, "Transport": respT}, "")
	return sess
}

// startStream 起流循环。
func (s *Server) startStream(sess *session) {
	s.mu.Lock()
	if sess.stop != nil {
		s.mu.Unlock()
		return // 已在推流
	}
	sess.stop = make(chan struct{})
	stop := sess.stop
	s.mu.Unlock()
	packer := mediagen.NewPacker(rand.Uint32())
	src := sess.track.Source.Clone()
	go func() {
		if sess.tcp {
			for {
				select {
				case <-stop:
					return
				default:
				}
				if sess.isPaused() {
					time.Sleep(40 * time.Millisecond)
					continue
				}
				f := src.WaitNext()
				pkts := packer.Packetize(f)
				for _, p := range pkts {
					// $ + channel + len(2B) + rtp
					hdr := []byte{'$', byte(sess.ch), byte(len(p) >> 8), byte(len(p))}
					if err := sess.w.write(append(hdr, p...)); err != nil {
						return
					}
				}
			}
		} else {
			udp, err := net.DialUDP("udp", nil, &net.UDPAddr{IP: sess.client.IP, Port: sess.client.Port})
			if err != nil {
				return
			}
			defer udp.Close()
			for {
				select {
				case <-stop:
					return
				default:
				}
				if sess.isPaused() {
					time.Sleep(40 * time.Millisecond)
					continue
				}
				f := src.WaitNext()
				for _, p := range packer.Packetize(f) {
					if _, err := udp.Write(p); err != nil {
						return
					}
				}
			}
		}
	}()
}

func (s *Server) closeSession(sess *session) {
	s.mu.Lock()
	if sess.stop != nil {
		close(sess.stop)
		sess.stop = nil
	}
	delete(s.sess, sess.id)
	s.mu.Unlock()
}

func (sess *session) setPaused(v bool) {
	sess.pmu.Lock()
	sess.paused = v
	sess.pmu.Unlock()
}

func (sess *session) isPaused() bool {
	sess.pmu.Lock()
	defer sess.pmu.Unlock()
	return sess.paused
}

// ---------- 工具 ----------

// resp 经互斥锁写 RTSP 响应，CSeq 回显请求值（FFmpeg 等客户端会校验）。
func (w *connWriter) resp(req request, code int, headers map[string]string, body string) {
	reason := map[int]string{200: "OK", 404: "Not Found", 461: "Unsupported Transport"}[code]
	var b strings.Builder
	fmt.Fprintf(&b, "RTSP/1.0 %d %s\r\n", code, reason)
	fmt.Fprintf(&b, "CSeq: %s\r\n", req.headers["cseq"])
	fmt.Fprintf(&b, "Server: IpcCloud-SIM\r\n")
	for k, v := range headers {
		fmt.Fprintf(&b, "%s: %s\r\n", k, v)
	}
	if body != "" {
		fmt.Fprintf(&b, "Content-Length: %d\r\n", len(body))
	}
	b.WriteString("\r\n")
	b.WriteString(body)
	_ = w.write([]byte(b.String()))
}

// parseHead 解析请求行与头（不含 body，头块以 \r\n 结尾且尚未含空行）。
func parseHead(data []byte) (request, bool) {
	s := string(data)
	var req request
	lines := strings.Split(s, "\r\n")
	// 跳过前导空行（前一请求残留 CRLF 粘包时保持兼容）
	k := 0
	for k < len(lines) && strings.TrimSpace(lines[k]) == "" {
		k++
	}
	if k >= len(lines) {
		return req, false
	}
	parts := strings.Fields(lines[k])
	if len(parts) < 2 {
		return req, false
	}
	// 请求行格式：METHOD URI RTSP/1.0（版本号在行尾）；二进制 interleaved 数据不含 RTSP/ 后缀
	if !strings.HasPrefix(parts[len(parts)-1], "RTSP/") {
		return req, false
	}
	req.method = parts[0]
	// URI 可能是完整 URL（rtsp://host:port/path）或纯路径，统一取路径部分
	u := parts[1]
	if i := strings.Index(u, "://"); i >= 0 {
		rest := u[i+3:]
		if j := strings.Index(rest, "/"); j >= 0 {
			u = rest[j:]
		} else {
			u = "/"
		}
	}
	req.path = u
	req.headers = map[string]string{}
	for _, ln := range lines[k+1:] {
		i := strings.Index(ln, ":")
		if i < 0 {
			continue
		}
		// header 键统一小写，便于大小写不敏感查找
		req.headers[strings.ToLower(strings.TrimSpace(ln[:i]))] = strings.TrimSpace(ln[i+1:])
	}
	return req, true
}


