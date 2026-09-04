package media

import (
	"context"
	"fmt"
	"sync"
	"time"

	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// Scheduler 媒体节点调度：粘性映射 + 最小负载（§8.2）。
type Scheduler struct{}

func NewScheduler() *Scheduler { return &Scheduler{} }

func stickyKey(channelID, profile string) string { return fmt.Sprintf("channel:%s:%s:node", channelID, profile) }

// Pick 为通道选择节点：优先粘性，否则选健康节点中 streams/maxStreams/weight 最小者。
func (s *Scheduler) Pick(channelID, profile string) (*models.MediaNode, error) {
	ctx := context.Background()
	var nodes []models.MediaNode
	if err := store.DB.Where("status = ?", "online").Find(&nodes).Error; err != nil {
		return nil, err
	}
	byID := map[string]models.MediaNode{}
	for _, n := range nodes {
		byID[n.ID] = n
	}
	// 1. 粘性
	if sid, err := store.KVImpl.Get(ctx, stickyKey(channelID, profile)); err == nil && sid != "" {
		if n, ok := byID[sid]; ok {
			return &n, nil
		}
		_ = store.KVImpl.Del(ctx, stickyKey(channelID, profile))
	}
	if len(nodes) == 0 {
		return nil, fmt.Errorf("E4001 无可用媒体节点")
	}
	// 2. 最小负载
	best := &nodes[0]
	bestScore := loadScore(*best)
	for i := range nodes[1:] {
		sc := loadScore(nodes[i])
		if sc < bestScore {
			best, bestScore = &nodes[i], sc
		}
	}
	return best, nil
}

func loadScore(n models.MediaNode) float64 {
	max := float64(n.MaxStreams)
	if max <= 0 {
		max = 200
	}
	w := float64(n.Weight)
	if w <= 0 {
		w = 100
	}
	return float64(n.Streams) / max / w * 100
}

// SetSticky 写粘性映射。
func (s *Scheduler) SetSticky(channelID, profile, nodeID string) {
	_ = store.KVImpl.Set(context.Background(), stickyKey(channelID, profile), nodeID, 24*time.Hour)
}

// ClearSticky 清除粘性映射。
func (s *Scheduler) ClearSticky(channelID, profile string) {
	_ = store.KVImpl.Del(context.Background(), stickyKey(channelID, profile))
}

// NodeOffline 清理节点全部映射。
func (s *Scheduler) NodeOffline(nodeID string) {
	ctx := context.Background()
	var channels []models.Channel
	store.DB.Select("id").Find(&channels)
	for _, ch := range channels {
		if sid, _ := store.KVImpl.Get(ctx, stickyKey(ch.ID, "main")); sid == nodeID {
			store.KVImpl.Del(ctx, stickyKey(ch.ID, "main"))
		}
		if sid, _ := store.KVImpl.Get(ctx, stickyKey(ch.ID, "sub")); sid == nodeID {
			store.KVImpl.Del(ctx, stickyKey(ch.ID, "sub"))
		}
	}
}

var nodeCountMu sync.Mutex

// NodeStreamsAdd 调整节点流计数（Hook 驱动）。
func NodeStreamsAdd(nodeID string, delta int) {
	nodeCountMu.Lock()
	defer nodeCountMu.Unlock()
	var n models.MediaNode
	if store.DB.First(&n, "id = ?", nodeID).Error != nil {
		return
	}
	n.Streams += delta
	if n.Streams < 0 {
		n.Streams = 0
	}
	store.DB.Model(&n).Update("streams", n.Streams)
}
