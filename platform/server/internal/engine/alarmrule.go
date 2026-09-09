package engine

import (
	"log"
	"strconv"
	"strings"
	"time"

	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// deviceSideKinds 由 models.DeviceSideAlarmKinds 构建的查找表。
//
// 刻意使用白名单而非"非平台侧即设备侧"的黑名单：接入规范 §312 列出的 IDP 事件还包括
// tf_error / tf_inserted / tf_removed / rebooted / bind_state / stream_limit / ota_state，
// 它们经适配器归一化后同样以 alarm.* 到达 platformEvent。若按黑名单处理，这些类型会因
// "没有匹配规则"被静默丢弃，而 ALM-02 向导根本不提供它们供用户配置，用户将无从恢复。
var deviceSideKinds = func() map[string]bool {
	m := make(map[string]bool, len(models.DeviceSideAlarmKinds))
	for _, k := range models.DeviceSideAlarmKinds {
		m[k] = true
	}
	return m
}()

func isDeviceSideKind(kind string) bool { return deviceSideKinds[kind] }

// parseHHMM 解析 "HH:MM" 为当日分钟数；"24:00" 合法，表示当日结束（1440）。
func parseHHMM(s string) (int, bool) {
	parts := strings.Split(strings.TrimSpace(s), ":")
	if len(parts) != 2 {
		return 0, false
	}
	h, err1 := strconv.Atoi(strings.TrimSpace(parts[0]))
	m, err2 := strconv.Atoi(strings.TrimSpace(parts[1]))
	if err1 != nil || err2 != nil || h < 0 || h > 24 || m < 0 || m > 59 {
		return 0, false
	}
	return h*60 + m, true
}

// jsonInt 从 JSONB 取整数：经 encoding/json 反序列化后数字为 float64。
func jsonInt(v any) (int, bool) {
	switch n := v.(type) {
	case float64:
		return int(n), true
	case int:
		return n, true
	case int64:
		return int(n), true
	}
	return 0, false
}

// scheduleActiveAt 判断 at 是否落在布防/录像时段内。
//
// Schedule 结构与 REC-05 录像模板共用：
//
//	{"days":[1,2,3,4,5], "ranges":[["08:00","12:00"],["14:00","18:00"]]}
//
// days 为 ISO 周内日（1=周一 … 7=周日）；缺失或为空表示不限星期。
// ranges 起点包含、终点不包含；缺失或为空表示全天。
// 整个 schedule 为空表示全天候——无模板的规则据此放行。
//
// 调用方须先把 at 转换到目标时区，本函数只做纯粹的时刻比对。
//
// 入参形态：须为经 JSON 反序列化后的形态（数字为 float64、数组为 []any）。实际调用方
// 一律从数据库读取 AlarmTemplate.Schedule，天然满足。Seed 函数里用 []int / [][]string
// 直接构造的字面量只用于写库，不会传入此处；若将来有人直接传入这类结构，类型断言会失败
// 而退化为"不限星期 / 全天放行"——方向是多报而非丢事件，但需知晓。
//
// 不支持跨零点时段（如 22:00-02:00）：ALM-01 的周×24h 网格编辑器天然产出按日切分的
// 时段，跨零点会被拆成两天各一条。若手工写入此类区间，该条目不会匹配任何时刻。
//
// 交叉引用：engine 包内还有一份 record.go 的 scheduleMatches，求值**同一个** Schedule
// 结构却语义相反——那边 days 为空表示全天录且忽略 ranges、ranges 为空表示从不录、
// 按服务器本地时区求值；这边 days/ranges 为空都表示不限（全天候），按项目时区求值。
// 两份实现由同一个周×24h 网格编辑器产出的模板输入，用户会理所当然地认为语义相同——
// 内置模板（days 全选 + 00:00-24:00）下两者结论一致，掩盖了分歧，但一旦出现 days 或
// ranges 为空的自定义模板，两条链路立刻分叉。此为已知限制（详见设计文档 §6），
// 修改任一处求值逻辑前，必须同时确认另一处是否需要同步调整。
func scheduleActiveAt(schedule models.JSONB, at time.Time) bool {
	if len(schedule) == 0 {
		return true
	}
	// Go 的 Weekday() 是 0=周日…6=周六，需转为 ISO 的 1=周一…7=周日
	iso := int(at.Weekday())
	if iso == 0 {
		iso = 7
	}
	if days, ok := schedule["days"].([]any); ok && len(days) > 0 {
		hit := false
		for _, d := range days {
			if n, ok := jsonInt(d); ok && n == iso {
				hit = true
				break
			}
		}
		if !hit {
			return false
		}
	}
	ranges, ok := schedule["ranges"].([]any)
	if !ok || len(ranges) == 0 {
		return true
	}
	mins := at.Hour()*60 + at.Minute()
	for _, r := range ranges {
		pair, ok := r.([]any)
		if !ok || len(pair) < 2 {
			continue
		}
		s, ok1 := pair[0].(string)
		e, ok2 := pair[1].(string)
		if !ok1 || !ok2 {
			continue
		}
		from, ok1 := parseHHMM(s)
		to, ok2 := parseHHMM(e)
		if !ok1 || !ok2 {
			continue // 非法条目跳过，不因一条脏数据放行整个模板
		}
		if mins >= from && mins < to {
			return true
		}
	}
	return false
}

// projectLocation 取项目时区，用于布防时段判定。
//
// 必须按项目时区而非服务器本地时区求值，否则跨时区部署的布防时段会整体偏移。
// 解析失败回退 Asia/Shanghai 并记录，绝不因时区问题丢事件。
func projectLocation(projectID string) *time.Location {
	if projectID != "" {
		var p models.Project
		if store.DB.First(&p, "id = ?", projectID).Error == nil && p.TZ != "" {
			if loc, err := time.LoadLocation(p.TZ); err == nil {
				return loc
			}
			log.Printf("[alarm] 项目 %s 时区 %q 解析失败，回退 Asia/Shanghai", projectID, p.TZ)
		}
	}
	if loc, err := time.LoadLocation("Asia/Shanghai"); err == nil {
		return loc
	}
	return time.UTC
}

// channelRuleAllows 设备侧事件的通道级规则判定（ALM-02）。
//
// 严格模式：通道没有任何启用的规则即拦截。存量通道由 store.MigrateDefaultAlarmRules
// 在启动时补建默认规则，新通道由 devsvc.ApplyDefaultAlarmRule 兜底建规则，因此升级
// 与新接入都不会静默设备侧告警——三者必须成对存在。
//
// 边界：事件无法归属到通道时放行（宁可多报，不可丢事件）；规则未指定模板或模板已被
// 删除时视为全天候放行；同一通道多条规则取 OR；查询规则本身失败时同样放行——见下方
// .Find 处注释。
func channelRuleAllows(projectID, channelID, kind string) bool {
	if channelID == "" {
		return true
	}
	var rules []models.AlarmRule
	// 查询失败（数据库瞬时故障等）必须放行而非拦截：与 migrate.go 一整套"失败即不作为"
	// 的方向刻意相反——那里的失败只是延后补建、下次启动自愈；这里一旦拦截，被丢弃的
	// 告警事件不会再回来。宁可对暂时故障多报几条，也不用严格模式的名义丢事件。
	if err := store.DB.Where("channel_id = ? AND enabled = ?", channelID, true).Find(&rules).Error; err != nil {
		log.Printf("[alarm] 通道 %s 查询规则失败，本次放行：%v", channelID, err)
		return true
	}
	if len(rules) == 0 {
		return false
	}
	now := time.Now().In(projectLocation(projectID))
	for _, r := range rules {
		matched := false
		for _, k := range r.Kinds {
			if k == kind {
				matched = true
				break
			}
		}
		if !matched {
			continue
		}
		if r.TemplateID == "" {
			return true
		}
		var tpl models.AlarmTemplate
		if store.DB.First(&tpl, "id = ?", r.TemplateID).Error != nil {
			return true // 模板缺失视为全天候，不因配置残缺丢事件
		}
		if scheduleActiveAt(tpl.Schedule, now) {
			return true
		}
	}
	return false
}
