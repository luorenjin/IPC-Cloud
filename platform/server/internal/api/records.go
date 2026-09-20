package api

import (
	"fmt"
	"math"
	"strings"
	"time"

	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
	"github.com/jetscam/ipccloud/server/internal/timeutil"
)

// ---------- 录像计划模板 REC-05 ----------

func handleListRecordTemplates(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.RecordTemplate
	store.DB.Where("project_id = ?", ctx.ProjectID).Find(&items)
	ok(c, gin.H{"items": items})
}

func handleCreateRecordTemplate(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Name     string       `json:"name" binding:"required"`
		Kind     string       `json:"kind"`
		Schedule models.JSONB `json:"schedule" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if req.Kind == "" {
		req.Kind = "timer"
	}
	t := models.RecordTemplate{ID: "rt_" + models.NewID(), ProjectID: ctx.ProjectID,
		Name: req.Name, Kind: req.Kind, Schedule: req.Schedule,
		CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
	store.DB.Create(&t)
	ok(c, t)
}

// handleUpdateRecordTemplate REC-05：修改录像计划模板（内置不可改；自定义 ≤7 个）。
func handleUpdateRecordTemplate(c *gin.Context) {
	// 归属校验：模板必须属于当前项目（requirePerm 只看当前项目的权限位，不校验实体归属）
	t, okt := recordTemplateInProject(c, c.Param("id"))
	if !okt {
		return
	}
	if t.Builtin {
		fail(c, errs.EForbid.WithMsg("内置模板不可修改"))
		return
	}
	var req struct {
		Name     string       `json:"name"`
		Kind     string       `json:"kind"`
		Schedule models.JSONB `json:"schedule"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.Name != "" {
		updates["name"] = req.Name
	}
	if req.Kind == "timer" || req.Kind == "event" {
		updates["kind"] = req.Kind
	}
	if req.Schedule != nil {
		updates["schedule"] = req.Schedule
	}
	store.DB.Model(t).Updates(updates)
	store.DB.First(t, "id = ?", t.ID)
	ok(c, t)
}

func handleDeleteRecordTemplate(c *gin.Context) {
	t, okt := recordTemplateInProject(c, c.Param("id"))
	if !okt {
		return
	}
	if t.Builtin {
		fail(c, errs.EForbid.WithMsg("内置模板不可删除"))
		return
	}
	var used int64
	store.DB.Model(&models.RecordPlan{}).Where("template_id = ?", t.ID).Count(&used)
	if used > 0 {
		fail(c, errs.EBadRequest.WithMsg("模板已被录像计划使用"))
		return
	}
	store.DB.Delete(t)
	ok(c, nil)
}

// ---------- 录像设置 REC-06 ----------

func handleListRecordPlans(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.RecordPlan
	store.DB.Raw(`SELECT p.* FROM record_plans p JOIN channels c ON c.id = p.channel_id
		WHERE c.project_id = ?`, ctx.ProjectID).Scan(&items)
	ok(c, gin.H{"items": items})
}

func handleCreateRecordPlan(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		ChannelIDs []string `json:"channelIds" binding:"required"`
		TemplateID string   `json:"templateId" binding:"required"`
		Profile    string   `json:"profile"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if req.Profile == "" {
		req.Profile = "main"
	}
	created := 0
	for _, cid := range req.ChannelIDs {
		var ch models.Channel
		if store.DB.First(&ch, "id = ? AND project_id = ?", cid, ctx.ProjectID).Error != nil {
			continue
		}
		p := models.RecordPlan{ID: "rp_" + models.NewID(), ChannelID: cid,
			TemplateID: req.TemplateID, Profile: req.Profile, Enabled: true,
			CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
		if store.DB.Create(&p).Error == nil {
			created++
		}
	}
	ok(c, gin.H{"created": created})
}

func handleUpdateRecordPlan(c *gin.Context) {
	ctx := getCtx(c)
	id := c.Param("id")
	// 归属校验：录像计划经所属通道判定项目归属（见 scope.go）
	if _, okp := recordPlanInProject(c, id); !okp {
		return
	}
	var req struct {
		TemplateID string `json:"templateId"`
		Profile    string `json:"profile"`
		Enabled    *bool  `json:"enabled"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	// 换绑的模板也必须属于当前项目，否则计划会指向别处的模板
	if req.TemplateID != "" {
		if _, okt := recordTemplateInProject(c, req.TemplateID); !okt {
			return
		}
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.TemplateID != "" {
		updates["template_id"] = req.TemplateID
	}
	if req.Profile != "" {
		updates["profile"] = req.Profile
	}
	if req.Enabled != nil {
		updates["enabled"] = *req.Enabled
	}
	// 写条件经 channel_id 关联到本项目通道（双保险）
	store.DB.Model(&models.RecordPlan{}).
		Where("id = ? AND channel_id IN (SELECT id FROM channels WHERE project_id = ?)", id, ctx.ProjectID).
		Updates(updates)
	ok(c, nil)
}

func handleDeleteRecordPlan(c *gin.Context) {
	ctx := getCtx(c)
	id := c.Param("id")
	if _, okp := recordPlanInProject(c, id); !okp {
		return
	}
	store.DB.Where("id = ? AND channel_id IN (SELECT id FROM channels WHERE project_id = ?)", id, ctx.ProjectID).
		Delete(&models.RecordPlan{})
	ok(c, nil)
}

// handleRecordDays REC-02：日期选择器高亮——返回区间内有录像的日期列表（本地时区）。
func handleRecordDays(c *gin.Context) {
	var q struct {
		Start  int64  `form:"start" binding:"required"`
		End    int64  `form:"end" binding:"required"`
		Source string `form:"source"`
	}
	if err := c.ShouldBindQuery(&q); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	chID := c.Param("id")
	ctx := getCtx(c)
	// 归属校验：通道必须属于当前项目（否则能检索到别家项目的录像）
	if _, okc := channelInProject(c, chID); !okc {
		return
	}
	type row struct {
		StartTs int64
		EndTs   int64
	}
	var rows []row
	dq := store.DB.Model(&models.RecordIndex{}).Select("start_ts, end_ts").
		Where("channel_id = ? AND start_ts < ? AND end_ts > ?", chID, q.End, q.Start)
	if q.Source == "device" || q.Source == "platform" {
		dq = dq.Where("source = ?", q.Source)
	}
	dq.Find(&rows)
	// 按天展开（段可能跨天）。日期键与推进都按项目时区求值：服务端 time.Local 通常是 UTC，
	// 用它会把北京时间凌晨的录像记到前一天，回放页日期列表随之整体错一天。
	loc := store.ProjectLocation(ctx.ProjectID)
	seen := map[string]bool{}
	days := []string{}
	for _, r := range rows {
		cur := timeutil.DayStartMillis(time.UnixMilli(r.StartTs), loc)
		for cur < r.EndTs {
			key := timeutil.DayKey(time.UnixMilli(cur), loc)
			if !seen[key] {
				seen[key] = true
				days = append(days, key)
			}
			cur = timeutil.NextDayStart(time.UnixMilli(cur), loc).UnixMilli()
		}
	}
	ok(c, gin.H{"days": days})
}

// handleRecordDownload REC-08：时间轴框选下载——建任务（任务中心），返回覆盖区间的录像文件。
// MVP：平台录像按段返回原始 MP4 文件 URL（同源 /media 代理可下载）；合并为单文件为后续增强。
func handleRecordDownload(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Start  float64 `json:"start" binding:"required"`
		End    float64 `json:"end" binding:"required"`
		Source string  `json:"source"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if req.End <= req.Start {
		fail(c, errs.EBadRequest.WithMsg("结束时间需晚于开始时间"))
		return
	}
	chID := c.Param("id")
	if _, okc := channelInProject(c, chID); !okc {
		return
	}
	var recs []models.RecordIndex
	dq := store.DB.Where("channel_id = ? AND start_ts < ? AND end_ts > ?", chID, int64(req.End), int64(req.Start))
	if req.Source == "device" || req.Source == "platform" {
		dq = dq.Where("source = ?", req.Source)
	}
	dq.Order("start_ts ASC").Find(&recs)
	if len(recs) == 0 {
		fail(c, errs.ENotFound.WithMsg("所选时间范围内无平台录像"))
		return
	}
	// 节点公网地址（文件经 ZLM HTTP 静态服务）
	var node models.MediaNode
	store.DB.Where("status = ?", "online").Order("weight DESC").First(&node)
	files := make([]gin.H, 0, len(recs))
	var totalSize int64
	for _, r := range recs {
		rel := strings.TrimPrefix(r.Path, "/")
		url := "/media/" + rel // 前端同源代理路径
		if node.ID != "" {
			url = fmt.Sprintf("http://%s:%d/%s", node.PublicHost, node.HTTPPort, rel)
		}
		totalSize += r.Size
		files = append(files, gin.H{"start": r.StartTs, "end": r.EndTs, "size": r.Size, "url": url})
	}
	detail := fmt.Sprintf("%d 个文件，共 %s", len(files), humanSize(totalSize))
	t := models.Task{ID: "tk_" + models.NewID(), ProjectID: ctx.ProjectID, Type: "download",
		Title: "录像片段下载", Detail: detail, Status: "success", Progress: 100,
		Result: models.JSONB{"projectId": ctx.ProjectID, "channelId": chID, "files": files,
			"count": len(files), "size": totalSize, "note": "MVP：按段返回 MP4 文件，合并下载后续提供"},
		CreatedBy: ctx.UserID, CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
	store.DB.Create(&t)
	bus.Default.Publish(bus.Event{Type: "task.progress", ProjectID: ctx.ProjectID,
		Data: map[string]any{"taskId": t.ID, "type": t.Type, "status": t.Status, "progress": 100,
			"title": t.Title, "detail": t.Detail, "result": t.Result,
			"createdAt": t.CreatedAt, "updatedAt": t.UpdatedAt}})
	ok(c, gin.H{"taskId": t.ID, "files": files, "count": len(files), "size": totalSize})
}

// handleStorageOverview REC-07 存储概览。
func handleStorageOverview(c *gin.Context) {
	ctx := getCtx(c)
	// 表名用 GORM 的命名策略推导，不要硬编码：RecordIndex 实际建表为
	// record_indices 而非 record_indexes，写错会让 JOIN 静默失败、用量恒为 0。
	tbl := store.DB.NamingStrategy.TableName("RecordIndex")
	var used int64
	store.DB.Model(&models.RecordIndex{}).
		Joins("JOIN channels c ON c.id = "+tbl+".channel_id").
		Where("c.project_id = ?", ctx.ProjectID).
		Select("COALESCE(SUM(" + tbl + ".size),0)").Scan(&used)
	var count int64
	store.DB.Model(&models.RecordIndex{}).
		Joins("JOIN channels c ON c.id = "+tbl+".channel_id").
		Where("c.project_id = ?", ctx.ProjectID).Count(&count)
	// 总容量与保留天数从设置读取，缺省 500GB / 30 天
	var st models.Setting
	total := int64(500 * 1024 * 1024 * 1024)
	keepDays := 30
	if err := store.DB.First(&st, "scope = ? AND key = 'storage.totalBytes'", ctx.ProjectID).Error; err == nil {
		if v, ok := st.Value["value"].(float64); ok {
			total = int64(v)
		}
	}
	if err := store.DB.First(&st, "scope = ? AND key = 'storage.keepDays'", ctx.ProjectID).Error; err == nil {
		if v, ok := st.Value["value"].(float64); ok && v > 0 {
			keepDays = int(v)
		}
	}
	// 百分比四舍五入：原先的整数除法会把 7.66% 截断成 7%，与用户按容量反算的结果对不上。
	pct := 0
	if total > 0 {
		pct = int(math.Round(float64(used) * 100 / float64(total)))
	}
	ok(c, gin.H{"usedBytes": used, "totalBytes": total, "percent": pct, "segments": count,
		"keepDays": keepDays})
	if pct >= 90 {
		engine_CreateAlarmEvent(ctx.ProjectID, "", "", "disk_full", "warn",
			map[string]any{"percent": pct}, "")
	}
}
