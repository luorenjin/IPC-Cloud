package engine

// 媒体节点统计与心跳巡检（SYS-01）：周期拉取 getMediaList 计算
// 流数/播放路数/带宽(in/out)，读版本，维护最近心跳；
// 拉取失败（≈30s 无心跳）置离线、发 node.status 事件并产生 node_offline 告警。

import (
	"context"
	"log"
	"time"

	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/media"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// StartNodeStatsRunner 启动节点巡检循环（30s）。
func (e *Engine) StartNodeStatsRunner() {
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		e.tickNodeStats() // 启动即跑一轮，避免首分钟数据为空
		for range t.C {
			e.tickNodeStats()
		}
	}()
}

func (e *Engine) tickNodeStats() {
	var nodes []models.MediaNode
	if err := store.DB.Find(&nodes).Error; err != nil {
		log.Printf("[node-stats] list: %v", err)
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	for _, n := range nodes {
		zlm := media.ForNode(&n)
		list, err := zlm.GetMediaList(ctx)
		if err != nil {
			// 心跳失败：置离线并告警（仅状态变化时发事件，避免刷屏）
			if n.Status != "offline" {
				store.DB.Model(&models.MediaNode{}).Where("id = ?", n.ID).
					Updates(map[string]any{"status": "offline", "playing": 0, "bw_in": 0, "bw_out": 0})
				publishNodeStatus(n.ID, "offline")
				createAlarmEvent(firstProjectID(), "", "", "node_offline", "error",
					map[string]any{"nodeId": n.ID, "name": n.Name}, "")
			} else {
				store.DB.Model(&models.MediaNode{}).Where("id = ?", n.ID).Update("status", "offline")
			}
			continue
		}
		var streams, playing int
		var bwIn, bwOut int64
		for _, m := range list {
			streams++
			rc := int(toF(m["readerCount"]))
			playing += rc
			bs := int64(toF(m["bytesSpeed"]))
			bwIn += bs
			bwOut += bs * int64(rc)
		}
		updates := map[string]any{
			"status": "online", "last_keepalive": models.NowMilli(),
			"streams": streams, "playing": playing, "bw_in": bwIn, "bw_out": bwOut,
		}
		if n.Version == "" {
			if v, verr := zlm.Version(ctx); verr == nil && v != "" {
				updates["version"] = v
			}
		}
		if n.Status != "online" {
			publishNodeStatus(n.ID, "online")
		}
		store.DB.Model(&models.MediaNode{}).Where("id = ?", n.ID).Updates(updates)
	}
}

func publishNodeStatus(nodeID, status string) {
	bus.Default.Publish(bus.Event{Type: "node.status", Data: map[string]any{
		"nodeId": nodeID, "status": status, "ts": models.NowMilli()}})
}

// firstProjectID 取首个启用项目（节点为全局资源，告警归属用）。
func firstProjectID() string {
	var p models.Project
	if store.DB.Where("enabled = ?", true).Order("created_at ASC").First(&p).Error == nil {
		return p.ID
	}
	return ""
}
