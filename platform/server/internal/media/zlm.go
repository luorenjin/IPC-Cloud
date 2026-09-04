// Package media ZLMediaKit REST API 客户端与节点调度（接入规范 §8）。
package media

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
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
	var out struct {
		Code int             `json:"code"`
		Msg  string          `json:"msg"`
		Data json.RawMessage `json:"data"`
	}
	if err := json.Unmarshal(body, &out); err != nil {
		return nil, fmt.Errorf("zlm %s bad response: %w", api, err)
	}
	if out.Code != 0 {
		return nil, fmt.Errorf("zlm %s: code=%d %s", api, out.Code, out.Msg)
	}
	data := map[string]any{}
	if len(out.Data) > 0 {
		_ = json.Unmarshal(out.Data, &data)
	}
	return data, nil
}

func (z *ZLM) AddStreamProxy(key, proxyURL string, timeoutSec int) error {
	p := url.Values{"key": {key}, "url": {proxyURL}, "timeout_sec": {fmt.Sprint(timeoutSec)},
		"enable_rtsp": {"1"}, "enable_rtmp": {"1"}, "enable_hls": {"0"}, "retry_count": {"3"}}
	_, err := z.Call(context.Background(), "addStreamProxy", p)
	return err
}

func (z *ZLM) DelStreamProxy(key string) error {
	_, err := z.Call(context.Background(), "delStreamProxy", url.Values{"key": {key}})
	return err
}

// OpenRtpServer 打开收流端口；tcp_mode 0=UDP 1=TCP 被动 2=TCP 主动。
func (z *ZLM) OpenRtpServer(ctx context.Context, port int, tcpMode int, streamID string) (int, error) {
	data, err := z.Call(ctx, "openRtpServer",
		url.Values{"port": {fmt.Sprint(port)}, "tcp_mode": {fmt.Sprint(tcpMode)}, "stream_id": {streamID}})
	if err != nil {
		return 0, err
	}
	pf, _ := data["port"].(float64)
	return int(pf), nil
}

func (z *ZLM) CloseRtpServer(streamID string) error {
	_, err := z.Call(context.Background(), "closeRtpServer", url.Values{"stream_id": {streamID}})
	return err
}

func (z *ZLM) GetMediaList(ctx context.Context) ([]map[string]any, error) {
	data, err := z.Call(ctx, "getMediaList", nil)
	if err != nil {
		return nil, err
	}
	raw, _ := json.Marshal(data["list"])
	var list []map[string]any
	_ = json.Unmarshal(raw, &list)
	return list, nil
}

func (z *ZLM) StartRecord(app, stream string, typ int) error {
	_, err := z.Call(context.Background(), "startRecord",
		url.Values{"type": {fmt.Sprint(typ)}, "vhost": {"__defaultVhost__"}, "app": {app}, "stream": {stream}})
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

// ForNode 取节点客户端。
func ForNode(n *models.MediaNode) *ZLM { return NewZLM(n.APIURL, NodeSecret(n)) }

// NodeSecret 解密节点 secret。
func NodeSecret(n *models.MediaNode) string {
	s, _ := crypto.Dec(n.SecretEnc)
	return s
}
