package api

import (
	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/crypto"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// ---------- 角色 ACC-05/06 ----------

func handleListRoles(c *gin.Context) {
	ctx := getCtx(c)
	var roles []models.Role
	store.DB.Where("project_id = ?", ctx.ProjectID).Find(&roles)
	ok(c, gin.H{"items": roles})
}

func handleCreateRole(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Name     string       `json:"name" binding:"required"`
		Perms    models.JSONB `json:"perms"`
		Scope    models.JSONB `json:"scope"`
		CopyFrom string       `json:"copyFrom"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	perms, scope := req.Perms, req.Scope
	if req.CopyFrom != "" {
		var src models.Role
		if store.DB.First(&src, "id = ? AND project_id = ?", req.CopyFrom, ctx.ProjectID).Error == nil {
			if perms == nil {
				perms = src.Perms
			}
			if scope == nil {
				scope = src.Scope
			}
		}
	}
	if perms == nil {
		perms = models.JSONB{"menus": []string{"dashboard"}, "actions": []string{"view"}}
	}
	if scope == nil {
		scope = models.JSONB{}
	}
	r := models.Role{ID: "role_" + models.NewID(), ProjectID: ctx.ProjectID, Name: req.Name,
		Perms: perms, Scope: scope, CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
	store.DB.Create(&r)
	ok(c, r)
}

func handleUpdateRole(c *gin.Context) {
	id := c.Param("id")
	ctx := getCtx(c)
	var src models.Role
	if err := store.DB.First(&src, "id = ? AND project_id = ?", id, ctx.ProjectID).Error; err != nil {
		fail(c, errs.ENotFound)
		return
	}
	if src.Builtin {
		fail(c, errs.EForbid.WithMsg("预置角色不可修改"))
		return
	}
	var req struct {
		Name  string       `json:"name"`
		Perms models.JSONB `json:"perms"`
		Scope models.JSONB `json:"scope"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.Name != "" {
		updates["name"] = req.Name
	}
	if req.Perms != nil {
		updates["perms"] = req.Perms
	}
	if req.Scope != nil {
		updates["scope"] = req.Scope
	}
	store.DB.Model(&src).Updates(updates)
	ok(c, src)
}

func handleDeleteRole(c *gin.Context) {
	id := c.Param("id")
	ctx := getCtx(c)
	var r models.Role
	if err := store.DB.First(&r, "id = ? AND project_id = ?", id, ctx.ProjectID).Error; err != nil {
		fail(c, errs.ENotFound)
		return
	}
	if r.Builtin {
		fail(c, errs.EForbid.WithMsg("预置角色不可删除"))
		return
	}
	var used int64
	store.DB.Model(&models.UserRole{}).Where("role_id = ?", id).Count(&used)
	if used > 0 {
		fail(c, errs.EBadRequest.WithMsg("角色已被成员使用"))
		return
	}
	store.DB.Delete(&r)
	ok(c, nil)
}

// ---------- 成员 ACC-07 ----------

func handleListUsers(c *gin.Context) {
	ctx := getCtx(c)
	// 成员按租户隔离：成员列表含用户名/真实姓名/联系方式，跨租户可见是直接的租户数据泄露
	var users []models.User
	store.DB.Where("tenant_id = ?", ctx.TenantID).Find(&users)
	var urs []models.UserRole
	store.DB.Where("project_id = ?", ctx.ProjectID).Find(&urs)
	roleByUser := map[string]string{}
	for _, ur := range urs {
		roleByUser[ur.UserID] = ur.RoleID
	}
	items := make([]gin.H, 0, len(users))
	for _, u := range users {
		items = append(items, gin.H{
			"id": u.ID, "username": u.Username, "name": u.Name, "contact": u.Contact,
			"status": u.Status, "roleId": roleByUser[u.ID], "createdAt": u.CreatedAt,
		})
	}
	ok(c, gin.H{"items": items})
}

func handleCreateUser(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Username string `json:"username" binding:"required"`
		Name     string `json:"name"`
		Contact  string `json:"contact"`
		RoleID   string `json:"roleId" binding:"required"`
		Password string `json:"password" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if len(req.Password) < 8 {
		fail(c, errs.EBadRequest.WithMsg("初始密码至少 8 位"))
		return
	}
	var cnt int64
	store.DB.Model(&models.User{}).Where("username = ?", req.Username).Count(&cnt)
	if cnt > 0 {
		fail(c, errs.EBadRequest.WithMsg("用户名已存在"))
		return
	}
	// 角色必须属于当前项目：否则可以绑定别家项目的角色，凭空拿到一份不属于自己的权限集
	if _, okr := roleInProject(c, req.RoleID); !okr {
		return
	}
	u, _ := models.NewUser(ctx.TenantID, req.Username, req.Password, req.Name)
	u.Contact = req.Contact
	if err := store.DB.Create(u).Error; err != nil {
		fail(c, errs.EServerInternal)
		return
	}
	store.DB.Create(&models.UserRole{UserID: u.ID, RoleID: req.RoleID, ProjectID: ctx.ProjectID})
	ok(c, gin.H{"id": u.ID})
}

func handleUpdateUser(c *gin.Context) {
	id := c.Param("id")
	ctx := getCtx(c)
	// 成员是租户级实体：跨租户改成员资料/联系方式/启用状态等同于接管他人账号
	if _, oku := userInTenant(c, id); !oku {
		return
	}
	var req struct {
		Name    string `json:"name"`
		Contact string `json:"contact"`
		Status  string `json:"status"` // active/disabled
		RoleID  string `json:"roleId"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if req.RoleID != "" {
		if _, okr := roleInProject(c, req.RoleID); !okr {
			return
		}
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.Name != "" {
		updates["name"] = req.Name
	}
	if req.Contact != "" {
		updates["contact"] = req.Contact
	}
	if req.Status != "" {
		updates["status"] = req.Status
	}
	store.DB.Model(&models.User{}).Where("id = ? AND tenant_id = ?", id, ctx.TenantID).Updates(updates)
	if req.RoleID != "" && ctx != nil {
		store.DB.Where("user_id = ? AND project_id = ?", id, ctx.ProjectID).
			Delete(&models.UserRole{})
		store.DB.Create(&models.UserRole{UserID: id, RoleID: req.RoleID, ProjectID: ctx.ProjectID})
	}
	ok(c, nil)
}

func handleResetPassword(c *gin.Context) {
	id := c.Param("id")
	ctx := getCtx(c)
	// 漏了这一步就是「用一个 ID 直接重置别家租户管理员的密码」——最高危的一类越权，
	// 拿到的就是对方账号的完全控制权
	if _, oku := userInTenant(c, id); !oku {
		return
	}
	var req struct {
		Password string `json:"password" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil || len(req.Password) < 8 {
		fail(c, errs.EBadRequest.WithMsg("新密码至少 8 位"))
		return
	}
	store.DB.Model(&models.User{}).Where("id = ? AND tenant_id = ?", id, ctx.TenantID).
		Updates(map[string]any{"pwd_hash": crypto.HashPassword(req.Password), "updated_at": models.NowMilli()})
	ok(c, nil)
}

func handleDeleteUser(c *gin.Context) {
	id := c.Param("id")
	ctx := getCtx(c)
	u, oku := userInTenant(c, id)
	if !oku {
		return
	}
	var isSuper int64
	store.DB.Model(&models.UserRole{}).Where("user_id = ? AND role_id = 'role_super'", id).Count(&isSuper)
	if isSuper > 0 {
		fail(c, errs.EForbid.WithMsg("超级管理员不可删除"))
		return
	}
	store.DB.Delete(&models.UserRole{}, "user_id = ?", id)
	store.DB.Delete(u, "id = ? AND tenant_id = ?", id, ctx.TenantID)
	ok(c, nil)
}
