// Package engine 起播编排、ZLM Hook 处理、告警联动与回放会话管理。
package engine

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"time"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/config"
	"github.com/jetscam/ipccloud/server/internal/devsvc"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/media"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

type Engine struct {
	Cfg       *config.Config
	Scheduler *media.Scheduler
}

func New(cfg *config.Config) *Engine {
	return &Engine{Cfg: cfg, Scheduler: media.NewScheduler()}
}

// playTarget 起播结果。
type playTarget struct {
	ChannelID string
	Profile   string
	Node      *models.MediaNode
	App       string
	Stream    string
}

// streamNames 按来源生成 app/stream（§8.3）。
func streamNames(dev *models.Device, ch *models.Channel, profile string) (app, stream string) {
	switch dev.Source {
	case "idp":
		return "live", fmt.Sprintf("%s_%d_%s", dev.ID, ch.Idx, profile)
	case "gb28181":
		key := "gbStream"
		if profile == "sub" {
			key = "gbStreamSub"
		}
		gbCh, _ := ch.Meta[key].(string)
		if gbCh == "" {
			gbCh, _ = ch.Meta["gbChannelId"].(string)
		}
		return "rtp", gbCh + "" // streamKey 由调用方拼接 profile
	case "onvif", "rtsp":
		return "proxy", ch.ID + "_" + profile
	}
	return "proxy", ch.ID + "_" + profile
}

// rtspPullURL ONVIF/RTSP 的拉流地址。
func pullURL(dev *models.Device, ch *models.Channel, profile string) string {
	if dev.Source == "rtsp" {
		u, _ := dev.Identity["url"].(string)
		return u
	}
	key := "rtspMain"
	if profile == "sub" {
		key = "rtspSub"
	}
	if u, ok := ch.Meta[key].(string); ok {
		return u
	}
	return ""
}

// StartPlay 按需起播（LIVE-02）：选节点 → 起设备侧流/拉代理 → 等出流 → 返回播放地址。
func (e *Engine) StartPlay(userID, channelID, profile string) (map[string]any, error) {
	var ch models.Channel
	if err := store.DB.First(&ch, "id = ?", channelID).Error; err != nil {
		return nil, errs.ENotFound
	}
	if !ch.Enabled {
		return nil, errs.EForbid.WithMsg("通道已禁用")
	}
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", ch.DeviceID).Error; err != nil {
		return nil, errs.ENotFound
	}
	if dev.CapabilityMissing("live." + profile) {
		return nil, errs.EForbid.WithMsg("该通道不支持" + profile + "码流")
	}

	node, err := e.Scheduler.Pick(channelID, profile)
	if err != nil {
		return nil, errs.ENodeOffline
	}
	zlm := media.ForNode(node)

	var app, stream string
	up := false // 流已在节点注册（常驻推流/代理），可直接复用
	switch dev.Source {
	case "idp":
		app = "live"
		stream = fmt.Sprintf("%s_%d_%s", dev.ID, ch.Idx, profile)
		// 流已注册时不会再触发 regist 事件，重复起流会令 waitStreaming 死等超时
		if streamAlive(node, app, stream) {
			up = true
			break
		}
		pushToken, _ := media.PushToken(node.ID, app, stream)
		pushURL := fmt.Sprintf("rtmp://%s:%d/%s/%s", media.DevicePushHost(node), node.RTMPPort, app, stream)
		idp := adapter.Get("idp")
		devsvc.SetChannelStream(channelID, "starting")
		go func() {
			if err := idp.StartStream(context.Background(), adapter.StartOptions{
				ChannelID: channelID, Profile: profile, PushURL: pushURL,
				PushToken: pushToken, TTLSec: 60, App: app, Stream: stream,
			}); err != nil {
				log.Printf("[live] idp start: %v", err)
				devsvc.SetChannelStream(channelID, "error")
			}
		}()
	case "gb28181":
		app = "rtp"
		key := "gbStream"
		if profile == "sub" {
			key = "gbStreamSub"
		}
		gbCh, ok := ch.Meta[key].(string)
		if !ok || gbCh == "" {
			// meta 缺失时按 <gbChannelId>_<profile> 构造
			gbChID, _ := ch.Meta["gbChannelId"].(string)
			gbCh = gbChID + "_" + profile
		}
		// meta.gbStream 本身已含码流后缀（<gbChannelId>_<profile>），直接作为流名
		stream = gbCh
		// 流已注册说明收流端口仍开放且设备在推流，复用即可
		if streamAlive(node, app, stream) {
			up = true
			break
		}
		port, err := zlm.OpenRtpServer(context.Background(), 0, 1, stream)
		if err != nil {
			return nil, errs.ENodeOffline.WithMsg("openRtpServer 失败")
		}
		devsvc.SetChannelStream(channelID, "starting")
		gb := adapter.Get("gb28181")
		go func() {
			if err := gb.StartStream(context.Background(), adapter.StartOptions{
				ChannelID: channelID, Profile: profile, Node: node, RtpPort: port,
				App: app, Stream: stream,
			}); err != nil {
				log.Printf("[live] gb invite: %v", err)
				_ = zlm.CloseRtpServer(stream)
				devsvc.SetChannelStream(channelID, "error")
			}
		}()
	default: // onvif / rtsp：addStreamProxy
		app = "proxy"
		stream = ch.ID + "_" + profile
		// 代理流常驻时无新 regist 事件，直接复用，避免 addStreamProxy 幂等+死等
		if streamAlive(node, app, stream) {
			up = true
			break
		}
		url := pullURL(&dev, &ch, profile)
		if url == "" {
			return nil, errs.EForbid.WithMsg("缺少拉流地址，请重新同步设备")
		}
		devsvc.SetChannelStream(channelID, "starting")
		key := "__defaultVhost__/" + app + "/" + stream
		if err := zlm.AddStreamProxy(key, url, 8); err != nil {
			devsvc.SetChannelStream(channelID, "error")
			return nil, errs.EUnreachable.WithMsg(err.Error())
		}
	}

	// 复用已注册流：状态本就是 streaming，无需等待
	if up {
		devsvc.SetChannelStream(channelID, "streaming")
	} else if !e.waitStreaming(channelID, 10*time.Second) { // 等待出流（≤10s，§8.6）
		devsvc.SetChannelStream(channelID, "error")
		go e.StopPlay(channelID, profile, "timeout")
		return nil, errs.EStreamTimeout
	}

	token, _ := media.PlayToken(userID, channelID, app, stream, e.Cfg.PlayTokenTTLMin)
	devsvc.StreamSessionOpen(channelID, profile, node.ID, app, stream, "live")
	return map[string]any{
		"channelId": channelID, "profile": profile,
		"node":     map[string]any{"id": node.ID, "publicHost": node.PublicHost, "httpPort": node.HTTPPort, "httpsPort": node.HTTPSPort},
		"app":      app, "stream": stream,
		"wsFlv":    e.flvURL(node, app, stream, token, "ws"),
		"wssFlv":   e.flvURL(node, app, stream, token, "wss"),
		"token":    token,
	}, nil
}

func (e *Engine) flvURL(node *models.MediaNode, app, stream, token, scheme string) string {
	port := node.HTTPPort
	if scheme == "wss" {
		port = node.HTTPSPort
	}
	schemeHost := fmt.Sprintf("%s://%s:%d", scheme, node.PublicHost, port)
	return fmt.Sprintf("%s/%s/%s.live.flv?token=%s", schemeHost, app, stream, token)
}

// waitStreaming 轮询通道出流。
func (e *Engine) waitStreaming(channelID string, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		var ch models.Channel
		if store.DB.First(&ch, "id = ?", channelID).Error == nil && ch.StreamState == "streaming" {
			return true
		}
		time.Sleep(250 * time.Millisecond)
	}
	return false
}

// StopPlay 停流：设备侧停推 + 节点清理（BYE/closeRtpServer/delStreamProxy）。
func (e *Engine) StopPlay(channelID, profile, reason string) {
	var ch models.Channel
	if store.DB.First(&ch, "id = ?", channelID).Error != nil {
		return
	}
	var dev models.Device
	if store.DB.First(&dev, "id = ?", ch.DeviceID).Error != nil {
		return
	}
	stream := ""
	switch dev.Source {
	case "idp":
		stream = fmt.Sprintf("%s_%d_%s", dev.ID, ch.Idx, profile)
	case "gb28181":
		key := "gbStream"
		if profile == "sub" {
			key = "gbStreamSub"
		}
		gbCh, ok := ch.Meta[key].(string)
		if !ok || gbCh == "" {
			gbChID, _ := ch.Meta["gbChannelId"].(string)
			gbCh = gbChID + "_" + profile
		}
		// 与 StartPlay 一致：meta.gbStream 已含码流后缀
		stream = gbCh
	default:
		stream = ch.ID + "_" + profile
	}
	var ss models.StreamSession
	has := store.DB.First(&ss, "channel_id = ? AND app IN ('live','rtp','proxy') AND stream = ? AND ended_at = 0",
		channelID, stream).Error == nil
	_ = has

	if dev.Source != "onvif" && dev.Source != "rtsp" {
		adapter.Get(dev.Source).StopStream(adapter.StopOptions{
			ChannelID: channelID, App: "live", Stream: stream, Reason: reason,
		})
	}
	// 节点清理
	if node, errN := devsvc.NodeByID(ss.NodeID); errN == nil && dev.Source != "idp" {
		zlm := media.ForNode(node)
		if dev.Source == "gb28181" {
			_ = zlm.CloseRtpServer(stream)
		} else {
			_ = zlm.DelStreamProxy("__defaultVhost__/proxy/" + stream)
		}
	}
	devsvc.StreamSessionClose("live", stream)
	devsvc.StreamSessionClose("rtp", stream)
	devsvc.StreamSessionClose("proxy", stream)
}

// Snapshot 抓图：IDP 走设备；其余经节点 getSnap（需已出流）。
func (e *Engine) Snapshot(channelID string) (string, error) {
	var ch models.Channel
	if err := store.DB.First(&ch, "id = ?", channelID).Error; err != nil {
		return "", errs.ENotFound
	}
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", ch.DeviceID).Error; err != nil {
		return "", errs.ENotFound
	}
	if dev.Source == "idp" {
		uploadURL := fmt.Sprintf("/api/v1/upload/snapshot?channelId=%s&token=ipccloud", channelID)
		url, err := adapter.Get("idp").Snapshot(context.Background(), channelID, uploadURL)
		if err != nil {
			return "", err
		}
		if url != "" {
			store.DB.Model(&ch).Update("cover_url", url)
		}
		return url, nil
	}
	// ZLM 抓图（对正在播放的流）
	node, _ := e.Scheduler.Pick(channelID, "main")
	if node == nil {
		return "", errs.ENodeOffline
	}
	url := pullURL(&dev, &ch, "main")
	zlm := media.ForNode(node)
	b, err := zlm.GetSnap(url, 6)
	if err != nil {
		return "", err
	}
	fname := fmt.Sprintf("%s/%s_cover_%d.jpg", e.Cfg.DataDir, channelID, time.Now().UnixMilli())
	_ = storeWriteFile(fname, b)
	store.DB.Model(&ch).Update("cover_url", "/api/v1/static/"+baseName(fname))
	return "/api/v1/static/" + baseName(fname), nil
}

// RefreshCover 封面刷新（MGR-05）。
func (e *Engine) RefreshCover(channelID string) (string, error) {
	return e.Snapshot(channelID)
}

func (e *Engine) SubscribeEvents() {
	bus.Default.Subscribe("*", func(ev bus.Event) {
		switch {
		case ev.Type == "device.offline" || ev.Type == "device.online":
			e.platformEvent(ev, map[string]string{
				"device.offline": "device_offline", "device.online": "",
			}[ev.Type])
		case ev.Type == "node.status":
			if st, _ := ev.Data["status"].(string); st == "offline" {
				e.platformEvent(ev, "node_offline")
			}
		case strings.HasPrefix(ev.Type, "alarm.") && ev.Type != "alarm.new":
			// 适配器归一化后的设备告警（motion/tamper/io/humanoid…），按策略落库；
			// alarm.new 是落库后的通知事件，不得再次消费（会递归放大）
			e.platformEvent(ev, strings.TrimPrefix(ev.Type, "alarm."))
		case ev.Type == "ota.progress":
			e.taskProgress(ev)
		}
	})
}

func (e *Engine) platformEvent(ev bus.Event, kind string) {
	if kind == "" {
		return
	}
	// 第一层：项目级类型开关（ALM-05），缺省全开
	if !e.policyEnabled(ev.ProjectID, kind) {
		return
	}
	// 第二层：仅 ALM-03 设备侧智能事件受通道规则与布防时段约束；
	// 平台侧事件（设备离线/节点离线等）与通道无关，未列举类型一律放行。
	if isDeviceSideKind(kind) && !channelRuleAllows(ev.ProjectID, ev.ChannelID, kind) {
		return
	}
	createAlarmEvent(ev.ProjectID, ev.DeviceID, ev.ChannelID, kind, "warn", ev.Data, "")
}

func (e *Engine) taskProgress(ev bus.Event) {}

func (e *Engine) policyEnabled(projectID, kind string) bool {
	if projectID == "" {
		return true
	}
	var st models.Setting
	if err := store.DB.First(&st, "scope = ? AND key = 'alarm.policies'", projectID).Error; err == nil {
		if m, ok := st.Value[kind].(map[string]any); ok {
			if en, ok := m["enabled"].(bool); ok {
				return en
			}
		}
	}
	return true
}

func createAlarmEvent(projectID, deviceID, channelID, kind, level string, data map[string]any, snapshot string) string {
	ev := models.AlarmEvent{
		ID: "al_" + models.NewID(), ProjectID: projectID, DeviceID: deviceID,
		ChannelID: channelID, Kind: kind, Level: level, Ts: models.NowMilli(),
		Data: models.JSONB(data), SnapshotURL: snapshot,
	}
	if err := store.DB.Create(&ev).Error; err != nil {
		log.Printf("alarm create: %v", err)
		return ""
	}
	raw, _ := json.Marshal(ev)
	var m map[string]any
	_ = json.Unmarshal(raw, &m)
	bus.Default.Publish(bus.Event{Type: "alarm.new", ProjectID: projectID,
		DeviceID: deviceID, ChannelID: channelID, Data: m})
	return ev.ID
}

// CreateAlarmEvent 供告警服务导出使用。
func CreateAlarmEvent(projectID, deviceID, channelID, kind, level string, data map[string]any, snapshot string) string {
	return createAlarmEvent(projectID, deviceID, channelID, kind, level, data, snapshot)
}
