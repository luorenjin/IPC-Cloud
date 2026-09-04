// api 层共享小工具。
package api

import (
	"context"
	"strings"

	"github.com/jetscam/ipccloud/server/internal/adapter/onvif"
	"github.com/jetscam/ipccloud/server/internal/adapter/rtsp"
	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/config"
	"github.com/jetscam/ipccloud/server/internal/crypto"
	"github.com/jetscam/ipccloud/server/internal/devsvc"
	"github.com/jetscam/ipccloud/server/internal/engine"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/media"
	"github.com/jetscam/ipccloud/server/internal/models"
)

var appCfg *config.Config

// InitAPI 注入全局配置。
func InitAPI(cfg *config.Config) { appCfg = cfg }

func cfg_SIPServerID() string { return appCfg.SIPServerID }
func cfg_SIPDomain() string   { return appCfg.SIPDomain }
func cfg_SIPPort() int        { return appCfg.SIPPort }

func crypto_HMAC(code string) string { return crypto.HMACSHA256Hex(appCfg.JWTSecret+":vc", strings.ToUpper(code)) }
func crypto_Enc(s string) string     { return crypto.Enc(s) }

// toAppErr 将任意错误转为 *errs.AppError（识别 "EXXXX " 前缀）。
func toAppErr(err error) *errs.AppError {
	if err == nil {
		return nil
	}
	s := err.Error()
	for _, code := range []string{"E0400", "E0403", "E1001", "E1002", "E2001", "E2002", "E3001", "E3002",
		"E4001", "E4002", "E4003", "E5001", "E6001", "E6002", "E6003", "E7001", "E7002", "E7003",
		"E7004", "E7005", "E7006", "E8001", "E8002", "E8003"} {
		if strings.HasPrefix(s, code+" ") || s == code {
			e := errs.Get(code)
			if rest := strings.TrimPrefix(strings.TrimPrefix(s, code), " "); rest != "" {
				e = e.WithMsg(rest)
			}
			return e
		}
	}
	return errs.EServerInternal.WithMsg(s)
}

// onvifProbe 包装探测。
func onvifProbe(xaddr, user, pass string) (*onvif.ProbeInfo, error) {
	return onvif.New().Probe(context.Background(), xaddr, user, pass)
}

// rtspProbe 包装探测。
func rtspProbe(u string) (map[string]any, error) {
	return rtsp.Probe(u, 8_000_000_000)
}

// profileURI 取探测出的 RTSP 地址。
func profileURI(info *onvif.ProbeInfo, idx int) string {
	if idx < len(info.Profiles) {
		return info.Profiles[idx].StreamURI
	}
	return ""
}

func mediaForNode(n *models.MediaNode) *media.ZLM { return media.ForNode(n) }

func devsvc_NodeByID(id string) (*models.MediaNode, error) { return devsvc.NodeByID(id) }

func enginePublishNodeStatus(nodeID, status string) {
	bus.Default.Publish(bus.Event{Type: "node.status", Data: map[string]any{
		"nodeId": nodeID, "status": status, "ts": models.NowMilli()}})
}

func engine_CreateAlarmEvent(projectID, deviceID, channelID, kind, level string, data map[string]any, snapshot string) string {
	return engine.CreateAlarmEvent(projectID, deviceID, channelID, kind, level, data, snapshot)
}
