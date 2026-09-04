package engine

// 回放会话管理（REC-01~04）：设备端回放（IDP/GB）经适配器；平台录像走 ZLM 静态文件。

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"time"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/devsvc"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/media"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

type pbSession struct {
	ChannelID string `json:"channelId"`
	SessionID string `json:"sessionId"`
	Source    string `json:"source"` // device|platform
	Base      int64  `json:"base"`
	NodeID    string `json:"nodeId"`
	App       string `json:"app"`
	Stream    string `json:"stream"`
	Start     int64  `json:"start"`
	End       int64  `json:"end"`
}

func pbKey(sid string) string { return "pb:" + sid }

func (e *Engine) savePb(s *pbSession) {
	b, _ := json.Marshal(s)
	_ = store.KVSet(pbKey(s.SessionID), string(b), time.Hour)
}

func (e *Engine) loadPb(sid string) (*pbSession, error) {
	v, err := store.KVGet(pbKey(sid))
	if err != nil {
		return nil, errs.ENotFound.WithMsg("回放会话不存在或已过期")
	}
	var s pbSession
	_ = json.Unmarshal([]byte(v), &s)
	return &s, nil
}

func (e *Engine) delPb(sid string) { _ = store.KVDel(pbKey(sid)) }

// QueryRecords 录像检索（按来源聚合，结果缓存 5 分钟走 KV）。
func (e *Engine) QueryRecords(channelID string, start, end int64, types []string, source string) (map[string]any, error) {
	var ch models.Channel
	if err := store.DB.First(&ch, "id = ?", channelID).Error; err != nil {
		return nil, errs.ENotFound
	}
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", ch.DeviceID).Error; err != nil {
		return nil, errs.ENotFound
	}

	switch source {
	case "platform":
		var recs []models.RecordIndex
		q := store.DB.Where("channel_id = ? AND start_ts < ? AND end_ts > ?", channelID, end, start)
		if len(types) > 0 {
			q = q.Where("type IN ?", types)
		}
		if err := q.Order("start_ts ASC").Find(&recs).Error; err != nil {
			return nil, err
		}
		segs := make([]map[string]any, 0, len(recs))
		for _, r := range recs {
			segs = append(segs, map[string]any{
				"s": r.StartTs, "e": r.EndTs, "type": r.Type, "sizeKB": r.Size / 1024,
				"path": r.Path,
			})
		}
		return map[string]any{"source": "platform", "total": len(segs), "segments": segs}, nil

	default: // device
		if !dev.HasCapability("record.device.query") {
			return nil, errs.EGBRecordUnsup
		}
		cacheKey := fmt.Sprintf("recq:%s:%d:%d", channelID, start, end)
		if v, err := store.KVGet(cacheKey); err == nil {
			var m map[string]any
			if json.Unmarshal([]byte(v), &m) == nil {
				return m, nil
			}
		}
		segs, total, ok, err := adapter.Get(dev.Source).QueryRecords(context.Background(), adapter.RecordQuery{
			ChannelID: channelID, Start: start, End: end, Types: types, Page: 1, PageSize: 500,
		})
		if err != nil {
			return nil, err
		}
		if !ok {
			return nil, errs.EGBRecordUnsup
		}
		out := map[string]any{"source": "device", "total": total, "segments": segs}
		if b, err := json.Marshal(out); err == nil {
			_ = store.KVSet(cacheKey, string(b), 5*time.Minute)
		}
		return out, nil
	}
}

// StartPlayback 起回放：source=device 走适配器；platform 返回 ZLM 静态文件地址。
func (e *Engine) StartPlayback(userID, channelID string, start, end int64, speed float64, source string) (map[string]any, error) {
	var ch models.Channel
	if err := store.DB.First(&ch, "id = ?", channelID).Error; err != nil {
		return nil, errs.ENotFound
	}
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", ch.DeviceID).Error; err != nil {
		return nil, errs.ENotFound
	}
	sessionID := "pb_" + models.NewID()
	node, err := e.Scheduler.Pick(channelID, "main")
	if err != nil {
		return nil, errs.ENodeOffline
	}

	if source == "" {
		source = "device"
	}
	if source == "platform" {
		// ZLM 静态点播：取覆盖该时间点或最近的录像段
		var rec models.RecordIndex
		if err := store.DB.Where("channel_id = ? AND source = 'platform' AND start_ts <= ? AND end_ts >= ?",
			channelID, start, start).Order("start_ts ASC").First(&rec).Error; err != nil {
			return nil, errs.ENotFound.WithMsg("该时间点无平台录像")
		}
		s := &pbSession{ChannelID: channelID, SessionID: sessionID, Source: "platform",
			NodeID: node.ID, App: "vod", Start: start, End: end}
		e.savePb(s)
		// 录像文件位于节点 ZLM www 根下（Path 为相对路径 record/…），经 ZLM HTTP 静态服务点播
		rel := strings.TrimPrefix(rec.Path, "/")
		url := fmt.Sprintf("http://%s:%d/%s", node.PublicHost, node.HTTPPort, rel)
		return map[string]any{
			"sessionId": sessionID, "source": "platform", "url": url,
			"seekable": true,
		}, nil
	}

	// 设备端回放
	if !dev.HasCapability("record.device.play") {
		return nil, errs.EGBRecordUnsup
	}
	var app, stream string
	switch dev.Source {
	case "idp":
		app = "record"
		stream = fmt.Sprintf("%s_%d_%s", dev.ID, ch.Idx, sessionID)
	case "gb28181":
		app = "rtp"
		gbCh, _ := ch.Meta["gbStream"].(string)
		stream = gbCh + "_pb_" + sessionID
	default:
		return nil, errs.EGBRecordUnsup
	}

	if dev.Source == "idp" {
		pushToken, _ := media.PushToken(node.ID, app, stream)
		pushURL := fmt.Sprintf("rtmp://%s:%d/%s/%s", media.DevicePushHost(node), node.RTMPPort, app, stream)
		err = adapter.Get("idp").StartPlayback(context.Background(), adapter.PlaybackStart{
			ChannelID: channelID, SessionID: sessionID, Start: start, End: end,
			Speed: speed, PushURL: pushURL, PushToken: pushToken,
		})
		if err != nil {
			return nil, err
		}
	} else {
		zlm := media.ForNode(node)
		port, err := zlm.OpenRtpServer(context.Background(), 0, 1, stream)
		if err != nil {
			return nil, errs.ENodeOffline.WithMsg("openRtpServer 失败")
		}
		err = adapter.Get("gb28181").StartPlayback(context.Background(), adapter.PlaybackStart{
			ChannelID: channelID, SessionID: sessionID, Start: start, End: end,
			Speed: speed, Node: node, RtpPort: port,
		})
		if err != nil {
			_ = zlm.CloseRtpServer(stream)
			return nil, err
		}
	}

	if !e.waitStreaming(channelID, 12*time.Second) {
		go adapter.Get(dev.Source).StopPlayback(context.Background(), channelID, sessionID)
		return nil, errs.EStreamTimeout
	}
	devsvc.StreamSessionOpen(channelID, "main", node.ID, app, stream, "playback")
	token, _ := media.PlayToken(userID, channelID, app, stream, e.Cfg.PlayTokenTTLMin)
	e.savePb(&pbSession{ChannelID: channelID, SessionID: sessionID, Source: "device",
		Base: start, NodeID: node.ID, App: app, Stream: stream, Start: start, End: end})
	return map[string]any{
		"sessionId": sessionID, "source": "device",
		"node":     map[string]any{"id": node.ID, "publicHost": node.PublicHost, "httpPort": node.HTTPPort, "httpsPort": node.HTTPSPort},
		"app":      app, "stream": stream,
		"wsFlv":    e.flvURL(node, app, stream, token, "ws"),
		"wssFlv":   e.flvURL(node, app, stream, token, "wss"),
		"token":    token,
	}, nil
}

// PlaybackCtrl 回放控制。
func (e *Engine) PlaybackCtrl(sessionID, op string, speed float64, seekTs int64) error {
	s, err := e.loadPb(sessionID)
	if err != nil {
		return err
	}
	if s.Source == "platform" {
		return nil // 平台录像由播放器本地控制
	}
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", channelDeviceID(s.ChannelID)).Error; err != nil {
		return errs.ENotFound
	}
	if dev.Source == "idp" && !dev.HasCapability("record.device.seek") && op == "seek" {
		return errs.EForbid.WithMsg("该设备不支持回放定位")
	}
	if dev.Source == "idp" && !dev.HasCapability("record.device.speed") && op == "speed" {
		return errs.EForbid.WithMsg("该设备不支持回放倍速")
	}
	return adapter.Get(dev.Source).PlaybackCtrl(context.Background(), adapter.PlaybackCtrl{
		ChannelID: s.ChannelID, SessionID: s.SessionID, Op: op,
		Speed: speed, SeekTs: seekTs, BaseTs: s.Base,
	})
}

func channelDeviceID(channelID string) string {
	var ch models.Channel
	if store.DB.First(&ch, "id = ?", channelID).Error == nil {
		return ch.DeviceID
	}
	return ""
}

// StopPlayback 结束回放。
func (e *Engine) StopPlayback(sessionID string) error {
	s, err := e.loadPb(sessionID)
	if err != nil {
		return nil
	}
	defer e.delPb(sessionID)
	if s.Source != "platform" {
		adapter.Get(channelSource(s.ChannelID)).StopPlayback(context.Background(), s.ChannelID, s.SessionID)
		devsvc.StreamSessionClose(s.App, s.Stream)
	}
	return nil
}

func channelSource(channelID string) string {
	var ch models.Channel
	if store.DB.First(&ch, "id = ?", channelID).Error == nil {
		var dev models.Device
		if store.DB.First(&dev, "id = ?", ch.DeviceID).Error == nil {
			return dev.Source
		}
	}
	return "idp"
}

var _ = log.Printf
