package engine

// 平台录像计划执行器（REC-06）：按模板周计划启停 ZLM startRecord。
// 常驻拉流：计划激活且无活跃流时经 StartPlay 起内部流。

import (
	"context"
	"log"
	"strings"
	"sync"
	"time"

	"github.com/jetscam/ipccloud/server/internal/devsvc"
	"github.com/jetscam/ipccloud/server/internal/media"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// StartRecordRunner 启动计划循环。
func (e *Engine) StartRecordRunner() {
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for range t.C {
			e.tickRecordPlans()
		}
	}()
}

// ---------- 事件录像状态机（REC-06 真正前缓冲）----------
//
// event 模板命中布防窗口时持续做短分段滚动录像（见 tickRecordPlans），分段落盘时
// 先归为 Type=pending（见 hooks.go 的 onRecordMP4 + classifyRecordType），是否
// "转正"为 Type=event 由告警到来时决定（见 engine.go 的 triggerEventRecording）。
// 这里只维护"当前 app/stream 是否处于布防窗口 / 告警后延长窗口"这一小块进程内状态，
// 重启丢失可接受——与 StreamSession 之外现有的 MVP 精度一致，不做持久化。
type eventStreamState struct {
	ExtendUntil int64 // 毫秒时间戳；now < ExtendUntil 期间新完成的分段直接落 Type=event
}

var (
	eventStreamsMu sync.Mutex
	eventStreams   = map[string]*eventStreamState{}
)

// markEventArmed 标记/取消标记某 app/stream 当前是否处于 event 模板的布防窗口内。
// armed=false 时仅在没有未过期的告警延长窗口时才彻底移除状态，避免布防窗口本身
// 结束的一刻，正好清掉刚触发告警、eventPostSec 延长窗口仍未到期的状态。
func markEventArmed(app, stream string, armed bool) {
	key := app + "/" + stream
	eventStreamsMu.Lock()
	defer eventStreamsMu.Unlock()
	st, ok := eventStreams[key]
	if armed {
		if !ok {
			eventStreams[key] = &eventStreamState{}
		}
		return
	}
	if ok && st.ExtendUntil <= models.NowMilli() {
		delete(eventStreams, key)
	}
}

// onEventPlanStopped 计划不再匹配（流已停止录像）时清理状态。
func onEventPlanStopped(app, stream string) {
	key := app + "/" + stream
	eventStreamsMu.Lock()
	defer eventStreamsMu.Unlock()
	delete(eventStreams, key)
}

// extendEventWindow 告警命中时延长某 app/stream 的"直接落 event"窗口，只延长不缩短
// （多次告警重叠时，取最晚的到期时间）。
func extendEventWindow(app, stream string, until int64) {
	key := app + "/" + stream
	eventStreamsMu.Lock()
	defer eventStreamsMu.Unlock()
	st, ok := eventStreams[key]
	if !ok {
		st = &eventStreamState{}
		eventStreams[key] = st
	}
	if until > st.ExtendUntil {
		st.ExtendUntil = until
	}
}

// classifyRecordType 供 onRecordMP4 落库时判定分段 Type：
// 不在事件状态机内（timer 模板，或非本次录像涉及的流）→ timer；
// 在，且仍处于某次告警的前后缓冲确认窗口内 → event；
// 在，但尚未被任何告警认领 → pending（仅布防期间的滚动缓冲，等待 GC 回收或转正）。
func classifyRecordType(app, stream string) string {
	key := app + "/" + stream
	eventStreamsMu.Lock()
	defer eventStreamsMu.Unlock()
	st, ok := eventStreams[key]
	if !ok {
		return "timer"
	}
	if models.NowMilli() < st.ExtendUntil {
		return "event"
	}
	return "pending"
}

// tickRecordPlans 单轮调度。
//
// event 模板（REC-06）不是"命中告警才开始录"，而是在模板自身的 Schedule（即布防/预录
// 窗口，与 timer 模板复用同一套周×24h 网格）内持续做短分段录像，形成滚动缓冲区；
// ZLM 的 startRecord 无法回溯，只有这样才能在告警命中时把"告警前 N 秒"的已录分段一并
// 保留。分段粒度用 record.eventPreSec（默认 5s）而非 timer 固定的 60s，越短预录越精确。
// 是否落库为 event/pending/timer 由 eventStreams 状态机 + onRecordMP4 落库时判定，
// 这里只负责维护"当前是否处于布防窗口"（见 markEventArmed/onEventPlanStopped）。
func (e *Engine) tickRecordPlans() {
	var plans []models.RecordPlan
	store.DB.Raw(`SELECT p.* FROM record_plans p JOIN channels c ON c.id = p.channel_id
		JOIN devices d ON d.id = c.device_id
		WHERE p.enabled = true AND c.enabled = true AND d.deleted_at = 0`).Scan(&plans)
	activeKeys := map[string]bool{}
	for _, p := range plans {
		var tpl models.RecordTemplate
		if store.DB.First(&tpl, "id = ?", p.TemplateID).Error != nil {
			continue
		}
		var ch models.Channel
		if store.DB.First(&ch, "id = ?", p.ChannelID).Error != nil || !ch.Enabled {
			continue
		}
		var dev models.Device
		if store.DB.First(&dev, "id = ?", ch.DeviceID).Error != nil {
			continue
		}
		app, stream := e.recordStreamKey(&dev, &ch, p.Profile)
		isEvent := tpl.Kind == "event"
		now := time.Now()
		if !scheduleMatches(tpl.Schedule, now) {
			if isEvent {
				markEventArmed(app, stream, false)
			}
			continue
		}
		key := app + "/" + stream
		activeKeys[key] = true
		maxSec := 60
		if isEvent {
			maxSec = eventPreSec(ch.ProjectID)
			markEventArmed(app, stream, true)
		} else {
			markEventArmed(app, stream, false)
		}
		node, err := e.Scheduler.Pick(ch.ID, p.Profile)
		if err != nil {
			continue
		}
		// 无活跃流则内部起流；会话存在但 ZLM 已无该流（如节点重启）视为陈旧会话
		if _, ok := devsvc.StreamSessionByStreamKey(app, stream); !ok {
			go e.startInternalStream(&dev, &ch, p.Profile, node, maxSec)
			continue
		}
		if !streamAlive(node, app, stream) {
			devsvc.StreamSessionClose(app, stream)
			go e.startInternalStream(&dev, &ch, p.Profile, node, maxSec)
			continue
		}
		_ = media.ForNode(node).StartRecord(app, stream, media.RecordTypeMP4, maxSec)
	}
	// 停止不再匹配的录像
	var sess []models.StreamSession
	store.DB.Where("kind = 'record' AND ended_at = 0").Find(&sess)
	for _, ss := range sess {
		if !activeKeys[ss.App+"/"+ss.Stream] {
			if node, err := devsvc.NodeByID(ss.NodeID); err == nil {
				_ = media.ForNode(node).StopRecord(ss.App, ss.Stream, media.RecordTypeMP4)
			}
			devsvc.StreamSessionClose(ss.App, ss.Stream)
			onEventPlanStopped(ss.App, ss.Stream)
		}
	}
}

// triggerEventRecording REC-06 事件录像的告警接入点（由 engine.go 的 platformEvent
// 异步调用，与 captureAlarmSnapshot 同一风格）。对该通道下处于布防窗口的 event 模板
// 录像计划：把覆盖 [alarmTs-eventPreSec, alarmTs] 的 pending 分段转正为 event（脱离
// pending 的快速 TTL 清理，转入正常 keepDays/totalBytes 保留策略），并延长该流"新分段
// 直接落 event"的窗口至 alarmTs+eventPostSec（只延长不缩短，多次告警重叠取最晚）。
//
// 只处理 event 模板的计划——timer 模板本就全程落 Type=timer，不受告警影响。
func (e *Engine) triggerEventRecording(projectID, channelID string, alarmTs int64) {
	var ch models.Channel
	if store.DB.First(&ch, "id = ?", channelID).Error != nil {
		return
	}
	var dev models.Device
	if store.DB.First(&dev, "id = ?", ch.DeviceID).Error != nil {
		return
	}
	var plans []models.RecordPlan
	store.DB.Where("channel_id = ? AND enabled = true", channelID).Find(&plans)
	for _, p := range plans {
		var tpl models.RecordTemplate
		if store.DB.First(&tpl, "id = ?", p.TemplateID).Error != nil || tpl.Kind != "event" {
			continue
		}
		if !scheduleMatches(tpl.Schedule, time.Now()) {
			continue
		}
		app, stream := e.recordStreamKey(&dev, &ch, p.Profile)
		preSec := eventPreSec(projectID)
		cutoff := alarmTs - int64(preSec)*1000
		store.DB.Model(&models.RecordIndex{}).
			Where("channel_id = ? AND type = 'pending' AND end_ts >= ?", channelID, cutoff).
			Update("type", "event")
		extendEventWindow(app, stream, alarmTs+int64(eventPostSec(projectID))*1000)
	}
}

func (e *Engine) recordStreamKey(dev *models.Device, ch *models.Channel, profile string) (string, string) {
	switch dev.Source {
	case "idp":
		return "live", streamKeyIDP(dev, ch, profile)
	case "gb28181":
		key := "gbStream"
		if profile == "sub" {
			key = "gbStreamSub"
		}
		// meta.gbStream 本身已含码流后缀（<gbChannelId>_<profile>），直接作为流名；
		// 缺失时按 gbChannelId + profile 构造（与 StartPlay/StopPlay 一致）
		if gbCh, _ := ch.Meta[key].(string); gbCh != "" {
			return "rtp", gbCh
		}
		gbChID, _ := ch.Meta["gbChannelId"].(string)
		return "rtp", gbChID + "_" + profile
	default:
		return "proxy", ch.ID + "_" + profile
	}
}

func streamKeyIDP(dev *models.Device, ch *models.Channel, profile string) string {
	return dev.ID + "_" + itoa(ch.Idx) + "_" + profile
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b [8]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	return string(b[i:])
}

// streamAlive 检查 ZLM 上该流是否仍在（任一 schema 注册即认为存活）。
func streamAlive(node *models.MediaNode, app, stream string) bool {
	list, err := media.ForNode(node).GetMediaList(context.Background())
	if err != nil {
		return true // 查询失败时保守认为存活，避免误停误起
	}
	for _, m := range list {
		if m["app"] == app && m["stream"] == stream {
			return true
		}
	}
	return false
}

// startInternalStream 内部起流（kind=record，不受 none_reader 影响）。
// 起流成功后立即 StartRecord：等下个 tick 再录会在 none_reader 延迟内被 ZLM 关流。
func (e *Engine) startInternalStream(dev *models.Device, ch *models.Channel, profile string, node *models.MediaNode, maxSecond int) {
	app, stream := e.recordStreamKey(dev, ch, profile)
	if _, err := e.StartPlay("system", ch.ID, profile); err != nil {
		log.Printf("[record-runner] internal start %s: %v", stream, err)
		return
	}
	devsvc.StreamSessionOpen(ch.ID, profile, node.ID, app, stream, "record")
	if err := media.ForNode(node).StartRecord(app, stream, media.RecordTypeMP4, maxSecond); err != nil {
		log.Printf("[record-runner] startRecord %s/%s: %v", app, stream, err)
	}
}

// ---------- 录像相关设置项（沿用通用 PUT /settings，按项目 scope）----------

// settingInt 读取 scope=projectID 下的整数设置项，取不到或非法时返回 def。
func settingInt(projectID, key string, def int) int {
	var st models.Setting
	if err := store.DB.First(&st, "scope = ? AND key = ?", projectID, key).Error; err != nil {
		return def
	}
	if v, ok := st.Value["value"].(float64); ok && v > 0 {
		return int(v)
	}
	return def
}

// eventPreSec event 模板布防窗口内的滚动分段时长（秒），默认 5：越短前缓冲越精确，
// 但分段越多、GC 压力越大，故设为可配置。
func eventPreSec(projectID string) int { return settingInt(projectID, "record.eventPreSec", 5) }

// eventPostSec 告警命中后延长"直接落 event"窗口的时长（秒），默认 30。
func eventPostSec(projectID string) int { return settingInt(projectID, "record.eventPostSec", 30) }

// pendingTTLSec 未被告警认领的 pending 分段的快速回收 TTL（秒），默认 90——
// 需明显大于 eventPreSec 默认值，留出容错余量（见 recordgc.go）。
func pendingTTLSec(projectID string) int { return settingInt(projectID, "record.pendingTTLSec", 90) }

// scheduleMatches schedule {"days":[1..7],"ranges":[["00:00","24:00"]]}。
//
// 交叉引用：engine 包内还有一份 alarmrule.go 的 scheduleActiveAt，求值**同一个**
// Schedule 结构却语义相反——这边 days 为空表示全天录且忽略 ranges、ranges 为空表示
// 从不录、按服务器本地时区求值；那边 days/ranges 为空都表示不限（全天候放行），按
// 项目时区求值。两份实现由同一个周×24h 网格编辑器产出的模板输入，用户会理所当然地
// 认为语义相同——内置模板（days 全选 + 00:00-24:00）下两者结论一致，掩盖了分歧，但
// 一旦出现 days 或 ranges 为空的自定义模板，两条链路立刻分叉。此为已知限制（详见
// 设计文档 §6），修改任一处求值逻辑前，必须同时确认另一处是否需要同步调整。
func scheduleMatches(sch models.JSONB, now time.Time) bool {
	rawDays, _ := sch["days"].([]any)
	rawRanges, _ := sch["ranges"].([]any)
	if len(rawDays) == 0 {
		return true
	}
	weekday := int(now.Weekday())
	if weekday == 0 {
		weekday = 7
	}
	dayOK := false
	for _, d := range rawDays {
		if int(toF(d)) == weekday {
			dayOK = true
		}
	}
	if !dayOK {
		return false
	}
	cur := now.Hour()*60 + now.Minute()
	for _, r := range rawRanges {
		parts, _ := r.([]any)
		if len(parts) != 2 {
			continue
		}
		from := parseHM(toStr(parts[0]))
		to := parseHM(toStr(parts[1]))
		if cur >= from && cur < to {
			return true
		}
	}
	return false
}

func toF(v any) float64 {
	if f, ok := v.(float64); ok {
		return f
	}
	return 0
}

func toStr(v any) string {
	if s, ok := v.(string); ok {
		return s
	}
	return ""
}

func parseHM(s string) int {
	s = strings.TrimSpace(s)
	var h, m int
	// 需要索引到 s[4]（"HH:MM" 的分钟十位），条件须为 len>=5；原先写成 len>=4 时，
	// 4 字符输入（如 "8:30"）会通过判断却越界访问 s[4] 而 panic。只修越界，不改变
	// 本函数对合法 "HH:MM" 输入的既有解析结果。
	if len(s) >= 5 {
		h = int(s[0]-'0')*10 + int(s[1]-'0')
		m = int(s[3]-'0')*10 + int(s[4]-'0')
	}
	return h*60 + m
}
