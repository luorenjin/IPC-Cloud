package api

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"gorm.io/gorm"

	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
	"github.com/jetscam/ipccloud/server/internal/timeutil"
)

// ---------- 全局设置 SET-01 ----------

func handleGetSettings(c *gin.Context) {
	scope := c.Query("scope")
	if scope == "" {
		scope = "global"
	}
	var items []models.Setting
	store.DB.Where("scope = ?", scope).Find(&items)
	out := models.JSONB{}
	for _, it := range items {
		out[it.Key] = it.Value
	}
	ok(c, out)
}

func handleSetSetting(c *gin.Context) {
	var req struct {
		Key   string       `json:"key" binding:"required"`
		Value models.JSONB `json:"value" binding:"required"`
		Scope string       `json:"scope"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	scope := req.Scope
	if scope == "" {
		scope = "global"
	}
	var st models.Setting
	store.DB.Where("scope = ? AND key = ?", scope, req.Key).Assign(models.Setting{
		Scope: scope, Key: req.Key, Value: req.Value,
	}).FirstOrCreate(&st)
	store.DB.Model(&st).Update("value", req.Value)
	ok(c, req.Value)
}

// ---------- IDP 服务配置 SET-02 ----------

func handleIdpConfig(c *gin.Context) {
	var idpCount int64
	store.DB.Model(&models.Device{}).Where("source = 'idp'").Count(&idpCount)
	ok(c, gin.H{
		"broker":   appCfg.MQTTBroker,
		"tls":      appCfg.MQTTTLS,
		"caStatus": "active",
		"idpCount": idpCount,
		"crl":      []any{},
	})
}

func handleIdpCRLAdd(c *gin.Context) {
	var req struct {
		DeviceID string `json:"deviceId" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	_ = store.KVSet(crlKey(req.DeviceID), "1", 365*24*time.Hour)
	// 维护索引（列表展示用）
	id := strings.ToUpper(strings.TrimSpace(req.DeviceID))
	idx, _ := store.KVGet("idp:crl:index")
	if idx == "" {
		_ = store.KVSet("idp:crl:index", id, 365*24*time.Hour)
	} else {
		found := false
		for _, s := range strings.Split(idx, ",") {
			if s == id {
				found = true
				break
			}
		}
		if !found {
			_ = store.KVSet("idp:crl:index", idx+","+id, 365*24*time.Hour)
		}
	}
	ok(c, nil)
}

// crlKey CRL 条目键（按设备 ID）。
func crlKey(deviceID string) string { return "idp:crl:" + strings.ToUpper(strings.TrimSpace(deviceID)) }

// crlQuery 查询设备是否在 CRL（吊销列表）中；适配器在 hello/bind 时调用。
func crlQuery(deviceID string) bool {
	_, err := store.KVGet(crlKey(deviceID))
	return err == nil
}

// handleIdpCRLList SET-02：CRL 条目列表。Redis 无扫描接口（KV 抽象），
// 以 KV 维护索引键 idp:crl:index（逗号分隔设备 ID）。
func handleIdpCRLList(c *gin.Context) {
	items := []gin.H{}
	if q := c.Query("deviceId"); q != "" {
		if crlQuery(q) {
			items = append(items, gin.H{"deviceId": strings.ToUpper(strings.TrimSpace(q))})
		}
		ok(c, gin.H{"items": items})
		return
	}
	if v, err := store.KVGet("idp:crl:index"); err == nil && v != "" {
		for _, id := range strings.Split(v, ",") {
			if id != "" {
				items = append(items, gin.H{"deviceId": id})
			}
		}
	}
	ok(c, gin.H{"items": items})
}

// ---------- 操作日志 ACC-08 ----------

// auditFilter 操作日志的筛选条件（列表与导出共用）。
//
// 两端口径必须一致：导出按钮承诺的是「导出当前筛选结果」，
// 此前导出只按项目过滤，带筛选导出会拿到未筛选的全量（且与页面显示不符）。
// 返回 false 表示参数非法，错误响应已在函数内发出，调用方直接 return。
func auditFilter(c *gin.Context, ctx *Ctx) (*gorm.DB, bool) {
	// 租户维度必须先加：登录事件没有项目（project_id 为空），
	// 只按 project_id 过滤会让各租户的登录记录互相可见。
	q := store.DB.Model(&models.AuditLog{}).
		Where("tenant_id = ?", ctx.TenantID).
		Where("project_id = ? OR project_id = ''", ctx.ProjectID)
	if a := c.Query("action"); a != "" {
		q = q.Where("action = ?", a)
	}
	if u := c.Query("username"); u != "" {
		q = q.Where("username = ?", u)
	}
	if tt := c.Query("targetType"); tt != "" {
		// 对象类型在 target 里（`device:<id>`；无 ID 的写操作只有 `device`，如 POST /devices）。
		// 用 `= tt OR LIKE 'tt:%'` 而不是 `LIKE 'tt%'`：后者会让 alarm 误命中 alarm_rule。
		q = q.Where("(target = ? OR target LIKE ?)", tt, tt+":%")
	}
	// 时间范围按**项目时区**的自然日、闭开区间 [startDate 00:00, endDate+1 00:00)。
	// 前端传 YYYY-MM-DD，不传浏览器侧算出的毫秒：那会把浏览器时区烘进筛选条件，
	// 而值班看的是项目时区的“这一天”（server 容器 TZ 为 UTC，不能用 time.Local）。
	// 时区要查库，所以只在真带了日期参数时才算。
	if s, e := c.Query("startDate"), c.Query("endDate"); s != "" || e != "" {
		loc := store.ProjectLocation(ctx.ProjectID)
		if s != "" {
			t, err := time.ParseInLocation("2006-01-02", s, loc)
			if err != nil {
				fail(c, errs.EBadRequest.WithMsg("startDate 需为 YYYY-MM-DD 格式"))
				return nil, false
			}
			q = q.Where("ts >= ?", t.UnixMilli())
		}
		if e != "" {
			t, err := time.ParseInLocation("2006-01-02", e, loc)
			if err != nil {
				fail(c, errs.EBadRequest.WithMsg("endDate 需为 YYYY-MM-DD 格式"))
				return nil, false
			}
			q = q.Where("ts < ?", timeutil.NextDayStart(t, loc).UnixMilli())
		}
	}
	return q, true
}

func handleListAuditLogs(c *gin.Context) {
	ctx := getCtx(c)
	page, size := pageParams(c)
	q, valid := auditFilter(c, ctx)
	if !valid {
		return
	}
	var total int64
	q.Count(&total)
	var items []models.AuditLog
	q.Order("ts DESC").Offset((page - 1) * size).Limit(size).Find(&items)
	ok(c, gin.H{"total": total, "items": items})
}

func handleExportAuditLogs(c *gin.Context) {
	ctx := getCtx(c)
	q, valid := auditFilter(c, ctx)
	if !valid {
		return
	}
	var items []models.AuditLog
	// 与列表同一套筛选；上限 10000 行，超出部分不导（保持原有防护，避免一次拉爆内存）
	q.Order("ts DESC").Limit(10000).Find(&items)
	c.Header("Content-Disposition", "attachment; filename=audit-logs.csv")
	c.Header("Content-Type", "text/csv; charset=utf-8")
	c.Writer.WriteString("time,user,action,target,result,ip\n")
	for _, it := range items {
		c.Writer.WriteString(time.UnixMilli(it.Ts).Format("2006-01-02 15:04:05") + "," +
			it.Username + "," + it.Action + "," + it.Target + "," + it.Result + "," + it.IP + "\n")
	}
}

// ---------- 任务中心 ----------

// 任务归属项目记录在 Result.projectId 里（Task 表本身无 project_id 列），
// 两个读取端点都必须按它过滤，否则会跨项目泄露其他租户的任务。
func handleGetTask(c *gin.Context) {
	ctx := getCtx(c)
	var t models.Task
	if err := store.DB.Where("id = ? AND result->>'projectId' = ?",
		c.Param("id"), ctx.ProjectID).First(&t).Error; err != nil {
		fail(c, errs.ENotFound)
		return
	}
	ok(c, t)
}

func handleListTasks(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.Task
	q := store.DB.Where("result->>'projectId' = ?", ctx.ProjectID)
	if s := c.Query("status"); s != "" {
		q = q.Where("status = ?", s)
	}
	q.Order("created_at DESC").Limit(100).Find(&items)
	ok(c, gin.H{"items": items})
}

// ---------- 仪表盘 DASH-01 ----------

func handleDashboard(c *gin.Context) {
	ctx := getCtx(c)
	pid := ctx.ProjectID
	var deviceTotal, deviceOnline, channelTotal, playing int64
	store.DB.Model(&models.Device{}).Where("project_id = ? AND deleted_at = 0", pid).Count(&deviceTotal)
	store.DB.Model(&models.Device{}).Where("project_id = ? AND deleted_at = 0 AND status = 'online'", pid).Count(&deviceOnline)
	store.DB.Model(&models.Channel{}).Where("project_id = ? AND enabled = ?", pid, true).Count(&channelTotal)
	store.DB.Model(&models.StreamSession{}).Joins("JOIN channels c ON c.id = stream_sessions.channel_id").
		Where("c.project_id = ? AND stream_sessions.ended_at = 0 AND stream_sessions.kind = 'live'", pid).Count(&playing)

	bySource := models.JSONB{}
	var srcRows []struct {
		Source string
		Cnt    int64
	}
	store.DB.Model(&models.Device{}).Select("source, COUNT(*) as cnt").
		Where("project_id = ? AND deleted_at = 0", pid).Group("source").Scan(&srcRows)
	for _, r := range srcRows {
		bySource[r.Source] = r.Cnt
	}

	var alarmToday int64
	// 「今日」必须按项目时区取零点：服务端容器 time.Local 通常是 UTC，
	// 直接用它会让"今日告警"整体偏移（UTC+8 下把前一日 08:00 之后都算作今天）。
	dayStart := timeutil.DayStartMillis(time.Now(), store.ProjectLocation(pid))
	store.DB.Model(&models.AlarmEvent{}).Where("project_id = ? AND ts >= ?", pid, dayStart).Count(&alarmToday)

	var nodes []models.MediaNode
	store.DB.Find(&nodes)
	nodeHealth := make([]gin.H, 0, len(nodes))
	for _, n := range nodes {
		nodeHealth = append(nodeHealth, gin.H{"id": n.ID, "name": n.Name, "status": n.Status,
			"streams": n.Streams, "maxStreams": n.MaxStreams})
	}

	recentAlarms := []models.AlarmEvent{}
	store.DB.Where("project_id = ?", pid).Order("ts DESC").Limit(10).Find(&recentAlarms)

	ok(c, gin.H{
		"deviceTotal": deviceTotal, "deviceOnline": deviceOnline,
		"channelTotal": channelTotal, "playing": playing,
		"bySource": bySource, "alarmToday": alarmToday,
		"nodes": nodeHealth, "recentAlarms": recentAlarms,
	})
}

// ---------- 静态文件与快照上传 ----------

func handleStatic(c *gin.Context) {
	name := c.Param("name")
	if strings.Contains(name, "..") || strings.Contains(name, "/") {
		fail(c, errs.EBadRequest)
		return
	}
	p := filepath.Join(appCfg.DataDir, name)
	if _, err := os.Stat(p); err != nil {
		fail(c, errs.ENotFound)
		return
	}
	c.File(p)
}

// handleUploadSnapshot 设备快照上传（预签名 URL 落点）。
func handleUploadSnapshot(c *gin.Context) {
	channelID := c.Query("channelId")
	body, err := c.GetRawData()
	if err != nil || len(body) == 0 {
		fail(c, errs.EBadRequest)
		return
	}
	name := "snap_" + channelID + "_" + itoa(int(models.NowMilli())) + ".jpg"
	if err := os.WriteFile(filepath.Join(appCfg.DataDir, name), body, 0o644); err != nil {
		fail(c, errs.EServerInternal)
		return
	}
	url := "/api/v1/static/" + name
	if channelID != "" {
		store.DB.Model(&models.Channel{}).Where("id = ?", channelID).Update("cover_url", url)
	}
	ok(c, gin.H{"url": url})
}

var _ = json.Marshal
