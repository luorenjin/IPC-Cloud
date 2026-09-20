// 任务生命周期治理（P-18）：回收僵尸任务、清理过期终态任务，避免任务表只增不减。
package task

import (
	"log"
	"time"

	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

const (
	// staleTimeout 进行中的任务超过此时长无进度更新，判定为僵尸任务并标记失败。
	staleTimeout = 30 * time.Minute
	// retainDays 终态任务保留天数，超期自动清理。
	retainDays = 7
	// gcInterval 巡检周期。
	gcInterval = 10 * time.Minute
)

// StartGC 启动后台巡检协程（进程级单例，随进程退出）。
func StartGC() {
	go func() {
		// 启动后先等待数据库就绪，再做首轮巡检
		time.Sleep(30 * time.Second)
		for {
			sweepStale()
			purgeExpired()
			time.Sleep(gcInterval)
		}
	}()
}

// sweepStale 将长时间无更新的进行中任务标记为失败，避免前端红点长期不消。
func sweepStale() {
	deadline := models.NowMilli() - staleTimeout.Milliseconds()
	res := store.DB.Model(&models.Task{}).
		Where("status IN ? AND updated_at < ?", []string{"pending", "running"}, deadline).
		Updates(map[string]any{
			"status":     "failed",
			"detail":     "任务超时无响应，已自动终止",
			"updated_at": models.NowMilli(),
		})
	if res.RowsAffected > 0 {
		log.Printf("[task-gc] 回收僵尸任务 %d 条", res.RowsAffected)
	}
}

// purgeExpired 清理超期的终态任务。
func purgeExpired() {
	before := models.NowMilli() - int64(retainDays)*86400_000
	res := store.DB.Where("created_at < ? AND status IN ?", before,
		[]string{"success", "partial", "failed", "canceled"}).Delete(&models.Task{})
	if res.RowsAffected > 0 {
		log.Printf("[task-gc] 清理过期任务 %d 条", res.RowsAffected)
	}
}
