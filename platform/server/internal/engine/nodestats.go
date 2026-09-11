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
		// getMediaList 对同一路流按输出协议（ts/rtmp/fmp4/rtsp）各返回一条记录，
		// 直接遍历会把「流数 / 入带宽」重复放大数倍（实测 1 路流会被算成 4）。按 app/stream 去重：
		//   streams —— 唯一流数（与 Hook 驱动的 media.NodeStreamsAdd 保持同一口径）
		//   bwIn    —— 源流码率，各协议上报的是同一个源速率，只取一条
		//   bwOut   —— 各协议各自的观看者下行之和
		seenStream := make(map[string]bool, len(list))
		var streams int
		var bwIn, bwOut int64
		for _, m := range list {
			app, _ := m["app"].(string)
			stream, _ := m["stream"].(string)
			bs := int64(toF(m["bytesSpeed"]))
			rc := int(toF(m["readerCount"]))
			if key := app + "\x00" + stream; !seenStream[key] {
				seenStream[key] = true
				streams++
				bwIn += bs
			}
			bwOut += bs * int64(rc)
		}
		// 「在线通道」取该节点上有未结束会话的通道数（去重），而不是 readerCount 之和：
		// 后者是"观看者数"，没有观众时恒为 0，会造成总览「播放中 1」而节点「在线通道 0」的矛盾。
		var playing int64
		store.DB.Model(&models.StreamSession{}).
			Where("node_id = ? AND ended_at = 0", n.ID).
			Distinct("channel_id").Count(&playing)
		updates := map[string]any{
			"status": "online", "last_keepalive": models.NowMilli(),
			"streams": streams, "playing": int(playing), "bw_in": bwIn, "bw_out": bwOut,
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
