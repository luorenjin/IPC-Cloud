package rtsp

import (
	"errors"
	"testing"
)

// 历史 bug：探测成功（err == nil）时诊断结果组装会 panic，
// 因为 map[bool]string{true: "", false: err.Error()} 的两个 value 都会先求值。
// 该 panic 会击穿整个 HTTP handler 导致服务进程崩溃。
func TestDiagnoseItem_探测成功不panic(t *testing.T) {
	defer func() {
		if r := recover(); r != nil {
			t.Fatalf("探测成功时不应 panic，实际 panic: %v", r)
		}
	}()

	it := diagnoseItem("rtsp", nil, 12)
	if it["ok"] != true {
		t.Fatalf("err 为 nil 时 ok 应为 true，实际 %v", it["ok"])
	}
	if it["msg"] != "" {
		t.Fatalf("成功时 msg 应为空，实际 %q", it["msg"])
	}
	if it["cost"] != int64(12) {
		t.Fatalf("cost 应透传耗时，实际 %v", it["cost"])
	}
	if it["item"] != "rtsp" {
		t.Fatalf("item 应为 rtsp，实际 %v", it["item"])
	}
}

func TestDiagnoseItem_探测失败带出原因(t *testing.T) {
	it := diagnoseItem("rtsp", errors.New("连接被拒绝"), 5000)
	if it["ok"] != false {
		t.Fatalf("err 非 nil 时 ok 应为 false")
	}
	if it["msg"] != "连接被拒绝" {
		t.Fatalf("失败时 msg 应为错误原因，实际 %q", it["msg"])
	}
}
