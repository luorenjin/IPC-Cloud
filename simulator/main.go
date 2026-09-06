// ipcsim IPC 终端模拟器：一键拉起 IDP / GB28181 / ONVIF / RTSP 四种协议模拟设备，
// 配合 IpcCloud 平台完成 接入-绑定-在线-预览-回放-告警 管理闭环。
//
// 用法（参数可由 SIM_* 环境变量提供默认值）：
//
//	ipcsim -mode all -broker tcp://127.0.0.1:1883 -platform http://127.0.0.1:8080 \
//	       -sip-host 127.0.0.1 -sip-port 5060 -adv-host 192.168.1.10 -assets ./assets
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"syscall"
	"time"

	"github.com/jetscam/ipccloud/simulator/gb"
	"github.com/jetscam/ipccloud/simulator/idp"
	"github.com/jetscam/ipccloud/simulator/mediagen"
	"github.com/jetscam/ipccloud/simulator/onvif"
	"github.com/jetscam/ipccloud/simulator/rtsp"
)

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func envOrInt(key string, def int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

// detectLocalIP 探测本机对外 IP（容器内返回容器 IP）。
func detectLocalIP() string {
	conn, err := net.Dial("udp", "8.8.8.8:80")
	if err == nil {
		defer conn.Close()
		if addr, ok := conn.LocalAddr().(*net.UDPAddr); ok {
			return addr.IP.String()
		}
	}
	addrs, _ := net.InterfaceAddrs()
	for _, a := range addrs {
		if ipn, ok := a.(*net.IPNet); ok && !ipn.IP.IsLoopback() && ipn.IP.To4() != nil {
			return ipn.IP.String()
		}
	}
	return "127.0.0.1"
}

func main() {
	var (
		mode      = flag.String("mode", envOr("SIM_MODE", "all"), "运行模式：all|idp|gb|onvif|rtsp")
		broker    = flag.String("broker", envOr("SIM_BROKER", "tcp://127.0.0.1:1883"), "MQTT broker（IDP）")
		platform  = flag.String("platform", envOr("SIM_PLATFORM", "http://127.0.0.1:8080"), "平台 HTTP 地址（快照上传）")
		sipHost   = flag.String("sip-host", envOr("SIM_SIP_HOST", "127.0.0.1"), "GB28181 SIP 服务器地址")
		sipPort   = flag.Int("sip-port", envOrInt("SIM_SIP_PORT", 5060), "GB28181 SIP 服务器端口")
		advHost   = flag.String("adv-host", envOr("SIM_ADV_HOST", ""), "对外通告地址（ONVIF StreamUri/XAddrs；默认自动探测）")
		assets    = flag.String("assets", envOr("SIM_ASSETS", "assets"), "资产目录（testsrc.h264/frame.jpg）")
		idpID     = flag.String("idp-id", envOr("SIM_IDP_ID", "210235BJSNJC00001"), "IDP 设备 ID（17 位）")
		gbID      = flag.String("gb-id", envOr("SIM_GB_ID", "34020000001320000001"), "GB28181 设备编号（20 位）")
		gbPwd     = flag.String("gb-pwd", envOr("SIM_GB_PWD", "ipccloud-gb-pass"), "GB28181 注册密码")
		onvifPort = flag.Int("onvif-port", envOrInt("SIM_ONVIF_PORT", 8899), "ONVIF Device Service 端口")
		rtspPort  = flag.Int("rtsp-port", envOrInt("SIM_RTSP_PORT", 8554), "RTSP 服务端口")
		onvifUser = flag.String("onvif-user", envOr("SIM_ONVIF_USER", "admin"), "ONVIF 用户名")
		onvifPass = flag.String("onvif-pass", envOr("SIM_ONVIF_PASS", "admin123"), "ONVIF 密码")
		alarmSec  = flag.Int("alarm-sec", envOrInt("SIM_ALARM_SEC", 120), "模拟告警上报周期秒（0 关闭）")
	)
	flag.Parse()

	localIP := detectLocalIP()
	advert := *advHost
	if advert == "" {
		advert = localIP
	}

	src, err := mediagen.NewSource(filepath.Join(*assets, "testsrc.h264"), 25)
	if err != nil {
		log.Fatalf("[ipcsim] load h264 asset: %v", err)
	}
	snapshot, _ := os.ReadFile(filepath.Join(*assets, "frame.jpg"))

	var stops []func()
	run := func(name string, start func() error, stop func()) {
		if err := start(); err != nil {
			log.Fatalf("[ipcsim] %s start: %v", name, err)
		}
		log.Printf("[ipcsim] %s started", name)
		stops = append(stops, stop)
	}

	// ---------- RTSP 拉流源（ONVIF/RTSP 直连设备的媒体源） ----------
	needRTSP := *mode == "all" || *mode == "onvif" || *mode == "rtsp"
	if needRTSP {
		srv := rtsp.New(fmt.Sprintf(":%d", *rtspPort),
			&rtsp.Track{Path: "/onvif1", Source: src, Width: 1280, Height: 720},
			&rtsp.Track{Path: "/onvif2", Source: src, Width: 1280, Height: 720},
			&rtsp.Track{Path: "/live1", Source: src, Width: 1280, Height: 720},
			&rtsp.Track{Path: "/live2", Source: src, Width: 1280, Height: 720},
		)
		srv.Advertise = advert // SDP Content-Base 需要可达 host，否则客户端 DESCRIBE 后即断开
		go func() {
			if err := srv.ListenAndServe(); err != nil {
				log.Printf("[ipcsim] rtsp server: %v", err)
			}
		}()
		log.Printf("[ipcsim] rtsp server on :%d (tracks /onvif1 /onvif2 /live1 /live2)", *rtspPort)
	}

	// ---------- ONVIF 设备（Device Service + WS-Discovery） ----------
	if *mode == "all" || *mode == "onvif" {
		dev := &onvif.Device{
			LocalIP: advert, HTTPPort: *onvifPort, RTSPPort: *rtspPort,
			User: *onvifUser, Password: *onvifPass, Serial: "SIM-ONVIF-0001",
		}
		run("onvif", dev.Start, dev.Stop)
	}

	// ---------- IDP 设备（MQTT 生命周期 + RTMP 推流） ----------
	var idpDev *idp.Device
	if *mode == "all" || *mode == "idp" {
		idpDev = &idp.Device{
			ID: *idpID, Broker: *broker, Source: src,
			PlatformAddr: *platform, SnapshotJPEG: snapshot,
		}
		run("idp", idpDev.Start, idpDev.Stop)
	}

	// ---------- GB28181 设备（SIP UA + PS/RTP 推流） ----------
	var gbDev *gb.Device
	if *mode == "all" || *mode == "gb" {
		gbDev = &gb.Device{
			GBID: *gbID, Password: *gbPwd,
			ServerHost: *sipHost, ServerPort: *sipPort,
			LocalIP: localIP, Source: src,
		}
		run("gb28181", gbDev.Start, gbDev.Stop)
	}

	// ---------- 周期模拟告警 ----------
	if *alarmSec > 0 {
		go func() {
			t := time.NewTicker(time.Duration(*alarmSec) * time.Second)
			defer t.Stop()
			for range t.C {
				if idpDev != nil {
					idpDev.EmitMotion()
					log.Printf("[ipcsim] idp motion alarm sent")
				}
				if gbDev != nil {
					gbDev.EmitAlarm()
					log.Printf("[ipcsim] gb alarm sent")
				}
			}
		}()
	}

	log.Printf("[ipcsim] mode=%s localIP=%s advert=%s — Ctrl+C 退出", *mode, localIP, advert)
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit
	for _, s := range stops {
		s()
	}
	log.Printf("[ipcsim] bye")
}
