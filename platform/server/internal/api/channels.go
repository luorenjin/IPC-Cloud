package api

import (
	"encoding/json"
	"strconv"

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

// handlePTZPresetsList LIVE-07：预置位列表（平台侧注册表：编号+名称；
// 设备侧 preset 由 cmd.ptz preset_set/goto/del 驱动）。
func handlePTZPresetsList(c *gin.Context) {
	chID := c.Param("id")
	var ch models.Channel
	if store.DB.First(&ch, "id = ?", chID).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	items := []gin.H{}
	for id, raw := range loadPresets(chID) {
		if r, okk := raw.(map[string]any); okk {
			items = append(items, gin.H{"id": id,
				"index": r["index"], "name": r["name"]})
		}
	}
	ok(c, gin.H{"items": items})
}

// handlePTZPresetAdd 新增预置位（cmd.ptz preset_set）。
func handlePTZPresetAdd(c *gin.Context) {
	chID := c.Param("id")
	var req struct {
		Name   string `json:"name"`
		Preset int    `json:"preset"`
	}
	_ = c.ShouldBindJSON(&req)
	if req.Preset <= 0 || req.Preset > 255 {
		// 自动分配下一个可用编号
		req.Preset = nextPresetIndex(chID)
	}
	if err := appPTZ(chID, "preset_set", 0, 0, 0, 50, req.Preset); err != nil {
		fail(c, toAppErr(err))
		return
	}
	savePresetName(c, chID, req.Preset, req.Name)
	ok(c, gin.H{"preset": req.Preset, "name": req.Name})
}

// handlePTZPresetGoto 调用预置位。
func handlePTZPresetGoto(c *gin.Context) {
	var req struct {
		ID     string `json:"id"`
		Preset int    `json:"preset"`
	}
	_ = c.ShouldBindJSON(&req)
	idx := req.Preset
	if idx <= 0 {
		idx = presetIndexOf(c.Param("id"), req.ID)
	}
	if idx <= 0 {
		fail(c, errs.EBadRequest.WithMsg("预置位不存在"))
		return
	}
	if err := appPTZ(c.Param("id"), "preset_goto", 0, 0, 0, 50, idx); err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, nil)
}

// handlePTZPresetDelete 删除预置位。
func handlePTZPresetDelete(c *gin.Context) {
	chID := c.Param("id")
	pid := c.Param("pid")
	idx := presetIndexOf(chID, pid)
	if idx > 0 {
		_ = appPTZ(chID, "preset_del", 0, 0, 0, 50, idx)
	}
	removePresetName(c, chID, pid, idx)
	ok(c, nil)
}

// ---------- 预置位名称存储（KV：preset:<channelId> → {id:{index,name}}） ----------

func presetKey(chID string) string { return "preset:" + chID }

func loadPresets(chID string) map[string]any {
	v, err := store.KVGet(presetKey(chID))
	if err != nil || v == "" {
		return map[string]any{}
	}
	m := map[string]any{}
	_ = json.Unmarshal([]byte(v), &m)
	return m
}

func savePresets(chID string, m map[string]any) {
	b, _ := json.Marshal(m)
	_ = store.KVSet(presetKey(chID), string(b), 0)
}

func savePresetName(c *gin.Context, chID string, idx int, name string) {
	if name == "" {
		return
	}
	m := loadPresets(chID)
	id := "ps_" + models.NewID()
	m[id] = map[string]any{"index": idx, "name": name}
	savePresets(chID, m)
	c.Set("presetId", id)
}

func presetIndexOf(chID, pid string) int {
	m := loadPresets(chID)
	if raw, okk := m[pid].(map[string]any); okk {
		if f, okf := raw["index"].(float64); okf {
			return int(f)
		}
	}
	// 前端可能直接传编号（p.id ?? p.index）
	if n, err := strconv.Atoi(pid); err == nil {
		return n
	}
	return 0
}

func removePresetName(c *gin.Context, chID, pid string, idx int) {
	m := loadPresets(chID)
	if _, has := m[pid]; has {
		delete(m, pid)
	} else if idx > 0 {
		for k, raw := range m {
			if r, okk := raw.(map[string]any); okk {
				if f, okf := r["index"].(float64); okf && int(f) == idx {
					delete(m, k)
				}
			}
		}
	}
	savePresets(chID, m)
}

func nextPresetIndex(chID string) int {
	m := loadPresets(chID)
	used := map[int]bool{}
	for _, raw := range m {
		if r, okk := raw.(map[string]any); okk {
			if f, okf := r["index"].(float64); okf {
				used[int(f)] = true
			}
		}
	}
	for i := 1; i <= 255; i++ {
		if !used[i] {
			return i
		}
	}
	return 1
}

// ---------- 通道列表 ----------

// handleListChannels 通道列表（预览树用）。
func handleListChannels(c *gin.Context) {
	ctx := getCtx(c)
	q := store.DB.Where("project_id = ? AND enabled = ?", ctx.ProjectID, true)
	// ids 精确筛选：供首页告警流按需取通道名，避免为几个名字全量拉取
	if ids := c.Query("ids"); ids != "" {
		q = q.Where("id IN ?", splitComma(ids))
	}
	if dev := c.Query("deviceId"); dev != "" {
		q = q.Where("device_id = ?", dev)
	}
	var chs []models.Channel
	q.Order("device_id ASC, idx ASC").Find(&chs)
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
