package engine

import (
	"encoding/json"
	"fmt"
	"log"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/devsvc"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/media"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// hookParams 兼容 form 与 JSON。
func hookParams(c *gin.Context) map[string]string {
	out := map[string]string{}
	if ct := c.ContentType(); strings.HasPrefix(ct, "application/json") {
		var m map[string]any
		if json.NewDecoder(c.Request.Body).Decode(&m) == nil {
			for k, v := range m {
				out[k] = fmt.Sprint(v)
			}
		}
		return out
	}
	_ = c.Request.ParseForm()
	for k, v := range c.Request.PostForm {
		if len(v) > 0 {
			out[k] = v[0]
		}
	}
	for k, v := range c.Request.URL.Query() {
		if _, ok := out[k]; !ok && len(v) > 0 {
			out[k] = v[0]
		}
	}
	return out
}

func hookOK(c *gin.Context, extra map[string]any) {
	body := map[string]any{"code": 0, "msg": "success"}
	for k, v := range extra {
		body[k] = v
	}
	c.JSON(200, body)
}

func hookFail(c *gin.Context, msg string) {
	c.JSON(200, gin.H{"code": -1, "msg": msg})
}

// RegisterHooks 挂载 ZLM Hook（§8.5）。
func (e *Engine) RegisterHooks(g *gin.RouterGroup) {
	g.POST("/on_publish", e.onPublish)
	g.POST("/on_play", e.onPlay)
	g.POST("/on_stream_changed", e.onStreamChanged)
	g.POST("/on_stream_none_reader", e.onStreamNoneReader)
	g.POST("/on_stream_not_found", e.onStreamNotFound)
	g.POST("/on_flow_report", e.onFlowReport)
	g.POST("/on_record_mp4", e.onRecordMP4)
	g.POST("/on_server_keepalive", e.onServerKeepalive)
	g.POST("/on_server_started", e.onServerKeepalive)
	g.POST("/on_rtp_server_timeout", e.onRtpServerTimeout)
}

// onPublish 校验一次性推流 token（§8.4）。
func (e *Engine) onPublish(c *gin.Context) {
	p := hookParams(c)
	app, stream := p["app"], p["stream"]
	// GB28181 RTP 收流端口由平台 openRtpServer 按需开启，设备侧 RTP 无法携带
	// 推流 token；仅校验流键归属已存在通道，防止任意伪造。
	if app == "rtp" {
		if _, _, ok := devsvc.FindChannelByStreamKey(app, stream); !ok {
			log.Printf("[hook] publish denied %s/%s: unknown rtp stream", app, stream)
			hookFail(c, "unknown rtp stream")
			return
		}
		hookOK(c, nil)
		return
	}
	token := p["token"]
	if token == "" {
		token = paramFromParams(p["params"], "token")
	}
	nodeID := p["mediaServerId"]
	if err := media.VerifyPush(token, nodeID, app, stream); err != nil {
		log.Printf("[hook] publish denied %s/%s: %v", app, stream, err)
		hookFail(c, err.Error())
		return
	}
	hookOK(c, nil)
}

// onPlay 校验播放 token 与通道权限。
func (e *Engine) onPlay(c *gin.Context) {
	p := hookParams(c)
	app, stream := p["app"], p["stream"]
	token := paramFromParams(p["params"], "token")
	if token == "" {
		token = p["token"]
	}
	ch, _, found := devsvc.FindChannelByStreamKey(app, stream)
	if !found {
		hookFail(c, "unknown stream")
		return
	}
	if err := media.VerifyPlay(token, ch.ID, app, stream); err != nil {
		hookFail(c, err.Error())
		return
	}
	hookOK(c, nil)
}

func paramFromParams(raw, key string) string {
	if raw == "" {
		return ""
	}
	if v, err := url.ParseQuery(strings.TrimPrefix(raw, "?")); err == nil {
		return v.Get(key)
	}
	return ""
}

// onStreamChanged 流注册/注销：驱动通道状态、粘性映射与节点计数。
func (e *Engine) onStreamChanged(c *gin.Context) {
	p := hookParams(c)
	app, stream := p["app"], p["stream"]
	regist := p["regist"] == "true" || p["regist"] == "1"
	nodeID := p["mediaServerId"]

	ch, profile, found := devsvc.FindChannelByStreamKey(app, stream)
	if found {
		if regist {
			devsvc.SetChannelStream(ch.ID, "streaming")
			if nodeID != "" {
				e.Scheduler.SetSticky(ch.ID, profile, nodeID)
				media.NodeStreamsAdd(nodeID, 1)
			}
		} else {
			devsvc.SetChannelStream(ch.ID, "idle")
			if nodeID != "" {
				media.NodeStreamsAdd(nodeID, -1)
			}
			e.cleanupStream(app, stream, ch.ID)
		}
	}
	hookOK(c, nil)
}

// cleanupStream 注销时清理会话与国标 BYE。
func (e *Engine) cleanupStream(app, stream, channelID string) {
	devsvc.StreamSessionClose(app, stream)
	if app == "rtp" {
		if ch, _, ok := devsvc.FindChannelByStreamKey(app, stream); ok {
			adapterGet("gb28181").StopStream(mapStopOptions(ch.ID, stream))
		}
	}
}

// onStreamNoneReader 无人观看：按 app 分派停流（§8.5）。
func (e *Engine) onStreamNoneReader(c *gin.Context) {
	p := hookParams(c)
	app, stream := p["app"], p["stream"]
	ch, profile, found := devsvc.FindChannelByStreamKey(app, stream)
	if !found {
		hookOK(c, nil)
		return
	}
	switch app {
	case "live": // IDP → media.stop（设备停止推流后 ZLM 自动注销）
		adapterGet("idp").StopStream(mapStopOptions(ch.ID, stream))
	case "rtp": // GB → BYE + closeRtpServer
		e.StopPlay(ch.ID, profile, "none_reader")
	case "proxy": // → delStreamProxy
		if node, errN := devsvc.NodeByID(p["mediaServerId"]); errN == nil {
			_ = media.ForNode(node).DelStreamProxy("__defaultVhost__/proxy/" + stream)
		}
		devsvc.StreamSessionClose(app, stream)
	}
	hookOK(c, nil)
}

// onStreamNotFound 按需拉流：起播后让播放器等待（close=false）。
func (e *Engine) onStreamNotFound(c *gin.Context) {
	p := hookParams(c)
	app, stream := p["app"], p["stream"]
	ch, profile, found := devsvc.FindChannelByStreamKey(app, stream)
	if !found || !ch.Enabled {
		hookFail(c, "stream not found")
		return
	}
	go func() {
		if _, err := e.StartPlay("system", ch.ID, profile); err != nil {
			log.Printf("[hook] on-demand start %s: %v", stream, err)
		}
	}()
	hookOK(c, map[string]any{"close": false})
}

// onFlowReport 流量入库。
func (e *Engine) onFlowReport(c *gin.Context) {
	p := hookParams(c)
	app, stream := p["app"], p["stream"]
	var total uint64
	fmt.Sscanf(p["totalBytes"], "%d", &total)
	store.DB.Model(&models.StreamSession{}).
		Where("app = ? AND stream = ? AND ended_at = 0", app, stream).
		Update("bytes", int64(total))
	hookOK(c, nil)
}

// onRecordMP4 平台录像索引。
// 注意 ZLM 的 start_time/time_len/file_size 是浮点（JSON 序列化可能为科学计数法），须用 ParseFloat。
// Path 存 hook 的 url 相对路径（record/<app>/<stream>/…mp4），回放按 ZLM HTTP 静态服务拼接。
func (e *Engine) onRecordMP4(c *gin.Context) {
	p := hookParams(c)
	app, stream := p["app"], p["stream"]
	ch, _, found := devsvc.FindChannelByStreamKey(app, stream)
	if !found {
		hookOK(c, nil)
		return
	}
	startTs, _ := strconv.ParseFloat(p["start_time"], 64)
	durS, _ := strconv.ParseFloat(p["time_len"], 64)
	size, _ := strconv.ParseFloat(p["file_size"], 64)
	path := p["url"]
	if path == "" {
		path = p["file_path"]
	}
	rec := models.RecordIndex{
		ID: "ri_" + models.NewID(), ChannelID: ch.ID, Source: "platform",
		StartTs: int64(startTs * 1000), EndTs: int64((startTs + durS) * 1000),
		Type: "timer", Path: path, Size: int64(size),
	}
	store.DB.Create(&rec)
	hookOK(c, nil)
}

// onServerKeepalive 节点心跳。
func (e *Engine) onServerKeepalive(c *gin.Context) {
	p := hookParams(c)
	nodeID := p["mediaServerId"]
	if nodeID != "" {
		store.DB.Model(&models.MediaNode{}).Where("id = ?", nodeID).
			Updates(map[string]any{"status": "online", "last_keepalive": models.NowMilli()})
	}
	hookOK(c, nil)
}

// onRtpServerTimeout GB 收流超时。
func (e *Engine) onRtpServerTimeout(c *gin.Context) {
	p := hookParams(c)
	streamID := p["stream_id"]
	if ch, profile, ok := devsvc.FindChannelByStreamKey("rtp", streamID); ok {
		devsvc.SetChannelStream(ch.ID, "error")
		_ = profile
		e.StopPlay(ch.ID, profile, "rtp_timeout")
	}
	hookOK(c, nil)
}

var _ = errs.ENotFound
var _ = time.Now
