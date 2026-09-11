package api

import (
	"github.com/gin-gonic/gin"
	"gorm.io/gorm"

	"github.com/jetscam/ipccloud/server/internal/devsvc"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// ---------- 项目 ACC-03 ----------

func handleListProjects(c *gin.Context) {
	ctx := getCtx(c)
	q := store.DB.Order("created_at ASC")
	// 只列出租户内的项目：别家租户的项目名/数量同属租户数据。
	// 上下文 TenantID 为空时（老会话等无租户上下文的场景）退回全量，
	// 否则会一个项目都列不出来，项目切换器直接空掉。
	if ctx != nil && ctx.TenantID != "" {
		q = q.Where("tenant_id = ?", ctx.TenantID)
	}
	var projs []models.Project
	q.Find(&projs)
	ok(c, gin.H{"items": projs})
}

func handleCreateProject(c *gin.Context) {
	var req struct {
		Name string `json:"name" binding:"required"`
		TZ   string `json:"tz"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if req.TZ == "" {
		req.TZ = "Asia/Shanghai"
	}
	ctx := getCtx(c)
	p := models.Project{ID: "p_" + models.NewID(), TenantID: ctx.TenantID, Name: req.Name, TZ: req.TZ,
		Settings: models.JSONB{"streamIdleSec": 30}, Enabled: true,
		CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
	if err := store.DB.Create(&p).Error; err != nil {
		fail(c, errs.EServerInternal.WithMsg(err.Error()))
		return
	}
	// 创建者成为项目管理员
	if ctx != nil {
		store.DB.Create(&models.UserRole{UserID: ctx.UserID, RoleID: "role_admin", ProjectID: p.ID})
	}
	// 内置录像/布防模板 + 默认策略（ADD-09）：新项目开箱即用。
	// 存量项目不写这两个设置，因而保持关闭，不会因升级而突然录像或改变告警行为。
	if tplID := store.SeedRecordTemplates(p.ID); tplID != "" {
		store.DB.Create(&models.Setting{Scope: p.ID, Key: devsvc.RecordDefaultsKey,
			Value: models.JSONB{"enabled": true, "templateId": tplID, "profile": "main"}})
	}
	if tplID := store.SeedAlarmTemplates(p.ID); tplID != "" {
		store.DB.Create(&models.Setting{Scope: p.ID, Key: devsvc.AlarmDefaultsKey,
			Value: models.JSONB{"enabled": true, "templateId": tplID,
				"kinds": models.DeviceSideAlarmKinds}})
	}
	ok(c, p)
}

func handleUpdateProject(c *gin.Context) {
	id := c.Param("id")
	ctx := getCtx(c)
	// 项目按租户判定归属（多项目成员本就需要访问多个项目）；
	// 而权限按**待修改的目标项目**校验，而非请求头里的当前项目：
	// 否则在 A 项目具备 config 权限即可改名/改时区/启停 B 项目（跨租户同理）。
	if _, okp := projectInTenant(c, id); !okp {
		return
	}
	if !roleHasAction(loadRole(ctx.UserID, id), "config") {
		fail(c, errs.EForbid.WithMsg("无该项目的配置权限"))
		return
	}
	var req struct {
		Name      *string `json:"name"`
		TZ        *string `json:"tz"`
		Enabled   *bool   `json:"enabled"`
		SetupDone *bool   `json:"setupDone"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.Name != nil {
		updates["name"] = *req.Name
	}
	if req.TZ != nil {
		updates["tz"] = *req.TZ
	}
	if req.Enabled != nil {
		updates["enabled"] = *req.Enabled
	}
	if req.SetupDone != nil {
		updates["setup_done"] = *req.SetupDone
	}
	if err := store.DB.Model(&models.Project{}).Where("id = ? AND tenant_id = ?", id, ctx.TenantID).Updates(updates).Error; err != nil {
		fail(c, errs.EServerInternal)
		return
	}
	var p models.Project
	store.DB.First(&p, "id = ?", id)
	ok(c, p)
}

// handleDeleteProject 删除项目（PRD §8 接口清单 DELETE /projects）。
//
// 只允许删除"空项目"——项目下仍有**存活**设备/通道时拒绝，与 handleDeleteGroup 的
// "含设备的分组须先转移"保持同一范式。两处守卫都以 devices.deleted_at = 0 为准：
// 设备删除是软删且不连带删通道，若按原始行数判断，项目一旦接入过设备便再也删不掉。
// 软删设备及其残留通道、录像计划与索引，均由下面的级联清除。
func handleDeleteProject(c *gin.Context) {
	id := c.Param("id")
	ctx := getCtx(c)
	if ctx == nil {
		fail(c, errs.EUnauthorized)
		return
	}
	// 先判租户归属再判权限：handleListProjects 返回全量项目时，项目是否存在并非机密，
	// 据此换取"项目不存在"与"无权限"两种准确提示。
	var p models.Project
	if err := store.DB.First(&p, "id = ? AND tenant_id = ?", id, ctx.TenantID).Error; err != nil {
		fail(c, errs.ENotFound.WithMsg("项目不存在"))
		return
	}
	// 权限按"待删除的目标项目"校验，而非请求头里的当前会话项目：
	// 否则在 A 项目具备 delete 权限即可删掉 B 项目。
	if !roleHasAction(loadRole(ctx.UserID, id), "delete") {
		fail(c, errs.EForbid.WithMsg("无该项目的删除权限"))
		return
	}
	var devCnt int64
	store.DB.Model(&models.Device{}).Where("project_id = ? AND deleted_at = 0", id).Count(&devCnt)
	if devCnt > 0 {
		fail(c, errs.EBadRequest.WithMsg("项目下存在 "+itoa(int(devCnt))+" 台设备，请先删除或转移后再删除项目"))
		return
	}
	// 通道守卫只统计"存活设备"的通道：设备删除是软删（handleDeleteDevice 仅置 deleted_at，
	// 不删通道），若把残留通道也计入，项目一旦接入过设备就永远删不掉——而产品并未提供
	// 单独清理通道的入口。这些残留由下面的级联一并清除。
	var chCnt int64
	store.DB.Model(&models.Channel{}).
		Joins("JOIN devices ON devices.id = channels.device_id").
		Where("channels.project_id = ? AND devices.deleted_at = 0", id).Count(&chCnt)
	if chCnt > 0 {
		fail(c, errs.EBadRequest.WithMsg("项目下存在 "+itoa(int(chCnt))+" 个通道，请先清理后再删除项目"))
		return
	}
	var total int64
	store.DB.Model(&models.Project{}).Count(&total)
	if total <= 1 {
		fail(c, errs.EBadRequest.WithMsg("至少需要保留一个项目"))
		return
	}

	// 级联清理项目级数据；audit_logs 不清除——审计留痕不随业务数据删除。
	err := store.DB.Transaction(func(tx *gorm.DB) error {
		// 录像计划与索引以 channel_id 关联，须在删通道之前清理（含软删设备残留的通道）
		var chIDs []string
		tx.Model(&models.Channel{}).Where("project_id = ?", id).Pluck("id", &chIDs)
		if len(chIDs) > 0 {
			for _, m := range []any{&models.RecordPlan{}, &models.RecordIndex{}} {
				if e := tx.Where("channel_id IN ?", chIDs).Delete(m).Error; e != nil {
					return e
				}
			}
		}
		for _, m := range []any{
			&models.Channel{}, &models.Device{},
			&models.DeviceGroup{}, &models.Role{}, &models.UserRole{},
			&models.RecordTemplate{}, &models.AlarmTemplate{}, &models.AlarmRule{},
			&models.AlarmEvent{}, &models.IdpPreadd{}, &models.GbWhitelist{}, &models.GbPending{},
		} {
			if e := tx.Where("project_id = ?", id).Delete(m).Error; e != nil {
				return e
			}
		}
		// 项目级设置（scope = 项目 ID），如 ADD-09 的 recordDefaults
		if e := tx.Where("scope = ?", id).Delete(&models.Setting{}).Error; e != nil {
			return e
		}
		return tx.Delete(&models.Project{}, "id = ?", id).Error
	})
	if err != nil {
		fail(c, errs.EServerInternal.WithMsg(err.Error()))
		return
	}
	// 操作日志由 AuditMiddleware 统一记录（action=delete、target=project:<id>），此处不重复写入。
	ok(c, nil)
}

// ---------- 分组 ACC-04 ----------

func handleListGroups(c *gin.Context) {
	ctx := getCtx(c)
	if ctx == nil {
		fail(c, errs.EUnauthorized)
		return
	}
	// projectId 是客户端给的：只允许当前项目，或租户内且调用者确实拥有角色的项目。
	// 不做校验就等于任何登录用户都能用一个 ID 读到别家项目的分组名与设备数。
	pid := c.Query("projectId")
	if pid == "" || pid == ctx.ProjectID {
		pid = ctx.ProjectID
	} else {
		if _, okp := projectInTenant(c, pid); !okp {
			return
		}
		if loadRole(ctx.UserID, pid) == nil {
			fail(c, errs.EForbid.WithMsg("无该项目的访问权限"))
			return
		}
	}
	var groups []models.DeviceGroup
	store.DB.Where("project_id = ?", pid).Order("sort ASC").Find(&groups)
	// 每个分组附带直属设备数：设备页分组树要显示计数，前端不应为此再拉一遍全量设备自己统计。
	type groupCount struct {
		GroupID string
		Cnt     int64
	}
	var cnts []groupCount
	store.DB.Model(&models.Device{}).
		Select("group_id, COUNT(*) AS cnt").
		Where("project_id = ? AND deleted_at = 0 AND group_id <> ''", pid).
		Group("group_id").Scan(&cnts)
	byGroup := make(map[string]int64, len(cnts))
	for _, c := range cnts {
		byGroup[c.GroupID] = c.Cnt
	}
	items := make([]gin.H, 0, len(groups))
	for _, g := range groups {
		items = append(items, gin.H{
			"id": g.ID, "projectId": g.ProjectID, "parentId": g.ParentID,
			"name": g.Name, "sort": g.Sort, "createdAt": g.CreatedAt,
			"deviceCount": byGroup[g.ID],
		})
	}
	ok(c, gin.H{"items": items})
}

type groupReq struct {
	Name     string `json:"name" binding:"required"`
	ParentID string `json:"parentId"`
}

func handleCreateGroup(c *gin.Context) {
	ctx := getCtx(c)
	var req groupReq
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	// 深度 ≤4
	depth := 1
	parent := req.ParentID
	for parent != "" {
		var g models.DeviceGroup
		// 父分组按项目归属查：否则可以指名别家项目的分组作父节点，把树接到别人家去
		if store.DB.First(&g, "id = ? AND project_id = ?", parent, ctx.ProjectID).Error != nil {
			break
		}
		depth++
		parent = g.ParentID
		if depth > 4 {
			fail(c, errs.EBadRequest.WithMsg("分组层级不能超过 4 级"))
			return
		}
	}
	var cnt int64
	store.DB.Model(&models.DeviceGroup{}).Where("project_id = ? AND parent_id = ? AND name = ?",
		ctx.ProjectID, req.ParentID, req.Name).Count(&cnt)
	if cnt > 0 {
		fail(c, errs.EBadRequest.WithMsg("同级分组名已存在"))
		return
	}
	g := models.DeviceGroup{ID: "grp_" + models.NewID(), ProjectID: ctx.ProjectID,
		Name: req.Name, ParentID: req.ParentID, CreatedAt: models.NowMilli()}
	store.DB.Create(&g)
	ok(c, g)
}

func handleUpdateGroup(c *gin.Context) {
	id := c.Param("id")
	ctx := getCtx(c)
	g0, okg := groupInProject(c, id)
	if !okg {
		return
	}
	var req struct {
		Name string `json:"name"`
		Sort *int   `json:"sort"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	updates := map[string]any{}
	if req.Name != "" {
		updates["name"] = req.Name
	}
	if req.Sort != nil {
		updates["sort"] = *req.Sort
	}
	store.DB.Model(&models.DeviceGroup{}).Where("id = ? AND project_id = ?", id, ctx.ProjectID).Updates(updates)
	var g models.DeviceGroup
	store.DB.First(&g, "id = ? AND project_id = ?", id, g0.ProjectID)
	ok(c, g)
}

// handleDeleteGroup 删除含设备的分组须先转移。
func handleDeleteGroup(c *gin.Context) {
	id := c.Param("id")
	ctx := getCtx(c)
	if _, okg := groupInProject(c, id); !okg {
		return
	}
	var cnt int64
	store.DB.Model(&models.Device{}).Where("group_id = ? AND deleted_at = 0", id).Count(&cnt)
	if cnt > 0 {
		fail(c, errs.EBadRequest.WithMsg("分组下存在设备，请先转移"))
		return
	}
	var children int64
	store.DB.Model(&models.DeviceGroup{}).Where("parent_id = ?", id).Count(&children)
	if children > 0 {
		fail(c, errs.EBadRequest.WithMsg("存在子分组"))
		return
	}
	store.DB.Delete(&models.DeviceGroup{}, "id = ? AND project_id = ?", id, ctx.ProjectID)
	ok(c, nil)
}

// handleMoveDevices 拖拽移动设备到分组。
func handleMoveDevices(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		DeviceIDs []string `json:"deviceIds" binding:"required"`
		GroupID   string   `json:"groupId"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	// 目标分组必须属于当前项目（空串 = 移出分组）
	if req.GroupID != "" {
		if _, okg := groupInProject(c, req.GroupID); !okg {
			return
		}
	}
	// 设备 ID 也限定在当前项目内：否则传入别家项目的设备 ID 就能把它们拽进本项目的分组
	store.DB.Model(&models.Device{}).Where("id IN ? AND project_id = ?", req.DeviceIDs, ctx.ProjectID).
		Update("group_id", req.GroupID)
	ok(c, nil)
}
