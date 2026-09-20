package media

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

// fakeZLM 起一个假的 ZLM API：前 failFirst 次 openRtpServer 返回失败，之后成功。
func fakeZLM(t *testing.T, failFirst int) (*ZLM, func() (opens, closes int)) {
	t.Helper()
	var mu sync.Mutex
	opens, closes := 0, 0
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		switch {
		case strings.Contains(r.URL.Path, "openRtpServer"):
			opens++
			if opens <= failFirst {
				w.Write([]byte(`{"code":-1,"msg":"该端口已被占用"}`))
				return
			}
			w.Write([]byte(`{"code":0,"port":30001}`))
		case strings.Contains(r.URL.Path, "closeRtpServer"):
			closes++
			w.Write([]byte(`{"code":0}`))
		default:
			w.Write([]byte(`{"code":0}`))
		}
	}))
	t.Cleanup(srv.Close)
	return NewZLM(srv.URL, "test-secret"), func() (int, int) {
		mu.Lock()
		defer mu.Unlock()
		return opens, closes
	}
}

// P2-3 回归：openRtpServer 因残留收流端口失败时，必须回收后重试一次，
// 而不是把失败直接抛给用户（表现为"起播失败：openRtpServer 失败"，但重试通常就能成功）。
func TestOpenRtpServerWithRecycle(t *testing.T) {
	z, stats := fakeZLM(t, 1)

	port, err := z.OpenRtpServerWithRecycle(context.Background(), 1, "stream-a")
	if err != nil {
		t.Fatalf("回收后重试仍失败：%v", err)
	}
	if port != 30001 {
		t.Fatalf("port = %d，期望 30001", port)
	}
	opens, closes := stats()
	if opens != 2 {
		t.Fatalf("openRtpServer 调用 %d 次，期望 2 次（首次失败 + 回收后重试）", opens)
	}
	if closes != 1 {
		t.Fatalf("closeRtpServer 调用 %d 次，期望 1 次（回收残留端口）", closes)
	}
}

// 首次即成功时不应产生任何多余的关闭/重试动作。
func TestOpenRtpServerWithRecycleNoRetryOnSuccess(t *testing.T) {
	z, stats := fakeZLM(t, 0)

	if _, err := z.OpenRtpServerWithRecycle(context.Background(), 1, "stream-b"); err != nil {
		t.Fatalf("首次成功却返回错误：%v", err)
	}
	opens, closes := stats()
	if opens != 1 || closes != 0 {
		t.Fatalf("open=%d close=%d，期望 open=1 close=0", opens, closes)
	}
}

// 端口持续失败时（节点真的不可用）仍要返回错误，不能吞掉。
func TestOpenRtpServerWithRecycleKeepsError(t *testing.T) {
	z, stats := fakeZLM(t, 99)

	if _, err := z.OpenRtpServerWithRecycle(context.Background(), 1, "stream-c"); err == nil {
		t.Fatal("端口始终不可用时必须返回错误")
	}
	opens, closes := stats()
	if opens != 2 || closes != 1 {
		t.Fatalf("open=%d close=%d，期望恰好重试一次（open=2 close=1）", opens, closes)
	}
}
