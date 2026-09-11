// Package media ZLMediaKit REST API 客户端与节点调度（接入规范 §8）。
package media

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/jetscam/ipccloud/server/internal/crypto"
	"github.com/jetscam/ipccloud/server/internal/models"
)

// ZLM 单节点的 API 客户端。
type ZLM struct {
	apiURL string // http://host:port
	secret string
	hc     *http.Client
}

func NewZLM(apiURL, secret string) *ZLM {
	return &ZLM{apiURL: strings.TrimRight(apiURL, "/"), secret: secret,
		hc: &http.Client{Timeout: 8 * time.Second}}
}

// Call 调用 ZLM API，返回解析后的 data（失败返回 error）。
func (z *ZLM) Call(ctx context.Context, api string, params url.Values) (map[string]any, error) {
	top, err := z.callTop(ctx, api, params)
	if err != nil {
		return nil, err
	}
	data := map[string]any{}
	if raw, ok := top["data"]; ok {
		if b, err := json.Marshal(raw); err == nil {
			_ = json.Unmarshal(b, &data)
		}
	}
	return data, nil
}

// callTop 调用 ZLM API 并返回完整顶层响应。
// 部分接口的业务字段不在 data 内（如 openRtpServer 的 port 为顶层字段）。
func (z *ZLM) callTop(ctx context.Context, api string, params url.Values) (map[string]any, error) {
	if params == nil {
		params = url.Values{}
	}
	params.Set("secret", z.secret)
	u := z.apiURL + "/index/api/" + api + "?" + params.Encode()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return nil, err
	}
	resp, err := z.hc.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	var out map[string]any
	if err := json.Unmarshal(body, &out); err != nil {
		return nil, fmt.Errorf("zlm %s bad response: %w", api, err)
	}
	if cf, _ := out["code"].(float64); cf != 0 {
		msg, _ := out["msg"].(string)
		return nil, fmt.Errorf("zlm %s: code=%v %s", api, out["code"], msg)
	}
	return out, nil
}

// AddStreamProxy 创建拉流代理。key 格式 __defaultVhost__/<app>/<stream>；
// 新版 ZLM 要求 vhost/app/stream/url 为独立参数（不再接受 key）。
func (z *ZLM) AddStreamProxy(key, proxyURL string, timeoutSec int) error {
	parts := strings.Split(key, "/")
	if len(parts) != 3 {
		return fmt.Errorf("bad proxy key %s", key)
	}
	p := url.Values{"vhost": {parts[0]}, "app": {parts[1]}, "stream": {parts[2]}, "url": {proxyURL},
		"timeout_sec": {fmt.Sprint(timeoutSec)},
		"enable_rtsp": {"1"}, "enable_rtmp": {"1"}, "enable_hls": {"0"}, "retry_count": {"3"}}
	if _, err := z.Call(context.Background(), "addStreamProxy", p); err != nil {
		// 同 key 代理已存在视为成功（幂等）：key 由通道/码流决定，URL 一致
		if !strings.Contains(err.Error(), "already exists") {
			return err
		}
	}
	return nil
}

func (z *ZLM) DelStreamProxy(key string) error {
	_, err := z.Call(context.Background(), "delStreamProxy", url.Values{"key": {key}})
	return err
}

// OpenRtpServer 打开收流端口；tcp_mode 0=UDP 1=TCP 被动 2=TCP 主动。
// 注意：port 在 ZLM 响应顶层而非 data 内，须用 callTop。
func (z *ZLM) OpenRtpServer(ctx context.Context, port int, tcpMode int, streamID string) (int, error) {
	top, err := z.callTop(ctx, "openRtpServer",
		url.Values{"port": {fmt.Sprint(port)}, "tcp_mode": {fmt.Sprint(tcpMode)}, "stream_id": {streamID}})
	if err != nil {
		return 0, err
	}
	pf, _ := top["port"].(float64)
	return int(pf), nil
}

func (z *ZLM) CloseRtpServer(streamID string) error {
	_, err := z.Call(context.Background(), "closeRtpServer", url.Values{"stream_id": {streamID}})
	return err
}

// OpenRtpServerWithRecycle 打开 GB28181 收流端口；失败时先回收同名残留端口再重试一次。
//
// openRtpServer 失败最常见的原因不是节点不可达，而是上一次会话异常结束（INVITE 超时、
// 设备掉线、节点重启）导致同名流的收流端口没被回收——此时直接返回失败会让用户看到
// "起播失败：openRtpServer 失败"，而回收后重试通常立刻成功。
func (z *ZLM) OpenRtpServerWithRecycle(ctx context.Context, tcpMode int, streamID string) (int, error) {
	port, err := z.OpenRtpServer(ctx, 0, tcpMode, streamID)
	if err == nil {
		return port, nil
	}
	log.Printf("[zlm] openRtpServer(%s) 失败：%v；回收残留收流端口后重试", streamID, err)
	if cerr := z.CloseRtpServer(streamID); cerr != nil {
		log.Printf("[zlm] closeRtpServer(%s) 回收失败：%v", streamID, cerr)
	}
	return z.OpenRtpServer(ctx, 0, tcpMode, streamID)
}

func (z *ZLM) GetMediaList(ctx context.Context) ([]map[string]any, error) {
	// getMediaList 的 data 是顶层数组（无 "list" 包装），须经 callTop 读取
	top, err := z.callTop(ctx, "getMediaList", nil)
	if err != nil {
		return nil, err
	}
	raw, _ := json.Marshal(top["data"])
	var list []map[string]any
	_ = json.Unmarshal(raw, &list)
	return list, nil
}

// ZLM startRecord/stopRecord 的 type 参数：0=HLS，1=MP4（注意不是"自动/手动"）。
const (
	RecordTypeHLS = 0
	RecordTypeMP4 = 1
)

// StartRecord 启动录制（typ 取 RecordTypeMP4/RecordTypeHLS）。
// max_second 控制分段时长：分段完成才触发 on_record_mp4 落库回放索引；
// config.ini 的 fileSecond 键名无效（正确键为 mp4_max_second），故经 API 显式指定。
func (z *ZLM) StartRecord(app, stream string, typ int) error {
	_, err := z.Call(context.Background(), "startRecord",
		url.Values{"type": {fmt.Sprint(typ)}, "vhost": {"__defaultVhost__"},
			"app": {app}, "stream": {stream}, "max_second": {"60"}})
	return err
}

func (z *ZLM) StopRecord(app, stream string, typ int) error {
	_, err := z.Call(context.Background(), "stopRecord",
		url.Values{"type": {fmt.Sprint(typ)}, "vhost": {"__defaultVhost__"}, "app": {app}, "stream": {stream}})
	return err
}

// GetSnap 抓图返回 JPEG 字节。
func (z *ZLM) GetSnap(rawURL string, timeoutSec int) ([]byte, error) {
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(timeoutSec+2)*time.Second)
	defer cancel()
	p := url.Values{"url": {rawURL}, "timeout_sec": {fmt.Sprint(timeoutSec)}, "expire_sec": {"1"}}
	p.Set("secret", z.secret)
	u := z.apiURL + "/index/api/getSnap?" + p.Encode()
	req, _ := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	resp, err := z.hc.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("snap http %d", resp.StatusCode)
	}
	return io.ReadAll(resp.Body)
}

// KickSessions 踢掉观看者（节点详情用）。
func (z *ZLM) KickSession(id string) error {
	_, err := z.Call(context.Background(), "kick_session", url.Values{"id": {id}})
	return err
}

// CloseStreams 强制关闭媒体源（SYS-02 踢流：断开推流并释放收流资源）。
func (z *ZLM) CloseStreams(app, stream string) error {
	_, err := z.Call(context.Background(), "close_streams",
		url.Values{"vhost": {"__defaultVhost__"}, "app": {app}, "stream": {stream}, "force": {"1"}})
	return err
}

// Version 读取 ZLM 版本（HTTP 响应 Server 头，无独立 getVersion API）。
func (z *ZLM) Version(ctx context.Context) (string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, z.apiURL+"/index/api/getServerConfig?secret="+url.QueryEscape(z.secret), nil)
	if err != nil {
		return "", err
	}
	resp, err := z.hc.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 1<<10))
	srv := resp.Header.Get("Server")
	if i := strings.Index(srv, "("); i >= 0 && strings.HasSuffix(srv, ")") {
		return srv[i+1 : len(srv)-1], nil
	}
	return srv, nil
}

// ForNode 取节点客户端。
func ForNode(n *models.MediaNode) *ZLM { return NewZLM(n.APIURL, NodeSecret(n)) }

// NodeSecret 解密节点 secret。
func NodeSecret(n *models.MediaNode) string {
	s, _ := crypto.Dec(n.SecretEnc)
	return s
}
