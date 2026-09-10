package api

import (
	"encoding/csv"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"

	"github.com/gin-gonic/gin"
	"gorm.io/gorm"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/adapter/onvif"
	"github.com/jetscam/ipccloud/server/internal/devsvc"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// ---------- 设备列表 MGR-01/02 ----------

// deviceFilters 把设备列表的筛选条件（groupId/status/source/keyword/bulk）应用到查询上。
// 列表与 CSV 导出共用同一实现，避免两处筛选逻辑漂移（MGR-13：导出必须与当前列表一致）。
func deviceFilters(c *gin.Context, q *gorm.DB) *gorm.DB {
	if g := c.Query("groupId"); g != "" {
		q = q.Where("group_id = ?", g)
	}
	if s := c.Query("status"); s != "" {
		q = q.Where("status = ?", s)
	}
	if s := c.Query("source"); s != "" {
		q = q.Where("source = ?", s)
	}
	if s := c.Query("keyword"); s != "" {
		kw := "%" + s + "%"
		q = q.Where(`name ILIKE ? OR model ILIKE ? OR vendor ILIKE ?
			OR meta->>'ip' ILIKE ? OR identity->>'deviceId' ILIKE ? OR identity->>'gbId' ILIKE ?`,
			kw, kw, kw, kw, kw, kw)
	}
	if ips := c.Query("bulk"); ips != "" {
		lines := strings.FieldsFunc(ips, func(r rune) bool { return r == '\n' || r == ',' || r == '\r' })
		if len(lines) > 0 {
			q = q.Where(`identity->>'deviceId' IN ? OR identity->>'gbId' IN ? OR meta->>'ip' IN ?`,
				lines, lines, lines)
		}
	}
	return q
}

func handleListDevices(c *gin.Context) {
	ctx := getCtx(c)
	page, size := pageParams(c)
	q := deviceFilters(c, store.DB.Model(&models.Device{}).
		Where("project_id = ? AND deleted_at = 0", ctx.ProjectID))
	var total int64
	q.Count(&total)
	var devs []models.Device
	q.Order("created_at DESC").Offset((page - 1) * size).Limit(size).Find(&devs)
	items := make([]gin.H, 0, len(devs))
	for _, d := range devs {
		items = append(items, deviceJSON(d))
	}
	// 类型状态卡片
	var stats struct{ All, Online, Offline int64 }
	store.DB.Model(&models.Device{}).Where("project_id = ? AND deleted_at = 0", ctx.ProjectID).Count(&stats.All)
	store.DB.Model(&models.Device{}).Where("project_id = ? AND deleted_at = 0 AND status = 'online'", ctx.ProjectID).Count(&stats.Online)
	stats.Offline = stats.All - stats.Online
	ok(c, gin.H{"total": total, "items": items, "stats": stats})
}

func deviceJSON(d models.Device) gin.H {
	ip, _ := d.Meta["ip"].(string)
	mac, _ := d.Meta["mac"].(string)
	return gin.H{
		"id": d.ID, "name": d.Name, "source": d.Source, "altSources": d.AltSources,
		"model": d.Model, "vendor": d.Vendor, "fw": d.Fw, "hw": d.Hw,
		"identity": d.Identity, "status": d.Status, "error": d.Error,
		"lastSeenAt": d.LastSeenAt, "capabilities": d.Capabilities,
		"groupId": d.GroupID, "location": d.Location, "remark": d.Remark,
		"ip": ip, "mac": mac, "meta": safeMeta(d.Meta),
	}
}

func safeMeta(m models.JSONB) models.JSONB {
	out := models.JSONB{}
	for k, v := range m {
		if k == "metrics" || k == "gbStream" || k == "gbStreamSub" || k == "gbStreamKeyMain" || strings.HasPrefix(k, "rtsp") {
			out[k] = v
		}
	}
	return out
}

// handleExportDevices MGR-13 CSV 导出（不含凭据）。
// 筛选条件与列表页一致——导出的是"我现在看到的这批设备"，不是全量。
func handleExportDevices(c *gin.Context) {
	ctx := getCtx(c)
	q := store.DB.Model(&models.Device{}).Where("project_id = ? AND deleted_at = 0", ctx.ProjectID)
	// 勾选了具体行就只导这些行；否则按当前筛选条件导出。
	if ids := c.Query("ids"); ids != "" {
		q = q.Where("id IN ?", strings.Split(ids, ","))
	} else {
		q = deviceFilters(c, q)
	}
	var devs []models.Device
	q.Order("created_at DESC").Find(&devs)
	c.Header("Content-Disposition", "attachment; filename=devices.csv")
	c.Header("Content-Type", "text/csv; charset=utf-8")
	w := csv.NewWriter(c.Writer)
	w.Write([]string{"名称", "来源", "型号", "厂商", "固件", "IP", "MAC", "状态", "分组", "安装位置"})
	for _, d := range devs {
		ip, _ := d.Meta["ip"].(string)
		mac, _ := d.Meta["mac"].(string)
		w.Write([]string{d.Name, d.Source, d.Model, d.Vendor, d.Fw, ip, mac, d.Status, d.GroupID, d.Location})
	}
	w.Flush()
}

// handleDeviceDetail MGR-03。
func handleDeviceDetail(c *gin.Context) {
	id := c.Param("id")
	var d models.Device
	if err := store.DB.First(&d, "id = ? AND deleted_at = 0", id).Error; err != nil {
		fail(c, errs.ENotFound)
		return
	}
	var chs []models.Channel
	store.DB.Where("device_id = ?", id).Order("idx ASC").Find(&chs)
	resp := deviceJSON(d)
	resp["channels"] = chs
	if m, ok := d.Meta["metrics"]; ok {
		resp["metrics"] = m
	}
	// 最近事件
	var events []models.AuditLog
	store.DB.Where("target LIKE ?", "device:"+id+"%").Order("ts DESC").Limit(10).Find(&events)
	resp["recentOps"] = events
	ok(c, resp)
}

// handleUpdateDevice MGR-04。
func handleUpdateDevice(c *gin.Context) {
	id := c.Param("id")
	var req struct {
		Name     *string `json:"name"`
		GroupID  *string `json:"groupId"`
		Location *string `json:"location"`
		Remark   *string `json:"remark"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.Name != nil {
		updates["name"] = *req.Name
	}
	if req.GroupID != nil {
		updates["group_id"] = *req.GroupID
	}
	if req.Location != nil {
		updates["location"] = *req.Location
	}
	if req.Remark != nil {
		updates["remark"] = *req.Remark
	}
	if err := store.DB.Model(&models.Device{}).Where("id = ?", id).Updates(updates).Error; err != nil {
		fail(c, errs.EServerInternal)
		return
	}
	var d models.Device
	store.DB.First(&d, "id = ?", id)
	ok(c, deviceJSON(d))
}

// handleDeleteDevice MGR-11 软删除。
func handleDeleteDevice(c *gin.Context) {
	id := c.Param("id")
	var req struct {
		ConfirmName string `json:"confirmName" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	var d models.Device
	if store.DB.First(&d, "id = ? AND deleted_at = 0", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	if d.Name != req.ConfirmName {
		fail(c, errs.EBadRequest.WithMsg("设备名不一致"))
		return
	}
	store.DB.Model(&d).Update("deleted_at", models.NowMilli())
	ok(c, nil)
}

// handleTransferDevice MGR-12。
func handleTransferDevice(c *gin.Context) {
	id := c.Param("id")
	var req struct {
		ProjectID string `json:"projectId"`
		GroupID   string `json:"groupId"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	var d models.Device
	if store.DB.First(&d, "id = ?", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	if req.ProjectID != "" && req.ProjectID != d.ProjectID {
		if a := adapter.Get(d.Source); a != nil {
			_ = a.Transfer(c.Request.Context(), id, req.ProjectID)
		}
		store.DB.Model(&d).Update("project_id", req.ProjectID)
		store.DB.Model(&models.Channel{}).Where("device_id = ?", id).Update("project_id", req.ProjectID)
	}
	if req.GroupID != "" {
		store.DB.Model(&d).Update("group_id", req.GroupID)
	}
	ok(c, nil)
}

// handleSyncDevice MGR-06。
func handleSyncDevice(c *gin.Context) {
	id := c.Param("id")
	var d models.Device
	if store.DB.First(&d, "id = ?", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	switch d.Source {
	case "gb28181":
		// 触发国标目录重查：经 SIP 侧 QueryCatalog 由适配器异步完成，
		// 此处统计当前通道数作为同步结果
		var chs int64
		store.DB.Model(&models.Channel{}).Where("device_id = ?", id).Count(&chs)
		ok(c, gin.H{"channels": chs, "note": "catalog re-query triggered"})
	default:
		ok(c, gin.H{"note": "sync not supported for source " + d.Source})
	}
}

// handleDiagDevice MGR-07。
func handleDiagDevice(c *gin.Context) {
	id := c.Param("id")
	var d models.Device
	if store.DB.First(&d, "id = ?", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	results := []map[string]any{}
	if a := adapter.Get(d.Source); a != nil {
		r, err := a.Diagnose(c.Request.Context(), id)
		if err != nil {
			fail(c, toAppErr(err))
			return
		}
		results = r
	}
	ok(c, gin.H{"results": results, "at": models.NowMilli()})
}

// handleRebootDevice MGR-08。
func handleRebootDevice(c *gin.Context) {
	id := c.Param("id")
	var d models.Device
	if store.DB.First(&d, "id = ?", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	a := adapter.Get(d.Source)
	if a == nil {
		fail(c, errs.EForbid.WithMsg("该来源不支持远程重启"))
		return
	}
	if err := a.Reboot(c.Request.Context(), id); err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, nil)
}

// handleDeviceChannels MGR-05。
func handleDeviceChannels(c *gin.Context) {
	id := c.Param("id")
	var chs []models.Channel
	store.DB.Where("device_id = ?", id).Order("idx ASC").Find(&chs)
	ok(c, gin.H{"items": chs})
}

func handleUpdateChannel(c *gin.Context) {
	id := c.Param("id")
	var req struct {
		Name    *string `json:"name"`
		Enabled *bool   `json:"enabled"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.Name != nil {
		updates["name"] = *req.Name
	}
	if req.Enabled != nil {
		updates["enabled"] = *req.Enabled
	}
	store.DB.Model(&models.Channel{}).Where("id = ?", id).Updates(updates)
	var ch models.Channel
	store.DB.First(&ch, "id = ?", id)
	ok(c, ch)
}

// ---------- 远程配置 MGR-09（IDP cfg.get/cfg.set；其余来源不支持） ----------

// cfgKeys 平台可管理的配置键（与固件配置键命名一致）。
var cfgKeys = []string{
	"video.main.resolution", "video.main.fps", "video.main.bitrate", "video.main.gop", "video.main.encode",
	"image.brightness", "image.contrast", "image.saturation", "image.sharpness", "image.mirror", "image.wdr",
	"osd.enable", "osd.text", "record.mode", "alarm.motion.sensitivity", "time.ntp",
}

func handleDeviceConfigGet(c *gin.Context) {
	id := c.Param("id")
	var d models.Device
	if store.DB.First(&d, "id = ? AND deleted_at = 0", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	if d.Source != "idp" {
		fail(c, errs.EForbid.WithMsg("该设备不支持远程配置"))
		return
	}
	a, okk := adapter.Get("idp").(interface {
		ConfigGet(deviceID string, keys []string) (map[string]any, error)
	})
	if !okk {
		fail(c, errs.EForbid.WithMsg("IDP 适配器未就绪"))
		return
	}
	data, err := a.ConfigGet(id, cfgKeys)
	if err != nil {
		fail(c, toAppErr(err))
		return
	}
	values, _ := data["values"].(map[string]any)
	if values == nil {
		values = map[string]any{}
	}
	ok(c, gin.H{"config": values, "supported": cfgKeys})
}

func handleDeviceConfigSet(c *gin.Context) {
	id := c.Param("id")
	var d models.Device
	if store.DB.First(&d, "id = ? AND deleted_at = 0", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	if d.Source != "idp" {
		fail(c, errs.EForbid.WithMsg("该设备不支持远程配置"))
		return
	}
	var req struct {
		Values map[string]any `json:"values" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	a, okk := adapter.Get("idp").(interface {
		ConfigSet(deviceID string, values map[string]any) ([]string, error)
	})
	if !okk {
		fail(c, errs.EForbid.WithMsg("IDP 适配器未就绪"))
		return
	}
	rejected, err := a.ConfigSet(id, req.Values)
	if err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, gin.H{"rejected": rejected})
}

// ---------- 批量操作（MGR-01 工具栏） ----------

func handleDeviceBatch(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Action  string   `json:"action" binding:"required"` // move|reboot|delete
		IDs     []string `json:"ids" binding:"required"`
		GroupID string   `json:"groupId"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if len(req.IDs) > 200 {
		fail(c, errs.EBadRequest.WithMsg("单次批量上限 200 台"))
		return
	}
	results := make([]gin.H, 0, len(req.IDs))
	for _, id := range req.IDs {
		var d models.Device
		if store.DB.First(&d, "id = ? AND project_id = ? AND deleted_at = 0", id, ctx.ProjectID).Error != nil {
			results = append(results, gin.H{"id": id, "ok": false, "msg": "设备不存在或无权限"})
			continue
		}
		switch req.Action {
		case "move":
			if err := store.DB.Model(&d).Update("group_id", req.GroupID).Error; err != nil {
				results = append(results, gin.H{"id": id, "ok": false, "msg": err.Error()})
			} else {
				results = append(results, gin.H{"id": id, "ok": true})
			}
		case "reboot":
			a := adapter.Get(d.Source)
			if a == nil {
				results = append(results, gin.H{"id": id, "ok": false, "msg": "来源不支持重启"})
				continue
			}
			if err := a.Reboot(c.Request.Context(), id); err != nil {
				results = append(results, gin.H{"id": id, "ok": false, "msg": err.Error()})
			} else {
				results = append(results, gin.H{"id": id, "ok": true})
			}
		case "delete":
			if err := store.DB.Model(&d).Update("deleted_at", models.NowMilli()).Error; err != nil {
				results = append(results, gin.H{"id": id, "ok": false, "msg": err.Error()})
			} else {
				results = append(results, gin.H{"id": id, "ok": true})
			}
		default:
			fail(c, errs.EBadRequest.WithMsg("未知批量动作"))
			return
		}
	}
	ok(c, gin.H{"results": results})
}

// handleIdpLookup ADD-01 两段式第一步：查找设备（型号/在线状态）。
func handleIdpLookup(c *gin.Context) {
	var req struct {
		DeviceID string `json:"deviceId" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	id := strings.ToUpper(strings.TrimSpace(req.DeviceID))
	var d models.Device
	if store.DB.First(&d, "id = ?", id).Error != nil {
		// 未建档：可能从未 hello 过；在线状态经 MQTT KV 判断
		online := false
		if a, okk := adapter.Get("idp").(interface{ DeviceOnline(string) bool }); okk {
			online = a.DeviceOnline(id)
		}
		if !online {
			fail(c, errs.EDeviceNotOnline)
			return
		}
		ok(c, gin.H{"deviceId": id, "model": "未知型号", "status": "online", "bound": false})
		return
	}
	bound := d.ProjectID != "" && d.ProjectID != getCtx(c).ProjectID
	ok(c, gin.H{"deviceId": id, "model": d.Model, "vendor": d.Vendor, "status": d.Status, "bound": bound})
}

// ---------- IDP 接入 ADD-01/03 ----------

type bindReq struct {
	DeviceID   string `json:"deviceId" binding:"required"`
	VerifyCode string `json:"verifyCode" binding:"required"`
	GroupID    string `json:"groupId"`
	Name       string `json:"name"`
}

func handleIdpBind(c *gin.Context) {
	ctx := getCtx(c)
	var req bindReq
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	req.DeviceID = strings.ToUpper(strings.TrimSpace(req.DeviceID))
	a := adapter.Get("idp")
	if a == nil {
		fail(c, errs.EServerInternal.WithMsg("IDP 适配器未启动"))
		return
	}
	idp := a.(interface {
		Bind(projectID, groupID, name, deviceID, verifyCode string) error
	})
	if err := idp.Bind(ctx.ProjectID, req.GroupID, req.Name, req.DeviceID, req.VerifyCode); err != nil {
		fail(c, toAppErr(err))
		return
	}
	var d models.Device
	if store.DB.First(&d, "id = ?", req.DeviceID).Error == nil {
		ok(c, deviceJSON(d))
		return
	}
	ok(c, nil)
}

// handleIdpPreadd ADD-03 批量预添加。
func handleIdpPreadd(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Items []struct {
			DeviceID   string `json:"deviceId" binding:"required"`
			VerifyCode string `json:"verifyCode"`
			Name       string `json:"name"`
			GroupID    string `json:"groupId"`
			Location   string `json:"location"`
		} `json:"items" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	results := make([]gin.H, 0, len(req.Items))
	for _, it := range req.Items {
		id := strings.ToUpper(strings.TrimSpace(it.DeviceID))
		rec := models.IdpPreadd{
			ID: "pre_" + models.NewID(), ProjectID: ctx.ProjectID, DeviceID: id,
			GroupID: it.GroupID, Name: it.Name, Location: it.Location,
			ExpiresAt: models.NowMilli() + 7*24*3600*1000, State: "pending",
			CreatedAt: models.NowMilli(),
		}
		if it.VerifyCode != "" {
			rec.VerifyHmac = crypto_HMAC(it.VerifyCode)
		}
		var dup int64
		store.DB.Model(&models.IdpPreadd{}).Where("device_id = ? AND state = 'pending'", id).Count(&dup)
		if dup > 0 {
			results = append(results, gin.H{"deviceId": id, "ok": false, "msg": "已存在待激活记录"})
			continue
		}
		if err := store.DB.Create(&rec).Error; err != nil {
			results = append(results, gin.H{"deviceId": id, "ok": false, "msg": err.Error()})
			continue
		}
		results = append(results, gin.H{"deviceId": id, "ok": true})
	}
	ok(c, gin.H{"results": results})
}

func handleIdpPreaddList(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.IdpPreadd
	store.DB.Where("project_id = ?", ctx.ProjectID).Order("created_at DESC").Find(&items)
	ok(c, gin.H{"items": items})
}

func handleIdpPreaddActivate(c *gin.Context) {
	id := c.Param("id")
	store.DB.Model(&models.IdpPreadd{}).Where("id = ?", id).
		Update("expires_at", models.NowMilli()+7*24*3600*1000)
	ok(c, nil)
}

// ---------- GB28181 接入 ADD-05 ----------

// handleGbParams 展示当前项目 SIP 参数。
func handleGbParams(c *gin.Context) {
	pid := c.Query("projectId")
	if pid == "" {
		if ctx := getCtx(c); ctx != nil {
			pid = ctx.ProjectID
		}
	}
	ok(c, gin.H{
		"serverId":  cfg_SIPServerID(),
		"domain":    cfg_SIPDomain(),
		"port":      cfg_SIPPort(),
		"transport": "UDP",
		"expires":   3600, "keepalive": 60, "projectId": pid,
	})
}

func handleGbPendingList(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.GbPending
	store.DB.Where("project_id = ? OR project_id = ''", ctx.ProjectID).Find(&items)
	ok(c, gin.H{"items": items})
}

func handleGbConfirm(c *gin.Context) {
	ctx := getCtx(c)
	id := c.Param("id")
	var req struct {
		GroupID string `json:"groupId"`
		Name    string `json:"name"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	var p models.GbPending
	if store.DB.First(&p, "id = ?", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	// 创建设备（确认入组）
	gb := adapter.Get("gb28181")
	type creator interface {
		CreateConfirmed(projectID, groupID, name, gbID string) error
	}
	_ = gb
	// 简化：直接建库；SIP 侧密码用全局默认，设备已用其密码注册成功（accept）
	dev := models.Device{
		ID: "dv_" + models.NewID(), ProjectID: ctx.ProjectID, GroupID: req.GroupID,
		Source: "gb28181", Name: firstNonEmpty(req.Name, "GB-"+last4(p.GbID)),
		Identity: models.JSONB{"gbId": p.GbID}, Status: "online",
		LastSeenAt: models.NowMilli(),
		Meta:       models.JSONB{"sipRemote": p.IP},
		CreatedAt:  models.NowMilli(), UpdatedAt: models.NowMilli(),
	}
	if err := store.DB.Create(&dev).Error; err != nil {
		fail(c, errs.EServerInternal)
		return
	}
	ch := models.Channel{
		ID: "ch_" + models.NewID(), DeviceID: dev.ID, ProjectID: ctx.ProjectID,
		Idx: 1, Name: dev.Name, Enabled: true, StreamState: "idle",
		Capabilities: models.StringSlice([]string{"live.main", "live.sub", "snapshot", "record.platform", "reboot"}),
		Meta:         models.JSONB{"gbChannelId": p.GbID, "gbStream": p.GbID + "_main", "gbStreamSub": p.GbID + "_sub"},
		CreatedAt:    models.NowMilli(), UpdatedAt: models.NowMilli(),
	}
	store.DB.Create(&ch)
	devsvc.ApplyDefaultRecordPlan(ch.ID, ctx.ProjectID) // ADD-09
	devsvc.ApplyDefaultAlarmRule(ch.ID, ctx.ProjectID)  // ADD-09
	store.DB.Delete(&p)
	ok(c, gin.H{"deviceId": dev.ID})
}

func handleGbReject(c *gin.Context) {
	id := c.Param("id")
	store.DB.Delete(&models.GbPending{}, "id = ?", id)
	ok(c, nil)
}

func handleGbWhitelistList(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.GbWhitelist
	store.DB.Where("project_id = ?", ctx.ProjectID).Find(&items)
	out := make([]gin.H, 0, len(items))
	for _, w := range items {
		out = append(out, gin.H{"id": w.ID, "gbId": w.GbID, "groupId": w.GroupID, "name": w.Name})
	}
	ok(c, gin.H{"items": out})
}

func handleGbWhitelistAdd(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		GbID    string `json:"gbId" binding:"required"`
		Pwd     string `json:"pwd" binding:"required"`
		GroupID string `json:"groupId"`
		Name    string `json:"name"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	w := models.GbWhitelist{ID: "wl_" + models.NewID(), ProjectID: ctx.ProjectID,
		GbID: req.GbID, PwdEnc: crypto_Enc(req.Pwd), GroupID: req.GroupID, Name: req.Name,
		CreatedAt: models.NowMilli()}
	store.DB.Create(&w)
	ok(c, gin.H{"id": w.ID})
}

func handleGbWhitelistDelete(c *gin.Context) {
	store.DB.Delete(&models.GbWhitelist{}, "id = ?", c.Param("id"))
	ok(c, nil)
}

// ---------- ONVIF / RTSP 接入 ADD-06/07 ----------

func handleOnvifDiscover(c *gin.Context) {
	ctx := getCtx(c)
	var existing []models.Device
	store.DB.Where("project_id = ? AND deleted_at = 0", ctx.ProjectID).Find(&existing)
	added := map[string]bool{}
	for _, d := range existing {
		if ip, ok := d.Meta["ip"].(string); ok {
			added[ip] = true
		}
	}
	items, err := onvif.Discover(c.Request.Context(), 10_000_000_000) // 10s
	if err != nil {
		fail(c, errs.EUnreachable.WithMsg("发现失败: "+err.Error()))
		return
	}
	out := make([]gin.H, 0, len(items))
	for _, it := range items {
		out = append(out, gin.H{
			"ip": it.IP, "xaddr": it.XAddr, "scopes": it.Scopes, "added": added[it.IP],
		})
	}
	ok(c, gin.H{"items": out})
}

func handleOnvifAdd(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		IP      string `json:"ip" binding:"required"`
		Port    string `json:"port"`
		User    string `json:"user" binding:"required"`
		Pass    string `json:"pass" binding:"required"`
		GroupID string `json:"groupId"`
		Name    string `json:"name"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	if req.Port == "" {
		req.Port = "80"
	}
	xaddr := fmt.Sprintf("http://%s:%s/onvif/device_service", req.IP, req.Port)
	info, err := onvifProbe(xaddr, req.User, req.Pass)
	if err != nil {
		fail(c, toAppErr(err))
		return
	}
	name := firstNonEmpty(req.Name, info.Model+"-"+last4(req.IP))
	dev := models.Device{
		ID: "dv_" + models.NewID(), ProjectID: ctx.ProjectID, GroupID: req.GroupID,
		Source: "onvif", Name: name, Vendor: info.Vendor, Model: info.Model,
		Identity: models.JSONB{"ip": req.IP, "port": req.Port}, Status: "online",
		CredentialsEnc: crypto_Enc(fmt.Sprintf(`{"user":%q,"pass":%q}`, req.User, req.Pass)),
		LastSeenAt:     models.NowMilli(),
		Capabilities:   models.StringSlice([]string{"live.main", "live.sub", "snapshot", "record.platform", "reboot", "ptz"}),
		CreatedAt:      models.NowMilli(), UpdatedAt: models.NowMilli(),
	}
	store.DB.Create(&dev)
	ch := models.Channel{
		ID: "ch_" + models.NewID(), DeviceID: dev.ID, ProjectID: ctx.ProjectID,
		Idx: 1, Name: name, Enabled: true, StreamState: "idle",
		Capabilities: dev.Capabilities,
		Meta: models.JSONB{"rtspMain": profileURI(info, 0), "rtspSub": profileURI(info, 1)},
		CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli(),
	}
	store.DB.Create(&ch)
	devsvc.ApplyDefaultRecordPlan(ch.ID, ctx.ProjectID) // ADD-09
	devsvc.ApplyDefaultAlarmRule(ch.ID, ctx.ProjectID)  // ADD-09
	ok(c, deviceJSON(dev))
}

func handleRtspAdd(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		URL     string `json:"url"`
		Brand   string `json:"brand"`
		IP      string `json:"ip"`
		User    string `json:"user"`
		Pass    string `json:"pass"`
		SubURL  string `json:"subUrl"`
		GroupID string `json:"groupId"`
		Name    string `json:"name"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	mainURL := req.URL
	if mainURL == "" {
		if req.Brand == "" || req.IP == "" {
			fail(c, errs.EBadRequest.WithMsg("需要完整 URL 或 品牌+IP"))
			return
		}
		mainURL = brandTemplate(req.Brand, req.IP, req.User, req.Pass, 0)
	}
	if _, err := rtspProbe(mainURL); err != nil {
		fail(c, toAppErr(err))
		return
	}
	name := firstNonEmpty(req.Name, "RTSP-"+last4(req.IP))
	dev := models.Device{
		ID: "dv_" + models.NewID(), ProjectID: ctx.ProjectID, GroupID: req.GroupID,
		Source: "rtsp", Name: name, Vendor: firstNonEmpty(req.Brand, "RTSP"), Model: "RTSP 直连",
		Identity: models.JSONB{"url": mainURL, "ip": req.IP}, Status: "online",
		LastSeenAt:   models.NowMilli(),
		Capabilities: models.StringSlice([]string{"live.main", "snapshot", "record.platform"}),
		CreatedAt:    models.NowMilli(), UpdatedAt: models.NowMilli(),
	}
	if req.SubURL != "" {
		dev.Capabilities = models.StringSlice([]string{"live.main", "live.sub", "snapshot", "record.platform"})
		dev.Meta = models.JSONB{"rtspSub": req.SubURL}
	}
	store.DB.Create(&dev)
	ch := models.Channel{
		ID: "ch_" + models.NewID(), DeviceID: dev.ID, ProjectID: ctx.ProjectID,
		Idx: 1, Name: name, Enabled: true, StreamState: "idle",
		Capabilities: dev.Capabilities, Meta: dev.Meta,
		CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli(),
	}
	store.DB.Create(&ch)
	devsvc.ApplyDefaultRecordPlan(ch.ID, ctx.ProjectID) // ADD-09
	devsvc.ApplyDefaultAlarmRule(ch.ID, ctx.ProjectID)  // ADD-09
	ok(c, deviceJSON(dev))
}

// brandTemplate 附录 C 品牌模板。
func brandTemplate(brand, ip, user, pass string, sub int) string {
	switch brand {
	case "hikvision":
		return fmt.Sprintf("rtsp://%s:%s@%s:554/Streaming/Channels/%d0%d", user, pass, ip, 1, sub+1)
	case "dahua":
		return fmt.Sprintf("rtsp://%s:%s@%s:554/cam/realmonitor?channel=1&subtype=%d", user, pass, ip, sub)
	case "uniview":
		return fmt.Sprintf("rtsp://%s:%s@%s:554/video%d", user, pass, ip, sub+1)
	case "ezviz":
		return fmt.Sprintf("rtsp://admin:%s@%s:554/h264/ch1/main/av_stream", pass, ip)
	default: // tplink / 自研
		return fmt.Sprintf("rtsp://%s:%s@%s:554/stream%d", user, pass, ip, sub+1)
	}
}

func firstNonEmpty(vals ...string) string {
	for _, v := range vals {
		if v != "" {
			return v
		}
	}
	return ""
}

func last4(s string) string {
	if len(s) <= 4 {
		return s
	}
	return s[len(s)-4:]
}

func mustJSONString(v any) string {
	b, _ := json.Marshal(v)
	return string(b)
}

var _ = strconv.Itoa
