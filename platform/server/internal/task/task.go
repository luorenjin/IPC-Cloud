// Package task 长任务中心（发现/导入/升级/下载）。进度持久化并经事件总线推送。
package task

import (
	"sync"

	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// 终态集合：处于这些状态的任务不会再推进，可直接删除。
var finalStatus = map[string]bool{
	"success": true, "failed": true, "partial": true, "canceled": true,
}

// IsFinal 判断任务是否已处于终态。
func IsFinal(status string) bool { return finalStatus[status] }

type Manager struct {
	bus *bus.Bus
	mu  sync.Mutex
}

func New(b *bus.Bus) *Manager { return &Manager{bus: b} }

// Create 建任务并返回 id。projectID 为项目隔离依据，title 供前端直接展示。
func (m *Manager) Create(projectID, typ, title, userID string, result models.JSONB) string {
	if result == nil {
		result = models.JSONB{}
	}
	t := models.Task{ID: "tk_" + models.NewID(), ProjectID: projectID, Type: typ, Title: title,
		Status: "running", Result: result, CreatedBy: userID,
		CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
	store.DB.Create(&t)
	m.publish(&t)
	return t.ID
}

// Update 推进任务状态；progress 传 -1 表示不变，detail 传空字符串表示不变。
func (m *Manager) Update(id, status string, progress int, detail string, result models.JSONB) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var t models.Task
	if err := store.DB.First(&t, "id = ?", id).Error; err != nil {
		return
	}
	// 已取消的任务不再被后台推进，避免取消结果被回写覆盖。
	if t.Status == "canceled" {
		return
	}
	if status != "" {
		t.Status = status
	}
	if progress >= 0 {
		t.Progress = progress
	}
	if detail != "" {
		t.Detail = detail
	}
	if result != nil {
		t.Result = result
	}
	t.UpdatedAt = models.NowMilli()
	store.DB.Save(&t)
	m.publish(&t)
}

// Cancel 取消进行中的任务；已处终态则返回 false。
func (m *Manager) Cancel(id, reason string) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	var t models.Task
	if err := store.DB.First(&t, "id = ?", id).Error; err != nil {
		return false
	}
	if IsFinal(t.Status) {
		return false
	}
	t.Status = "canceled"
	if reason != "" {
		t.Detail = reason
	}
	t.UpdatedAt = models.NowMilli()
	store.DB.Save(&t)
	m.publish(&t)
	return true
}

// Canceled 供长任务循环体轮询：任务是否已被用户取消。
func (m *Manager) Canceled(id string) bool {
	var t models.Task
	if store.DB.Select("status").First(&t, "id = ?", id).Error != nil {
		return false
	}
	return t.Status == "canceled"
}

// publish 统一推送任务快照（前端按 taskId 增量合并）。
func (m *Manager) publish(t *models.Task) {
	m.bus.Publish(bus.Event{Type: "task.progress", ProjectID: t.ProjectID,
		Data: map[string]any{
			"taskId": t.ID, "type": t.Type, "title": t.Title, "detail": t.Detail,
			"status": t.Status, "progress": t.Progress, "result": t.Result,
			"createdAt": t.CreatedAt, "updatedAt": t.UpdatedAt,
		}})
}

// FailStep 追加一条失败记录到任务结果。
func (m *Manager) FailStep(id, key string, item any) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var t models.Task
	if store.DB.First(&t, "id = ?", id).Error != nil {
		return
	}
	if t.Result == nil {
		t.Result = models.JSONB{}
	}
	fails, _ := t.Result["failed"].([]any)
	t.Result["failed"] = append(fails, item)
	t.UpdatedAt = models.NowMilli()
	store.DB.Save(&t)
}
