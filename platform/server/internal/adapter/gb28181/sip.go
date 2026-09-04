// Package gb28181 GB28181-2016 SIP 适配器（接入规范 §6）。
// 信令 UDP+TCP 同端口双栈：REGISTER(Digest)/Keepalive/Catalog/DeviceInfo/
// INVITE(Play|Playback)/INFO/BYE/RecordInfo/Alarm。
package gb28181

import (
	"bufio"
	"context"
	"crypto/md5"
	"fmt"
	"io"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
)

// SIPMessage 最小 SIP 报文模型。
type SIPMessage struct {
	IsResp    bool
	Method    string // 请求方法
	URI       string // Request-URI
	Status    int    // 响应码
	Reason    string
	Via       []string
	From      string
	To        string
	CallID    string
	CSeqNum   int
	CSeqMeth  string
	Auth      string // Authorization
	WWWAuth   string // WWW-Authenticate
	Contact   string
	Subject   string
	Expires   string
	Body      string
	UserAgent string
}

// Parse 解析 SIP 报文。
func Parse(raw []byte) *SIPMessage {
	s := string(raw)
	var head, body string
	if i := strings.Index(s, "\r\n\r\n"); i >= 0 {
		head, body = s[:i], s[i+4:]
	} else if i := strings.Index(s, "\n\n"); i >= 0 {
		head, body = s[:i], s[i+2:]
	} else {
		head = s
	}
	lines := strings.Split(strings.ReplaceAll(head, "\r\n", "\n"), "\n")
	m := &SIPMessage{Body: body}
	if len(lines) == 0 {
		return nil
	}
	first := lines[0]
	if strings.HasPrefix(first, "SIP/2.0") {
		m.IsResp = true
		fmt.Sscanf(first, "SIP/2.0 %d", &m.Status)
		m.Reason = strings.TrimSpace(strings.TrimPrefix(strings.TrimPrefix(first, "SIP/2.0"), fmt.Sprint(m.Status)))
	} else {
		parts := strings.Fields(first)
		if len(parts) >= 2 {
			m.Method = parts[0]
			m.URI = parts[1]
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
			var n int
			var meth string
			fmt.Sscanf(v, "%d %s", &n, &meth)
			m.CSeqNum, m.CSeqMeth = n, meth
		case "authorization":
			m.Auth = v
		case "www-authenticate":
			m.WWWAuth = v
		case "contact":
			m.Contact = v
		case "subject":
			m.Subject = v
		case "expires":
			m.Expires = v
		case "user-agent":
			m.UserAgent = v
		}
	}
	return m
}

// Serialize 还原为报文（Content-Length 按需补齐）。
func (m *SIPMessage) Serialize() []byte {
	var b strings.Builder
	if m.IsResp {
		b.WriteString(fmt.Sprintf("SIP/2.0 %d %s\r\n", m.Status, m.Reason))
	} else {
		uri := m.URI
		if uri == "" {
			uri = "*"
		}
		b.WriteString(m.Method + " " + uri + " SIP/2.0\r\n")
	}
	for _, v := range m.Via {
		b.WriteString("Via: " + v + "\r\n")
	}
	b.WriteString("From: " + m.From + "\r\n")
	b.WriteString("To: " + m.To + "\r\n")
	b.WriteString("Call-ID: " + m.CallID + "\r\n")
	b.WriteString(fmt.Sprintf("CSeq: %d %s\r\n", m.CSeqNum, m.CSeqMeth))
	if m.WWWAuth != "" {
		b.WriteString("WWW-Authenticate: " + m.WWWAuth + "\r\n")
	}
	if m.Auth != "" {
		b.WriteString("Authorization: " + m.Auth + "\r\n")
	}
	if m.Contact != "" {
		b.WriteString("Contact: " + m.Contact + "\r\n")
	}
	if m.Subject != "" {
		b.WriteString("Subject: " + m.Subject + "\r\n")
	}
	if m.Expires != "" {
		b.WriteString("Expires: " + m.Expires + "\r\n")
	}
	if m.UserAgent != "" {
		b.WriteString("User-Agent: " + m.UserAgent + "\r\n")
	}
	b.WriteString("Max-Forwards: 70\r\n")
	if m.Body != "" {
		ct := "Application/SDP"
		if strings.HasPrefix(strings.TrimSpace(m.Body), "<") {
			ct = "Application/MANSCDP+xml"
		}
		b.WriteString("Content-Type: " + ct + "\r\n")
		b.WriteString(fmt.Sprintf("Content-Length: %d\r\n", len(m.Body)))
	}
	b.WriteString("\r\n")
	b.WriteString(m.Body)
	return []byte(b.String())
}

// digestChallenge 生成 401 挑战头。
func digestChallenge(realm, nonce string) string {
	return fmt.Sprintf(`Digest realm="%s", nonce="%s"`, realm, nonce)
}

// verifyDigest 校验 Digest 响应（§6.2 REGISTER）。
func verifyDigest(authHeader, method, username, password, uri string) bool {
	if authHeader == "" {
		return false
	}
	kv := map[string]string{}
	for _, part := range strings.Split(strings.TrimPrefix(authHeader, "Digest "), ",") {
		i := strings.Index(part, "=")
		if i < 0 {
			continue
		}
		k := strings.TrimSpace(part[:i])
		v := strings.Trim(strings.TrimSpace(part[i+1:]), `"`)
		kv[k] = v
	}
	realm, nonce := kv["realm"], kv["nonce"]
	resp, ok := kv["response"]
	if realm == "" || nonce == "" || !ok {
		return false
	}
	h := func(s string) string { return fmt.Sprintf("%x", md5.Sum([]byte(s))) }
	ha1 := h(username + ":" + realm + ":" + password)
	ha2 := h(method + ":" + uri)
	var want string
	if qop, ok := kv["qop"]; ok && strings.Contains(qop, "auth") {
		want = h(ha1 + ":" + nonce + ":" + kv["nc"] + ":" + kv["cnonce"] + ":" + kv["qop"] + ":" + ha2)
	} else {
		want = h(ha1 + ":" + nonce + ":" + ha2)
	}
	return want == resp
}

// ---------- 传输层 ----------

// Peer 会话对端：UDP 地址或 TCP 长连接。
type Peer struct {
	UDP *net.UDPAddr
	TCP *tcpSession
}

func (p *Peer) transport() string {
	if p.TCP != nil {
		return "TCP"
	}
	return "UDP"
}

// ip 对端来源 IP（日志/待确认入库用）。
func (p *Peer) ip() string {
	if p.TCP != nil {
		if h, _, err := net.SplitHostPort(p.TCP.conn.RemoteAddr().String()); err == nil {
			return h
		}
		return ""
	}
	return p.UDP.IP.String()
}

func (p *Peer) String() string {
	if p.TCP != nil {
		return "tcp://" + p.TCP.conn.RemoteAddr().String()
	}
	return p.UDP.String()
}

// tcpSession 一条 SIP over TCP 长连接（多协程写串行化）。
type tcpSession struct {
	conn net.Conn
	wmu  sync.Mutex
}

func (s *tcpSession) write(raw []byte) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.conn.Write(raw)
	return err
}

// Transport SIP 传输：同端口 UDP + TCP 双栈，事务按 Via branch 匹配；
// TCP 侧请求/响应均在原长连接上收发。
type Transport struct {
	udp  *net.UDPConn
	ln   net.Listener
	mu   sync.Mutex
	wait map[string]chan *SIPMessage
	subs []func(peer *Peer, msg *SIPMessage)
	ctx  context.Context
}

func NewTransport(ctx context.Context, bindAddr string) (*Transport, error) {
	udpAddr, err := net.ResolveUDPAddr("udp", bindAddr)
	if err != nil {
		return nil, err
	}
	udp, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return nil, err
	}
	ln, err := net.Listen("tcp", bindAddr)
	if err != nil {
		udp.Close()
		return nil, fmt.Errorf("sip tcp bind %s: %w", bindAddr, err)
	}
	t := &Transport{udp: udp, ln: ln, wait: map[string]chan *SIPMessage{}, ctx: ctx}
	go t.readLoopUDP()
	go t.acceptLoopTCP()
	return t, nil
}

func (t *Transport) readLoopUDP() {
	buf := make([]byte, 65535)
	for {
		n, remote, err := t.udp.ReadFromUDP(buf)
		if err != nil {
			if t.ctx.Err() != nil {
				return
			}
			continue
		}
		if msg := Parse(buf[:n]); msg != nil {
			t.dispatch(&Peer{UDP: remote}, msg)
		}
	}
}

func (t *Transport) acceptLoopTCP() {
	for {
		conn, err := t.ln.Accept()
		if err != nil {
			if t.ctx.Err() != nil {
				return
			}
			continue
		}
		go t.readLoopTCP(&tcpSession{conn: conn})
	}
}

func (t *Transport) readLoopTCP(s *tcpSession) {
	peer := &Peer{TCP: s}
	r := bufio.NewReaderSize(s.conn, 64*1024)
	for {
		// 空闲 5 分钟断开（GB28181 保活周期通常 ≤60s）
		_ = s.conn.SetReadDeadline(time.Now().Add(5 * time.Minute))
		raw, err := readSIPMessage(r)
		if err != nil {
			_ = s.conn.Close()
			return
		}
		if msg := Parse(raw); msg != nil {
			t.dispatch(peer, msg)
		}
	}
}

// readSIPMessage 从 TCP 读取一条完整报文（头部 + Content-Length 体；
// 一条连接可承载多条报文）。
func readSIPMessage(r *bufio.Reader) ([]byte, error) {
	var head []byte
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return nil, err
		}
		head = append(head, line...)
		if line == "\r\n" || line == "\n" {
			break
		}
		if len(head) > 1<<20 {
			return nil, fmt.Errorf("sip message too large")
		}
	}
	cl := 0
	for _, ln := range strings.Split(string(head), "\n") {
		ln = strings.TrimRight(ln, "\r")
		if i := strings.Index(ln, ":"); i >= 0 &&
			strings.EqualFold(strings.TrimSpace(ln[:i]), "Content-Length") {
			cl, _ = strconv.Atoi(strings.TrimSpace(ln[i+1:]))
		}
	}
	if cl <= 0 {
		return head, nil
	}
	if cl > 8<<20 {
		return nil, fmt.Errorf("sip body too large")
	}
	body := make([]byte, cl)
	if _, err := io.ReadFull(r, body); err != nil {
		return nil, err
	}
	return append(head, body...), nil
}

// dispatch 分发：响应按 Via branch 唤醒等待事务；请求交订阅者。
func (t *Transport) dispatch(peer *Peer, msg *SIPMessage) {
	if msg.IsResp {
		if len(msg.Via) == 0 {
			return
		}
		branch := viaBranch(msg.Via[0])
		t.mu.Lock()
		ch, ok := t.wait[branch]
		if ok {
			delete(t.wait, branch)
		}
		t.mu.Unlock()
		if ok {
			select {
			case ch <- msg:
			default:
			}
		}
		return
	}
	for _, sub := range t.subs {
		sub(peer, msg)
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

// OnRequest 订阅请求。
func (t *Transport) OnRequest(h func(peer *Peer, msg *SIPMessage)) {
	t.subs = append(t.subs, h)
}

// Send 发送报文到对端（UDP 地址或 TCP 长连接）。
func (t *Transport) Send(peer *Peer, msg *SIPMessage) error {
	if peer == nil {
		return fmt.Errorf("nil sip peer")
	}
	raw := msg.Serialize()
	if peer.TCP != nil {
		return peer.TCP.write(raw)
	}
	_, err := t.udp.WriteToUDP(raw, peer.UDP)
	return err
}

// Request 发送请求并等待响应（超时后重发 1 次）。
func (t *Transport) Request(peer *Peer, msg *SIPMessage, timeout time.Duration) (*SIPMessage, error) {
	if len(msg.Via) == 0 {
		return nil, fmt.Errorf("no via")
	}
	branch := viaBranch(msg.Via[0])
	ch := make(chan *SIPMessage, 1)
	t.mu.Lock()
	t.wait[branch] = ch
	t.mu.Unlock()
	defer func() {
		t.mu.Lock()
		delete(t.wait, branch)
		t.mu.Unlock()
	}()
	if err := t.Send(peer, msg); err != nil {
		return nil, err
	}
	select {
	case resp := <-ch:
		return resp, nil
	case <-time.After(timeout):
		_ = t.Send(peer, msg)
		select {
		case resp := <-ch:
			return resp, nil
		case <-time.After(timeout):
			return nil, fmt.Errorf("E7003 SIP timeout")
		}
	}
}

func localURI(user, host string, port int) string {
	return fmt.Sprintf("<sip:%s@%s:%d>", user, host, port)
}