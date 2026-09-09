// Package devsvc 设备/通道领域服务：适配器与 API 共用的统一模型维护逻辑。
package devsvc

import (
	"encoding/json"
	"strconv"
	"strings"

	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// SetDeviceStatus 更新设备状态并广播统一事件。
func SetDeviceStatus(deviceID, status string, appErr map[string]any) {
	var dev models.Device
	if store.DB.First(&dev, "id = ?", deviceID).Error != nil {
		return
	}
	dev.Status = status
	dev.LastSeenAt = models.NowMilli()
	if appErr == nil {
		dev.Error = models.JSONB{}
	} else {
		b, _ := json.Marshal(appErr)
		e := models.JSONB{}
		_ = json.Unmarshal(b, &e)
		dev.Error = e
	}
	store.DB.Save(&dev)
	bus.Default.Publish(bus.Event{
		Type: "device." + status, DeviceID: deviceID, Source: dev.Source, ProjectID: dev.ProjectID,
		Data: map[string]any{"name": dev.Name, "error": dev.Error},
	})
	if status == "offline" {
		store.DB.Model(&models.StreamSession{}).
			Where("ended_at = 0 AND channel_id IN (?)",
				store.DB.Model(&models.Channel{}).Select("id").Where("device_id = ?", deviceID)).
				Update("ended_at", models.NowMilli())
	}
}

// TouchDevice 更新 lastSeen。
func TouchDevice(deviceID string) {
	store.DB.Model(&models.Device{}).Where("id = ?", deviceID).Update("last_seen_at", models.NowMilli())
}

// HelloChannel hello.channels 条目（规范 §5.5.2）。
type HelloChannel struct {
	Ch       int              `json:"ch"`
	Name     string           `json:"name"`
	Profiles []map[string]any `json:"profiles"`
}

// UpsertChannels 按 hello 的 channels 覆盖通道与能力。
func UpsertChannels(deviceID, projectID string, caps []string, channels []HelloChannel) {
	for _, hc := range channels {
		var ch models.Channel
		err := store.DB.First(&ch, "device_id = ? AND idx = ?", deviceID, hc.Ch).Error
		raw, _ := json.Marshal(hc.Profiles)
		profs := models.JSONB{}
		_ = json.Unmarshal(raw, &profs)
		if err != nil {
			ch = models.Channel{
				ID: "ch_" + models.NewID(), DeviceID: deviceID, ProjectID: projectID,
				Idx: hc.Ch, Name: hc.Name, Enabled: true, StreamState: "idle",
				Profiles: profs, Capabilities: models.StringSlice(caps),
				CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli(),
			}
			store.DB.Create(&ch)
			ApplyDefaultRecordPlan(ch.ID, projectID) // ADD-09
			ApplyDefaultAlarmRule(ch.ID, projectID)  // ADD-09
		} else {
			ch.Name = hc.Name
			ch.Profiles = profs
			ch.Capabilities = models.StringSlice(caps)
			ch.UpdatedAt = models.NowMilli()
			store.DB.Save(&ch)
		}
	}
}

// RecordDefaultsKey 项目级"新通道默认录像策略"的设置键（scope = projectID）。
// 取值形如 {"enabled":true,"templateId":"rt_xxx","profile":"main"}。
const RecordDefaultsKey = "recordDefaults"

// ApplyDefaultRecordPlan 为新建通道套用项目默认录像计划（PRD ADD-09 的录像部分）。
//
// 设计要点：
//   - 未配置该设置时视为**关闭**——存量项目不会因为升级而突然开始录像占用存储；
//     新建项目由 handleCreateProject 显式写入 enabled=true。
//   - 幂等：通道已有录像计划则跳过，不覆盖用户手工配置（通道可能被适配器重复 upsert）。
//   - 任何一步不满足都静默跳过，绝不阻断设备接入主流程。
//
// 五处通道创建点共用：UpsertChannels(IDP)、gb28181 注册、国标确认、ONVIF、RTSP。
func ApplyDefaultRecordPlan(channelID, projectID string) {
	if channelID == "" || projectID == "" {
		return
	}
	var st models.Setting
	if store.DB.First(&st, "scope = ? AND key = ?", projectID, RecordDefaultsKey).Error != nil {
		return // 未配置 = 关闭
	}
	if enabled, _ := st.Value["enabled"].(bool); !enabled {
		return
	}
	tplID, _ := st.Value["templateId"].(string)
	if tplID == "" {
		return
	}
	// 模板须属于本项目，避免跨项目引用导致计划指向不可见模板
	var tpl models.RecordTemplate
	if store.DB.First(&tpl, "id = ? AND project_id = ?", tplID, projectID).Error != nil {
		return
	}
	var exists int64
	store.DB.Model(&models.RecordPlan{}).Where("channel_id = ?", channelID).Count(&exists)
	if exists > 0 {
		return
	}
	profile, _ := st.Value["profile"].(string)
	if profile != "sub" {
		profile = "main"
	}
	store.DB.Create(&models.RecordPlan{
		ID: "rp_" + models.NewID(), ChannelID: channelID, TemplateID: tplID,
		Profile: profile, Enabled: true,
		CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli(),
	})
}

// AlarmDefaultsKey 项目级"新通道默认告警策略"的设置键（scope = projectID）。
// 取值形如 {"enabled":true,"kinds":["motion",...],"templateId":"at_xxx"}。
const AlarmDefaultsKey = "alarmDefaults"

// ApplyDefaultAlarmRule 为新建通道套用项目默认告警规则（PRD ADD-09 的告警部分）。
//
// 与 ApplyDefaultRecordPlan **刻意不同**："未配置"与"显式关闭"在这里含义不同，不是
// 照搬录像语义时的遗漏：
//
//   - 录像的"未配置=关闭"是安全默认——不建录像计划=不占存储，可逆，随时能后补。
//   - 告警走的是严格模式（engine.channelRuleAllows）：通道没有任何启用的规则即拦截。
//     "未配置=关闭"放到这里等于"未配置=永久丢事件"——被拦截的告警不会重新出现，
//     不可逆。空库首装后接入的第一批设备、以及升级后新增的任何通道，若没有配套的
//     迁移或兜底，都会在拿到第一条规则之前的时间窗里丢光设备侧告警。
//
// 因此默认必须为通道建规则：读不到 alarmDefaults 设置时，退化为建一条兜底规则
// （全部 models.DeviceSideAlarmKinds + 本项目内置"全天候"布防模板；项目连内置模板
// 都没有时 TemplateID 留空——engine 把空/缺失模板判定为全天候放行，方向仍是安全的）。
// 只有设置存在且 enabled 显式为 false 时，才视为运营者主动选择不建规则。
//
// 幂等：通道已有规则则跳过，不覆盖用户手工配置。任何失败都静默跳过，不阻断设备接入
// 主流程。
func ApplyDefaultAlarmRule(channelID, projectID string) {
	if channelID == "" || projectID == "" {
		return
	}
	var exists int64
	store.DB.Model(&models.AlarmRule{}).Where("channel_id = ?", channelID).Count(&exists)
	if exists > 0 {
		return
	}

	var st models.Setting
	hasSetting := store.DB.First(&st, "scope = ? AND key = ?", projectID, AlarmDefaultsKey).Error == nil
	if hasSetting {
		if enabled, ok := st.Value["enabled"].(bool); ok && !enabled {
			return // 显式关闭：运营者主动选择不建规则
		}
	}

	// kinds：设置里给了非空列表就用它，否则（未配置 / 配置了但列表为空）退化为全集，
	// 避免建出一条 Kinds 为空、永不匹配、形同虚设的规则。
	kinds := models.DeviceSideAlarmKinds
	if hasSetting {
		if rawKinds, ok := st.Value["kinds"].([]any); ok {
			ks := make([]string, 0, len(rawKinds))
			for _, k := range rawKinds {
				if s, ok := k.(string); ok && s != "" {
					ks = append(ks, s)
				}
			}
			if len(ks) > 0 {
				kinds = ks
			}
		}
	}

	// 模板：设置里指定了就校验归属本项目，无效则降级；未指定（含未配置该设置的
	// 兜底路径）就取本项目内置"全天候"模板；项目连该模板都没有则留空——全天候放行。
	tplID := ""
	if hasSetting {
		if id, ok := st.Value["templateId"].(string); ok {
			tplID = id
		}
	}
	if tplID != "" {
		var tpl models.AlarmTemplate
		if store.DB.First(&tpl, "id = ? AND project_id = ?", tplID, projectID).Error != nil {
			tplID = "" // 模板不可用时降级为全天候，而非放弃建规则
		}
	}
	if tplID == "" {
		var tpl models.AlarmTemplate
		if store.DB.Where("project_id = ? AND builtin = ? AND name = ?", projectID, true, "全天候").
			First(&tpl).Error == nil {
			tplID = tpl.ID
		}
	}

	store.DB.Create(&models.AlarmRule{
		ID: "ar_" + models.NewID(), ProjectID: projectID, ChannelID: channelID,
		Kinds: models.StringSlice(kinds), TemplateID: tplID, Enabled: true,
		CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli(),
	})
}

// SetChannelStream 通道流状态变更（ZLM Hook 与起播流程共用）。
func SetChannelStream(channelID, state string) {
	store.DB.Model(&models.Channel{}).Where("id = ?", channelID).
		Updates(map[string]any{"stream_state": state, "updated_at": models.NowMilli()})
	var ch models.Channel
	if store.DB.First(&ch, "id = ?", channelID).Error != nil {
		return
	}
	bus.Default.Publish(bus.Event{Type: "channel.stream", ChannelID: channelID,
		DeviceID: ch.DeviceID, ProjectID: ch.ProjectID,
		Data:     map[string]any{"state": state, "name": ch.Name}})
}

// StreamSessionOpen 记录流会话。
func StreamSessionOpen(channelID, profile, nodeID, app, stream, kind string) *models.StreamSession {
	ss := &models.StreamSession{
		ID: "ss_" + models.NewID(), ChannelID: channelID, Profile: profile,
		NodeID: nodeID, App: app, Stream: stream, Kind: kind, StartedAt: models.NowMilli(),
	}
	store.DB.Create(ss)
	return ss
}

// StreamSessionClose 关闭流会话。
func StreamSessionClose(app, stream string) {
	store.DB.Model(&models.StreamSession{}).Where("app = ? AND stream = ? AND ended_at = 0", app, stream).
		Update("ended_at", models.NowMilli())
}

// StreamSessionByStreamKey 按流键查会话。
func StreamSessionByStreamKey(app, stream string) (*models.StreamSession, bool) {
	var ss models.StreamSession
	if store.DB.First(&ss, "app = ? AND stream = ? AND ended_at = 0", app, stream).Error != nil {
		return nil, false
	}
	return &ss, true
}

// FindChannelByStreamKey 按流名反查通道（on_stream_not_found 按需拉流）。
// 流命名见接入规范 §8.3：live=<DeviceID>_<ch>_<profile>；proxy=<channelId>_<profile>；
// rtp 流名在 GB INVITE 时写入 channel.meta.gbStream（主）与 meta.gbStreamSub（子）。
func FindChannelByStreamKey(app, stream string) (models.Channel, string, bool) {
	var ch models.Channel
	prof := "main"
	switch {
	case strings.HasSuffix(stream, "_sub"):
		prof = "sub"
	}
	base := stream
	switch {
	case strings.HasSuffix(stream, "_main"):
		base = stream[:len(stream)-5]
	case strings.HasSuffix(stream, "_sub"):
		base = stream[:len(stream)-4]
	}
	switch app {
	case "live": // <DeviceID>_<ch>_<profile>
		cut := strings.LastIndex(base, "_")
		if cut <= 0 {
			return ch, "", false
		}
		devID := base[:cut]
		idx, errI := strconv.Atoi(base[cut+1:])
		if errI == nil {
			// 流名含通道序号，优先精确匹配 idx
			if store.DB.First(&ch, "device_id = ? AND idx = ?", devID, idx).Error == nil {
				return ch, prof, true
			}
		}
		if store.DB.First(&ch, "device_id = ?", devID).Error == nil {
			return ch, prof, true
		}
	case "proxy": // <channelId>_<profile>
		if store.DB.First(&ch, "id = ?", base).Error == nil {
			return ch, prof, true
		}
	case "rtp": // 流名即 meta.gbStream/gbStreamSub（<gbChannelId>_<profile>，已含后缀）
		if store.DB.First(&ch, "meta->>'gbStream' = ? OR meta->>'gbStreamSub' = ?",
			stream, stream).Error == nil {
			return ch, prof, true
		}
		// 设备端回放流：<gbStream>_pb_<sid>
		if cut := strings.Index(stream, "_pb_"); cut > 0 {
			base := stream[:cut]
			if store.DB.First(&ch, "meta->>'gbStream' = ? OR meta->>'gbStreamSub' = ?",
				base, base).Error == nil {
				return ch, "main", true
			}
		}
	case "record": // IDP 设备端回放：<DeviceID>_<ch>_pb_<sid>
		if cut := strings.Index(stream, "_pb_"); cut > 0 {
			base2 := stream[:cut] // <DeviceID>_<ch>
			cut2 := strings.LastIndex(base2, "_")
			if cut2 > 0 {
				devID := base2[:cut2]
				if idx, errI := strconv.Atoi(base2[cut2+1:]); errI == nil {
					if store.DB.First(&ch, "device_id = ? AND idx = ?", devID, idx).Error == nil {
						return ch, "main", true
					}
				}
			}
			if store.DB.First(&ch, "device_id = ?", base2).Error == nil {
				return ch, "main", true
			}
		}
	}
	return ch, "", false
}

// NodeByID 取节点。
func NodeByID(id string) (*models.MediaNode, error) {
	var n models.MediaNode
	if err := store.DB.First(&n, "id = ?", id).Error; err != nil {
		return nil, err
	}
	return &n, nil
}
