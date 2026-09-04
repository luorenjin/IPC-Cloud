// Package task 长任务中心（发现/导入/升级/下载）。进度持久化并经事件总线推送。
package task

import (
	"sync"

	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

type Manager struct {
	bus *bus.Bus
	mu  sync.Mutex
}

func New(b *bus.Bus) *Manager { return &Manager{bus: b} }

// Create 建任务并返回 id。
func (m *Manager) Create(typ, userID string, result models.JSONB) string {
	t := models.Task{ID: "tk_" + models.NewID(), Type: typ, Status: "running",
		Result: result, CreatedBy: userID, CreatedAt: models.NowMilli(), UpdatedAt: models.NowMilli()}
	store.DB.Create(&t)
	return t.ID
}

// Update 推进任务状态；progress 传 -1 表示不变。
func (m *Manager) Update(id, status string, progress int, result models.JSONB) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var t models.Task
	if err := store.DB.First(&t, "id = ?", id).Error; err != nil {
		return
	}
	if status != "" {
		t.Status = status
	}
	if progress >= 0 {
		t.Progress = progress
	}
	if result != nil {
		t.Result = result
	}
	t.UpdatedAt = models.NowMilli()
	store.DB.Save(&t)
	pid, _ := result["projectId"].(string)
	m.bus.Publish(bus.Event{Type: "task.progress", ProjectID: pid,
		Data: map[string]any{"taskId": id, "status": t.Status, "progress": t.Progress, "result": t.Result}})
}

// FailStep 追加一条失败记录到任务结果。
func (m *Manager) FailStep(id, key string, item any) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var t models.Task
	if store.DB.First(&t, "id = ?", id).Error != nil {
		return
	}
	fails, _ := t.Result["failed"].([]any)
	t.Result["failed"] = append(fails, item)
	t.UpdatedAt = models.NowMilli()
	store.DB.Save(&t)
}
