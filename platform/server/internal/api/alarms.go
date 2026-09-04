package api

import (
	"encoding/json"

	"github.com/gin-gonic/gin"

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
		ChannelIDs []string    `json:"channelIds" binding:"required"`
		Kinds      []string    `json:"kinds" binding:"required"`
		TemplateID string      `json:"templateId"`
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
	q := store.DB.Model(&models.AlarmEvent{}).Where("project_id = ?", ctx.ProjectID)
	if k := c.Query("kind"); k != "" {
		q = q.Where("kind = ?", k)
	}
	if lvl := c.Query("level"); lvl != "" {
		q = q.Where("level = ?", lvl)
	}
	if s := c.Query("read"); s != "" {
		q = q.Where("read = ?", s == "true")
	}
	var total int64
	q.Count(&total)
	var items []models.AlarmEvent
	q.Order("ts DESC").Offset((page - 1) * size).Limit(size).Find(&items)
	var unread int64
	store.DB.Model(&models.AlarmEvent{}).Where("project_id = ? AND read = ?", ctx.ProjectID, false).Count(&unread)
	ok(c, gin.H{"total": total, "unread": unread, "items": items})
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
