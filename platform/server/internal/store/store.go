// Package store 数据库初始化、迁移与种子数据。
package store

import (
	"log"
	"time"

	"gorm.io/driver/postgres"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"

	"github.com/jetscam/ipccloud/server/internal/config"
	"github.com/jetscam/ipccloud/server/internal/models"
)

var DB *gorm.DB

func Open(cfg *config.Config) *gorm.DB {
	db, err := gorm.Open(postgres.Open(cfg.DBDSN), &gorm.Config{
		Logger: logger.Default.LogMode(logger.Warn),
	})
	if err != nil {
		log.Fatalf("connect postgres: %v", err)
	}
	DB = db
	if err := db.AutoMigrate(
		&models.Tenant{}, &models.Project{}, &models.DeviceGroup{},
		&models.Role{}, &models.User{}, &models.UserRole{},
		&models.Device{}, &models.Channel{},
		&models.MediaNode{}, &models.StreamSession{},
		&models.RecordTemplate{}, &models.RecordPlan{}, &models.RecordIndex{},
		&models.AlarmTemplate{}, &models.AlarmRule{}, &models.AlarmEvent{},
		&models.AuditLog{}, &models.Task{},
		&models.IdpPreadd{}, &models.GbWhitelist{}, &models.GbPending{},
		&models.Setting{},
	); err != nil {
		log.Fatalf("auto migrate: %v", err)
	}
	return db
}

// Seed 首次启动创建租户/项目/默认角色/超管。
func Seed(cfg *config.Config) {
	var tenant models.Tenant
	if err := DB.First(&tenant).Error; err == nil {
		return
	}
	tenant = models.Tenant{ID: "t_default", Name: "IpcCloud"}
	DB.Create(&tenant)

	proj := models.Project{
		ID: "p_default", TenantID: tenant.ID, Name: "默认项目", TZ: "Asia/Shanghai",
		Settings: models.JSONB{"streamIdleSec": cfg.StreamIdleSec}, Enabled: true,
		CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli(),
	}
	DB.Create(&proj)

	roles := []models.Role{
		{ID: "role_super", ProjectID: proj.ID, Name: "超级管理员", Builtin: true,
			Perms: models.JSONB{"menus": []string{"*"}, "actions": []string{"*"}}, Scope: models.JSONB{},
			CreatedAt: models.NowMilli()},
		{ID: "role_admin", ProjectID: proj.ID, Name: "项目管理员", Builtin: false,
			Perms: models.JSONB{"menus": []string{"*"}, "actions": []string{"view", "preview", "playback", "ptz", "config", "delete"}},
			Scope: models.JSONB{}, CreatedAt: models.NowMilli()},
		{ID: "role_duty", ProjectID: proj.ID, Name: "值班员", Builtin: false,
			Perms: models.JSONB{"menus": []string{"dashboard", "devices", "live", "playback", "alarms"}, "actions": []string{"view", "preview", "playback", "ptz"}},
			Scope: models.JSONB{}, CreatedAt: models.NowMilli()},
		{ID: "role_readonly", ProjectID: proj.ID, Name: "只读", Builtin: false,
			Perms: models.JSONB{"menus": []string{"dashboard", "devices", "live"}, "actions": []string{"view", "preview"}},
			Scope: models.JSONB{}, CreatedAt: models.NowMilli()},
	}
	DB.Create(&roles)

	u, _ := models.NewUser(tenant.ID, cfg.AdminUser, cfg.AdminPassword, "Administrator")
	DB.Create(u)
	DB.Create(&models.UserRole{UserID: u.ID, RoleID: "role_super", ProjectID: proj.ID})

	// 内置录像/布防模板 + 默认策略（ADD-09），与 api/projects.go 的 handleCreateProject
	// 同构：使 p_default 与后续新建项目行为一致、开箱即用，且设置在 UI 里可见可改。
	// 此前两个模板集都建了、但两个默认策略设置都没写，导致 p_default 下新接入的通道
	// 拿不到任何默认录像计划/告警规则（告警侧后果见 devsvc.ApplyDefaultAlarmRule 的注释）。
	//
	// key 直接写字面量而非引用 devsvc.RecordDefaultsKey / devsvc.AlarmDefaultsKey——
	// store 包不能导入 devsvc（devsvc 已导入 store，引用会形成循环依赖）。这两个字面量
	// 必须与 devsvc.RecordDefaultsKey（"recordDefaults"）/ devsvc.AlarmDefaultsKey
	// （"alarmDefaults"）保持一致，修改任一处需同步核对。
	if tplID := SeedRecordTemplates(proj.ID); tplID != "" {
		DB.Create(&models.Setting{Scope: proj.ID, Key: "recordDefaults",
			Value: models.JSONB{"enabled": true, "templateId": tplID, "profile": "main"}})
	}
	if tplID := SeedAlarmTemplates(proj.ID); tplID != "" {
		DB.Create(&models.Setting{Scope: proj.ID, Key: "alarmDefaults",
			Value: models.JSONB{"enabled": true, "templateId": tplID,
				"kinds": models.DeviceSideAlarmKinds}})
	}

	log.Printf("seed done (admin/%s)", cfg.AdminPassword)
	_ = time.Now
}

// SeedRecordTemplates 为项目创建 REC-05 的三个内置录像模板，返回"全天候"模板 ID。
//
// 初始播种与新建项目（handleCreateProject）共用：此前内置模板只在播种时为默认项目创建，
// 新建项目一个模板都没有，会使 ADD-09 的默认录像策略对新项目无从指向。
func SeedRecordTemplates(projectID string) string {
	now := models.NowMilli()
	defs := []struct {
		name string
		days []int
	}{
		{"全天候", []int{1, 2, 3, 4, 5, 6, 7}},
		{"工作日", []int{1, 2, 3, 4, 5}},
		{"周末", []int{6, 7}},
	}
	defaultID := ""
	for i, d := range defs {
		t := models.RecordTemplate{
			ID: "rt_" + models.NewID(), ProjectID: projectID, Name: d.name, Kind: "timer",
			Schedule: models.JSONB{"days": d.days, "ranges": [][]string{{"00:00", "24:00"}}},
			Builtin:  true, CreatedAt: now, UpdatedAt: now,
		}
		if DB.Create(&t).Error == nil && i == 0 {
			defaultID = t.ID // 首个"全天候"作为新项目默认录像模板
		}
	}
	return defaultID
}

// SeedAlarmTemplates 为项目创建 ALM-01 的三个内置布防模板，返回"全天候"模板 ID。
//
// 与 SeedRecordTemplates 同构，成因也相同：内置布防模板此前只在初始播种时为默认项目
// 创建，新建项目一个都没有，导致默认告警策略无从指向。
func SeedAlarmTemplates(projectID string) string {
	now := models.NowMilli()
	defs := []struct {
		name string
		days []int
	}{
		{"全天候", []int{1, 2, 3, 4, 5, 6, 7}},
		{"工作日", []int{1, 2, 3, 4, 5}},
		{"周末", []int{6, 7}},
	}
	defaultID := ""
	for i, d := range defs {
		t := models.AlarmTemplate{
			ID: "at_" + models.NewID(), ProjectID: projectID, Name: d.name,
			Schedule: models.JSONB{"days": d.days, "ranges": [][]string{{"00:00", "24:00"}}},
			Builtin:  true, CreatedAt: now, UpdatedAt: now,
		}
		if DB.Create(&t).Error == nil && i == 0 {
			defaultID = t.ID // 首个"全天候"作为默认布防模板
		}
	}
	return defaultID
}
