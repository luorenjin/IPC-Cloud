package api

import (
	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/engine"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

var appEngine *engine.Engine

// handlePlay LIVE-01/02：按需起播。
func handlePlay(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Profile string `json:"profile"`
	}
	_ = c.ShouldBindJSON(&req)
	if req.Profile == "" {
		req.Profile = "main"
	}
	resp, err := appEngine.StartPlay(ctx.UserID, c.Param("id"), req.Profile)
	if err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, resp)
}

// handleStopPlay 显式停流。
func handleStopPlay(c *gin.Context) {
	var req struct {
		Profile string `json:"profile"`
	}
	_ = c.ShouldBindJSON(&req)
	if req.Profile == "" {
		req.Profile = "main"
	}
	appEngine.StopPlay(c.Param("id"), req.Profile, "user_stop")
	ok(c, nil)
}

// handleSnapshot 抓图。
func handleSnapshot(c *gin.Context) {
	url, err := appEngine.Snapshot(c.Param("id"))
	if err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, gin.H{"url": url})
}

// handleRefreshCover 封面刷新（MGR-05）。
func handleRefreshCover(c *gin.Context) {
	url, err := appEngine.RefreshCover(c.Param("id"))
	if err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, gin.H{"url": url})
}

// handlePTZ LIVE-07。
func handlePTZ(c *gin.Context) {
	var req struct {
		Op     string  `json:"op" binding:"required"` // move|stop|preset_set|preset_goto|preset_del
		Pan    float64 `json:"pan"`
		Tilt   float64 `json:"tilt"`
		Zoom   float64 `json:"zoom"`
		Speed  float64 `json:"speed"`
		Preset int     `json:"preset"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if req.Speed == 0 {
		req.Speed = 50
	}
	if err := appPTZ(c.Param("id"), req.Op, req.Pan, req.Tilt, req.Zoom, req.Speed, req.Preset); err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, nil)
}

// ---------- 录像检索与回放 REC-01~04 ----------

func handleChannelRecords(c *gin.Context) {
	var q struct {
		Start  int64  `form:"start" binding:"required"`
		End    int64  `form:"end" binding:"required"`
		Types  string `form:"types"`
		Source string `form:"source"`
	}
	if err := c.ShouldBindQuery(&q); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	var types []string
	for _, t := range splitComma(q.Types) {
		types = append(types, t)
	}
	resp, err := appEngine.QueryRecords(c.Param("id"), q.Start, q.End, types, q.Source)
	if err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, resp)
}

func handleStartPlayback(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Start  int64   `json:"start" binding:"required"`
		End    int64   `json:"end"`
		Speed  float64 `json:"speed"`
		Source string  `json:"source"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if req.Speed == 0 {
		req.Speed = 1
	}
	resp, err := appEngine.StartPlayback(ctx.UserID, c.Param("id"), req.Start, req.End, req.Speed, req.Source)
	if err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, resp)
}

func handlePlaybackCtrl(c *gin.Context) {
	var req struct {
		Op     string   `json:"op" binding:"required"`
		Speed  *float64 `json:"speed"`
		SeekTs int64    `json:"seekTs"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	speed := 1.0
	if req.Speed != nil {
		speed = *req.Speed
	}
	if err := appEngine.PlaybackCtrl(c.Param("sid"), req.Op, speed, req.SeekTs); err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, nil)
}

func handleStopPlayback(c *gin.Context) {
	_ = appEngine.StopPlayback(c.Param("sid"))
	ok(c, nil)
}

// handleListChannels 通道列表（预览树用）。
func handleListChannels(c *gin.Context) {
	ctx := getCtx(c)
	var chs []models.Channel
	store.DB.Where("project_id = ? AND enabled = ?", ctx.ProjectID, true).
		Order("device_id ASC, idx ASC").Find(&chs)
	ok(c, gin.H{"items": chs})
}

func splitComma(s string) []string {
	out := []string{}
	cur := ""
	for _, r := range s {
		if r == ',' {
			if cur != "" {
				out = append(out, cur)
			}
			cur = ""
			continue
		}
		cur += string(r)
	}
	if cur != "" {
		out = append(out, cur)
	}
	return out
}

// SetEngine 注入 engine。
func SetEngine(e *engine.Engine) { appEngine = e }

// appPTZ 分派 PTZ（IDP 支持；其他来源能力集不含 ptz）。
func appPTZ(channelID, op string, pan, tilt, zoom, speed float64, preset int) error {
	var ch models.Channel
	if err := store.DB.First(&ch, "id = ?", channelID).Error; err != nil {
		return errs.ENotFound
	}
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", ch.DeviceID).Error; err != nil {
		return errs.ENotFound
	}
	if dev.Source != "idp" {
		return errs.EForbid.WithMsg("该通道不支持云台控制")
	}
	if a, ok := adapter.Get("idp").(interface {
		PTZ(channelID, op string, pan, tilt, zoom, speed float64, preset int) error
	}); ok {
		return a.PTZ(channelID, op, pan, tilt, zoom, speed, preset)
	}
	return errs.EForbid.WithMsg("该通道不支持云台控制")
}
