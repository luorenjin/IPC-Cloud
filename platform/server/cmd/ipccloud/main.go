// IpcCloud 平台服务端入口。
package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/adapter/gb28181"
	"github.com/jetscam/ipccloud/server/internal/adapter/idp"
	"github.com/jetscam/ipccloud/server/internal/adapter/onvif"
	"github.com/jetscam/ipccloud/server/internal/adapter/rtsp"
	"github.com/jetscam/ipccloud/server/internal/api"
	"github.com/jetscam/ipccloud/server/internal/auth"
	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/config"
	"github.com/jetscam/ipccloud/server/internal/crypto"
	"github.com/jetscam/ipccloud/server/internal/engine"
	"github.com/jetscam/ipccloud/server/internal/store"
	"github.com/jetscam/ipccloud/server/internal/task"
	"github.com/jetscam/ipccloud/server/internal/wshub"
)

func main() {
	cfg := config.Load()
	crypto.SetMasterKey(cfg.EncryptionKey)
	auth.Init(cfg)
	store.Open(cfg)
	store.OpenRedis(cfg)
	store.Seed(cfg)

	_ = os.MkdirAll(cfg.DataDir, 0o755)
	gin.SetMode(gin.ReleaseMode)

	// 事件总线 → WS
	hub := wshub.New()
	hub.Bridge(bus.Default)

	// 引擎（起播编排 + Hook + 告警）
	eng := engine.New(cfg)
	api.SetEngine(eng)
	eng.SubscribeEvents()
	eng.StartRecordRunner()
	eng.StartNodeStatsRunner()

	// 任务中心生命周期治理（P-18）：回收僵尸任务 + 清理过期任务
	task.StartGC()

	// 适配器
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	mustStart := func(a adapter.Adapter, name string) {
		if err := a.Start(ctx); err != nil {
			log.Printf("[adapter] %s start: %v", name, err)
		}
	}
	idpAdapter := idp.New(cfg)
	adapter.Register(idpAdapter)
	mustStart(idpAdapter, "idp")

	gbAdapter := gb28181.New(cfg)
	adapter.Register(gbAdapter)
	mustStart(gbAdapter, "gb28181")

	onvifAdapter := onvif.New()
	adapter.Register(onvifAdapter)
	mustStart(onvifAdapter, "onvif")

	rtspAdapter := rtsp.New()
	adapter.Register(rtspAdapter)
	mustStart(rtspAdapter, "rtsp")

	api.InitAPI(cfg)

	// HTTP
	srv := api.Router(hub)
	log.Printf("IpcCloud server listening on %s", cfg.HTTPAddr)
	go func() {
		if err := srv.Run(cfg.HTTPAddr); err != nil {
			log.Fatalf("http server: %v", err)
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit
	log.Printf("shutting down...")
	cancel()
	adapter.StopAll()
	time.Sleep(300 * time.Millisecond)
}
