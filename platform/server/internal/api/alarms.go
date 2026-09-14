package api

import (
	"encoding/json"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/gin-gonic/gin"
	"gorm.io/gorm"

	"github.com/jetscam/ipccloud/server/internal/engine"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// ---------- 布防模板 ALM-01 ----------

func handleListAlarmTemplates(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.AlarmTemplate
	store.DB.Where("project_id = ?", ctx.ProjectID).Find(&items)
	ok(c, gin.H{"items": items})
}

func handleCreateAlarmTemplate(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		Name     string       `json:"name" binding:"required"`
		Schedule models.JSONB `json:"schedule" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	t := models.AlarmTemplate{ID: "at_" + models.NewID(), ProjectID: ctx.ProjectID,
		Name: req.Name, Schedule: req.Schedule, CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
	store.DB.Create(&t)
	ok(c, t)
}

// handleUpdateAlarmTemplate ALM-01：修改布防模板（内置模板不可改；级联提示由前端负责）。
func handleUpdateAlarmTemplate(c *gin.Context) {
	// 归属校验：模板必须属于当前项目（requirePerm 只看当前项目的权限位，不校验实体归属）
	t, okt := alarmTemplateInProject(c, c.Param("id"))
	if !okt {
		return
	}
	if t.Builtin {
		fail(c, errs.EForbid.WithMsg("内置模板不可修改"))
		return
	}
	var req struct {
		Name     string       `json:"name"`
		Schedule models.JSONB `json:"schedule"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.Name != "" {
		updates["name"] = req.Name
	}
	if req.Schedule != nil {
		updates["schedule"] = req.Schedule
	}
	store.DB.Model(t).Updates(updates)
	store.DB.First(t, "id = ?", t.ID)
	ok(c, t)
}

func handleDeleteAlarmTemplate(c *gin.Context) {
	t, okt := alarmTemplateInProject(c, c.Param("id"))
	if !okt {
		return
	}
	if t.Builtin {
		fail(c, errs.EForbid.WithMsg("内置模板不可删除"))
		return
	}
	var used int64
	store.DB.Model(&models.AlarmRule{}).Where("template_id = ?", t.ID).Count(&used)
	if used > 0 {
		fail(c, errs.EBadRequest.WithMsg("模板已被告警规则使用"))
		return
	}
	store.DB.Delete(t)
	ok(c, nil)
}

// ---------- 告警规则 ALM-02/03 ----------

func handleListAlarmRules(c *gin.Context) {
	ctx := getCtx(c)
	var items []models.AlarmRule
	store.DB.Where("project_id = ?", ctx.ProjectID).Find(&items)
	ok(c, gin.H{"items": items})
}

func handleCreateAlarmRule(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		ChannelIDs []string `json:"channelIds" binding:"required"`
		Kinds      []string `json:"kinds" binding:"required"`
		TemplateID string   `json:"templateId"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	created := []string{}
	for _, cid := range req.ChannelIDs {
		r := models.AlarmRule{ID: "ar_" + models.NewID(), ProjectID: ctx.ProjectID,
			ChannelID: cid, Kinds: models.StringSlice(req.Kinds), TemplateID: req.TemplateID,
			Enabled: true, CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
		if err := store.DB.Create(&r).Error; err == nil {
			created = append(created, r.ID)
		}
	}
	ok(c, gin.H{"ids": created})
}

func handleUpdateAlarmRule(c *gin.Context) {
	ctx := getCtx(c)
	if _, okr := alarmRuleInProject(c, c.Param("id")); !okr {
		return
	}
	id := c.Param("id")
	var req struct {
		Kinds      []string `json:"kinds"`
		TemplateID string   `json:"templateId"`
		Enabled    *bool    `json:"enabled"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	// 换绑的模板也必须属于当前项目，否则规则会指向别处的模板
	if req.TemplateID != "" {
		if _, okt := alarmTemplateInProject(c, req.TemplateID); !okt {
			return
		}
	}
	updates := map[string]any{"updated_at": models.NowMilli()}
	if req.Kinds != nil {
		updates["kinds"] = models.StringSlice(req.Kinds)
	}
	if req.TemplateID != "" {
		updates["template_id"] = req.TemplateID
	}
	if req.Enabled != nil {
		updates["enabled"] = *req.Enabled
	}
	store.DB.Model(&models.AlarmRule{}).Where("id = ? AND project_id = ?", id, ctx.ProjectID).Updates(updates)
	ok(c, nil)
}

func handleDeleteAlarmRule(c *gin.Context) {
	ctx := getCtx(c)
	if _, okr := alarmRuleInProject(c, c.Param("id")); !okr {
		return
	}
	store.DB.Delete(&models.AlarmRule{}, "id = ? AND project_id = ?", c.Param("id"), ctx.ProjectID)
	ok(c, nil)
}

// ---------- 告警策略 ALM-04/05 ----------

func handleGetAlarmPolicies(c *gin.Context) {
	ctx := getCtx(c)
	var st models.Setting
	if err := store.DB.First(&st, "scope = ? AND key = 'alarm.policies'", ctx.ProjectID).Error; err != nil {
		// 默认全开
		def := models.JSONB{}
		for _, k := range []string{"device_offline", "stream_lost", "record_fail", "disk_full",
			"auth_fail", "node_offline", "motion", "humanoid", "intrusion", "linecross", "tamper", "io"} {
			def[k] = map[string]any{"enabled": true, "web": true, "ring": false}
		}
		ok(c, def)
		return
	}
	ok(c, st.Value)
}

func handleSetAlarmPolicies(c *gin.Context) {
	ctx := getCtx(c)
	var req models.JSONB
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	var st models.Setting
	store.DB.Where("scope = ? AND key = 'alarm.policies'", ctx.ProjectID).Assign(models.Setting{
		Scope: ctx.ProjectID, Key: "alarm.policies", Value: req,
	}).FirstOrCreate(&st)
	store.DB.Model(&st).Update("value", req)
	ok(c, req)
}

// ---------- 消息中心 ALM-06/07 ----------

// alarmQuery 消息中心的公共筛选条件（列表 / 已读计数 / 删除已读 共用一份）。
//
// 必须同源：确认框里"将删除 N 条"的 N 由列表接口按这组条件统计，用户点确认后按同一组
// 条件删除。若各写一份，口径漂移会直接变成"说好删 3 条、实际删了 30 条"。
// 注意 scope 用 kind NOT IN 而不是平台侧白名单——平台侧类型会随功能增加，白名单必漏。
func alarmQuery(projectID, scope, kind, level, read string, fromTs int64) *gorm.DB {
	q := store.DB.Model(&models.AlarmEvent{}).Where("project_id = ?", projectID)
	if kind != "" {
		q = q.Where("kind = ?", kind)
	}
	switch scope {
	case "device":
		q = q.Where("kind IN ?", models.DeviceSideAlarmKinds)
	case "platform":
		q = q.Where("kind NOT IN ?", models.DeviceSideAlarmKinds)
	}
	if level != "" {
		q = q.Where("level = ?", level)
	}
	if read != "" {
		q = q.Where("read = ?", read == "true")
	}
	// 时间范围由服务端过滤：此前只在前端裁当前页，会出现"选了近 1 小时、总数与翻页仍是全量"
	// 的错位，而且"删除已读"的范围无法与显示口径对齐。
	if fromTs > 0 {
		q = q.Where("ts >= ?", fromTs)
	}
	return q
}

func handleListAlarms(c *gin.Context) {
	ctx := getCtx(c)
	page, size := pageParams(c)
	// 同一套筛选同时作用于「总数 / 列表 / focus 定位页码 / 已读计数」。
	// 前端按 tab 或时间过滤时必须走这里，否则 total/分页与实际列表不一致（翻页出现空页）。
	filter := func() *gorm.DB {
		fromTs, _ := strconv.ParseInt(c.Query("fromTs"), 10, 64)
		return alarmQuery(ctx.ProjectID, c.Query("scope"), c.Query("kind"),
			c.Query("level"), c.Query("read"), fromTs)
	}

	var total int64
	filter().Count(&total)
	var items []models.AlarmEvent
	filter().Order("ts DESC").Offset((page - 1) * size).Limit(size).Find(&items)
	var unread int64
	store.DB.Model(&models.AlarmEvent{}).Where("project_id = ? AND read = ?", ctx.ProjectID, false).Count(&unread)
	// readTotal：当前筛选下的已读条数，供前端「删除已读」显示待删条数与按钮可用性。
	// 勾选「仅看未读」时筛选里已含 read=false，此处必然为 0 —— 即该视图下没有可删的已读。
	var readTotal int64
	filter().Where("read = ?", true).Count(&readTotal)

	// focus=<id>：深链定位（总览页「最近告警」点击）。返回该条在同一筛选条件、同一排序
	// 下的页码，前端据此跳到对应页并高亮，避免为定位一条记录把全量列表拉回来。
	focusPage := 0
	if fid := c.Query("focus"); fid != "" {
		var ev models.AlarmEvent
		if store.DB.First(&ev, "id = ? AND project_id = ?", fid, ctx.ProjectID).Error == nil {
			var newer int64
			filter().Where("(ts > ? OR (ts = ? AND id > ?))", ev.Ts, ev.Ts, ev.ID).Count(&newer)
			focusPage = int(newer)/size + 1
		}
	}
	ok(c, gin.H{"total": total, "unread": unread, "readTotal": readTotal,
		"items": items, "focusPage": focusPage})
}

// handleDeleteAlarms 清理消息中心：删除所选 / 一键删除已读（ALM-06）。
//
// 一个端点覆盖两种范围，与设备批量操作同构（单台改密也是 ids 只有一个元素的同一请求）：
//   - ids 非空：删除勾选的记录。**允许含未读**——勾选是用户的显式意图，前端会在确认框里
//     提示其中未读的条数；若在此偷偷跳过未读，用户看到"删了但还在"只会更困惑。
//   - allRead：删除当前筛选条件下的全部已读记录（跨全部页，不受分页限制）。
//
// 快照文件随之级联清理（见 removeAlarmSnapshotFiles）：光删记录不删图，磁盘只增不减。
func handleDeleteAlarms(c *gin.Context) {
	ctx := getCtx(c)
	var req struct {
		IDs     []string `json:"ids"`
		AllRead bool     `json:"allRead"`
		Scope   string   `json:"scope"`
		Kind    string   `json:"kind"`
		Level   string   `json:"level"`
		FromTs  int64    `json:"fromTs"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	// 每次调用重建查询：删除前要用同一组条件先把快照 URL 捞出来，
	// 共用一个 *gorm.DB 会在 Pluck 之后带上 SELECT 状态。
	var buildQuery func() *gorm.DB
	switch {
	case len(req.IDs) > 0:
		if len(req.IDs) > 200 {
			fail(c, errs.EBadRequest.WithMsg("单次最多删除 200 条"))
			return
		}
		buildQuery = func() *gorm.DB {
			return store.DB.Model(&models.AlarmEvent{}).
				Where("project_id = ? AND id IN ?", ctx.ProjectID, req.IDs)
		}
	case req.AllRead:
		buildQuery = func() *gorm.DB {
			return alarmQuery(ctx.ProjectID, req.Scope, req.Kind, req.Level, "", req.FromTs).
				Where("read = ?", true)
		}
	default:
		fail(c, errs.EBadRequest.WithMsg("未指定删除范围"))
		return
	}
	// 先取快照 URL：记录删掉之后就无从知道这批曾经引用过哪些图了。
	// coalesce 是必要的：AutoMigrate 给存量行补的列是 NULL 而不是 ''。
	var snapURLs []string
	buildQuery().Where("coalesce(snapshot_url,'') <> ''").Pluck("snapshot_url", &snapURLs)

	// 带 WHERE 的批量硬删除（AlarmEvent 无软删除字段）：消息中心清理就是要真的清掉，
	// 否则"已读清理"形同虚设——记录还在表里，只是换个地方堆着。
	res := buildQuery().Delete(&models.AlarmEvent{})
	if res.Error != nil {
		fail(c, errs.EServerInternal)
		return
	}
	filesRemoved, filesKept := removeAlarmSnapshotFiles(snapURLs)
	ok(c, gin.H{"deleted": res.RowsAffected, "filesRemoved": filesRemoved, "filesKept": filesKept})
}

// removeAlarmSnapshotFiles 级联清理快照文件，返回 (已删, 因仍被引用/删不掉而保留)。
//
// **不能见 URL 就删**：同一个文件可能仍被别人引用——
//  1. 通道封面：IDP 抓图返回的 URL 会被 engine.Snapshot 同时写进 channels.cover_url，
//     实测存在 cover_url == 某条告警 snapshot_url 的情况，直接删会把封面打成破图；
//  2. 其它存活告警：同一张图被多条记录引用并不罕见（比如同一通道连续告警共用了一次抓图），
//     删"所选"里的其中一条时另一条还在。
//
// 因此以"删除动作完成之后"的两条查询为准（存活的告警引用 + 通道封面引用），命中即保留。
func removeAlarmSnapshotFiles(urls []string) (removed, kept int) {
	names := make([]string, 0, len(urls))
	seen := map[string]bool{}
	for _, u := range urls {
		name := strings.TrimPrefix(u, "/api/v1/static/")
		// 只处理本站静态目录、且命中我们自己的两种命名；路径分隔符一律拒绝，
		// 避免任何形式的上跳（文件名由服务端生成，但 DB 值不可当作可信输入）。
		if name == u || seen[name] || name == "" || !strings.HasSuffix(name, ".jpg") {
			continue
		}
		if strings.ContainsAny(name, "/\\") {
			continue
		}
		if !strings.HasPrefix(name, "snap_") && !strings.Contains(name, "_cover_") {
			continue
		}
		seen[name] = true
		names = append(names, name)
	}
	if len(names) == 0 {
		return 0, 0
	}
	// 一次请求最多删这么多文件："清空全部已读"可能带上万条记录，同步删文件会拖死请求
	// （且客户端一直转圈）。超出部分本轮不处理，只记日志。
	const maxFilesPerRequest = 1000
	if len(names) > maxFilesPerRequest {
		log.Printf("[alarm] 本次待删快照 %d 个，超过单次上限 %d，本轮只清理前 %d 个",
			len(names), maxFilesPerRequest, maxFilesPerRequest)
		names = names[:maxFilesPerRequest]
	}
	urls = urls[:0]
	for _, n := range names {
		urls = append(urls, "/api/v1/static/"+n)
	}
	stillUsed := map[string]bool{}
	var rows []string
	store.DB.Model(&models.AlarmEvent{}).Where("snapshot_url IN ?", urls).Pluck("snapshot_url", &rows)
	for _, u := range rows {
		stillUsed[u] = true
	}
	rows = nil
	store.DB.Model(&models.Channel{}).Where("cover_url IN ?", urls).Pluck("cover_url", &rows)
	for _, u := range rows {
		stillUsed[u] = true
	}

	for _, u := range urls {
		if stillUsed[u] {
			kept++
			continue
		}
		// 文件已被手工删除/权限不足都归入保留：告警记录已删成功，不应因清理失败向用户报错。
		if err := os.Remove(filepath.Join(appCfg.DataDir, filepath.Base(u))); err != nil {
			kept++
			continue
		}
		removed++
	}
	return removed, kept
}

func handleReadAlarm(c *gin.Context) {
	ctx := getCtx(c)
	if _, oke := alarmEventInProject(c, c.Param("id")); !oke {
		return
	}
	store.DB.Model(&models.AlarmEvent{}).Where("id = ? AND project_id = ?", c.Param("id"), ctx.ProjectID).Update("read", true)
	ok(c, nil)
}

func handleReadAllAlarms(c *gin.Context) {
	ctx := getCtx(c)
	store.DB.Model(&models.AlarmEvent{}).Where("project_id = ? AND read = ?", ctx.ProjectID, false).Update("read", true)
	ok(c, nil)
}

func handleAlarmDetail(c *gin.Context) {
	ev, oke := alarmEventInProject(c, c.Param("id"))
	if !oke {
		return
	}
	resp := map[string]any{}
	b, _ := json.Marshal(ev)
	_ = json.Unmarshal(b, &resp)
	var dev models.Device
	if store.DB.First(&dev, "id = ?", ev.DeviceID).Error == nil {
		resp["device"] = gin.H{"id": dev.ID, "name": dev.Name, "model": dev.Model, "source": dev.Source}
	}
	var rules []models.AlarmRule
	store.DB.Where("channel_id = ?", ev.ChannelID).Find(&rules)
	resp["rules"] = rules
	ok(c, resp)
}

var _ = engine.CreateAlarmEvent
