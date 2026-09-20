package engine

// 平台录像计划执行器（REC-06）：按模板周计划启停 ZLM startRecord。
// 常驻拉流：计划激活且无活跃流时经 StartPlay 起内部流。

import (
	"context"
	"log"
	"strings"
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

type planState struct {
	PlanID    string
	ChannelID string
	Profile   string
	App       string
	Stream    string
}

// tickRecordPlans 单轮调度。
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
		now := time.Now()
		if !scheduleMatches(tpl.Schedule, now) {
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
		key := app + "/" + stream
		activeKeys[key] = true
		node, err := e.Scheduler.Pick(ch.ID, p.Profile)
		if err != nil {
			continue
		}
		// 无活跃流则内部起流；会话存在但 ZLM 已无该流（如节点重启）视为陈旧会话
		if _, ok := devsvc.StreamSessionByStreamKey(app, stream); !ok {
			go e.startInternalStream(&dev, &ch, p.Profile, node)
			continue
		}
		if !streamAlive(node, app, stream) {
			devsvc.StreamSessionClose(app, stream)
			go e.startInternalStream(&dev, &ch, p.Profile, node)
			continue
		}
		_ = media.ForNode(node).StartRecord(app, stream, media.RecordTypeMP4)
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
		}
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
func (e *Engine) startInternalStream(dev *models.Device, ch *models.Channel, profile string, node *models.MediaNode) {
	app, stream := e.recordStreamKey(dev, ch, profile)
	if _, err := e.StartPlay("system", ch.ID, profile); err != nil {
		log.Printf("[record-runner] internal start %s: %v", stream, err)
		return
	}
	devsvc.StreamSessionOpen(ch.ID, profile, node.ID, app, stream, "record")
	if err := media.ForNode(node).StartRecord(app, stream, media.RecordTypeMP4); err != nil {
		log.Printf("[record-runner] startRecord %s/%s: %v", app, stream, err)
	}
}

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
