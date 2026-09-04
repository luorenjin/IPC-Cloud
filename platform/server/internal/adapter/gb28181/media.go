package gb28181

import (
	"context"
	"encoding/xml"
	"fmt"
	"strings"
	"time"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

func devChannel(channelID string) (*models.Device, *models.Channel, error) {
	var ch models.Channel
	if err := store.DB.First(&ch, "id = ?", channelID).Error; err != nil {
		return nil, nil, errs.ENotFound
	}
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", ch.DeviceID).Error; err != nil {
		return nil, nil, errs.ENotFound
	}
	if dev.Source != Source {
		return nil, nil, errs.EForbid.WithMsg("非国标设备")
	}
	return &dev, &ch, nil
}

func gbChannelIDOf(dev *models.Device, ch *models.Channel, profile string) string {
	key := "gbStream"
	if profile == "sub" {
		key = "gbStreamSub"
	}
	if v, ok := ch.Meta[key].(string); ok && v != "" {
		return v
	}
	if v, ok := ch.Meta["gbChannelId"].(string); ok {
		return v
	}
	if v, ok := dev.Identity["gbId"].(string); ok {
		return v
	}
	return ""
}

// StartStream INVITE 实时点播（§6.2 INVITE）。
func (a *Adapter) StartStream(ctx context.Context, opt adapter.StartOptions) error {
	dev, ch, err := devChannel(opt.ChannelID)
	if err != nil {
		return err
	}
	gbChID := gbChannelIDOf(dev, ch, opt.Profile)
	if gbChID == "" {
		return errs.EGBRecordUnsup.WithMsg("通道缺少国标编号")
	}
	tcpMode := 1
	if v, ok := dev.Meta["transport"].(string); ok && v == "udp" {
		tcpMode = 0
	}
	_, _, err = a.Invite(gbChID, "Play", "", "", "0", opt.Node, opt.RtpPort, tcpMode)
	return err
}

// StopStream BYE + closeRtpServer 由 engine 处理节点部分；此处只发 BYE。
func (a *Adapter) StopStream(opt adapter.StopOptions) {
	dev, ch, err := devChannel(opt.ChannelID)
	if err != nil {
		return
	}
	prof := "main"
	if strings.HasSuffix(opt.Stream, "_sub") {
		prof = "sub"
	}
	if gbChID := gbChannelIDOf(dev, ch, prof); gbChID != "" {
		a.Bye(gbChID)
	}
}

func (a *Adapter) Snapshot(ctx context.Context, channelID, uploadURL string) (string, error) {
	return "", errs.EForbid.WithMsg("国标设备抓图经 ZLM getSnap 实现")
}

// QueryRecords RecordInfo（§6.2）。
func (a *Adapter) QueryRecords(ctx context.Context, q adapter.RecordQuery) ([]map[string]any, int, bool, error) {
	dev, ch, err := devChannel(q.ChannelID)
	if err != nil {
		return nil, 0, false, err
	}
	gbChID := gbChannelIDOf(dev, ch, "main")
	if gbChID == "" {
		return nil, 0, true, errs.ENotFound
	}
	typ := "all"
	if len(q.Types) > 0 {
		switch q.Types[0] {
		case "timer":
			typ = "time"
		case "event":
			typ = "alarm"
		case "manual":
			typ = "manual"
		}
	}
	items, err := a.QueryRecordsRaw(gbChID, q.Start, q.End, typ)
	if err != nil {
		return nil, 0, true, err
	}
	segs := make([]map[string]any, 0, len(items))
	for _, it := range items {
		s := parseGBTime(it.StartTime)
		e := parseGBTime(it.EndTime)
		segType := "timer"
		switch it.Type {
		case "alarm":
			segType = "event"
		case "manual":
			segType = "manual"
		}
		segs = append(segs, map[string]any{
			"s": s, "e": e, "type": segType, "sizeKB": it.FileSize,
		})
	}
	_ = dev
	return segs, len(segs), true, nil
}

func parseGBTime(s string) int64 {
	t, err := time.ParseInLocation("2006-01-02 15:04:05", s, time.UTC)
	if err != nil {
		return 0
	}
	return t.UnixMilli()
}

// StartPlayback Playback INVITE。
func (a *Adapter) StartPlayback(ctx context.Context, p adapter.PlaybackStart) error {
	dev, ch, err := devChannel(p.ChannelID)
	if err != nil {
		return err
	}
	gbChID := gbChannelIDOf(dev, ch, "main")
	if gbChID == "" {
		return errs.ENotFound
	}
	_, _, err = a.Invite(gbChID, "Playback",
		toGBTime(p.Start), toGBTime(p.End), "1", p.Node, p.RtpPort, 1)
	return err
}

// PlaybackCtrl INFO（PAUSE/PLAY Scale/Range/TEARDOWN→BYE）。
func (a *Adapter) PlaybackCtrl(ctx context.Context, p adapter.PlaybackCtrl) error {
	dev, ch, err := devChannel(p.ChannelID)
	if err != nil {
		return err
	}
	gbChID := gbChannelIDOf(dev, ch, "main")
	if gbChID == "" {
		return errs.ENotFound
	}
	var body string
	switch p.Op {
	case "pause":
		body = "PAUSE RTSP/1.0\r\nCSeq: 2\r\n"
	case "resume":
		body = "PLAY RTSP/1.0\r\nCSeq: 3\r\n"
	case "speed":
		body = fmt.Sprintf("PLAY RTSP/1.0\r\nCSeq: 4\r\nScale: %s\r\n", trimFloat(p.Speed))
	case "seek":
		sec := (p.SeekTs - p.BaseTs) / 1000
		if sec < 0 {
			sec = 0
		}
		body = fmt.Sprintf("PLAY RTSP/1.0\r\nCSeq: 5\r\nRange: npt=%d-\r\n", sec)
	default:
		return errs.EBadRequest
	}
	return a.Info(gbChID, body)
}

func (a *Adapter) StopPlayback(ctx context.Context, channelID, sessionID string) {
	dev, ch, err := devChannel(channelID)
	if err != nil {
		return
	}
	if gbChID := gbChannelIDOf(dev, ch, "main"); gbChID != "" {
		a.Bye(gbChID)
	}
}

// Reboot DeviceControl TeleBoot。
func (a *Adapter) Reboot(ctx context.Context, deviceID string) error {
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", deviceID).Error; err != nil {
		return errs.ENotFound
	}
	gbID, _ := dev.Identity["gbId"].(string)
	sn := a.nextSN()
	body := xmlEncodeUTF8(gbControl{CmdType: "DeviceControl", SN: sn, DeviceID: gbID, TeleBoot: "Boot"})
	return a.Info(gbID, body)
}

// gbControl DeviceControl 控制体（TeleBoot 重启）。
type gbControl struct {
	XMLName  xml.Name `xml:"Control"`
	CmdType  string   `xml:"CmdType"`
	SN       int64    `xml:"SN"`
	DeviceID string   `xml:"DeviceID"`
	TeleBoot string   `xml:"TeleBoot"`
}

func (a *Adapter) Diagnose(ctx context.Context, deviceID string) ([]map[string]any, error) {
	gbID := ""
	var dev models.Device
	if store.DB.First(&dev, "id = ?", deviceID).Error == nil {
		gbID, _ = dev.Identity["gbId"].(string)
	}
	out := []map[string]any{}
	if a.remoteOf(gbID) != nil {
		out = append(out, map[string]any{"item": "sip", "ok": true, "cost": 0})
	} else {
		out = append(out, map[string]any{"item": "sip", "ok": false, "cost": 0,
			"msg": "设备未注册"})
	}
	return out, nil
}

func (a *Adapter) Transfer(ctx context.Context, deviceID, projectID string) error {
	return nil // 国标设备转移仅平台侧字段变更
}

func (a *Adapter) Unbind(ctx context.Context, deviceID string) error {
	return nil
}

func trimFloat(f float64) string {
	s := fmt.Sprintf("%.2f", f)
	s = strings.TrimRight(s, "0")
	return strings.TrimSuffix(s, ".")
}
