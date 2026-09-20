package media

import (
	"testing"

	"github.com/jetscam/ipccloud/server/internal/models"
)

// 历史 bug：`for i := range nodes[1:]` 导致索引错位且最后一个节点永不参与比较。
// 这里覆盖"最优节点位于末位"这一必然暴露该 bug 的排布。
func TestPickLeastLoaded_最后一个节点也参与调度(t *testing.T) {
	nodes := []models.MediaNode{
		{ID: "n1", Streams: 90, MaxStreams: 100, Weight: 100},
		{ID: "n2", Streams: 50, MaxStreams: 100, Weight: 100},
		{ID: "n3", Streams: 1, MaxStreams: 100, Weight: 100}, // 最空闲，且在末位
	}
	if got := pickLeastLoaded(nodes); got == nil || got.ID != "n3" {
		id := "<nil>"
		if got != nil {
			id = got.ID
		}
		t.Fatalf("应选中末位的最空闲节点 n3，实际选中 %s", id)
	}
}

func TestPickLeastLoaded_首位最优时也正确(t *testing.T) {
	nodes := []models.MediaNode{
		{ID: "n1", Streams: 1, MaxStreams: 100, Weight: 100},
		{ID: "n2", Streams: 80, MaxStreams: 100, Weight: 100},
	}
	if got := pickLeastLoaded(nodes); got == nil || got.ID != "n1" {
		t.Fatalf("应选中 n1")
	}
}

func TestPickLeastLoaded_单节点与空集(t *testing.T) {
	one := []models.MediaNode{{ID: "only", Streams: 999, MaxStreams: 1, Weight: 1}}
	if got := pickLeastLoaded(one); got == nil || got.ID != "only" {
		t.Fatalf("单节点应被选中，即使已过载")
	}
	if got := pickLeastLoaded(nil); got != nil {
		t.Fatalf("空集应返回 nil")
	}
}

// 权重越高越优先分担负载：同样的流数下，高权重节点得分更低。
func TestPickLeastLoaded_权重影响选择(t *testing.T) {
	nodes := []models.MediaNode{
		{ID: "low", Streams: 10, MaxStreams: 100, Weight: 10},
		{ID: "high", Streams: 10, MaxStreams: 100, Weight: 100},
	}
	if got := pickLeastLoaded(nodes); got == nil || got.ID != "high" {
		t.Fatalf("同流数下应优先选权重更高的节点")
	}
}

// loadScore 对未配置的 MaxStreams/Weight 应使用缺省值而非除零。
func TestLoadScore_零值不产生NaN或Inf(t *testing.T) {
	s := loadScore(models.MediaNode{ID: "n", Streams: 10, MaxStreams: 0, Weight: 0})
	if s != s { // NaN
		t.Fatalf("得分不应为 NaN")
	}
	if s <= 0 {
		t.Fatalf("得分应为正数，实际 %v", s)
	}
}
