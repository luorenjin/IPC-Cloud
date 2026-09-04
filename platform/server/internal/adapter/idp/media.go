package idp

import (
	"context"
	"strconv"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// deviceChannel 由通道解析设备与通道。
func deviceChannel(channelID string) (*models.Device, *models.Channel, error) {
	var ch models.Channel
	if err := store.DB.First(&ch, "id = ?", channelID).Error; err != nil {
		return nil, nil, errs.ENotFound
	}
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", ch.DeviceID).Error; err != nil {
		return nil, nil, errs.ENotFound
	}
	if dev.Source != Source {
		return nil, nil, errs.EForbid.WithMsg("非 IDP 设备")
	}
	return &dev, &ch, nil
}

// StartStream 下发 media.start（§5.8；engine 负责选节点与等待出流）。
func (a *Adapter) StartStream(ctx context.Context, opt adapter.StartOptions) error {
	dev, ch, err := deviceChannel(opt.ChannelID)
	if err != nil {
		return err
	}
	_, err = a.request(dev.ID, "media", "media.start", map[string]any{
		"ch": ch.Idx, "profile": opt.Profile, "url": opt.PushURL,
		"token": opt.PushToken, "ttlS": 60,
	})
	return err
}

// StopStream 下发 media.stop。
func (a *Adapter) StopStream(opt adapter.StopOptions) {
	dev, ch, err := deviceChannel(opt.ChannelID)
	if err != nil {
		return
	}
	prof := "main"
	if len(opt.Stream) > 4 && opt.Stream[len(opt.Stream)-4:] == "_sub" {
		prof = "sub"
	}
	a.publishTo(dev.ID, "media", Envelope{Type: "media.stop",
		Data: map[string]any{"ch": ch.Idx, "profile": prof}})
}

// QueryRecords record.query（§5.9）。
func (a *Adapter) QueryRecords(ctx context.Context, q adapter.RecordQuery) ([]map[string]any, int, bool, error) {
	dev, ch, err := deviceChannel(q.ChannelID)
	if err != nil {
		return nil, 0, false, err
	}
	if !dev.HasCapability("record.device.query") {
		return nil, 0, false, nil
	}
	data, err := a.request(dev.ID, "record", "record.query", map[string]any{
		"ch": ch.Idx, "start": q.Start, "end": q.End, "types": q.Types,
		"page": q.Page, "pageSize": q.PageSize,
	})
	if err != nil {
		return nil, 0, true, err
	}
	segs, _ := data["segments"].([]any)
	out := make([]map[string]any, 0, len(segs))
	for _, s := range segs {
		if m, ok := s.(map[string]any); ok {
			out = append(out, m)
		}
	}
	total := int(floatOf(data["total"]))
	return out, total, true, nil
}

// StartPlayback record.play。
func (a *Adapter) StartPlayback(ctx context.Context, p adapter.PlaybackStart) error {
	dev, ch, err := deviceChannel(p.ChannelID)
	if err != nil {
		return err
	}
	if !dev.HasCapability("record.device.play") {
		return errs.EGBRecordUnsup
	}
	_, err = a.request(dev.ID, "record", "record.play", map[string]any{
		"ch": ch.Idx, "sessionId": p.SessionID, "start": p.Start, "end": p.End,
		"speed": p.Speed, "url": p.PushURL, "token": p.PushToken, "ttlS": 60,
	})
	return err
}

// PlaybackCtrl record.ctrl。
func (a *Adapter) PlaybackCtrl(ctx context.Context, p adapter.PlaybackCtrl) error {
	dev, _, err := deviceChannel(p.ChannelID)
	if err != nil {
		return err
	}
	data := map[string]any{"sessionId": p.SessionID, "op": p.Op}
	switch p.Op {
	case "seek":
		data["ts"] = p.SeekTs
	case "speed":
		data["speed"] = p.Speed
	}
	_, err = a.request(dev.ID, "record", "record.ctrl", data)
	return err
}

// StopPlayback record.stop。
func (a *Adapter) StopPlayback(ctx context.Context, channelID, sessionID string) {
	dev, _, err := deviceChannel(channelID)
	if err != nil {
		return
	}
	a.publishTo(dev.ID, "record", Envelope{Type: "record.stop",
		Data: map[string]any{"sessionId": sessionID}})
}

// Reboot cmd.reboot。
func (a *Adapter) Reboot(ctx context.Context, deviceID string) error {
	_, err := a.request(deviceID, "cmd", "cmd.reboot", map[string]any{})
	return err
}

// Diagnose cmd.diag。
func (a *Adapter) Diagnose(ctx context.Context, deviceID string) ([]map[string]any, error) {
	data, err := a.request(deviceID, "cmd", "cmd.diag", map[string]any{
		"items": []string{"ping", "dns", "stun"},
	})
	if err != nil {
		return nil, err
	}
	if raw, ok := data["results"].([]any); ok {
		out := make([]map[string]any, 0, len(raw))
		for _, r := range raw {
			if m, ok := r.(map[string]any); ok {
				out = append(out, m)
			}
		}
		return out, nil
	}
	return nil, nil
}

// ConfigGet cfg.get。
func (a *Adapter) ConfigGet(deviceID string, keys []string) (map[string]any, error) {
	return a.request(deviceID, "cfg", "cfg.get", map[string]any{"keys": keys})
}

// ConfigSet cfg.set；返回 rejected 列表（§5.7）。
func (a *Adapter) ConfigSet(deviceID string, values map[string]any) ([]string, error) {
	data, err := a.request(deviceID, "cfg", "cfg.set", map[string]any{"values": values})
	if err != nil {
		return nil, err
	}
	var rejected []string
	if raw, ok := data["rejected"].([]any); ok {
		for _, r := range raw {
			if s, ok := r.(string); ok {
				rejected = append(rejected, s)
			}
		}
	}
	return rejected, nil
}

// PTZ cmd.ptz。
func (a *Adapter) PTZ(channelID, op string, pan, tilt, zoom, speed float64, preset int) error {
	dev, ch, err := deviceChannel(channelID)
	if err != nil {
		return err
	}
	if !dev.HasCapability("ptz") {
		return errs.EForbid.WithMsg("该设备不支持云台控制")
	}
	data := map[string]any{"ch": ch.Idx, "op": op, "speed": speed}
	if pan != 0 {
		data["pan"] = pan
	}
	if tilt != 0 {
		data["tilt"] = tilt
	}
	if zoom != 0 {
		data["zoom"] = zoom
	}
	if preset > 0 {
		data["preset"] = preset
	}
	_, err = a.request(dev.ID, "cmd", "cmd.ptz", data)
	return err
}

func floatOf(v any) float64 {
	switch x := v.(type) {
	case float64:
		return x
	case string:
		f, _ := strconv.ParseFloat(x, 64)
		return f
	}
	return 0
}