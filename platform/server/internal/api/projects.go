package api

import (
	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// ---------- 项目 ACC-03 ----------

func handleListProjects(c *gin.Context) {
	var projs []models.Project
	store.DB.Order("created_at ASC").Find(&projs)
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
	ok(c, p)
}

func handleUpdateProject(c *gin.Context) {
	id := c.Param("id")
	var req struct {
		Name    *string `json:"name"`
		TZ      *string `json:"tz"`
		Enabled *bool   `json:"enabled"`
		SetupDone *bool `json:"setupDone"`
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
	if err := store.DB.Model(&models.Project{}).Where("id = ?", id).Updates(updates).Error; err != nil {
		fail(c, errs.EServerInternal)
		return
	}
	var p models.Project
	store.DB.First(&p, "id = ?", id)
	ok(c, p)
}

// ---------- 分组 ACC-04 ----------

func handleListGroups(c *gin.Context) {
	pid := c.Query("projectId")
	if pid == "" {
		if ctx := getCtx(c); ctx != nil {
			pid = ctx.ProjectID
		}
	}
	var groups []models.DeviceGroup
	store.DB.Where("project_id = ?", pid).Order("sort ASC").Find(&groups)
	ok(c, gin.H{"items": groups})
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
		if store.DB.First(&g, "id = ?", parent).Error != nil {
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
	store.DB.Model(&models.DeviceGroup{}).Where("id = ?", id).Updates(updates)
	var g models.DeviceGroup
	store.DB.First(&g, "id = ?", id)
	ok(c, g)
}

// handleDeleteGroup 删除含设备的分组须先转移。
func handleDeleteGroup(c *gin.Context) {
	id := c.Param("id")
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
	store.DB.Delete(&models.DeviceGroup{}, "id = ?", id)
	ok(c, nil)
}

// handleMoveDevices 拖拽移动设备到分组。
func handleMoveDevices(c *gin.Context) {
	var req struct {
		DeviceIDs []string `json:"deviceIds" binding:"required"`
		GroupID   string   `json:"groupId"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	store.DB.Model(&models.Device{}).Where("id IN ?", req.DeviceIDs).
		Update("group_id", req.GroupID)
	ok(c, nil)
}
