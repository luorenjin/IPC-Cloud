// Package gb GB28181-2016 设备侧 UA 模拟：REGISTER(Digest)/Keepalive/Catalog/
// DeviceInfo/RecordInfo/INVITE(Play|Playback)/INFO/BYE + PS/RTP 推流。
package gb

import (
	"bytes"
	"crypto/md5"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/jetscam/ipccloud/simulator/mediagen"
)

// Device GB28181 模拟设备。
type Device struct {
	GBID       string // 20 位国标设备编号
	Password   string
	ServerHost string
	ServerPort int
	LocalIP    string
	LocalPort  int // SIP UDP 监听端口
	Source     *mediagen.Source
	OnLog      func(format string, args ...any)

	udp     *net.UDPConn
	server  *net.UDPAddr
	mu      sync.Mutex
	cseq    int
	sn      int64
	waiters map[string]chan *SIPMessage // branch → 响应等待
	streams map[string]*session         // gbChannelID → 推流会话
	keepN   int64
}

// session 一路点播会话。
type session struct {
	gbCh       string
	ssrc       string
	tcpMode    bool
	playback   bool
	targetIP   string
	targetPort int
	stop       chan struct{}

	pmu    sync.Mutex
	paused bool
}

func (s *session) setPaused(v bool) {
	s.pmu.Lock()
	s.paused = v
	s.pmu.Unlock()
}

func (s *session) isPaused() bool {
	s.pmu.Lock()
	defer s.pmu.Unlock()
	return s.paused
}

// Start 启动 UA：绑定本地端口并注册。
func (d *Device) Start() error {
	d.waiters = map[string]chan *SIPMessage{}
	d.streams = map[string]*session{}
	if d.LocalPort == 0 {
		d.LocalPort = 5061 + int(time.Now().UnixNano()%1000)
	}
	if d.Password == "" {
		d.Password = "ipccloud-gb-pass"
	}
	addr := &net.UDPAddr{IP: net.ParseIP(d.LocalIP), Port: d.LocalPort}
	udp, err := net.ListenUDP("udp", addr)
	if err != nil {
		return fmt.Errorf("sip udp bind: %w", err)
	}
	d.udp = udp
	srv, err := net.ResolveUDPAddr("udp", net.JoinHostPort(d.ServerHost, strconv.Itoa(d.ServerPort)))
	if err != nil {
		udp.Close()
		return fmt.Errorf("resolve sip server: %w", err)
	}
	d.server = srv
	go d.readLoop()
	if err := d.register(); err != nil {
		return fmt.Errorf("register: %w", err)
	}
	go d.keepaliveLoop()
	go d.reRegisterLoop()
	d.logf("registered as %s (udp %s:%d)", d.GBID, d.LocalIP, d.LocalPort)
	return nil
}

// reRegisterLoop 周期重注册（§6.2）：平台重启会丢失内存注册表，
// 按规范设备应在注册到期前重发 REGISTER，此处以 60s 周期保持注册新鲜。
func (d *Device) reRegisterLoop() {
	t := time.NewTicker(60 * time.Second)
	defer t.Stop()
	for range t.C {
		if err := d.register(); err != nil {
			d.logf("re-register: %v", err)
		}
	}
}

// Stop 停止一切会话并注销。
func (d *Device) Stop() {
	d.mu.Lock()
	for k, s := range d.streams {
		close(s.stop)
		delete(d.streams, k)
	}
	d.mu.Unlock()
	if d.udp != nil {
		_ = d.udp.Close()
	}
}

func (d *Device) logf(format string, args ...any) {
	if d.OnLog != nil {
		d.OnLog("[gb "+d.GBID[len(d.GBID)-4:]+"] "+format, args...)
	} else {
		log.Printf("[gb %s] "+format, append([]any{d.GBID}, args...)...)
	}
}

// ---------- SIP 报文 ----------

// SIPMessage 最小 SIP 报文。
type SIPMessage struct {
	IsResp    bool
	Status    int
	Reason    string
	Method    string
	URI       string
	Via       []string
	From      string
	To        string
	CallID    string
	CSeqNum   int
	CSeqMeth  string
	Auth      string
	WWWAuth   string
	Contact   string
	Expires   string
	Body      string
}

// parseSIP 解析报文。
func parseSIP(raw []byte) *SIPMessage {
	s := string(raw)
	head, body := s, ""
	if i := strings.Index(s, "\r\n\r\n"); i >= 0 {
		head, body = s[:i], s[i+4:]
	}
	lines := strings.Split(strings.ReplaceAll(head, "\r\n", "\n"), "\n")
	m := &SIPMessage{Body: body}
	if len(lines) == 0 {
		return nil
	}
	if strings.HasPrefix(lines[0], "SIP/2.0") {
		m.IsResp = true
		fmt.Sscanf(lines[0], "SIP/2.0 %d", &m.Status)
		m.Reason = strings.TrimSpace(strings.TrimPrefix(strings.TrimPrefix(lines[0], "SIP/2.0"), strconv.Itoa(m.Status)))
	} else {
		parts := strings.Fields(lines[0])
		if len(parts) >= 2 {
			m.Method, m.URI = parts[0], parts[1]
		}
	}
	for _, ln := range lines[1:] {
		i := strings.Index(ln, ":")
		if i < 0 {
			continue
		}
		k := strings.ToLower(strings.TrimSpace(ln[:i]))
		v := strings.TrimSpace(ln[i+1:])
		switch k {
		case "via":
			m.Via = append(m.Via, v)
		case "from":
			m.From = v
		case "to":
			m.To = v
		case "call-id":
			m.CallID = v
		case "cseq":
			fmt.Sscanf(v, "%d %s", &m.CSeqNum, &m.CSeqMeth)
		case "authorization":
			m.Auth = v
		case "www-authenticate":
			m.WWWAuth = v
		case "contact":
			m.Contact = v
		case "expires":
			m.Expires = v
		}
	}
	return m
}

// serialize 序列化。
func (m *SIPMessage) serialize() []byte {
	var b bytes.Buffer
	if m.IsResp {
		fmt.Fprintf(&b, "SIP/2.0 %d %s\r\n", m.Status, m.Reason)
	} else {
		fmt.Fprintf(&b, "%s %s SIP/2.0\r\n", m.Method, m.URI)
	}
	for _, v := range m.Via {
		fmt.Fprintf(&b, "Via: %s\r\n", v)
	}
	fmt.Fprintf(&b, "From: %s\r\n", m.From)
	fmt.Fprintf(&b, "To: %s\r\n", m.To)
	fmt.Fprintf(&b, "Call-ID: %s\r\n", m.CallID)
	fmt.Fprintf(&b, "CSeq: %d %s\r\n", m.CSeqNum, m.CSeqMeth)
	if m.WWWAuth != "" {
		fmt.Fprintf(&b, "WWW-Authenticate: %s\r\n", m.WWWAuth)
	}
	if m.Auth != "" {
		fmt.Fprintf(&b, "Authorization: %s\r\n", m.Auth)
	}
	if m.Contact != "" {
		fmt.Fprintf(&b, "Contact: %s\r\n", m.Contact)
	}
	if m.Expires != "" {
		fmt.Fprintf(&b, "Expires: %s\r\n", m.Expires)
	}
	if m.Body != "" {
		ct := "Application/SDP"
		if strings.HasPrefix(strings.TrimSpace(m.Body), "<") {
			ct = "Application/MANSCDP+xml"
		}
		fmt.Fprintf(&b, "Content-Type: %s\r\nContent-Length: %d\r\n", ct, len(m.Body))
	}
	b.WriteString("\r\n")
	b.WriteString(m.Body)
	return b.Bytes()
}

func randHex(n int) string {
	b := make([]byte, n)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

// ---------- 传输 ----------

func (d *Device) send(m *SIPMessage) error {
	_, err := d.udp.WriteToUDP(m.serialize(), d.server)
	return err
}

// request 发送请求并按 branch 等响应。
func (d *Device) request(m *SIPMessage, timeout time.Duration) (*SIPMessage, error) {
	if len(m.Via) == 0 {
		return nil, fmt.Errorf("no via")
	}
	branch := viaBranch(m.Via[0])
	ch := make(chan *SIPMessage, 1)
	d.mu.Lock()
	d.waiters[branch] = ch
	d.mu.Unlock()
	defer func() {
		d.mu.Lock()
		delete(d.waiters, branch)
		d.mu.Unlock()
	}()
	if err := d.send(m); err != nil {
		return nil, err
	}
	select {
	case r := <-ch:
		return r, nil
	case <-time.After(timeout):
		_ = d.send(m)
		select {
		case r := <-ch:
			return r, nil
		case <-time.After(timeout):
			return nil, fmt.Errorf("sip timeout")
		}
	}
}

func viaBranch(via string) string {
	i := strings.Index(via, "branch=")
	if i < 0 {
		return via
	}
	rest := via[i+7:]
	if j := strings.Index(rest, ";"); j >= 0 {
		return rest[:j]
	}
	return rest
}

func (d *Device) readLoop() {
	buf := make([]byte, 65535)
	for {
		n, _, err := d.udp.ReadFromUDP(buf)
		if err != nil {
			return
		}
		msg := parseSIP(buf[:n])
		if msg == nil {
			continue
		}
		if msg.IsResp {
			if len(msg.Via) == 0 {
				continue
			}
			d.mu.Lock()
			ch, ok := d.waiters[viaBranch(msg.Via[0])]
			d.mu.Unlock()
			if ok {
				select {
				case ch <- msg:
				default:
				}
			}
			continue
		}
		// 并发处理：handleRequest 内的 sendBody 会等待平台 200 OK，
		// 若同步执行会阻塞 readLoop，导致响应无法被读取（自死锁）。
		go d.handleRequest(msg)
	}
}

// respond 回响应。
func (d *Device) respond(req *SIPMessage, code int, reason string) {
	resp := &SIPMessage{IsResp: true, Status: code, Reason: reason,
		Via: req.Via, From: req.From, To: req.To, CallID: req.CallID,
		CSeqNum: req.CSeqNum, CSeqMeth: req.CSeqMeth}
	if code != 100 && !strings.Contains(req.To, "tag=") {
		resp.To = req.To + ";tag=" + randHex(6)
	}
	_ = d.send(resp)
}

// ---------- 注册 ----------

func (d *Device) buildRegister(expires int) *SIPMessage {
	d.mu.Lock()
	d.cseq++
	cseq := d.cseq
	d.mu.Unlock()
	return &SIPMessage{
		Method: "REGISTER",
		URI:    fmt.Sprintf("sip:34020000002000000001@3402000000"),
		Via:    []string{fmt.Sprintf("SIP/2.0/UDP %s:%d;rport;branch=z9hG4bK%s", d.LocalIP, d.LocalPort, randHex(10))},
		From:   fmt.Sprintf("<sip:%s@3402000000>;tag=%s", d.GBID, randHex(6)),
		To:     fmt.Sprintf("<sip:%s@3402000000>", d.GBID),
		CallID: randHex(16), CSeqNum: cseq, CSeqMeth: "REGISTER",
		Contact: fmt.Sprintf("<sip:%s@%s:%d>", d.GBID, d.LocalIP, d.LocalPort),
		Expires: strconv.Itoa(expires),
	}
}

// register Digest 注册。
func (d *Device) register() error {
	resp, err := d.request(d.buildRegister(3600), 6*time.Second)
	if err != nil {
		return err
	}
	if resp.Status != 401 {
		if resp.Status == 200 {
			return nil
		}
		return fmt.Errorf("register %d %s", resp.Status, resp.Reason)
	}
	// 计算 Digest
	kv := map[string]string{}
	for _, part := range strings.Split(strings.TrimPrefix(resp.WWWAuth, "Digest "), ",") {
		i := strings.Index(part, "=")
		if i < 0 {
			continue
		}
		kv[strings.TrimSpace(part[:i])] = strings.Trim(strings.TrimSpace(part[i+1:]), `"`)
	}
	realm, nonce := kv["realm"], kv["nonce"]
	if realm == "" || nonce == "" {
		return fmt.Errorf("bad digest challenge")
	}
	uri := fmt.Sprintf("sip:%s@3402000000", d.GBID)
	h := func(s string) string { sum := md5.Sum([]byte(s)); return hex.EncodeToString(sum[:]) }
	ha1 := h(d.GBID + ":" + realm + ":" + d.Password)
	ha2 := h("REGISTER:" + uri)
	cnonce := randHex(8)
	nc := "00000001"
	response := h(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2)
	reg := d.buildRegister(3600)
	reg.Auth = fmt.Sprintf(`Digest username="%s", realm="%s", nonce="%s", uri="%s", response="%s", algorithm=MD5, cnonce="%s", nc=%s, qop=auth`,
		d.GBID, realm, nonce, uri, response, cnonce, nc)
	resp, err = d.request(reg, 6*time.Second)
	if err != nil {
		return err
	}
	if resp.Status != 200 {
		return fmt.Errorf("register %d %s (check password)", resp.Status, resp.Reason)
	}
	return nil
}

// ---------- Keepalive 与查询 ----------

func (d *Device) nextSN() int64 {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.sn++
	return d.sn
}

// sendBody 发送带 MANSCDP 体 MESSAGE。
func (d *Device) sendBody(body string) error {
	d.mu.Lock()
	d.cseq++
	cseq := d.cseq
	d.mu.Unlock()
	m := &SIPMessage{
		Method: "MESSAGE",
		URI:    fmt.Sprintf("sip:34020000002000000001@3402000000"),
		Via:    []string{fmt.Sprintf("SIP/2.0/UDP %s:%d;rport;branch=z9hG4bK%s", d.LocalIP, d.LocalPort, randHex(10))},
		From:   fmt.Sprintf("<sip:%s@3402000000>;tag=%s", d.GBID, randHex(6)),
		To:     "<sip:34020000002000000001@3402000000>",
		CallID: randHex(16), CSeqNum: cseq, CSeqMeth: "MESSAGE",
		Contact: fmt.Sprintf("<sip:%s@%s:%d>", d.GBID, d.LocalIP, d.LocalPort),
		Body:    body,
	}
	_, err := d.request(m, 5*time.Second)
	return err
}

func (d *Device) keepaliveLoop() {
	t := time.NewTicker(30 * time.Second)
	defer t.Stop()
	for range t.C {
		body := fmt.Sprintf(`<?xml version="1.0" encoding="utf-8"?>
<Notify>
<CmdType>Keepalive</CmdType><SN>%d</SN><DeviceID>%s</DeviceID><Status>OK</Status>
</Notify>`, d.nextSN(), d.GBID)
		if err := d.sendBody(body); err != nil {
			d.logf("keepalive: %v", err)
		}
	}
}

// handleRequest 处理平台请求。
func (d *Device) handleRequest(msg *SIPMessage) {
	switch msg.Method {
	case "MESSAGE":
		d.respond(msg, 200, "OK")
		d.handleMANSCDP(msg.Body)
	case "INVITE":
		d.respond(msg, 100, "Trying")
		d.handleInvite(msg)
	case "ACK":
		// 推流已随 INVITE 处理启动
	case "BYE":
		d.respond(msg, 200, "OK")
		d.stopAllStreams()
	case "INFO":
		d.respond(msg, 200, "OK")
		d.handleInfo(msg.Body)
	default:
		d.respond(msg, 200, "OK")
	}
}

// handleMANSCDP 处理查询并回 Response。
func (d *Device) handleMANSCDP(body string) {
	cmdType := xmlText(body, "CmdType")
	sn := xmlText(body, "SN")
	deviceID := xmlText(body, "DeviceID")
	var respBody string
	switch cmdType {
	case "Catalog":
		respBody = fmt.Sprintf(`<?xml version="1.0" encoding="utf-8"?>
<Response>
<CmdType>Catalog</CmdType><SN>%s</SN><DeviceID>%s</DeviceID><SumNum>1</SumNum>
<DeviceList Num="1">
<Item><DeviceID>%s</DeviceID><Name>SIM-GB01</Name><Manufacturer>JetsCam</Manufacturer>
<Model>SIM-GB28181</Model><Owner>Owner</Owner><CivilCode>3402000000</CivilCode>
<Address>Simulator</Address><ParentID>%s</ParentID><Status>ONLINE</Status></Item>
</DeviceList>
</Response>`, sn, d.GBID, d.GBID, d.GBID)
	case "DeviceInfo":
		respBody = fmt.Sprintf(`<?xml version="1.0" encoding="utf-8"?>
<Response>
<CmdType>DeviceInfo</CmdType><SN>%s</SN><DeviceID>%s</DeviceID>
<Manufacturer>JetsCam</Manufacturer><Model>SIM-GB28181</Model>
<Firmware>1.0.0-sim</Firmware><Result>OK</Result>
</Response>`, sn, d.GBID)
	case "RecordInfo":
		respBody = d.recordInfoXML(sn, deviceID)
	default:
		return
	}
	if err := d.sendBody(respBody); err != nil {
		d.logf("response %s: %v", cmdType, err)
	}
}

// recordInfoXML 模拟录像段（过去 12h 一段）。
func (d *Device) recordInfoXML(sn, deviceID string) string {
	now := time.Now()
	st := now.Add(-12 * time.Hour).UTC().Format("2006-01-02 15:04:05")
	et := now.UTC().Format("2006-01-02 15:04:05")
	return fmt.Sprintf(`<?xml version="1.0" encoding="utf-8"?>
<Response>
<CmdType>RecordInfo</CmdType><SN>%s</SN><DeviceID>%s</DeviceID><Name>SIM</Name>
<SumNum>1</SumNum>
<RecordList Num="1">
<Item><DeviceID>%s</DeviceID><Name>record1</Name>
<StartTime>%s</StartTime><EndTime>%s</EndTime><Secrecy>0</Secrecy><Type>time</Type>
<FileSize>1024000</FileSize>
</Item>
</RecordList>
</Response>`, sn, d.GBID, deviceID, st, et)
}

// EmitAlarm 上报移动侦测告警（AlarmMethod=1 → motion）。
func (d *Device) EmitAlarm() {
	body := fmt.Sprintf(`<?xml version="1.0" encoding="utf-8"?>
<Notify>
<CmdType>Alarm</CmdType><SN>%d</SN><DeviceID>%s</DeviceID>
<AlarmPriority>2</AlarmPriority><AlarmMethod>1</AlarmMethod>
<AlarmTime>%s</AlarmTime>
<Info><AlarmType>1</AlarmType></Info>
</Notify>`, d.nextSN(), d.GBID, time.Now().Format("2006-01-02T15:04:05"))
	_ = d.sendBody(body)
}

func xmlText(doc, tag string) string {
	i := strings.Index(doc, "<"+tag+">")
	if i < 0 {
		return ""
	}
	rest := doc[i+len(tag)+2:]
	j := strings.Index(rest, "</"+tag+">")
	if j < 0 {
		return ""
	}
	return strings.TrimSpace(rest[:j])
}

// ---------- INVITE / INFO ----------

// handleInvite 解析 SDP 并启动推流。
func (d *Device) handleInvite(msg *SIPMessage) {
	sdp := msg.Body
	playback := strings.Contains(sdp, "s=Playback")
	targetIP := sdpValue(sdp, "c=IN IP4 ")
	port := 0
	tcpMode := false
	for _, ln := range strings.Split(sdp, "\n") {
		ln = strings.TrimSpace(ln)
		if strings.HasPrefix(ln, "m=video ") {
			fields := strings.Fields(ln)
			if len(fields) >= 2 {
				port, _ = strconv.Atoi(fields[1])
				if len(fields) >= 3 && strings.Contains(fields[2], "TCP") {
					tcpMode = true
				}
			}
		}
	}
	ssrc := sdpValue(sdp, "y=")
	if port == 0 {
		d.logf("invite 488: body=%q", sdp)
		d.respond(msg, 488, "Not Acceptable Here")
		return
	}
	d.respond(msg, 200, "OK")
	session := &session{gbCh: d.GBID, ssrc: ssrc, tcpMode: tcpMode, playback: playback,
		targetIP: targetIP, targetPort: port, stop: make(chan struct{})}
	d.mu.Lock()
	d.streams[d.GBID] = session
	d.mu.Unlock()
	go d.pushStream(session)
}

// pushStream PS/RTP 推流循环。
func (d *Device) pushStream(s *session) {
	var sender interface{ Write([]byte) (int, error) }
	var tc *mediagen.TCPSender
	if s.tcpMode {
		c, err := mediagen.DialTCP(net.JoinHostPort(s.targetIP, strconv.Itoa(s.targetPort)))
		if err != nil {
			d.logf("tcp connect %s:%d: %v", s.targetIP, s.targetPort, err)
			return
		}
		tc = c
		defer c.Close()
	} else {
		raddr, err := net.ResolveUDPAddr("udp", net.JoinHostPort(s.targetIP, strconv.Itoa(s.targetPort)))
		if err != nil {
			d.logf("resolve udp %s:%d: %v", s.targetIP, s.targetPort, err)
			return
		}
		c, err := net.DialUDP("udp", nil, raddr)
		if err != nil {
			d.logf("udp connect: %v", err)
			return
		}
		defer c.Close()
		sender = c
	}
	ssrcNum, _ := strconv.ParseUint(s.ssrc, 10, 32)
	packer := mediagen.NewPacker(uint32(ssrcNum))
	mux := mediagen.NewPSMuxer()
	src := d.Source.Clone()
	d.logf("streaming %s → %s:%d (%s, ssrc=%s)", s.gbCh, s.targetIP, s.targetPort,
		map[bool]string{true: "TCP", false: "UDP"}[s.tcpMode], s.ssrc)
	frameCount := 0
	for {
		select {
		case <-s.stop:
			return
		default:
		}
		f := src.WaitNext()
		if s.isPaused() {
			continue
		}
		// 关键帧前插入 SPS/PPS：ParseAnnexB 已剥离参数集，PS 流 ES 中必须携带
		// SPS/PPS，ZLM 才能建立 H264 轨道并注册流。
		withPSM := f.Key || frameCount == 0
		if withPSM {
			f.NALUs = append([][]byte{src.SPS(), src.PPS()}, f.NALUs...)
		}
		psBytes := mux.Pack(f, withPSM)
		frameCount++
		pkts := packer.PacketizePS(psBytes, f)
		if s.tcpMode {
			if err := tc.Send(pkts); err != nil {
				d.logf("tcp send: %v", err)
				return
			}
		} else {
			for _, p := range pkts {
				if _, err := sender.Write(p); err != nil {
					d.logf("udp send: %v", err)
					return
				}
			}
		}
	}
}

// stopAllStreams 停止全部推流。
func (d *Device) stopAllStreams() {
	d.mu.Lock()
	for k, s := range d.streams {
		close(s.stop)
		delete(d.streams, k)
	}
	d.mu.Unlock()
}

// handleInfo 处理 MANSRTSP 回放控制（PAUSE/PLAY）。
func (d *Device) handleInfo(body string) {
	d.mu.Lock()
	defer d.mu.Unlock()
	for _, s := range d.streams {
		if strings.Contains(body, "PAUSE") {
			s.setPaused(true)
		} else if strings.Contains(body, "PLAY") && !strings.Contains(body, "Scale") {
			s.setPaused(false)
		}
	}
}

func sdpValue(sdp, key string) string {
	for _, ln := range strings.Split(sdp, "\n") {
		ln = strings.TrimSpace(ln)
		if strings.HasPrefix(ln, key) {
			return strings.TrimSpace(ln[len(key):])
		}
	}
	return ""
}
