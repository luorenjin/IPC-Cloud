// 存储保留清理 worker（REC-07 落地）。
//
// ZLMediaKit 本身没有 MP4 录像的保留/替换机制（config.ini 的 [hls] segNum/segRetain
// 只管 HLS 环形缓冲，与 MP4 录制无关）：分段只会持续产生、从不清理，这正是
// ipccloud_zlmdata 卷失控增长到 94G 的直接原因。本文件补上两条独立节奏的清理：
//   - sweepPending：快速回收未被告警认领的 pending 分段（REC-06 滚动缓冲区若无此步
//     配合，会无限增长，见 record.go 的事件录像状态机）。
//   - sweepRetention：按项目的 storage.keepDays / storage.totalBytes 策略清理已确认
//     的 timer/event/manual 分段，DB 行与物理文件成对删除，避免悬挂状态。
package engine

import (
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

const (
	pendingSweepInterval   = 30 * time.Second
	retentionSweepInterval = 10 * time.Minute
)

// StartRecordGC 启动录像保留清理协程（与 StartRecordRunner 并列，进程级单例）。
func (e *Engine) StartRecordGC() {
	go func() {
		time.Sleep(30 * time.Second)
		e.sweepRetention() // 启动后先跑一轮，不等首个 10 分钟
		pendingTicker := time.NewTicker(pendingSweepInterval)
		retentionTicker := time.NewTicker(retentionSweepInterval)
		defer pendingTicker.Stop()
		defer retentionTicker.Stop()
		for {
			select {
			case <-pendingTicker.C:
				e.sweepPending()
			case <-retentionTicker.C:
				e.sweepRetention()
			}
		}
	}()
}

// sweepPending 逐项目回收超过 pendingTTLSec 仍未被告警转正的 pending 分段。
func (e *Engine) sweepPending() {
	var projects []models.Project
	store.DB.Find(&projects)
	tbl := store.DB.NamingStrategy.TableName("RecordIndex")
	for _, proj := range projects {
		cutoff := models.NowMilli() - int64(pendingTTLSec(proj.ID))*1000
		var rows []models.RecordIndex
		store.DB.Joins("JOIN channels c ON c.id = "+tbl+".channel_id").
			Where("c.project_id = ? AND "+tbl+".type = 'pending' AND "+tbl+".end_ts < ?", proj.ID, cutoff).
			Find(&rows)
		e.purgeRecordRows(rows)
	}
}

// sweepRetention 逐项目应用保留策略清理。
func (e *Engine) sweepRetention() {
	var projects []models.Project
	store.DB.Find(&projects)
	for _, proj := range projects {
		e.sweepProjectRetention(proj.ID)
	}
}

// sweepProjectRetention 对单个项目：已确认分段（timer/event/manual）按 StartTs 从旧到新
// 删除，直到 keepDays 与 totalBytes 都不再超限——与 handleStorageOverview（REC-07 概览
// 接口，platform/server/internal/api/records.go）复用同一套设置读取口径与默认值。
func (e *Engine) sweepProjectRetention(projectID string) {
	tbl := store.DB.NamingStrategy.TableName("RecordIndex")
	var rows []models.RecordIndex
	store.DB.Joins("JOIN channels c ON c.id = "+tbl+".channel_id").
		Where("c.project_id = ? AND "+tbl+".type IN ('timer','event','manual')", projectID).
		Order(tbl + ".start_ts ASC").Find(&rows)
	if len(rows) == 0 {
		return
	}
	keepDays := settingInt(projectID, "storage.keepDays", 30)
	totalBytes := settingInt64(projectID, "storage.totalBytes", 500*1024*1024*1024)
	cutoff := models.NowMilli() - int64(keepDays)*86400_000
	var total int64
	for _, r := range rows {
		total += r.Size
	}
	var purge []models.RecordIndex
	for _, r := range rows {
		if r.StartTs >= cutoff && total <= totalBytes {
			break
		}
		purge = append(purge, r)
		total -= r.Size
	}
	e.purgeRecordRows(purge)
}

// purgeRecordRows 成对删除 RecordIndex 行与物理文件；单个文件删除失败（含文件已不存在）
// 不阻塞其余行处理，避免野文件把整个清理流程卡死。
func (e *Engine) purgeRecordRows(rows []models.RecordIndex) {
	if len(rows) == 0 {
		return
	}
	ids := make([]string, len(rows))
	for i, r := range rows {
		ids[i] = r.ID
		if p := recordFilePath(e.Cfg.RecordDir, r.Path); p != "" {
			if err := os.Remove(p); err != nil && !os.IsNotExist(err) {
				log.Printf("[record-gc] 删除录像文件失败 %s: %v", p, err)
			}
		}
	}
	res := store.DB.Where("id IN ?", ids).Delete(&models.RecordIndex{})
	if res.RowsAffected > 0 {
		log.Printf("[record-gc] 清理录像索引 %d 条", res.RowsAffected)
	}
}

// recordFilePath 把 RecordIndex.Path 映射为 server 容器内可见的物理路径。
//
// RecordIndex.Path 是 ZLM on_record_mp4 hook 回传的 url，相对于 ZLM 的 www 根目录，
// 固定带 "record/" 前缀（如 "record/rtp/<stream>/…mp4"，见 handleQueryDownload 的用法：
// 直接拼到 ZLM 静态服务地址后面）。而 ZLM_RECORD_DIR 这个卷挂载点本身就是 zlmdata 卷
// 在 zlm 容器内的挂载目标 /opt/media/bin/www/record，即 www 根目录下的 record 目录，
// 目录下直接是 live/proxy/rtp 等 app 子目录、没有再嵌一层 "record/"——所以这里必须先
// 去掉 Path 的 "record/" 前缀再拼接，否则会拼出多一层、永远不存在的路径，导致
// os.Remove 静默命中 IsNotExist、文件永远删不掉（DB 索引清了，物理文件却在堆积）。
// ZLM_RECORD_DIR 未配置时返回空串，跳过文件删除（仅清 DB 索引）。
func recordFilePath(recordDir, path string) string {
	if recordDir == "" || path == "" {
		return ""
	}
	rel := strings.TrimPrefix(path, "/")
	rel = strings.TrimPrefix(rel, "record/")
	return filepath.Join(recordDir, rel)
}

// settingInt64 读取 scope=projectID 下的 int64 设置项，取不到或非法时返回 def。
func settingInt64(projectID, key string, def int64) int64 {
	var st models.Setting
	if err := store.DB.First(&st, "scope = ? AND key = ?", projectID, key).Error; err != nil {
		return def
	}
	if v, ok := st.Value["value"].(float64); ok && v > 0 {
		return int64(v)
	}
	return def
}
