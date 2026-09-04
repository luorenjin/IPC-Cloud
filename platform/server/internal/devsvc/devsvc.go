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
		} else {
			ch.Name = hc.Name
			ch.Profiles = profs
			ch.Capabilities = models.StringSlice(caps)
			ch.UpdatedAt = models.NowMilli()
			store.DB.Save(&ch)
		}
	}
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
