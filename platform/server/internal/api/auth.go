package api

import (
	"time"

	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/auth"
	"github.com/jetscam/ipccloud/server/internal/crypto"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

type loginReq struct {
	Username string `json:"username" binding:"required"`
	Password string `json:"password" binding:"required"`
	Remember bool   `json:"remember"`
}

// handleLogin ACC-01。
func handleLogin(c *gin.Context) {
	var req loginReq
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	auditLogin := func(result string) {
		store.DB.Create(&models.AuditLog{
			ID: "lg_" + models.NewID(), UserID: req.Username, Username: req.Username,
			Action: "login", Target: "session:" + req.Username, Result: result,
			IP: c.ClientIP(), Detail: models.JSONB{"path": "/auth/login"}, Ts: models.NowMilli(),
		})
	}
	if left := auth.CheckLocked(req.Username); left > 0 {
		e := errs.EAccountLocked.WithMsg("账号已锁定，请 " + itoa(left) + " 秒后重试")
		auditLogin("fail")
		fail(c, e)
		return
	}
	var u models.User
	if err := store.DB.First(&u, "username = ?", req.Username).Error; err != nil ||
		!crypto.VerifyPassword(req.Password, u.PwdHash) {
		auditLogin("fail")
		if auth.RecordFail(req.Username) {
			fail(c, errs.EAccountLocked.WithMsg("连续 5 次登录失败，账号锁定 15 分钟"))
			return
		}
		fail(c, errs.EUnauthorized.WithMsg("用户名或密码错误"))
		return
	}
	auth.RecordSuccess(req.Username)
	refreshTTL := 7 * 24 * time.Hour
	if req.Remember {
		refreshTTL = 30 * 24 * time.Hour
	}
	access, err1 := auth.Sign("access", u.ID, u.TenantID, "", 2*time.Hour)
	refresh, err2 := auth.Sign("refresh", u.ID, u.TenantID, "", refreshTTL)
	if err1 != nil || err2 != nil {
		fail(c, errs.EServerInternal)
		return
	}
	auditLogin("success")
	ok(c, gin.H{"accessToken": access, "refreshToken": refresh,
		"user": gin.H{"id": u.ID, "username": u.Username, "name": u.Name}})
}

type refreshReq struct {
	RefreshToken string `json:"refreshToken" binding:"required"`
}

func handleRefresh(c *gin.Context) {
	var req refreshReq
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	claims, err := auth.Parse(req.RefreshToken)
	if err != nil || claims.Kind != "refresh" {
		fail(c, errs.EUnauthorized)
		return
	}
	access, _ := auth.Sign("access", claims.UserID, claims.TenantID, "", 2*time.Hour)
	refresh, _ := auth.Sign("refresh", claims.UserID, claims.TenantID, "", 7*24*time.Hour)
	ok(c, gin.H{"accessToken": access, "refreshToken": refresh})
}

func handleLogout(c *gin.Context) { ok(c, nil) }

// handleMe 当前用户信息 + 项目列表。
func handleMe(c *gin.Context) {
	ctx := getCtx(c)
	var u models.User
	if err := store.DB.First(&u, "id = ?", ctx.UserID).Error; err != nil {
		fail(c, errs.ENotFound)
		return
	}
	var projs []models.Project
	store.DB.Where("enabled = ?", true).Order("created_at ASC").Find(&projs)
	rolesByProject := map[string]string{}
	for _, p := range projs {
		if r := loadRole(u.ID, p.ID); r != nil {
			rolesByProject[p.ID] = r.Name
		}
	}
	ok(c, gin.H{"user": u, "projects": projs, "roles": rolesByProject, "setupDone": firstProjectSetupDone(projs)})
}

func firstProjectSetupDone(projs []models.Project) bool {
	for _, p := range projs {
		return p.SetupDone
	}
	return true
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	// 20 字节装得下 int64 的全部十进制位（最多 19 位）+ 符号。
	// 原来是 12 字节，而毫秒时间戳是 13 位，写快照文件名时会越界 panic（b[-1]）——
	// 表现是「上传快照 500 / 抓图永远是空图」。
	var b [20]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}

// ---------- 个人中心 ACC-09 ----------

type changePwdReq struct {
	OldPassword string `json:"oldPassword" binding:"required"`
	NewPassword string `json:"newPassword" binding:"required"`
}

func handleChangePassword(c *gin.Context) {
	ctx := getCtx(c)
	var req changePwdReq
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if len(req.NewPassword) < 8 || !hasLetterDigit(req.NewPassword) {
		fail(c, errs.EBadRequest.WithMsg("密码至少 8 位且包含字母与数字"))
		return
	}
	var u models.User
	if store.DB.First(&u, "id = ?", ctx.UserID).Error != nil ||
		!crypto.VerifyPassword(req.OldPassword, u.PwdHash) {
		fail(c, errs.EUnauthorized.WithMsg("原密码错误"))
		return
	}
	u.PwdHash = crypto.HashPassword(req.NewPassword)
	u.UpdatedAt = models.NowMilli()
	store.DB.Save(&u)
	ok(c, nil)
}

func hasLetterDigit(s string) bool {
	var l, d bool
	for _, r := range s {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z':
			l = true
		case r >= '0' && r <= '9':
			d = true
		}
	}
	return l && d
}

func handleUpdateMe(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Name    string `json:"name"`
		Contact string `json:"contact"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	store.DB.Model(&models.User{}).Where("id = ?", ctx.UserID).
		Updates(map[string]any{"name": req.Name, "contact": req.Contact})
	ok(c, nil)
}
