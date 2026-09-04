package gb28181

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/config"
	"github.com/jetscam/ipccloud/server/internal/crypto"
	"github.com/jetscam/ipccloud/server/internal/devsvc"
	"github.com/jetscam/ipccloud/server/internal/media"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

const Source = "gb28181"

// Adapter GB28181 平台侧（SIP 服务器）。
type Adapter struct {
	cfg      *config.Config
	tr       *Transport
	serverID string
	domain   string
	sipHost  string
	sipPort  int

	mu        sync.Mutex
	sn        int64
	snWait    map[int64]chan *MANSCDP
	nonces    map[string]time.Time
	regRemote map[string]*Peer // gbID → 最近注册对端（UDP 地址或 TCP 连接）
	ctx       context.Context
}

func New(cfg *config.Config) *Adapter {
	return &Adapter{
		cfg: cfg, serverID: cfg.SIPServerID, domain: cfg.SIPDomain,
		sipPort: cfg.SIPPort, snWait: map[int64]chan *MANSCDP{},
		nonces: map[string]time.Time{}, regRemote: map[string]*Peer{},
	}
}

func (a *Adapter) Source() string { return Source }

func (a *Adapter) Start(ctx context.Context) error {
	a.ctx = ctx
	bind := fmt.Sprintf(":%d", a.sipPort)
	if a.cfg.SIPIP != "" {
		bind = a.cfg.SIPIP + bind
	}
	tr, err := NewTransport(ctx, bind)
	if err != nil {
		return fmt.Errorf("sip bind %s: %w", bind, err)
	}
	a.tr = tr
	a.sipHost = a.cfg.SIPIP
	if a.sipHost == "" {
		a.sipHost = firstLocalIP()
	}
	log.Printf("[gb28181] sip listening %s udp+tcp (server=%s domain=%s)", bind, a.serverID, a.domain)
	tr.OnRequest(a.onRequest)
	go a.offlineWatcher(ctx)
	return nil
}

func (a *Adapter) Stop() {}

func firstLocalIP() string {
	conn, err := net.Dial("udp", "8.8.8.8:53")
	if err != nil {
		return "127.0.0.1"
	}
	defer conn.Close()
	return conn.LocalAddr().(*net.UDPAddr).IP.String()
}

// ---------- 注册与信令入口 ----------

func (a *Adapter) onRequest(peer *Peer, msg *SIPMessage) {
	switch msg.Method {
	case "REGISTER":
		a.handleRegister(peer, msg)
	case "MESSAGE", "NOTIFY":
		a.handleMessage(peer, msg)
	case "BYE":
		a.respond(peer, msg, 200, "OK")
	case "ACK":
	default:
		a.respond(peer, msg, 200, "OK")
	}
}

func (a *Adapter) respond(peer *Peer, req *SIPMessage, code int, reason string) {
	resp := &SIPMessage{IsResp: true, Status: code, Reason: reason,
		Via: req.Via, From: req.From, CallID: req.CallID,
		CSeqNum: req.CSeqNum, CSeqMeth: req.CSeqMeth}
	resp.To = req.To
	if !strings.Contains(req.To, "tag=") {
		resp.To = req.To + ";tag=" + randHex(6)
	}
	_ = a.tr.Send(peer, resp)
}

func fromUser(v string) string {
	i := strings.Index(v, "sip:")
	if i < 0 {
		return ""
	}
	rest := v[i+4:]
	if j := strings.Index(rest, "@"); j >= 0 {
		rest = rest[:j]
	} else if j := strings.IndexAny(rest, ";>"); j >= 0 {
		rest = rest[:j]
	}
	return rest
}

// handleRegister Digest 注册（§6.2）。
func (a *Adapter) handleRegister(peer *Peer, msg *SIPMessage) {
	gbID := fromUser(msg.From)
	if gbID == "" {
		a.respond(peer, msg, 400, "Bad Request")
		return
	}
	if msg.Auth == "" {
		nonce := randHex(16)
		a.mu.Lock()
		a.nonces[nonce] = time.Now().Add(5 * time.Minute)
		a.mu.Unlock()
		resp := &SIPMessage{IsResp: true, Status: 401, Reason: "Unauthorized",
			Via: msg.Via, From: msg.From,
			To:       msg.To + ";tag=" + randHex(6),
			CallID:   msg.CallID, CSeqNum: msg.CSeqNum, CSeqMeth: msg.CSeqMeth,
			WWWAuth:  digestChallenge(a.cfg.SIPRealm, nonce), Expires: msg.Expires}
		_ = a.tr.Send(peer, resp)
		return
	}
	pwd := a.devicePassword(gbID)
	uri := fmt.Sprintf("sip:%s@%s", gbID, a.domain)
	if !verifyDigest(msg.Auth, "REGISTER", gbID, pwd, uri) {
		a.respond(peer, msg, 401, "Unauthorized")
		log.Printf("[gb28181] %s auth failed", gbID)
		return
	}
	a.onRegistered(gbID, peer)
	a.respond(peer, msg, 200, "OK")
}

// devicePassword 取密码：设备凭据 > 白名单 > 全局默认设置。
func (a *Adapter) devicePassword(gbID string) string {
	var dev models.Device
	if store.DB.First(&dev, "identity->>'gbId' = ? AND deleted_at = 0", gbID).Error == nil {
		if dev.CredentialsEnc != "" {
			if raw, err := crypto.Dec(dev.CredentialsEnc); err == nil {
				var c map[string]string
				if json.Unmarshal([]byte(raw), &c) == nil && c["gbPwd"] != "" {
					return c["gbPwd"]
				}
			}
		}
	}
	var wl models.GbWhitelist
	if store.DB.First(&wl, "gb_id = ?", gbID).Error == nil && wl.PwdEnc != "" {
		if p, err := crypto.Dec(wl.PwdEnc); err == nil {
			return p
		}
	}
	var st models.Setting
	if store.DB.First(&st, "scope = 'global' AND key = 'gb.password'").Error == nil {
		if v, ok := st.Value["value"].(string); ok && v != "" {
			return v
		}
	}
	return "ipccloud-gb-pass"
}

// onRegistered 注册成功：更新设备/白名单自动入组/待确认。
func (a *Adapter) onRegistered(gbID string, peer *Peer) {
	a.mu.Lock()
	a.regRemote[gbID] = peer
	a.mu.Unlock()
	var dev models.Device
	err := store.DB.First(&dev, "identity->>'gbId' = ? AND deleted_at = 0", gbID).Error
	if err == nil {
		if dev.Status != "online" {
			devsvc.SetDeviceStatus(dev.ID, "online", nil)
		} else {
			devsvc.TouchDevice(dev.ID)
		}
		a.queryCatalog(dev.ID, gbID)
		a.queryDeviceInfo(dev.ID, gbID)
		return
	}
	var wl models.GbWhitelist
	if store.DB.First(&wl, "gb_id = ?", gbID).Error == nil {
		a.createGBDevice(wl.ProjectID, wl.GroupID, wl.Name, gbID, peer, &wl)
		return
	}
	// 未在白名单：进入待确认（归入最早项目展示）
	store.DB.Where("gb_id = ?", gbID).Assign(models.GbPending{
		GbID: gbID, IP: peer.ip(), FirstSeen: models.NowMilli(),
	}).FirstOrCreate(&models.GbPending{
		ID: "gbp_" + models.NewID(), GbID: gbID,
		IP: peer.ip(), FirstSeen: models.NowMilli(),
	})
	var proj models.Project
	if store.DB.Order("created_at ASC").First(&proj).Error == nil {
		bus.Default.Publish(bus.Event{Type: "gb.pending", ProjectID: proj.ID,
			Data: map[string]any{"gbId": gbID, "ip": peer.ip()}})
	}
	log.Printf("[gb28181] %s pending confirm (ip=%s transport=%s)", gbID, peer.ip(), peer.transport())
}

// createGBDevice 由白名单/确认入组创建设备与默认通道。
func (a *Adapter) createGBDevice(projectID, groupID, name, gbID string, peer *Peer, wl *models.GbWhitelist) {
	if name == "" {
		name = "GB-" + gbID[len(gbID)-4:]
	}
	pwd := a.devicePassword(gbID)
	if wl != nil && wl.PwdEnc != "" {
		pwd, _ = crypto.Dec(wl.PwdEnc)
	}
	dev := models.Device{
		ID: "dv_" + models.NewID(), ProjectID: projectID, GroupID: groupID,
		Source: Source, Name: name, Vendor: "未知", Model: "GB28181",
		Identity: models.JSONB{"gbId": gbID}, Status: "online",
		CredentialsEnc: crypto.Enc(fmt.Sprintf(`{"gbPwd":%q}`, pwd)),
		LastSeenAt:     models.NowMilli(),
		Meta:           models.JSONB{"sipRemote": peer.String(), "transport": strings.ToLower(peer.transport())},
		CreatedAt:      models.NowMilli(), UpdatedAt: models.NowMilli(),
	}
	if err := store.DB.Create(&dev).Error; err != nil {
		log.Printf("[gb28181] create device: %v", err)
		return
	}
	if wl != nil {
		store.DB.Delete(wl)
	}
	a.ensureChannel(&dev, 1, name, gbID)
	a.queryCatalog(dev.ID, gbID)
	a.queryDeviceInfo(dev.ID, gbID)
	log.Printf("[gb28181] device created %s (%s) project=%s", dev.Name, gbID, projectID)
}

// ensureChannel 确保通道存在（gbChannelId 唯一键）。
func (a *Adapter) ensureChannel(dev *models.Device, idx int, name, gbChannelID string) *models.Channel {
	var ch models.Channel
	err := store.DB.First(&ch, "device_id = ? AND meta->>'gbChannelId' = ?", dev.ID, gbChannelID).Error
	if err == nil {
		ch.Name = name
		ch.Idx = idx
		ch.UpdatedAt = models.NowMilli()
		store.DB.Save(&ch)
		return &ch
	}
	ch = models.Channel{
		ID: "ch_" + models.NewID(), DeviceID: dev.ID, ProjectID: dev.ProjectID,
		Idx: idx, Name: name, Enabled: true, StreamState: "idle",
		Capabilities: models.StringSlice(gbDefaultCaps()),
		Meta: models.JSONB{"gbChannelId": gbChannelID,
			"gbStream": gbChannelID + "_main", "gbStreamSub": gbChannelID + "_sub"},
		CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli(),
	}
	store.DB.Create(&ch)
	return &ch
}

// gbDefaultCaps 默认能力（§6.3 探测置位前保守声明）。
func gbDefaultCaps() []string {
	return []string{"live.main", "live.sub", "snapshot", "record.platform", "reboot"}
}

// gbDeviceCaps GB28181 设备能力（与适配器实际实现一致：直播、录像查询/回放、平台录像）。
func gbDeviceCaps() []string {
	return []string{"live.main", "live.sub", "record.device.query", "record.device.play", "record.platform"}
}

// ---------- MESSAGE（MANSCDP） ----------

func (a *Adapter) handleMessage(peer *Peer, msg *SIPMessage) {
	a.respond(peer, msg, 200, "OK")
	m, err := DecodeBody(msg.Body)
	if err != nil {
		log.Printf("[gb28181] %v", err)
		return
	}
	gbID := m.DeviceID
	if gbID == "" {
		gbID = fromUser(msg.From)
	}
	// 仅 Response 根元素参与 SN 事务匹配：设备侧 Keepalive/Alarm 等 Notify
	// 的 SN 独立计数，若误匹配会吞掉查询响应导致 Catalog 超时。
	isResp := m.XMLName.Local == "Response"
	a.mu.Lock()
	ch, waiting := a.snWait[m.SN]
	if waiting && isResp {
		delete(a.snWait, m.SN)
	}
	a.mu.Unlock()
	if waiting && isResp {
		select {
		case ch <- m:
		default:
		}
		return
	}
	switch m.CmdType {
	case "Keepalive":
		var dev models.Device
		if store.DB.First(&dev, "identity->>'gbId' = ? AND deleted_at = 0", gbID).Error == nil {
			devsvc.TouchDevice(dev.ID)
			if dev.Status != "online" {
				devsvc.SetDeviceStatus(dev.ID, "online", nil)
			}
		}
	case "Catalog":
		a.upsertCatalog(m)
	case "Alarm":
		a.onAlarm(m)
	}
}

// upsertCatalog 目录入库。
func (a *Adapter) upsertCatalog(m *MANSCDP) {
	var dev models.Device
	if store.DB.First(&dev, "identity->>'gbId' = ? AND deleted_at = 0", m.DeviceID).Error != nil {
		return
	}
	for i, it := range m.DeviceList.Items {
		if it.DeviceID == "" {
			continue
		}
		name := it.Name
		if name == "" {
			name = fmt.Sprintf("通道%d", i+1)
		}
		a.ensureChannel(&dev, i+1, name, it.DeviceID)
	}
	bus.Default.Publish(bus.Event{Type: "gb.catalog", DeviceID: dev.ID,
		ProjectID: dev.ProjectID, Data: map[string]any{"count": len(m.DeviceList.Items)}})
}

// onAlarm 告警归一化（§3.5）。
func (a *Adapter) onAlarm(m *MANSCDP) {
	var dev models.Device
	if store.DB.First(&dev, "identity->>'gbId' = ? AND deleted_at = 0", m.DeviceID).Error != nil {
		return
	}
	kind := "motion"
	switch m.AlarmMethod {
	case "2":
		kind = "tamper"
	case "5", "132":
		kind = "io"
	}
	var ch models.Channel
	store.DB.First(&ch, "device_id = ? AND idx = 1", dev.ID)
	bus.Default.Publish(bus.Event{Type: "alarm." + kind, DeviceID: dev.ID,
		ChannelID: ch.ID, Source: Source, ProjectID: dev.ProjectID,
		Data: map[string]any{"alarmType": m.AlarmType, "alarmTime": m.AlarmTime, "priority": m.AlarmPriority}})
}

// ---------- 离线检测 ----------

func (a *Adapter) offlineWatcher(ctx context.Context) {
	t := time.NewTicker(30 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			var devs []models.Device
			store.DB.Where("source = ? AND status = 'online' AND deleted_at = 0", Source).Find(&devs)
			for _, d := range devs {
				gbID, _ := d.Identity["gbId"].(string)
				a.mu.Lock()
				_, reg := a.regRemote[gbID]
				a.mu.Unlock()
				_ = reg
				if d.LastSeenAt > 0 && models.NowMilli()-d.LastSeenAt > 180_000 {
					devsvc.SetDeviceStatus(d.ID, "offline", nil)
				}
			}
		}
	}
}

// ---------- 查询 ----------

func (a *Adapter) nextSN() int64 {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.sn++
	return a.sn
}

func (a *Adapter) remoteOf(gbID string) *Peer {
	a.mu.Lock()
	defer a.mu.Unlock()
	if r, ok := a.regRemote[gbID]; ok {
		return r
	}
	// 通道 gbID 与设备 gbID 不同：反查设备注册地址
	var ch models.Channel
	if store.DB.First(&ch, "meta->>'gbChannelId' = ?", gbID).Error == nil {
		var dev models.Device
		if store.DB.First(&dev, "id = ? AND deleted_at = 0", ch.DeviceID).Error == nil {
			if id, ok := dev.Identity["gbId"].(string); ok {
				return a.regRemote[id]
			}
		}
	}
	return nil
}

// sendQuery 发送查询并等待 Response。
func (a *Adapter) sendQuery(gbID, body string, sn int64, wait time.Duration) (*MANSCDP, error) {
	remote := a.remoteOf(gbID)
	if remote == nil {
		return nil, fmt.Errorf("E1002 设备未注册")
	}
	ch := make(chan *MANSCDP, 1)
	a.mu.Lock()
	a.snWait[sn] = ch
	a.mu.Unlock()
	defer func() {
		a.mu.Lock()
		delete(a.snWait, sn)
		a.mu.Unlock()
	}()
	msg := a.buildRequest("MESSAGE", fmt.Sprintf("sip:%s@%s", gbID, a.domain), gbID, body, remote.transport())
	if _, err := a.tr.Request(remote, msg, wait); err != nil {
		return nil, err
	}
	select {
	case resp := <-ch:
		return resp, nil
	case <-time.After(wait):
		return nil, fmt.Errorf("E7003 查询超时")
	}
}

// QueryCatalog 目录查询。
func (a *Adapter) QueryCatalog(gbID string) ([]CatalogItem, error) {
	sn := a.nextSN()
	m, err := a.sendQuery(gbID, queryCatalogXML(gbID, sn), sn, 10*time.Second)
	if err != nil {
		return nil, err
	}
	return m.DeviceList.Items, nil
}

func (a *Adapter) queryCatalog(devID, gbID string) {
	go func() {
		items, err := a.QueryCatalog(gbID)
		if err != nil {
			log.Printf("[gb28181] catalog %s: %v", gbID, err)
			return
		}
		var dev models.Device
		if store.DB.First(&dev, "id = ?", devID).Error == nil {
			for i, it := range items {
				if it.DeviceID == "" {
					continue
				}
				name := it.Name
				if name == "" {
					name = fmt.Sprintf("通道%d", i+1)
				}
				a.ensureChannel(&dev, i+1, name, it.DeviceID)
			}
			bus.Default.Publish(bus.Event{Type: "gb.catalog", DeviceID: devID,
				ProjectID: dev.ProjectID, Data: map[string]any{"count": len(items)}})
		}
	}()
}

func (a *Adapter) queryDeviceInfo(devID, gbID string) {
	go func() {
		sn := a.nextSN()
		m, err := a.sendQuery(gbID, queryDeviceInfoXML(gbID, sn), sn, 10*time.Second)
		if err != nil {
			return
		}
		var dev models.Device
		if store.DB.First(&dev, "id = ?", devID).Error == nil {
			updates := map[string]any{"updated_at": models.NowMilli()}
			if m.Manufacturer != "" {
				updates["vendor"] = m.Manufacturer
			}
			if m.Model != "" {
				updates["model"] = m.Model
			}
			if m.Firmware != "" {
				updates["fw"] = m.Firmware
			}
			store.DB.Model(&dev).Updates(updates)
		}
	}()
}

// QueryRecords RecordInfo 查询。
func (a *Adapter) QueryRecordsRaw(gbChannelID string, startMs, endMs int64, recType string) ([]RecordItem, error) {
	sn := a.nextSN()
	if recType == "" {
		recType = "all"
	}
	m, err := a.sendQuery(gbChannelID, queryRecordInfoXML(gbChannelID, sn,
		toGBTime(startMs), toGBTime(endMs), recType), sn, 15*time.Second)
	if err != nil {
		return nil, err
	}
	return m.RecordList.Items, nil
}

func toGBTime(ms int64) string { return time.UnixMilli(ms).UTC().Format("2006-01-02 15:04:05") }

// ---------- INVITE / BYE / INFO ----------

// buildRequest 构造平台侧请求；transport 为对端注册所用的传输（UDP/TCP），
// Via/Contact 随之标记，保证回程与注册传输一致。
func (a *Adapter) buildRequest(method, uri, toUser, body, transport string) *SIPMessage {
	via := fmt.Sprintf("SIP/2.0/%s %s:%d;rport;branch=z9hG4bK%s", transport, a.sipHost, a.sipPort, randHex(10))
	contact := localURI(a.serverID, a.sipHost, a.sipPort)
	if transport == "TCP" {
		contact = fmt.Sprintf("<sip:%s@%s:%d;transport=TCP>", a.serverID, a.sipHost, a.sipPort)
	}
	return &SIPMessage{
		Method: method, URI: uri,
		Via:  []string{via},
		From: fmt.Sprintf("<sip:%s@%s>;tag=%s", a.serverID, a.domain, randHex(6)),
		To:   fmt.Sprintf("<sip:%s@%s>", toUser, a.domain),
		CallID:   randHex(16),
		CSeqNum:  1,
		CSeqMeth: method,
		Contact:  contact,
		Body:     body,
		UserAgent: "IpcCloud",
	}
}

// Invite 发送 INVITE（s=Play/Playback）并完成 ACK。tcpMode 1=TCP 被动 0=UDP。
func (a *Adapter) Invite(gbChannelID, session, start, end string, ssrcPrefix string,
	node *models.MediaNode, port, tcpMode int) (string, string, error) {

	remote := a.remoteOf(gbChannelID)
	if remote == nil {
		return "", "", fmt.Errorf("E1002 设备未注册")
	}
	host := media.DevicePushHost(node)
	if host == "" {
		host = a.sipHost
	}
	ssrc := ssrcPrefix + randDigits(9)
	var sb strings.Builder
	sb.WriteString("v=0\r\n")
	sb.WriteString(fmt.Sprintf("o=%s 0 0 IN IP4 %s\r\n", a.serverID, host))
	if session == "Play" {
		sb.WriteString("s=Play\r\n")
	} else {
		sb.WriteString("s=Playback\r\n")
	}
	sb.WriteString(fmt.Sprintf("c=IN IP4 %s\r\n", host))
	if start != "" && end != "" {
		sb.WriteString(fmt.Sprintf("t=%s %s\r\n", start, end))
	} else {
		sb.WriteString("t=0 0\r\n")
	}
	if tcpMode == 1 {
		sb.WriteString(fmt.Sprintf("m=video %d TCP/RTP/AVP 96 98 97\r\n", port))
		sb.WriteString("a=setup:passive\r\n")
		sb.WriteString("a=connection:new\r\n")
	} else {
		sb.WriteString(fmt.Sprintf("m=video %d RTP/AVP 96 98 97\r\n", port))
	}
	sb.WriteString("a=recvonly\r\n")
	sb.WriteString("a=rtpmap:96 PS/90000\r\n")
	sb.WriteString("a=rtpmap:98 H264/90000\r\n")
	sb.WriteString("a=rtpmap:97 MPEG4/90000\r\n")
	sb.WriteString(fmt.Sprintf("y=%s\r\n", ssrc))

	msg := a.buildRequest("INVITE", fmt.Sprintf("sip:%s@%s", gbChannelID, a.domain), gbChannelID, sb.String(), remote.transport())
	msg.Subject = fmt.Sprintf("%s:%s,%s:0", gbChannelID, ssrc, a.serverID)
	resp, err := a.tr.Request(remote, msg, 12*time.Second)
	if err != nil {
		return "", "", err
	}
	if resp.Status != 200 {
		return "", "", fmt.Errorf("E3001 INVITE %d %s", resp.Status, resp.Reason)
	}
	ack := &SIPMessage{Method: "ACK", URI: fmt.Sprintf("sip:%s@%s", gbChannelID, a.domain),
		Via: msg.Via, From: msg.From, To: resp.To, CallID: msg.CallID,
		CSeqNum: msg.CSeqNum, CSeqMeth: "ACK"}
	_ = a.tr.Send(remote, ack)
	return gbChannelID, ssrc, nil
}

// Bye 发送 BYE。
func (a *Adapter) Bye(gbChannelID string) {
	if remote := a.remoteOf(gbChannelID); remote != nil {
		_ = a.tr.Send(remote, a.buildRequest("BYE",
			fmt.Sprintf("sip:%s@%s", gbChannelID, a.domain), gbChannelID, "", remote.transport()))
	}
}

// Info 发送 MANSRTSP 回放控制体（PAUSE/PLAY Scale/PLAY Range）。
func (a *Adapter) Info(gbChannelID, body string) error {
	remote := a.remoteOf(gbChannelID)
	if remote == nil {
		return fmt.Errorf("E1002 设备未注册")
	}
	return a.tr.Send(remote, a.buildRequest("INFO",
		fmt.Sprintf("sip:%s@%s", gbChannelID, a.domain), gbChannelID, body, remote.transport()))
}

func randHex(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return hexEncode(b)
}

func hexEncode(b []byte) string {
	const digits = "0123456789abcdef"
	out := make([]byte, len(b)*2)
	for i, v := range b {
		out[i*2] = digits[v>>4]
		out[i*2+1] = digits[v&0x0f]
	}
	return string(out)
}

func randDigits(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	out := make([]byte, n)
	for i, v := range b {
		out[i] = byte('0' + int(v)%10)
	}
	return string(out)
}
