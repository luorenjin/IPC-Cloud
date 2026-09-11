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
	// ids 精确筛选优先于其他条件：调用方已明确要哪几台
	if ids := c.Query("ids"); ids != "" {
		return q.Where("id IN ?", splitComma(ids))
	}
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
	// 类型状态卡片（项目全量口径，与筛选/分页无关）。
	// 键名必须显式小写：匿名结构体没有 json tag 时，编码器直接输出 Go 字段名
	// （All/Online/Offline），前端读 stats.all 全取不到 → 静默回退 0，
	// 表现为「全部分组 (0)」「0 台 / 离线 0」而分组里明明有设备。
	var stats struct {
		All     int64 `json:"all"`
		Online  int64 `json:"online"`
		Offline int64 `json:"offline"`
	}
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
	// 最近一次诊断结果（MGR-07）：概览的「最近诊断结果」段与诊断 Tab 都读它。
	// 放在详情响应里而不是单开 GET /devices/:id/diag——详情页本来就要拉一次详情，
	// 前端不用为一个可选的展示项多打一次往返。
	if ld := recentDiag(d); ld != nil {
		resp["lastDiag"] = ld
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
	at := models.NowMilli()
	// 落地「最近一次」诊断结果。以前只回给这一次请求，刷新页面概览就回到「尚未诊断」，
	// 而 PRD MGR-07 要求诊断记录保留 7 天。存 meta.lastDiag（与 meta.metrics 同套路，不建新表），
	// 注意 meta 在响应里是白名单投影（safeMeta），不会因此泄到列表接口。
	if d.Meta == nil {
		d.Meta = models.JSONB{}
	}
	d.Meta["lastDiag"] = models.JSONB{"at": at, "results": results}
	_ = store.DB.Model(&d).Update("meta", d.Meta).Error
	ok(c, gin.H{"results": results, "at": at})
}

// 诊断记录保留窗口（PRD MGR-07：诊断记录保留 7 天）。
const diagRetentionMs = 7 * 24 * 60 * 60 * 1000

// recentDiag 取设备最近一次诊断结果；无记录或已超出保留窗口返回 nil。
// 过期不下发（而不是下发一个 8 天前的结果），前端于是回到「尚未诊断」，语义与「记录已过期」一致。
func recentDiag(d models.Device) models.JSONB {
	ld, _ := d.Meta["lastDiag"].(map[string]any)
	if ld == nil {
		return nil
	}
	// meta 经 JSONB 往返后数字是 float64
	at, _ := ld["at"].(float64)
	if at <= 0 || models.NowMilli()-int64(at) > diagRetentionMs {
		return nil
	}
	return models.JSONB(ld)
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

// handleGetRebootPlan MGR-08 定时重启：读取计划。
// 没配过也返回一份「已关闭」的空计划，前端不必把 404 当成错误处理。
func handleGetRebootPlan(c *gin.Context) {
	id := c.Param("id")
	var d models.Device
	if store.DB.First(&d, "id = ? AND deleted_at = 0", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	var p models.RebootPlan
	if store.DB.First(&p, "device_id = ?", id).Error != nil {
		ok(c, gin.H{"enabled": false, "schedule": emptyRebootSchedule(), "lastFiredKey": ""})
		return
	}
	ok(c, gin.H{"enabled": p.Enabled, "schedule": p.Schedule, "lastFiredKey": p.LastFiredKey})
}

// handleSetRebootPlan MGR-08 定时重启：保存计划（设备与计划一对一，upsert）。
func handleSetRebootPlan(c *gin.Context) {
	id := c.Param("id")
	var d models.Device
	if store.DB.First(&d, "id = ? AND deleted_at = 0", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	var req struct {
		Enabled  bool         `json:"enabled"`
		Schedule models.JSONB `json:"schedule"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	// 不支持重启的来源（RTSP）不允许**启用**定时任务——否则到点必然失败，
	// 每天在日志里刷一条 fail，用户会以为设备坏了。只是关掉计划时不该拦。
	if req.Enabled && !supportsReboot(&d) {
		fail(c, errs.EForbid.WithMsg("该设备不支持远程重启，无法启用定时任务"))
		return
	}
	sch, err := normalizeRebootSchedule(req.Schedule)
	if err != nil {
		fail(c, errs.EBadRequest.WithMsg(err.Error()))
		return
	}
	now := models.NowMilli()
	var existing models.RebootPlan
	if store.DB.First(&existing, "device_id = ?", id).Error == nil {
		err = store.DB.Model(&models.RebootPlan{}).Where("id = ?", existing.ID).Updates(map[string]any{
			"enabled": req.Enabled, "schedule": sch, "updated_at": now,
			// 改了时间/星期就清掉触发水位：否则把时间改到"当前这一分钟"会被旧水位挡住不触发
			"last_fired_key": "",
		}).Error
	} else {
		err = store.DB.Create(&models.RebootPlan{
			ID: "rb_" + models.NewID(), DeviceID: id,
			Enabled: req.Enabled, Schedule: sch, CreatedAt: now, UpdatedAt: now,
		}).Error
	}
	if err != nil {
		fail(c, errs.EServerInternal)
		return
	}
	ok(c, gin.H{"enabled": req.Enabled, "schedule": sch})
}

func emptyRebootSchedule() models.JSONB {
	return models.JSONB{"days": []any{}, "time": "03:00"}
}

// supportsReboot 设备是否具备远程重启能力（PRD MGR-08「无能力置灰」）。
// RTSP 源没有重启通道（adapter 直接返回 EForbid）；IDP 以固件上报的能力集为准；
// GB28181/ONVIF 分别走 TeleBoot / SystemReboot，默认支持。
func supportsReboot(d *models.Device) bool {
	if d.Source == "rtsp" {
		return false
	}
	if d.Source == "idp" && len(d.Capabilities) > 0 {
		return d.HasCapability("reboot")
	}
	return true
}

// normalizeRebootSchedule 校验并归一化 {"days":[1..7],"time":"HH:MM"}。
// days 为 ISO 星期（1=周一…7=周日），**空列表表示每天**，与录像计划 scheduleMatches 的约定一致。
func normalizeRebootSchedule(in models.JSONB) (models.JSONB, error) {
	hm, _ := in["time"].(string)
	norm, ok := parseHHMM(hm)
	if !ok {
		return nil, fmt.Errorf("重启时间需为 HH:MM（00:00–23:59）")
	}
	days := []any{}
	if raw, ok := in["days"].([]any); ok {
		for _, v := range raw {
			f, ok := v.(float64)
			if !ok || f != float64(int(f)) {
				return nil, fmt.Errorf("星期取值需为整数 1–7")
			}
			if n := int(f); n >= 1 && n <= 7 {
				days = append(days, float64(n))
			} else {
				return nil, fmt.Errorf("星期取值需在 1–7（周一至周日）之间")
			}
		}
	}
	return models.JSONB{"days": days, "time": norm}, nil
}

// parseHHMM 校验 HH:MM 并归一化为两位格式。
func parseHHMM(s string) (string, bool) {
	parts := strings.Split(s, ":")
	if len(parts) != 2 || len(parts[0]) != 2 || len(parts[1]) != 2 {
		return "", false
	}
	h, e1 := strconv.Atoi(parts[0])
	m, e2 := strconv.Atoi(parts[1])
	if e1 != nil || e2 != nil || h < 0 || h > 23 || m < 0 || m > 59 {
		return "", false
	}
	return fmt.Sprintf("%02d:%02d", h, m), true
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

// cfgKeys 平台可远程管理的配置键。
//
// ⚠️ 键名必须与固件 `firmware/core/src/config.c` 的规则表（register_common_rules +
// video pattern）逐字对齐。设备侧是按 pattern 匹配后再校验类型与取值范围的：
// 键名写错不会报错，只会进 cfg.set 的 rejected 列表，平台侧表现为「保存成功但没生效」。
// 编码键带通道号：video.<ch>.<name>.<field>，0/main = 主码流。
var cfgKeys = []string{
	// 画面（画面信息组，0–100 归一化；flip/mirror 为 0/1 开关）
	"image.brightness", "image.contrast", "image.saturation", "image.sharpness", "image.flip", "image.mirror",
	// 编码（主码流）
	"video.0.main.codec", "video.0.main.w", "video.0.main.h", "video.0.main.fps",
	"video.0.main.kbps", "video.0.main.gop", "video.0.main.rc",
	// OSD
	"osd.channelName.enable", "osd.time.enable",
	// 录像
	"record.enabled", "record.mode", "record.retention_days", "record.channel",
	// 移动侦测
	"alarm.motion.enable", "alarm.motion.sensitivity",
	// 时间同步
	"time.ntp.enable", "time.ntp.server", "time.timezone",
	// 网络（reboot_required；本阶段前端仅只读展示，见 [id].vue 的 time 页签，
	// 可编辑的 DHCP 开关/IP 输入框留给带强确认交互的后续任务）
	"net.dhcp", "net.ip",
	// 本地设置（设备维护页签）
	"localUser.name", "led.enable",
}

// cfgRebootRequired 是固件配置规则表里 reboot_required=true 项的静态镜像，
// 不做实时抓取：这个属性在固件侧是编译期常量（cfg_rule_t.reboot_required），
// 没必要为一个不会在运行时变化的标记多打一次设备往返。
// 与 firmware/core/src/config.c 的 video.%d.%s.w/h（:266-267）、net.dhcp/net.ip（:304-305）
// 逐字对齐——这四项是当前固件规则表里*仅有*的 reboot_required 键；
// 固件规则表调整后需要手动同步这里。
var cfgRebootRequired = map[string]bool{
	"video.0.main.w": true,
	"video.0.main.h": true,
	"net.dhcp":       true,
	"net.ip":         true,
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
	// 只收集「值为 true 且落在本次 supported 范围内」的键：既不整个暴露 cfgRebootRequired
	// 这张表本身，也避免未来表扩容后把当前设备/固件版本还不支持的键提前亮给前端。
	rebootRequired := make([]string, 0, len(cfgRebootRequired))
	for _, k := range cfgKeys {
		if cfgRebootRequired[k] {
			rebootRequired = append(rebootRequired, k)
		}
	}
	ok(c, gin.H{"config": values, "supported": cfgKeys, "rebootRequired": rebootRequired})
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

// handleDeviceConfigReset 恢复出厂设置：清除设备本地配置并重启，需输入设备名二次确认
// （镜像 handleDeleteDevice 的确认模式）。
func handleDeviceConfigReset(c *gin.Context) {
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
	if d.Source != "idp" {
		fail(c, errs.EForbid.WithMsg("该设备不支持远程配置"))
		return
	}
	if d.Name != req.ConfirmName {
		fail(c, errs.EBadRequest.WithMsg("设备名不一致"))
		return
	}
	a, okk := adapter.Get("idp").(interface {
		ConfigReset(deviceID string) error
	})
	if !okk {
		fail(c, errs.EForbid.WithMsg("IDP 适配器未就绪"))
		return
	}
	if err := a.ConfigReset(id); err != nil {
		fail(c, toAppErr(err))
		return
	}
	ok(c, nil)
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
		Meta:         models.JSONB{"rtspMain": profileURI(info, 0), "rtspSub": profileURI(info, 1)},
		CreatedAt:    models.NowMilli(), UpdatedAt: models.NowMilli(),
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
