// Package api 路由注册。
package api

import (
	"github.com/gin-gonic/gin"
	"github.com/jetscam/ipccloud/server/internal/wshub"
)

// Router 构建全部路由。
func Router(hub *wshub.Hub) *gin.Engine {
	r := gin.New()
	r.Use(gin.Logger(), gin.Recovery())
	r.MaxMultipartMemory = 64 << 20

	// 健康检查
	r.GET("/healthz", func(c *gin.Context) { c.JSON(200, gin.H{"ok": true}) })

	// ZLM Hook（无鉴权，内网；由 ZLM 侧 secret 校验）
	hooks := r.Group("/hooks/zlm")
	appEngine.RegisterHooks(hooks)

	// WebSocket 事件
	r.GET("/ws/v1/events", hub.Handler)

	// 认证
	v1 := r.Group("/api/v1")
	v1.Use(AuditMiddleware()) // ACC-08：写操作统一落操作日志
	{
		v1.POST("/auth/login", handleLogin)
		v1.POST("/auth/refresh", handleRefresh)
		v1.POST("/auth/logout", AuthMiddleware(), handleLogout)
		v1.GET("/me", AuthMiddleware(), handleMe)
		v1.PUT("/me/password", AuthMiddleware(), handleChangePassword)
		v1.PUT("/me", AuthMiddleware(), handleUpdateMe)

		// 静态与上传（内网）
		v1.GET("/static/:name", handleStatic)
		v1.PUT("/upload/snapshot", handleUploadSnapshot)
		v1.POST("/upload/snapshot", handleUploadSnapshot)

		// 项目（列表/创建无需项目上下文）
		v1.GET("/projects", AuthMiddleware(), handleListProjects)
		v1.POST("/projects", AuthMiddleware(), handleCreateProject)
		v1.PUT("/projects/:id", AuthMiddleware(), requirePerm("config"), handleUpdateProject)

		// 设备接入端点（接入规范 §9）
		v1.POST("/devices/idp/bind", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleIdpBind)
		v1.POST("/devices/idp/lookup", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleIdpLookup)
		v1.POST("/devices/batch", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeviceBatch)
		v1.POST("/devices/idp/preadd", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleIdpPreadd)
		v1.GET("/devices/idp/preadd", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleIdpPreaddList)
		v1.POST("/devices/idp/preadd/:id/activate", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleIdpPreaddActivate)
		v1.GET("/devices/gb28181/pending", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleGbPendingList)
		v1.POST("/devices/gb28181/pending/:id/confirm", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleGbConfirm)
		v1.POST("/devices/gb28181/pending/:id/reject", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleGbReject)
		v1.GET("/devices/gb28181/whitelist", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleGbWhitelistList)
		v1.POST("/devices/gb28181/whitelist", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleGbWhitelistAdd)
		v1.DELETE("/devices/gb28181/whitelist/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleGbWhitelistDelete)
		v1.POST("/devices/onvif/discover", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleOnvifDiscover)
		v1.POST("/devices/onvif", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleOnvifAdd)
		v1.POST("/devices/rtsp", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleRtspAdd)
		v1.GET("/devices/export", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleExportDevices)

		// 设备
		v1.GET("/devices", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListDevices)
		v1.GET("/devices/:id", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleDeviceDetail)
		v1.PUT("/devices/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleUpdateDevice)
		v1.DELETE("/devices/:id", AuthMiddleware(), requireProjectID(), requirePerm("delete"), handleDeleteDevice)
		v1.POST("/devices/:id/transfer", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleTransferDevice)
		v1.POST("/devices/:id/sync", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleSyncDevice)
		v1.POST("/devices/:id/diag", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDiagDevice)
		v1.POST("/devices/:id/reboot", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleRebootDevice)
		v1.GET("/devices/:id/config", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeviceConfigGet)
		v1.PUT("/devices/:id/config", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeviceConfigSet)
		v1.GET("/devices/:id/channels", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleDeviceChannels)
		v1.PUT("/channels/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleUpdateChannel)
		v1.GET("/channels", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListChannels)

		// 媒体
		v1.POST("/channels/:id/play", AuthMiddleware(), requireProjectID(), requirePerm("preview"), handlePlay)
		v1.POST("/channels/:id/stop", AuthMiddleware(), requireProjectID(), requirePerm("preview"), handleStopPlay)
		v1.POST("/channels/:id/snapshot", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleSnapshot)
		v1.POST("/channels/:id/cover", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleRefreshCover)
		v1.POST("/channels/:id/ptz", AuthMiddleware(), requireProjectID(), requirePerm("ptz"), handlePTZ)
		v1.GET("/channels/:id/ptz/presets", AuthMiddleware(), requireProjectID(), requirePerm("preview"), handlePTZPresetsList)
		v1.POST("/channels/:id/ptz/presets", AuthMiddleware(), requireProjectID(), requirePerm("ptz"), handlePTZPresetAdd)
		v1.POST("/channels/:id/ptz/preset/goto", AuthMiddleware(), requireProjectID(), requirePerm("ptz"), handlePTZPresetGoto)
		v1.DELETE("/channels/:id/ptz/presets/:pid", AuthMiddleware(), requireProjectID(), requirePerm("ptz"), handlePTZPresetDelete)
		v1.GET("/channels/:id/records", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleChannelRecords)
		v1.GET("/channels/:id/records/days", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleRecordDays)
		v1.POST("/channels/:id/records/download", AuthMiddleware(), requireProjectID(), requirePerm("playback"), handleRecordDownload)
		v1.POST("/channels/:id/playback", AuthMiddleware(), requireProjectID(), requirePerm("playback"), handleStartPlayback)
		v1.PUT("/playback/:sid", AuthMiddleware(), requireProjectID(), requirePerm("playback"), handlePlaybackCtrl)
		v1.DELETE("/playback/:sid", AuthMiddleware(), requireProjectID(), requirePerm("playback"), handleStopPlayback)

		// 分组
		v1.GET("/groups", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListGroups)
		v1.POST("/groups", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleCreateGroup)
		v1.PUT("/groups/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleUpdateGroup)
		v1.DELETE("/groups/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeleteGroup)
		v1.POST("/groups/move-devices", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleMoveDevices)

		// 角色与成员
		v1.GET("/roles", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListRoles)
		v1.POST("/roles", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleCreateRole)
		v1.PUT("/roles/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleUpdateRole)
		v1.DELETE("/roles/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeleteRole)
		v1.GET("/users", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListUsers)
		v1.POST("/users", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleCreateUser)
		v1.PUT("/users/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleUpdateUser)
		v1.DELETE("/users/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeleteUser)
		v1.POST("/users/:id/reset-password", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleResetPassword)

		// 告警
		v1.GET("/alarm-templates", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListAlarmTemplates)
		v1.POST("/alarm-templates", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleCreateAlarmTemplate)
		v1.PUT("/alarm-templates/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleUpdateAlarmTemplate)
		v1.DELETE("/alarm-templates/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeleteAlarmTemplate)
		v1.GET("/alarm-rules", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListAlarmRules)
		v1.POST("/alarm-rules", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleCreateAlarmRule)
		v1.PUT("/alarm-rules/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleUpdateAlarmRule)
		v1.DELETE("/alarm-rules/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeleteAlarmRule)
		v1.GET("/alarm-policies", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleGetAlarmPolicies)
		v1.PUT("/alarm-policies", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleSetAlarmPolicies)
		v1.GET("/alarms", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListAlarms)
		v1.GET("/alarms/:id", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleAlarmDetail)
		v1.POST("/alarms/:id/read", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleReadAlarm)
		v1.POST("/alarms/read-all", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleReadAllAlarms)

		// 录像设置
		v1.GET("/record-templates", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListRecordTemplates)
		v1.POST("/record-templates", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleCreateRecordTemplate)
		v1.PUT("/record-templates/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleUpdateRecordTemplate)
		v1.DELETE("/record-templates/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeleteRecordTemplate)
		v1.GET("/record-plans", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListRecordPlans)
		v1.POST("/record-plans", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleCreateRecordPlan)
		v1.PUT("/record-plans/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleUpdateRecordPlan)
		v1.DELETE("/record-plans/:id", AuthMiddleware(), requireProjectID(), requirePerm("config"), handleDeleteRecordPlan)
		v1.GET("/storage/overview", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleStorageOverview)

		// 媒体节点
		v1.GET("/media-nodes", AuthMiddleware(), handleListNodes)
		v1.POST("/media-nodes", AuthMiddleware(), requirePerm("config"), handleCreateNode)
		v1.PUT("/media-nodes/:id", AuthMiddleware(), requirePerm("config"), handleUpdateNode)
		v1.DELETE("/media-nodes/:id", AuthMiddleware(), requirePerm("config"), handleDeleteNode)
		v1.POST("/media-nodes/:id/selfcheck", AuthMiddleware(), requirePerm("config"), handleSelfCheckNode)
		v1.GET("/media-nodes/:id/streams", AuthMiddleware(), requirePerm("view"), handleNodeStreams)
		v1.POST("/media-nodes/:id/streams/:sid/kick", AuthMiddleware(), requirePerm("config"), handleKickStream)

		// 系统
		v1.GET("/settings", AuthMiddleware(), handleGetSettings)
		v1.PUT("/settings", AuthMiddleware(), requirePerm("config"), handleSetSetting)
		v1.GET("/idp/config", AuthMiddleware(), handleIdpConfig)
		v1.GET("/idp/crl", AuthMiddleware(), handleIdpCRLList)
		v1.POST("/idp/crl", AuthMiddleware(), requirePerm("config"), handleIdpCRLAdd)
		v1.GET("/audit-logs", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleListAuditLogs)
		v1.GET("/audit-logs/export", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleExportAuditLogs)
		v1.GET("/tasks", AuthMiddleware(), handleListTasks)
		v1.GET("/tasks/:id", AuthMiddleware(), handleGetTask)
		v1.GET("/dashboard", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleDashboard)
		v1.GET("/projects/:id/gb28181/params", AuthMiddleware(), requireProjectID(), requirePerm("view"), handleGbParams)
	}

	return r
}
