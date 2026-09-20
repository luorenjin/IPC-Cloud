package engine

// 设备定时重启执行器（MGR-08「立即/定时；记录成功/失败」）。
//
// 与录像计划同一档 30s 轮询。到点判定必须落在**项目时区**上：
// 容器 TZ 一般是 UTC，用 time.Local 会让「每天 03:00 重启」在 UTC+8 下
// 实际于本地 11:00 执行（详见 store.ProjectLocation 的说明）。

import (
	"context"
	"time"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// StartRebootRunner 启动定时重启循环。
func (e *Engine) StartRebootRunner() {
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for range t.C {
			e.tickRebootPlans()
		}
	}()
}

// tickRebootPlans 单轮调度：查启用中的计划 → 到点且当日未触发 → 落水位 → 异步下发。
func (e *Engine) tickRebootPlans() {
	var plans []models.RebootPlan
	store.DB.Where("enabled = true").Find(&plans)
	for _, p := range plans {
		var dev models.Device
		if store.DB.First(&dev, "id = ? AND deleted_at = 0", p.DeviceID).Error != nil {
			continue
		}
		now := time.Now().In(store.ProjectLocation(dev.ProjectID))
		key := now.Format("2006-01-02 15:04")
		// 水位先落库再下发：下发要走设备往返，可能长于轮询间隔，
		// 先落库才能保证同一分钟只触发一次。
		if p.LastFiredKey == key || !rebootDue(p.Schedule, now) {
			continue
		}
		store.DB.Model(&models.RebootPlan{}).Where("id = ?", p.ID).Update("last_fired_key", key)
		go e.fireScheduledReboot(dev, key)
	}
}

// rebootDue 到点判定：当前 HH:MM 等于计划时刻，且今天在计划星期内（空=每天）。
func rebootDue(sch models.JSONB, now time.Time) bool {
	hm, _ := sch["time"].(string)
	if hm == "" || hm != now.Format("15:04") {
		return false
	}
	rawDays, _ := sch["days"].([]any)
	if len(rawDays) == 0 {
		return true
	}
	weekday := int(now.Weekday())
	if weekday == 0 {
		weekday = 7
	}
	for _, d := range rawDays {
		if int(toF(d)) == weekday {
			return true
		}
	}
	return false
}

// fireScheduledReboot 下发重启并落审计日志。
//
// 定时任务没有登录用户，UserID 留空、Username 标注来源；日志页与审计页按
// target=device:<id> 关联到设备，因此 target 必须与手动重启保持同一格式。
func (e *Engine) fireScheduledReboot(dev models.Device, key string) {
	res := "success"
	detail := models.JSONB{"scheduled": true, "at": key}
	if a := adapter.Get(dev.Source); a == nil {
		res = "fail"
		detail["reason"] = "该来源不支持远程重启"
	} else if err := a.Reboot(context.Background(), dev.ID); err != nil {
		res = "fail"
		detail["reason"] = err.Error()
	}
	_ = store.DB.Create(&models.AuditLog{
		ID: "lg_" + models.NewID(),
		// 定时任务无登录上下文，租户/项目从设备所属项目倒推，
		// 否则该行 tenant_id 为空，租户过滤后谁都看不到（或反过来对所有人可见）。
		TenantID: store.ProjectTenant(dev.ProjectID), ProjectID: dev.ProjectID,
		Username: "定时重启", Action: "reboot", Target: "device:" + dev.ID,
		Result: res, Detail: detail, Ts: models.NowMilli(),
	}).Error
}
