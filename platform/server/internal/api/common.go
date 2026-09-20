// Package api REST API 与中间件（PRD §8 接口概览）。
package api

import (
	"net/http"
	"strconv"
	"strings"

	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/auth"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// Ctx 认证上下文。
type Ctx struct {
	UserID    string
	TenantID  string
	Username  string
	ProjectID string
	Role      *models.Role
}

func fail(c *gin.Context, e *errs.AppError) {
	c.JSON(e.HTTP, gin.H{"code": e.Code, "msg": e.Msg, "suggest": e.Suggest})
}

func ok(c *gin.Context, data any) {
	if data == nil {
		c.JSON(http.StatusOK, gin.H{"code": 0})
		return
	}
	c.JSON(http.StatusOK, data)
}

// AuthMiddleware JWT 认证。
func AuthMiddleware() gin.HandlerFunc {
	return func(c *gin.Context) {
		h := c.GetHeader("Authorization")
		if !strings.HasPrefix(h, "Bearer ") {
			fail(c, errs.EUnauthorized)
			c.Abort()
			return
		}
		claims, err := auth.Parse(h[7:])
		if err != nil || claims.Kind != "access" {
			fail(c, errs.EUnauthorized)
			c.Abort()
			return
		}
		var u models.User
		if err := store.DB.First(&u, "id = ?", claims.UserID).Error; err != nil || u.Status != "active" {
			fail(c, errs.EUnauthorized.WithMsg("账号不可用"))
			c.Abort()
			return
		}
		ctx := &Ctx{UserID: u.ID, TenantID: u.TenantID, Username: u.Username}
		if pid := c.GetHeader("X-Project-Id"); pid != "" {
			ctx.ProjectID = pid
		}
		if pid := c.Query("projectId"); pid != "" && ctx.ProjectID == "" {
			ctx.ProjectID = pid
		}
		if ctx.ProjectID != "" {
			ctx.Role = loadRole(u.ID, ctx.ProjectID)
		}
		c.Set("actx", ctx)
		c.Next()
	}
}

func loadRole(userID, projectID string) *models.Role {
	var r models.Role
	if err := store.DB.Raw(`SELECT r.* FROM roles r JOIN user_roles ur ON ur.role_id = r.id
		WHERE ur.user_id = ? AND ur.project_id = ? LIMIT 1`, userID, projectID).Scan(&r).Error; err != nil || r.ID == "" {
		return nil
	}
	return &r
}

// getCtx 取认证上下文（未登录返回 nil）。
func getCtx(c *gin.Context) *Ctx {
	v, exists := c.Get("actx")
	if !exists {
		return nil
	}
	return v.(*Ctx)
}

// hasPerm 判断上下文角色是否具备某动作权限（供处理器内做细粒度分支，不中断请求）。
func hasPerm(ctx *Ctx, action string) bool {
	if ctx == nil || ctx.Role == nil {
		return false
	}
	acts, _ := ctx.Role.Perms["actions"].([]any)
	for _, a := range acts {
		if s, _ := a.(string); s == "*" || s == action {
			return true
		}
	}
	return false
}

// requirePerm 校验动作权限（ACC-05）。super "*".
func requirePerm(action string) gin.HandlerFunc {
	return func(c *gin.Context) {
		ctx := getCtx(c)
		if ctx == nil || ctx.Role == nil {
			fail(c, errs.EForbid.WithMsg("未分配项目角色"))
			c.Abort()
			return
		}
		acts, _ := ctx.Role.Perms["actions"].([]any)
		menus, _ := ctx.Role.Perms["menus"].([]any)
		has := false
		for _, a := range acts {
			if s, _ := a.(string); s == "*" || s == action {
				has = true
				break
			}
		}
		if !has {
			fail(c, errs.EForbid)
			c.Abort()
			return
		}
		if len(menus) == 0 {
			// 无菜单权限的空角色不放行查看类动作
			if action == "view" || action == "preview" || action == "playback" {
				fail(c, errs.EForbid)
				c.Abort()
				return
			}
		}
		c.Next()
	}
}

// requireProjectID 确保存在项目上下文。
func requireProjectID() gin.HandlerFunc {
	return func(c *gin.Context) {
		ctx := getCtx(c)
		if ctx == nil || ctx.ProjectID == "" {
			fail(c, errs.EBadRequest.WithMsg("缺少项目上下文（X-Project-Id 或 projectId 参数）"))
			c.Abort()
			return
		}
		if ctx.Role == nil {
			ctx.Role = loadRole(ctx.UserID, ctx.ProjectID)
			if ctx.Role == nil {
				fail(c, errs.EForbid.WithMsg("非项目成员"))
				c.Abort()
				return
			}
		}
		c.Next()
	}
}

// pageParams 分页参数。
func pageParams(c *gin.Context) (page, size int) {
	page, size = 1, 50
	if v, err := strconv.Atoi(c.Query("page")); err == nil && v > 0 {
		page = v
	}
	if v, err := strconv.Atoi(c.Query("pageSize")); err == nil && v > 0 && v <= 500 {
		size = v
	}
	return
}
