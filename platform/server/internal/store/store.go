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

	// 内置录像/布防模板
	now := models.NowMilli()
	db := DB
	db.Create(&models.RecordTemplate{ID: "rt_24h", ProjectID: proj.ID, Name: "全天候", Kind: "timer",
		Schedule: models.JSONB{"days": []int{1, 2, 3, 4, 5, 6, 7}, "ranges": [][]string{{"00:00", "24:00"}}},
		Builtin: true, CreatedAt: now, UpdatedAt: now})
	db.Create(&models.RecordTemplate{ID: "rt_weekday", ProjectID: proj.ID, Name: "工作日", Kind: "timer",
		Schedule: models.JSONB{"days": []int{1, 2, 3, 4, 5}, "ranges": [][]string{{"00:00", "24:00"}}},
		Builtin: true, CreatedAt: now, UpdatedAt: now})
	db.Create(&models.RecordTemplate{ID: "rt_weekend", ProjectID: proj.ID, Name: "周末", Kind: "timer",
		Schedule: models.JSONB{"days": []int{6, 7}, "ranges": [][]string{{"00:00", "24:00"}}},
		Builtin: true, CreatedAt: now, UpdatedAt: now})
	for _, n := range []string{"at_allday", "at_workday", "at_weekend"} {
		_ = n
	}
	db.Create(&models.AlarmTemplate{ID: "at_24h", ProjectID: proj.ID, Name: "全天候",
		Schedule: models.JSONB{"days": []int{1, 2, 3, 4, 5, 6, 7}, "ranges": [][]string{{"00:00", "24:00"}}},
		Builtin: true, CreatedAt: now, UpdatedAt: now})
	db.Create(&models.AlarmTemplate{ID: "at_workday", ProjectID: proj.ID, Name: "工作日",
		Schedule: models.JSONB{"days": []int{1, 2, 3, 4, 5}, "ranges": [][]string{{"00:00", "24:00"}}},
		Builtin: true, CreatedAt: now, UpdatedAt: now})
	db.Create(&models.AlarmTemplate{ID: "at_weekend", ProjectID: proj.ID, Name: "周末",
		Schedule: models.JSONB{"days": []int{6, 7}, "ranges": [][]string{{"00:00", "24:00"}}},
		Builtin: true, CreatedAt: now, UpdatedAt: now})

	log.Printf("seed done (admin/%s)", cfg.AdminPassword)
	_ = time.Now
}
