package engine

// 媒体节点统计与心跳巡检（SYS-01）：周期拉取 getMediaList 计算
// 流数/播放路数/带宽(in/out)，读版本，维护最近心跳；
// 拉取失败（≈30s 无心跳）置离线、发 node.status 事件并产生 node_offline 告警。

import (
	"context"
	"log"
	"time"

	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/devsvc"
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
	// 对账用中间态：reachable = 本轮成功应答的节点；liveCh = 本轮确实有流出流的通道。
	// 两者在循环结束后交给 reconcileChannelStreams，用来清理「已无流却仍标 streaming」的残留通道。
	reachable := make(map[string]bool, len(nodes))
	liveCh := make(map[string]bool)
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
		reachable[n.ID] = true
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
				// 去重后反查通道，供状态对账使用（同一路流不必重复查库）
				if ch, _, ok := devsvc.FindChannelByStreamKey(app, stream); ok {
					liveCh[ch.ID] = true
				}
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
	e.reconcileChannelStreams(reachable, liveCh)
}

// reconcileChannelStreams 通道流状态对账：把「实际已无流、却仍标 streaming」的残留通道复位为 idle。
//
// 为什么必须有这一层：channels.stream_state 只由 devsvc.SetChannelStream 写入，而它的触发者
// 全是 ZLM Hook（on_stream_changed / on_stream_none_reader / on_rtp_server_timeout）与起播流程。
// 一旦这些事件不再到达——ZLM 崩溃后重启（进程重启不会补发 regist=false）、平台自身重启
// （内存中的推流/会话状态全丢）、流被外部直接删除——就没有任何机制把状态改回去，
// 详情页「码流状态 x/y 路」会永久显示「推流中」，与实际自相矛盾。
// on_server_started 只更新节点心跳，不做通道对账，所以对账放在本巡检里统一做。
//
// 判据必须保守，避免把「确实在推流」的通道误判为未推流：
//   - liveCh   —— 本轮在可达节点上真实存在的流所对应的通道，直接跳过；
//   - reachable—— 本轮成功应答的节点；通道的粘性节点已知但本轮不可达时，
//     说明是"状态未知"（可能是平台到节点的网络抖动）而非"流没了"，同样跳过，下轮再判。
//
// 只有「节点可达/未知节点均可查、且全节点列表里都没有这路流」才复位。
//
// 不变量：本处与 hooks.onStreamChanged 用的是**同一个**反查器 devsvc.FindChannelByStreamKey。
// 也就是说「Hook 会据此置 streaming 的流」与「本处会据此判定在流的流」是同一个集合，
// 不存在"某路流能被 Hook 认出来、却在这里认不出来"的命名形态，因此不会把真实在流的
// 通道误复位（新增流命名形态时改 FindChannelByStreamKey 一处即可，两侧同时生效）。
func (e *Engine) reconcileChannelStreams(reachable, liveCh map[string]bool) {
	var chs []models.Channel
	if err := store.DB.Where("stream_state = ?", "streaming").Find(&chs).Error; err != nil {
		log.Printf("[node-stats] reconcile list: %v", err)
		return
	}
	for _, ch := range chs {
		if liveCh[ch.ID] {
			continue
		}
		if nid := e.Scheduler.StickyNode(ch.ID, "main"); nid != "" && !reachable[nid] {
			continue
		}
		if nid := e.Scheduler.StickyNode(ch.ID, "sub"); nid != "" && !reachable[nid] {
			continue
		}
		log.Printf("[node-stats] reset stale stream state: channel=%s state=streaming -> idle", ch.ID)
		devsvc.SetChannelStream(ch.ID, "idle")
		// 流已不存在，其上未结束的会话是残留：不关掉会让节点「在线通道」
		// 与总览「播放中」长期虚高（巡检正是按 ended_at = 0 统计的）。
		store.DB.Model(&models.StreamSession{}).
			Where("channel_id = ? AND ended_at = 0", ch.ID).
			Update("ended_at", models.NowMilli())
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
