package mediagen

import (
	"encoding/binary"
	"net"
)

// Packer H264 → RTP（FU-A 分片，RFC 6184）。
type Packer struct {
	SSRC    uint32
	Payload uint8 // PT，默认 96
	Seq     uint16
	TS      uint32
	MTU     int
}

// NewPacker 构造 RTP 打包器。
func NewPacker(ssrc uint32) *Packer {
	return &Packer{SSRC: ssrc, Payload: 96, MTU: 1400}
}

// Packetize 将一帧切分为 RTP 包（marker 置于末包）。
func (p *Packer) Packetize(f *Frame) [][]byte {
	if p.MTU <= 0 {
		p.MTU = 1400
	}
	pkts := [][]byte{}
	send := func(payload []byte, mark bool) {
		hdr := make([]byte, 12)
		hdr[0] = 0x80
		hdr[1] = p.Payload
		if mark {
			hdr[1] |= 0x80
		}
		binary.BigEndian.PutUint16(hdr[2:], p.Seq)
		binary.BigEndian.PutUint32(hdr[4:], p.TS)
		binary.BigEndian.PutUint32(hdr[8:], p.SSRC)
		p.Seq++
		pkts = append(pkts, append(hdr, payload...))
	}
	for _, nalu := range f.NALUs {
		p.TS = uint32(f.PTS)
		if len(nalu) <= p.MTU-12 {
			send(nalu, false)
			continue
		}
		// FU-A
		rest := nalu[1:]
		first := true
		for len(rest) > 0 {
			n := p.MTU - 12 - 2
			if n > len(rest) {
				n = len(rest)
			}
			chunk := rest[:n]
			rest = rest[n:]
			ind := byte(0x1C | (nalu[0] & 0x60)) // F=0 | 原NRI | type=28(FU-A)，0x1C 即 type 位
			fuh := byte(nalu[0] & 0x1f)
			if first {
				fuh |= 0x80 // S
				first = false
			}
			if len(rest) == 0 {
				fuh |= 0x40 // E
			}
			send(append([]byte{ind, fuh}, chunk...), false)
		}
	}
	if len(pkts) > 0 {
		last := pkts[len(pkts)-1]
		last[1] |= 0x80 // marker
	}
	return pkts
}

// PacketizePS 将已封装的 PS 字节流拆为 RTP 包（PT=96 PS/90000）。
func (p *Packer) PacketizePS(ps []byte, f *Frame) [][]byte {
	if p.MTU <= 0 {
		p.MTU = 1400
	}
	p.TS = uint32(f.PTS)
	var pkts [][]byte
	for len(ps) > 0 {
		n := p.MTU - 12
		if n > len(ps) {
			n = len(ps)
		}
		payload := ps[:n]
		ps = ps[n:]
		mark := byte(0)
		if len(ps) == 0 {
			mark = 0x80
		}
		hdr := make([]byte, 12)
		hdr[0] = 0x80
		hdr[1] = p.Payload | mark
		binary.BigEndian.PutUint16(hdr[2:], p.Seq)
		binary.BigEndian.PutUint32(hdr[4:], p.TS)
		binary.BigEndian.PutUint32(hdr[8:], p.SSRC)
		p.Seq++
		pkts = append(pkts, append(hdr, payload...))
	}
	return pkts
}

// SendUDP 将 RTP 包组经 UDP 发送到目标。
func SendUDP(addr *net.UDPAddr, pkts [][]byte) error {
	conn, err := net.DialUDP("udp", nil, addr)
	if err != nil {
		return err
	}
	defer conn.Close()
	for _, p := range pkts {
		if _, err := conn.Write(p); err != nil {
			return err
		}
	}
	return nil
}

// TCPSender GB28181 TCP 媒体传输（$ + 通道 + 2 字节长度前缀，同 RTSP 交织分帧）。
type TCPSender struct {
	conn net.Conn
}

// DialTCP 建立 TCP 发送通道（GB28181 a=setup:active 设备主动连）。
func DialTCP(addr string) (*TCPSender, error) {
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		return nil, err
	}
	return &TCPSender{conn: conn}, nil
}

// Send 发送一组 RTP 包（$ 0x00 <len:2> <rtp>）。
func (t *TCPSender) Send(pkts [][]byte) error {
	var buf []byte
	for _, p := range pkts {
		l := make([]byte, 2)
		binary.BigEndian.PutUint16(l, uint16(len(p)))
		buf = append(buf, '$', 0x00)
		buf = append(buf, l...)
		buf = append(buf, p...)
	}
	_, err := t.conn.Write(buf)
	return err
}

// Close 关闭连接。
func (t *TCPSender) Close() { _ = t.conn.Close() }
