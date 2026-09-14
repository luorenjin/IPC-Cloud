// Package idp IDP 私有协议设备模拟：MQTT 生命周期（hello/report/bind）+ 命令处理 + RTMP 推流。
package idp

import (
	"bytes"
	"encoding/json"
	"fmt"
	"log"
	"math"
	"net/http"
	"strings"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"github.com/jetscam/ipccloud/simulator/mediagen"
)

// cmdMsg 下行命令消息（与平台 Envelope 对齐）。
type cmdMsg struct {
	V     int            `json:"v"`
	Type  string         `json:"type"`
	MsgID string         `json:"msgId"`
	Ts    int64          `json:"ts"`
	Code  int            `json:"code"`
	Msg   string         `json:"msg"`
	Data  map[string]any `json:"data"`
}

// Device IDP 模拟设备。
type Device struct {
	ID       string
	Broker   string
	Username string
	Password string
	Source   *mediagen.Source
	// PlatformAddr 平台 HTTP 地址（快照上传用），如 http://127.0.0.1:8080
	PlatformAddr string
	// SnapshotJPEG 快照图片内容（assets/frame.jpg）
	SnapshotJPEG []byte
	// PwdChanged 对应 hello.localUserChanged（接入规范 §5.5.2）的**初始值**：
	// 设备本地默认口令是否已修改。平台只在它为 false 时提示「仍在用出厂默认口令」。
	// 运行期会被改密（置 true）与恢复出厂（置 false）改写。
	PwdChanged bool
	// OnLog 可选日志钩子
	OnLog func(format string, args ...any)

	cli     mqtt.Client
	mu      sync.Mutex
	pushes  map[string]*pushSession
	nextMsg int64
	bound   bool
	cfg     map[string]any // 本地配置（见 config.go，键名/范围与固件规则表对齐）
	// startedAt 设备开机时刻，status.report.uptime 的基准（Start 时置位）。
	startedAt time.Time
	// pwdHash 改密后的口令哈希（只存哈希，不存明文）；pwdChanged 是当前是否已改过默认口令
	pwdHash    string
	pwdChanged bool
}

// pushSession 一次推流会话。
type pushSession struct {
	stop chan struct{}
}

// Start 连接 broker 并开始生命周期。
func (d *Device) Start() error {
	d.pushes = map[string]*pushSession{}
	// 设备开机时刻：uptime 基准（重启模拟器＝重启设备，uptime 归零）
	d.startedAt = time.Now()
	// 运行期改密状态从启动参数接过来（默认 true=已改密），之后由 cfg.set / cfg.reset 改写
	d.mu.Lock()
	d.pwdChanged = d.PwdChanged
	d.mu.Unlock()
	opts := mqtt.NewClientOptions().
		AddBroker(d.Broker).
		SetClientID("sim-" + d.ID).
		SetCleanSession(true).
		SetKeepAlive(30 * time.Second).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(3 * time.Second)
	if d.Username != "" {
		opts = opts.SetUsername(d.Username).SetPassword(d.Password)
	}
	// 遗嘱：异常断线通知平台离线
	will, _ := json.Marshal(envOf("lwt", "", map[string]any{}))
	opts = opts.SetWill(d.topic("up/status"), string(will), 1, false)
	opts.OnConnect = func(c mqtt.Client) {
		if tok := c.Subscribe(d.topic("down/#"), 1, d.onMessage); tok.Wait() && tok.Error() != nil {
			log.Printf("[idp %s] subscribe: %v", d.ID, tok.Error())
		}
		d.hello()
	}
	d.cli = mqtt.NewClient(opts)
	if tok := d.cli.Connect(); tok.WaitTimeout(15*time.Second) && tok.Error() != nil {
		return fmt.Errorf("mqtt connect %s: %w", d.Broker, tok.Error())
	}
	go d.reportLoop()
	go d.helloRetryLoop()
	return nil
}

// helloRetryLoop 未绑定时周期性重发 hello（规避与平台订阅的启动竞态）。
func (d *Device) helloRetryLoop() {
	t := time.NewTicker(15 * time.Second)
	defer t.Stop()
	for range t.C {
		if d.isBound() {
			return
		}
		d.hello()
	}
}

func (d *Device) isBound() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.bound
}

// Stop 断开并清理。
func (d *Device) Stop() {
	d.mu.Lock()
	for _, p := range d.pushes {
		close(p.stop)
	}
	d.pushes = map[string]*pushSession{}
	d.mu.Unlock()
	if d.cli != nil {
		d.cli.Disconnect(300)
	}
}

func (d *Device) topic(domain string) string {
	return fmt.Sprintf("idp/v1/%s/%s", d.ID, domain)
}

func (d *Device) logf(format string, args ...any) {
	if d.OnLog != nil {
		d.OnLog("[idp "+d.ID+"] "+format, args...)
	} else {
		log.Printf("[idp %s] "+format, append([]any{d.ID}, args...)...)
	}
}

// envOf 构造 IDP 信封。
func envOf(typ, msgID string, data map[string]any) map[string]any {
	if data == nil {
		data = map[string]any{}
	}
	return map[string]any{"v": 1, "type": typ, "msgId": msgID, "ts": time.Now().UnixMilli(), "data": data}
}

// publishUp 上行发布。
func (d *Device) publishUp(domain string, env map[string]any) {
	b, _ := json.Marshal(env)
	if tok := d.cli.Publish(d.topic("up/"+domain), 1, false, b); tok.WaitTimeout(5*time.Second) && tok.Error() != nil {
		d.logf("publish %s: %v", domain, tok.Error())
	}
}

// hello 上线声明（§5.5.2）。
func (d *Device) hello() {
	data := map[string]any{
		"model": "SIM-IPC-100", "vendor": "JetsCam", "fw": "1.0.0-sim", "hw": "GK7205V200",
		"channels": []map[string]any{{
			"ch":   1,
			"name": "SIM-CH1",
			"profiles": []map[string]any{
				{"profile": "main", "width": 640, "height": 360},
				{"profile": "sub", "width": 320, "height": 180},
			},
		}},
		"capabilities": []string{
			"live.main", "live.sub", "snapshot",
			"record.device.query", "record.device.play", "record.platform",
			"reboot", "ptz",
		},
		// 默认口令是否已修改：false 时平台在「本地账户」区块标黄提示。
		// 自研固件首次绑定/首次登录必须强制修改（接入规范 §590），所以这个字段要跟着 hello 一起来。
		// 这里是**当前**状态而不是启动参数原值：平台改密后应不再标黄，恢复出厂后应重新标黄
		"localUserChanged": d.pwdChangedNow(),
	}
	d.publishUp("status", envOf("status.hello", d.newMsgID(), data))
	// 平台对 hello 幂等处理（已绑定则直接刷新通道/在线态），视为已注册，
	// 避免 bound 恒为 false 导致的周期重发；cmd.bind 到达时仍会再次置位
	d.mu.Lock()
	d.bound = true
	d.mu.Unlock()
	d.logf("hello sent")
}

// reportLoop 周期状态上报（§5.5.2 status.report）。
//
// 指标必须是「活」的：早先这里是四个写死的常量（uptime 恒 3600、cpu/mem/temp 恒定），
// 于是平台详情页「在线时长」永远显示 01:00:00（用户报的死值），运行指标也不随后续上报变化，
// 连 30s 刷新链路都看不出来。
//
//	uptime —— 设备侧真实运行秒数（随上报自然增长；模拟器重启＝设备重启，故归零）
//	cpu/mem/temp —— 固定相位正弦的小幅波动，确定性、可复现（不引入随机数，避免同一份
//	                配置两次跑出不同上报值），数值范围对应一台普通 IPC 的空载波动。
func (d *Device) reportLoop() {
	t := time.NewTicker(30 * time.Second)
	defer t.Stop()
	for range t.C {
		up := time.Since(d.startedAt).Seconds()
		d.publishUp("status", envOf("status.report", d.newMsgID(), map[string]any{
			"cpu":    round1(12.5 + 4.0*math.Sin(up/37.0)),
			"mem":    round1(41.2 + 1.5*math.Sin(up/53.0)),
			"temp":   round1(38.5 + 1.2*math.Sin(up/41.0)),
			"uptime": int64(up),
		}))
	}
}

// round1 保留一位小数（与平台侧 fmtPercent/fmtTemp 的展示精度一致）。
func round1(v float64) float64 { return math.Round(v*10) / 10 }

func (d *Device) newMsgID() string {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.nextMsg++
	return fmt.Sprintf("m_sim_%s_%d", d.ID[len(d.ID)-4:], d.nextMsg)
}

// onMessage 下行命令处理。
func (d *Device) onMessage(_ mqtt.Client, msg mqtt.Message) {
	var env cmdMsg
	if err := json.Unmarshal(msg.Payload(), &env); err != nil {
		d.logf("bad payload: %v", err)
		return
	}
	// 平台回执（对设备上行/ack 的响应，type=cmd）一律忽略，避免与平台互相触发回执风暴
	if env.Type == "cmd" {
		return
	}
	typ := env.Type
	data := env.Data
	if data == nil {
		data = map[string]any{}
	}
	// 日志脱敏：cfg.set 的 values 可能带明文口令，不能直接 %v 打出去
	d.logf("cmd %s %v", typ, redactCmdData(data))
	switch typ {
	case "cmd.bind":
		d.bound = true
		d.ack(env, 0, "", map[string]any{})
	case "cmd.unbind":
		d.bound = false
		d.ack(env, 0, "", map[string]any{})
	case "cmd.transfer":
		d.ack(env, 0, "", map[string]any{})
	case "cmd.reboot":
		d.ack(env, 0, "", map[string]any{})
		go func() {
			time.Sleep(time.Second)
			d.hello()
		}()
	case "cmd.diag":
		d.ack(env, 0, "", map[string]any{"results": []map[string]any{
			{"item": "ping", "ok": true, "cost": 3},
			{"item": "dns", "ok": true, "cost": 5},
			{"item": "stun", "ok": true, "cost": 8},
		}})
	case "cmd.ptz":
		d.ack(env, 0, "", map[string]any{})
	case "cmd.snapshot":
		d.handleSnapshot(env, data)
	case "media.start":
		d.handleMediaStart(env, data)
	case "media.stop":
		d.handleMediaStop(data)
		d.ack(env, 0, "", map[string]any{})
	case "record.query":
		d.handleRecordQuery(env, data)
	case "record.play":
		d.handleRecordPlay(env, data)
	case "record.ctrl":
		d.ack(env, 0, "", map[string]any{})
	case "record.stop":
		sid, _ := data["sessionId"].(string)
		d.stopPush("rec:" + sid)
		d.ack(env, 0, "", map[string]any{})
	case "cfg.get":
		// keys 为空也要回全量：平台按 cfgKeys 下发，设备只回自己拥有的键（同固件按 pattern 匹配）。
		keys := []string{}
		if raw, ok := data["keys"].([]any); ok {
			for _, k := range raw {
				if s, ok := k.(string); ok {
					keys = append(keys, s)
				}
			}
		}
		d.ack(env, 0, "", map[string]any{"values": d.cfgGet(keys)})
	case "cfg.set":
		values, _ := data["values"].(map[string]any)
		rejected := d.cfgSet(values)
		d.ack(env, 0, "", map[string]any{"rejected": rejected})
		// 改过口令就重新宣告一次 hello：localUserChanged 是 hello 字段，
		// 不重发的话平台会一直显示“仍在用出厂默认口令”，直到下一次重连才自洽。
		// 失败（进了 rejected）不算数——没改成功就不该改变对外声称的状态。
		if cfgTouchedSecret(values, rejected) {
			go func() {
				time.Sleep(time.Second)
				d.hello()
			}()
		}
	case "cfg.reset":
		d.cfgReset()
		d.ack(env, 0, "", map[string]any{})
		go func() {
			time.Sleep(time.Second)
			d.hello()
		}()
	default:
		d.ack(env, 400, "unknown type", nil)
	}
}

func (d *Device) ack(req cmdMsg, code int, msg string, data map[string]any) {
	if data == nil {
		data = map[string]any{}
	}
	env := map[string]any{"v": 1, "type": "cmd", "msgId": req.MsgID,
		"ts": time.Now().UnixMilli(), "code": code, "msg": msg, "data": data}
	b, _ := json.Marshal(env)
	d.cli.Publish(d.topic("up/cmd"), 1, false, b)
}

// ---------- 媒体命令 ----------

func (d *Device) handleMediaStart(env cmdMsg, data map[string]any) {
	url, _ := data["url"].(string)
	token, _ := data["token"].(string)
	ch, _ := data["ch"].(float64)
	profile, _ := data["profile"].(string)
	if url == "" {
		d.ack(env, 400, "missing url", nil)
		return
	}
	key := fmt.Sprintf("live:%d:%s", int(ch), profile)
	if err := d.startPush(key, url, token); err != nil {
		d.ack(env, 500, err.Error(), nil)
		return
	}
	d.ack(env, 0, "", map[string]any{})
}

func (d *Device) handleMediaStop(data map[string]any) {
	ch, _ := data["ch"].(float64)
	profile, _ := data["profile"].(string)
	d.stopPush(fmt.Sprintf("live:%d:%s", int(ch), profile))
}

func (d *Device) handleRecordQuery(env cmdMsg, data map[string]any) {
	now := time.Now().UnixMilli()
	// 模拟录像：过去 12h 一段连续定时录像
	segs := []map[string]any{{"s": now - 12*3600*1000, "e": now, "type": "timer", "sizeKB": 864000}}
	d.ack(env, 0, "", map[string]any{"segments": segs, "total": len(segs)})
}

func (d *Device) handleRecordPlay(env cmdMsg, data map[string]any) {
	url, _ := data["url"].(string)
	token, _ := data["token"].(string)
	sid, _ := data["sessionId"].(string)
	if url == "" {
		d.ack(env, 400, "missing url", nil)
		return
	}
	if err := d.startPush("rec:"+sid, url, token); err != nil {
		d.ack(env, 500, err.Error(), nil)
		return
	}
	d.ack(env, 0, "", map[string]any{})
}

// handleSnapshot 上传快照到平台（§5.6 cmd.snapshot）。
func (d *Device) handleSnapshot(env cmdMsg, data map[string]any) {
	uploadURL, _ := data["uploadUrl"].(string)
	if uploadURL == "" || d.PlatformAddr == "" || len(d.SnapshotJPEG) == 0 {
		d.ack(env, 0, "", map[string]any{"url": ""})
		return
	}
	u := d.PlatformAddr + uploadURL
	req, err := http.NewRequest(http.MethodPost, u, bytes.NewReader(d.SnapshotJPEG))
	if err != nil {
		d.ack(env, 500, err.Error(), nil)
		return
	}
	req.Header.Set("Content-Type", "image/jpeg")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		d.ack(env, 500, "upload fail: "+err.Error(), nil)
		return
	}
	defer resp.Body.Close()
	// 平台 REST 的成功响应是「把 data 平铺在顶层」（api/common.go 的 ok()），
	// 不是 {code,data:{...}}——只有失败才回 {code,msg,suggest}。
	// 按嵌套结构解会静默拿到空 url，表现为「抓图成功但图片是空的」。
	var r struct {
		URL string `json:"url"`
	}
	_ = json.NewDecoder(resp.Body).Decode(&r)
	d.ack(env, 0, "", map[string]any{"url": r.URL})
}

// startPush 启动 RTMP 推流。
func (d *Device) startPush(key, url, token string) error {
	full := url
	if token != "" {
		sep := "?"
		if strings.Contains(full, "?") {
			sep = "&"
		}
		full += sep + "token=" + token
	}
	stop := make(chan struct{})
	c, app, stream, query, err := dialRTMP(full)
	if err != nil {
		return fmt.Errorf("rtmp dial: %w", err)
	}
	if err := c.publish(app, stream, query); err != nil {
		c.close()
		return fmt.Errorf("rtmp publish: %w", err)
	}
	d.mu.Lock()
	if old, ok := d.pushes[key]; ok {
		close(old.stop)
	}
	d.pushes[key] = &pushSession{stop: stop}
	d.mu.Unlock()
	d.logf("pushing %s → %s", key, url)
	go func() {
		defer c.close()
		sps, pps := d.Source.SPS(), d.Source.PPS()
		// 先发 AVC sequence header：ZLM 从 extradata 解出 SPS/PPS 得到宽高，
		// 帧内附带的参数集 NALU 不会被 FLV 解复用当作 extradata。
		if err := c.sendVideo(0, mediagen.AVCSeqHeader(sps, pps)); err != nil {
			d.logf("push %s broken: %v", key, err)
			return
		}
		// AAC 静音音频轨：真机推流通常带 AAC。纯视频 RTMP 流经 ZLM 转协议后
		// metadata 声明 audiocodecid 却无 audio tag，播放器转封装管线会死等
		// 音频初始化导致整流不出帧。
		aacPkt := func(seq bool, raw []byte) []byte {
			p := []byte{0xaf, 0x00} // AAC, seq header
			if !seq {
				p[1] = 0x01 // raw frame
			}
			return append(p, raw...)
		}
		if err := c.sendAudio(0, aacPkt(true, mediagen.AACSilenceASC)); err != nil {
			d.logf("push %s broken: %v", key, err)
			return
		}
		go func() {
			var ts uint32
			ticker := time.NewTicker(mediagen.AACSilenceFrameDurMs * time.Millisecond)
			defer ticker.Stop()
			for {
				select {
				case <-stop:
					return
				case <-ticker.C:
					if err := c.sendAudio(ts, aacPkt(false, mediagen.AACSilenceFrame)); err != nil {
						return
					}
					ts += mediagen.AACSilenceFrameDurMs
				}
			}
		}()
		for {
			select {
			case <-stop:
				return
			default:
			}
			f := d.Source.WaitNext()
			body := mediagen.AVCFrame(f, sps, pps)
			if err := c.sendVideo(uint32(f.DTS/90), body); err != nil {
				d.logf("push %s broken: %v", key, err)
				return
			}
		}
	}()
	return nil
}

// stopPush 停止指定推流。
func (d *Device) stopPush(key string) {
	d.mu.Lock()
	if p, ok := d.pushes[key]; ok {
		close(p.stop)
		delete(d.pushes, key)
	}
	d.mu.Unlock()
}

// EmitMotion 上报移动侦测事件（供闭环测试触发告警）。
func (d *Device) EmitMotion() {
	d.publishUp("event", envOf("event.motion", d.newMsgID(), map[string]any{
		"ch": 1, "ts": time.Now().UnixMilli(), "zone": 0,
	}))
}
