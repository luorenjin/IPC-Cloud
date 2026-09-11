package api

import (
	"encoding/json"

	"github.com/gin-gonic/gin"
	"gorm.io/gorm"

	"github.com/jetscam/ipccloud/server/internal/engine"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// ---------- 布防模板 ALM-01 ----------

func handleListAlarmTemplates(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.AlarmTemplate
	store.DB.Where("project_id = ?", ctx.ProjectID).Find(&items)
	ok(c, gin.H{"items": items})
}

func handleCreateAlarmTemplate(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Name     string       `json:"name" binding:"required"`
		Schedule models.JSONB `json:"schedule" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	t := models.AlarmTemplate{ID: "at_" + models.NewID(), ProjectID: ctx.ProjectID,
		Name: req.Name, Schedule: req.Schedule, CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
	store.DB.Create(&t)
	ok(c, t)
}

// handleUpdateAlarmTemplate ALM-01：修改布防模板（内置模板不可改；级联提示由前端负责）。
func handleUpdateAlarmTemplate(c *gin.Context) {
	var t models.AlarmTemplate
	if store.DB.First(&t, "id = ?", c.Param("id")).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	if t.Builtin {
		fail(c, errs.EForbid.WithMsg("内置模板不可修改"))
		return
	}
	var req struct {
		Name     string       `json:"name"`
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
	if req.Schedule != nil {
		updates["schedule"] = req.Schedule
	}
	store.DB.Model(&t).Updates(updates)
	store.DB.First(&t, "id = ?", c.Param("id"))
	ok(c, t)
}

func handleDeleteAlarmTemplate(c *gin.Context) {
	var t models.AlarmTemplate
	if store.DB.First(&t, "id = ?", c.Param("id")).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	if t.Builtin {
		fail(c, errs.EForbid.WithMsg("内置模板不可删除"))
		return
	}
	var used int64
	store.DB.Model(&models.AlarmRule{}).Where("template_id = ?", t.ID).Count(&used)
	if used > 0 {
		fail(c, errs.EBadRequest.WithMsg("模板已被告警规则使用"))
		return
	}
	store.DB.Delete(&t)
	ok(c, nil)
}

// ---------- 告警规则 ALM-02/03 ----------

func handleListAlarmRules(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.AlarmRule
	store.DB.Where("project_id = ?", ctx.ProjectID).Find(&items)
	ok(c, gin.H{"items": items})
}

func handleCreateAlarmRule(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		ChannelIDs []string `json:"channelIds" binding:"required"`
		Kinds      []string `json:"kinds" binding:"required"`
		TemplateID string   `json:"templateId"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	created := []string{}
	for _, cid := range req.ChannelIDs {
		r := models.AlarmRule{ID: "ar_" + models.NewID(), ProjectID: ctx.ProjectID,
			ChannelID: cid, Kinds: models.StringSlice(req.Kinds), TemplateID: req.TemplateID,
			Enabled: true, CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
		if err := store.DB.Create(&r).Error; err == nil {
			created = append(created, r.ID)
		}
	}
	ok(c, gin.H{"ids": created})
}

func handleUpdateAlarmRule(c *gin.Context) {
	id := c.Param("id")
	var req struct {
		Kinds      []string `json:"kinds"`
		TemplateID string   `json:"templateId"`
		Enabled    *bool    `json:"enabled"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.Kinds != nil {
		updates["kinds"] = models.StringSlice(req.Kinds)
	}
	if req.TemplateID != "" {
		updates["template_id"] = req.TemplateID
	}
	if req.Enabled != nil {
		updates["enabled"] = *req.Enabled
	}
	store.DB.Model(&models.AlarmRule{}).Where("id = ?", id).Updates(updates)
	ok(c, nil)
}

func handleDeleteAlarmRule(c *gin.Context) {
	store.DB.Delete(&models.AlarmRule{}, "id = ?", c.Param("id"))
	ok(c, nil)
}

// ---------- 告警策略 ALM-04/05 ----------

func handleGetAlarmPolicies(c *gin.Context) {
	ctx := getCtx(c)
	var st models.Setting
	if err := store.DB.First(&st, "scope = ? AND key = 'alarm.policies'", ctx.ProjectID).Error; err != nil {
		// 默认全开
		def := models.JSONB{}
		for _, k := range []string{"device_offline", "stream_lost", "record_fail", "disk_full",
			"auth_fail", "node_offline", "motion", "humanoid", "intrusion", "linecross", "tamper", "io"} {
			def[k] = map[string]any{"enabled": true, "web": true, "ring": false}
		}
		ok(c, def)
		return
	}
	ok(c, st.Value)
}

func handleSetAlarmPolicies(c *gin.Context) {
	ctx := getCtx(c)
	var req models.JSONB
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	var st models.Setting
	store.DB.Where("scope = ? AND key = 'alarm.policies'", ctx.ProjectID).Assign(models.Setting{
		Scope: ctx.ProjectID, Key: "alarm.policies", Value: req,
	}).FirstOrCreate(&st)
	store.DB.Model(&st).Update("value", req)
	ok(c, req)
}

// ---------- 消息中心 ALM-06/07 ----------

func handleListAlarms(c *gin.Context) {
	ctx := getCtx(c)
	page, size := pageParams(c)
	// 同一套筛选同时作用于「总数 / 列表 / focus 定位页码」，避免三处各写一遍导致口径漂移。
	applyFilters := func(q *gorm.DB) *gorm.DB {
		q = q.Where("project_id = ?", ctx.ProjectID)
		if k := c.Query("kind"); k != "" {
			q = q.Where("kind = ?", k)
		}
		// scope=device|platform：设备侧 = ALM-03 智能事件全集，其余一律归平台侧。
		// 用 kind NOT IN 而不是平台侧白名单——平台侧类型会随功能增加，白名单必漏。
		// 前端按 tab 过滤时必须走这里，否则 total/分页与实际列表不一致（翻页出现空页）。
		switch c.Query("scope") {
		case "device":
			q = q.Where("kind IN ?", models.DeviceSideAlarmKinds)
		case "platform":
			q = q.Where("kind NOT IN ?", models.DeviceSideAlarmKinds)
		}
		if lvl := c.Query("level"); lvl != "" {
			q = q.Where("level = ?", lvl)
		}
		if s := c.Query("read"); s != "" {
			q = q.Where("read = ?", s == "true")
		}
		return q
	}

	var total int64
	applyFilters(store.DB.Model(&models.AlarmEvent{})).Count(&total)
	var items []models.AlarmEvent
	applyFilters(store.DB.Model(&models.AlarmEvent{})).
		Order("ts DESC").Offset((page - 1) * size).Limit(size).Find(&items)
	var unread int64
	store.DB.Model(&models.AlarmEvent{}).Where("project_id = ? AND read = ?", ctx.ProjectID, false).Count(&unread)

	// focus=<id>：深链定位（总览页「最近告警」点击）。返回该条在同一筛选条件、同一排序
	// 下的页码，前端据此跳到对应页并高亮，避免为定位一条记录把全量列表拉回来。
	focusPage := 0
	if fid := c.Query("focus"); fid != "" {
		var ev models.AlarmEvent
		if store.DB.First(&ev, "id = ? AND project_id = ?", fid, ctx.ProjectID).Error == nil {
			var newer int64
			applyFilters(store.DB.Model(&models.AlarmEvent{})).
				Where("(ts > ? OR (ts = ? AND id > ?))", ev.Ts, ev.Ts, ev.ID).Count(&newer)
			focusPage = int(newer)/size + 1
		}
	}
	ok(c, gin.H{"total": total, "unread": unread, "items": items, "focusPage": focusPage})
}

func handleReadAlarm(c *gin.Context) {
	store.DB.Model(&models.AlarmEvent{}).Where("id = ?", c.Param("id")).Update("read", true)
	ok(c, nil)
}

func handleReadAllAlarms(c *gin.Context) {
	ctx := getCtx(c)
	store.DB.Model(&models.AlarmEvent{}).Where("project_id = ? AND read = ?", ctx.ProjectID, false).Update("read", true)
	ok(c, nil)
}

func handleAlarmDetail(c *gin.Context) {
	var ev models.AlarmEvent
	if err := store.DB.First(&ev, "id = ?", c.Param("id")).Error; err != nil {
		fail(c, errs.ENotFound)
		return
	}
	resp := map[string]any{}
	b, _ := json.Marshal(ev)
	_ = json.Unmarshal(b, &resp)
	var dev models.Device
	if store.DB.First(&dev, "id = ?", ev.DeviceID).Error == nil {
		resp["device"] = gin.H{"id": dev.ID, "name": dev.Name, "model": dev.Model, "source": dev.Source}
	}
	var rules []models.AlarmRule
	store.DB.Where("channel_id = ?", ev.ChannelID).Find(&rules)
	resp["rules"] = rules
	ok(c, resp)
}

var _ = engine.CreateAlarmEvent
