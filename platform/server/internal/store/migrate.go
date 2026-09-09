package store

import (
	"log"

	"github.com/jetscam/ipccloud/server/internal/models"
)

// MigrateDefaultAlarmRules 为存量通道补建默认告警规则。
//
// 背景：engine 现已对设备侧事件做严格模式判定（无规则即拦截）。存量部署一条规则都没有，
// 若不迁移，升级后设备侧告警会全部静默。本迁移使升级前后行为保持一致。
//
// 新建规则的 Kinds 取 models.DeviceSideAlarmKinds 全集，刻意不按通道能力集取交集：
// 当前没有任何设备声明 event.* 能力（模拟器不上报、服务端无写入代码、ONVIF 适配器也未按
// 接入规范 §447 置位），取交集会使每条规则的 Kinds 都为空，迁移等于没做、设备侧告警全部
// 静默。详见设计文档 §6.1。
//
// 两步顺序不可颠倒：先为缺少内置布防模板的项目补齐模板，再建引用该模板的规则。
// 幂等：以"项目无内置模板"与"通道无任何规则"为条件，重复启动不产生重复数据。
func MigrateDefaultAlarmRules() {
	var projectIDs []string
	if err := DB.Model(&models.Channel{}).Distinct().Pluck("project_id", &projectIDs).Error; err != nil {
		log.Printf("[migrate] 读取项目列表失败：%v", err)
		return
	}
	seeded, ruleCount, chCount, skipped := 0, 0, 0, 0
	for _, pid := range projectIDs {
		if pid == "" {
			// project_id 为空的通道是孤儿数据（正常流程不应产生）：无法定位所属项目
			// 就无法补齐布防模板、也无法建规则，其设备侧告警会被 channelRuleAllows
			// 永久拦截。跳过前记一条日志，否则这批通道的静默毫无线索。
			log.Printf("[migrate] 发现 project_id 为空的通道，跳过默认告警规则补建（需人工核查孤儿数据）")
			continue
		}
		// 步骤一：补齐内置布防模板
		//
		// 查询失败时必须跳过该项目而不是继续——Count 失败会使 tplCnt 保持零值，
		// 若不检查错误会被误判为"项目无内置模板"，对已有模板的项目重复播种。
		var tplCnt int64
		if err := DB.Model(&models.AlarmTemplate{}).Where("project_id = ? AND builtin = ?", pid, true).
			Count(&tplCnt).Error; err != nil {
			log.Printf("[migrate] 项目 %s 查询内置布防模板数失败，跳过：%v", pid, err)
			skipped++
			continue
		}
		var allDayID string
		if tplCnt == 0 {
			allDayID = SeedAlarmTemplates(pid)
			seeded++
		} else {
			var tpl models.AlarmTemplate
			if DB.Where("project_id = ? AND builtin = ? AND name = ?", pid, true, "全天候").
				First(&tpl).Error == nil {
				allDayID = tpl.ID
			}
		}
		// 步骤二：为无规则的通道补建默认规则
		//
		// 查询失败时必须跳过该项目而不是继续——Pluck 失败会使 chIDs 保持为空切片，
		// 若不检查错误会使该项目下全部通道被静默跳过，且没有任何日志线索，
		// 后果正是本迁移要解决的问题本身（设备侧告警全部静默）。
		var chIDs []string
		if err := DB.Model(&models.Channel{}).Where("project_id = ?", pid).Pluck("id", &chIDs).Error; err != nil {
			log.Printf("[migrate] 项目 %s 读取通道列表失败，跳过：%v", pid, err)
			skipped++
			continue
		}
		for _, cid := range chIDs {
			chCount++
			// 查询失败时必须跳过该通道而不是继续——Count 失败会使 cnt 保持零值，
			// 若不检查错误会被误判为"通道无规则"，对同一通道重复创建默认规则。
			var cnt int64
			if err := DB.Model(&models.AlarmRule{}).Where("channel_id = ?", cid).Count(&cnt).Error; err != nil {
				log.Printf("[migrate] 通道 %s 查询已有规则数失败，跳过：%v", cid, err)
				skipped++
				continue
			}
			if cnt > 0 {
				continue
			}
			r := models.AlarmRule{
				ID: "ar_" + models.NewID(), ProjectID: pid, ChannelID: cid,
				Kinds: models.StringSlice(models.DeviceSideAlarmKinds), TemplateID: allDayID,
				Enabled: true, CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli(),
			}
			if err := DB.Create(&r).Error; err != nil {
				log.Printf("[migrate] 通道 %s 建默认告警规则失败：%v", cid, err)
				continue
			}
			ruleCount++
		}
	}
	log.Printf("[migrate] 默认告警规则：补齐布防模板 %d 个项目，扫描通道 %d 个，新建规则 %d 条，因查询失败跳过 %d 处",
		seeded, chCount, ruleCount, skipped)
}
