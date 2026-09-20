// 操作日志中间件（ACC-08）：统一记录所有写操作（POST/PUT/DELETE），
// 含操作者、时间、对象、动作、结果、IP。GET 与设备侧/刷新类端点不记录。
package api

import (
	"strings"

	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// 资源类型映射（路径首段 → 对象类型）。
var auditResType = map[string]string{
	"devices":          "device",
	"channels":         "channel",
	"roles":            "role",
	"users":            "user",
	"groups":           "group",
	"projects":         "project",
	"media-nodes":      "node",
	"alarm-rules":      "alarm_rule",
	"alarm-policies":   "alarm_policy",
	"alarm-templates":  "alarm_template",
	"record-plans":     "record_plan",
	"record-templates": "record_template",
	"alarms":           "alarm",
	"tasks":            "task",
	"settings":         "setting",
	"idp":              "idp",
	"auth":             "session",
	"playback":         "playback",
	"upload":           "upload",
}

// 动作动词段（出现在路径中即作为动作名）。
var auditVerbs = map[string]bool{
	"bind": true, "preadd": true, "activate": true, "confirm": true, "reject": true,
	"discover": true, "whitelist": true, "sync": true, "diag": true, "reboot": true,
	"transfer": true, "play": true, "stop": true, "snapshot": true, "cover": true,
	"ptz": true, "presets": true, "goto": true, "playback": true, "download": true,
	"selfcheck": true, "kick": true, "read": true, "read-all": true, "reset-password": true,
	"move-devices": true, "batch": true, "config": true, "crl": true, "password": true,
	"clear": true, "cancel": true, "retry": true,
}

// auditSkip 这些写端点属于设备侧/系统内部/高频刷新，不计入用户操作日志。
func auditSkip(fullPath string) bool {
	for _, s := range []string{"/auth/refresh", "/auth/logout", "/upload/snapshot", "/hooks/"} {
		if strings.Contains(fullPath, s) {
			return true
		}
	}
	return false
}

// AuditMiddleware 在鉴权之后执行，记录写操作结果。
func AuditMiddleware() gin.HandlerFunc {
	return func(c *gin.Context) {
		m := c.Request.Method
		if m == "GET" || m == "HEAD" || m == "OPTIONS" {
			c.Next()
			return
		}
		fullPath := c.FullPath()
		if fullPath == "" || auditSkip(fullPath) {
			c.Next()
			return
		}
		c.Next() // 先执行处理器，再依据结果落日志

		ctx := getCtx(c)
		if ctx == nil {
			return // 未通过鉴权的请求不记操作日志（避免令牌噪声；登录失败属认证事件另计）
		}
		status := c.Writer.Status()
		result := "success"
		if status >= 400 {
			result = "fail"
		}
		userID, username, projectID := ctx.UserID, ctx.Username, ctx.ProjectID
		action, resType, resID := auditParse(fullPath, c)
		target := resType
		if resID != "" {
			target = resType + ":" + resID
		}
		log := models.AuditLog{
			ID: "lg_" + models.NewID(), UserID: userID, Username: username, ProjectID: projectID,
			Action: action, Target: target, Result: result, IP: c.ClientIP(),
			Detail: models.JSONB{"method": m, "path": fullPath, "status": status},
			Ts:     models.NowMilli(),
		}
		_ = store.DB.Create(&log).Error
	}
}

// auditParse 由路由模板与参数推导（动作、对象类型、对象 ID）。
func auditParse(fullPath string, c *gin.Context) (action, resType, resID string) {
	trimmed := strings.TrimPrefix(fullPath, "/api/v1/")
	segs := strings.Split(trimmed, "/")
	if len(segs) == 0 {
		return "unknown", "", ""
	}
	resType = auditResType[segs[0]]
	if resType == "" {
		resType = segs[0]
	}
	// 找动词段与 ID 段
	verb := ""
	for _, s := range segs[1:] {
		if auditVerbs[s] {
			verb = s
		}
	}
	resID = c.Param("id")
	if resID == "" {
		resID = c.Param("sid")
	}
	if verb != "" {
		action = verb
	} else {
		switch c.Request.Method {
		case "POST":
			action = "create"
		case "PUT":
			action = "update"
		case "DELETE":
			action = "delete"
		default:
			action = strings.ToLower(c.Request.Method)
		}
	}
	return action, resType, resID
}
