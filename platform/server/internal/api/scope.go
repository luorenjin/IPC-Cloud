package api

import (
	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// ---------- 实体归属校验（跨项目 / 跨租户越权防护） ----------
//
// requirePerm 校验的是「请求里的当前项目」的权限位，它并不回答「被操作的实体属于谁」。
// 于是只要持有一个合法 token，就能用**自己的** projectId 配上**别处**的实体 ID，
// 去读、改、删掉其它项目乃至其它租户的数据——批量设备接口（handleDeviceBatch）
// 早就带了 project_id 条件，其余按主键直查的接口都属于同一类缺陷。
//
// 因此：所有面向项目/租户的实体查询都必须过下面这组函数。约定命中不到一律 404，
// 不区分「不存在」与「不属于你」，避免把实体是否存在泄露给越权调用方。
// 写操作除前置校验外，还应在 Updates/Delete 的 WHERE 里再带一次归属条件：
// 前置校验与写条件之间存在竞态，双保险才能保证改不到别人家的行。

// scopeNotFound 统一按「不存在」拒绝。
func scopeNotFound(c *gin.Context) {
	fail(c, errs.ENotFound)
}

// deviceInProject 设备归属校验（软删设备视为不存在）。
func deviceInProject(c *gin.Context, id string) (*models.Device, bool) {
	ctx := getCtx(c)
	var d models.Device
	if ctx == nil ||
		store.DB.First(&d, "id = ? AND project_id = ? AND deleted_at = 0", id, ctx.ProjectID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &d, true
}

// channelInProject 通道归属校验。通道冗余存了 project_id，一次查询即可判定。
func channelInProject(c *gin.Context, id string) (*models.Channel, bool) {
	ctx := getCtx(c)
	var ch models.Channel
	if ctx == nil || store.DB.First(&ch, "id = ? AND project_id = ?", id, ctx.ProjectID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &ch, true
}

// groupInProject 设备分组归属校验。
func groupInProject(c *gin.Context, id string) (*models.DeviceGroup, bool) {
	ctx := getCtx(c)
	var g models.DeviceGroup
	if ctx == nil || store.DB.First(&g, "id = ? AND project_id = ?", id, ctx.ProjectID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &g, true
}

// recordTemplateInProject 录像模板归属校验（内置模板归属于创建它的项目）。
func recordTemplateInProject(c *gin.Context, id string) (*models.RecordTemplate, bool) {
	ctx := getCtx(c)
	var t models.RecordTemplate
	if ctx == nil || store.DB.First(&t, "id = ? AND project_id = ?", id, ctx.ProjectID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &t, true
}

// recordPlanInProject 录像计划本身不带 project_id（它挂在通道上），经所属通道判定归属；
// 判据与 handleListRecordPlans 的 JOIN 完全一致。
func recordPlanInProject(c *gin.Context, id string) (*models.RecordPlan, bool) {
	ctx := getCtx(c)
	var p models.RecordPlan
	if ctx == nil || store.DB.Raw(`SELECT p.* FROM record_plans p JOIN channels c ON c.id = p.channel_id
		WHERE p.id = ? AND c.project_id = ?`, id, ctx.ProjectID).Scan(&p).Error != nil || p.ID == "" {
		scopeNotFound(c)
		return nil, false
	}
	return &p, true
}

// roleInProject 角色归属校验（角色是项目级实体：把别家项目的角色挂到本项目的成员关系上
// 等于凭空获得一份不属于自己的权限集）。
func roleInProject(c *gin.Context, id string) (*models.Role, bool) {
	ctx := getCtx(c)
	var r models.Role
	if ctx == nil || store.DB.First(&r, "id = ? AND project_id = ?", id, ctx.ProjectID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &r, true
}

// alarmTemplateInProject 布防模板归属校验。
func alarmTemplateInProject(c *gin.Context, id string) (*models.AlarmTemplate, bool) {
	ctx := getCtx(c)
	var t models.AlarmTemplate
	if ctx == nil || store.DB.First(&t, "id = ? AND project_id = ?", id, ctx.ProjectID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &t, true
}

// alarmRuleInProject 告警规则归属校验。
func alarmRuleInProject(c *gin.Context, id string) (*models.AlarmRule, bool) {
	ctx := getCtx(c)
	var r models.AlarmRule
	if ctx == nil || store.DB.First(&r, "id = ? AND project_id = ?", id, ctx.ProjectID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &r, true
}

// alarmEventInProject 告警事件归属校验。
func alarmEventInProject(c *gin.Context, id string) (*models.AlarmEvent, bool) {
	ctx := getCtx(c)
	var ev models.AlarmEvent
	if ctx == nil || store.DB.First(&ev, "id = ? AND project_id = ?", id, ctx.ProjectID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &ev, true
}

// projectInTenant 项目归属校验。
//
// 项目按**租户**判定而非按当前项目：同一租户下的多项目成员本就需要访问多个项目
// （handleDeleteProject 的「权限按待删除的目标项目校验」也是这个道理），
// 所以这里只切断跨租户，改动权限仍由调用方按目标项目校验。
func projectInTenant(c *gin.Context, id string) (*models.Project, bool) {
	ctx := getCtx(c)
	var p models.Project
	if ctx == nil || store.DB.First(&p, "id = ? AND tenant_id = ?", id, ctx.TenantID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &p, true
}

// userInTenant 成员归属校验（成员是租户级实体，一个成员可被授予多个项目的角色）。
func userInTenant(c *gin.Context, id string) (*models.User, bool) {
	ctx := getCtx(c)
	var u models.User
	if ctx == nil || store.DB.First(&u, "id = ? AND tenant_id = ?", id, ctx.TenantID).Error != nil {
		scopeNotFound(c)
		return nil, false
	}
	return &u, true
}
