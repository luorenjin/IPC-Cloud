// Package rtsp RTSP 直连适配器（接入规范 §7.3）：探测与状态检查；取流经 ZLM addStreamProxy。
package rtsp

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"net/url"
	"strings"
	"time"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

const Source = "rtsp"

type Adapter struct{}

func New() *Adapter { return &Adapter{} }

func (a *Adapter) Source() string { return Source }
func (a *Adapter) Start(ctx context.Context) error {
	go a.statusWatcher(ctx)
	return nil
}
func (a *Adapter) Stop() {}

// Probe 探测 RTSP 地址（OPTIONS/DESCRIBE），返回 SDP 摘要。
func Probe(rawURL string, timeout time.Duration) (map[string]any, error) {
	u, err := url.Parse(rawURL)
	if err != nil || u.Host == "" {
		return nil, errs.EBadRequest.WithMsg("RTSP 地址无效")
	}
	if u.Port() == "" {
		u.Host = u.Host + ":554"
	}
	conn, err := net.DialTimeout("tcp", u.Host, timeout)
	if err != nil {
		return nil, errs.EUnreachable
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(timeout))

	send := func(req string) (map[string]string, string, error) {
		if _, err := conn.Write([]byte(req + "\r\n")); err != nil {
			return nil, "", err
		}
		r := bufio.NewReader(conn)
		hdrs := map[string]string{}
		statusLine, err := r.ReadString('\n')
		if err != nil {
			return nil, "", err
		}
		var body strings.Builder
		clen := 0
		for {
			line, err := r.ReadString('\n')
			if err != nil {
				return nil, "", err
			}
			line = strings.TrimRight(line, "\r\n")
			if line == "" {
				break
			}
			if i := strings.Index(line, ":"); i > 0 {
				hdrs[strings.ToLower(strings.TrimSpace(line[:i]))] = strings.TrimSpace(line[i+1:])
			}
		}
		if v, ok := hdrs["content-length"]; ok {
			fmt.Sscanf(v, "%d", &clen)
			buf := make([]byte, clen)
			_, _ = readFull(r, buf)
			body.Write(buf)
		}
		if !strings.Contains(statusLine, "200") {
			return hdrs, body.String(), errs.EProtocolError.WithMsg("RTSP 响应 " + strings.TrimSpace(statusLine))
		}
		return hdrs, body.String(), nil
	}

	path := u.Path
	if path == "" {
		path = "/"
	}
	opt := fmt.Sprintf("OPTIONS rtsp://%s%s RTSP/1.0\r\nCSeq: 1\r\n\r\n", u.Host, path)
	if _, _, err := send(opt); err != nil {
		return nil, err
	}
	desc := fmt.Sprintf("DESCRIBE rtsp://%s%s RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n", u.Host, path)
	_, body, err := send(desc)
	if err != nil {
		return nil, err
	}
	codec := ""
	switch {
	case strings.Contains(body, "sprop-vps") || strings.Contains(body, "H265") || strings.Contains(body, "h265"):
		codec = "h265"
	case strings.Contains(body, "H264") || strings.Contains(body, "h264"):
		codec = "h264"
	}
	return map[string]any{"codec": codec, "sdp": body}, nil
}

func readFull(r *bufio.Reader, buf []byte) (int, error) {
	n := 0
	for n < len(buf) {
		m, err := r.Read(buf[n:])
		n += m
		if err != nil {
			return n, err
		}
	}
	return n, nil
}

// ---------- Adapter 接口 ----------

func (a *Adapter) StartStream(ctx context.Context, opt adapter.StartOptions) error {
	return nil // 由 engine addStreamProxy 拉流
}
func (a *Adapter) StopStream(opt adapter.StopOptions) {}

func (a *Adapter) Snapshot(ctx context.Context, channelID, uploadURL string) (string, error) {
	return "", errs.EForbid.WithMsg("请经 ZLM 抓图")
}

func (a *Adapter) QueryRecords(ctx context.Context, q adapter.RecordQuery) ([]map[string]any, int, bool, error) {
	return nil, 0, false, nil
}
func (a *Adapter) StartPlayback(ctx context.Context, p adapter.PlaybackStart) error {
	return errs.EGBRecordUnsup
}
func (a *Adapter) PlaybackCtrl(ctx context.Context, p adapter.PlaybackCtrl) error {
	return errs.EGBRecordUnsup
}
func (a *Adapter) StopPlayback(ctx context.Context, channelID, sessionID string) {}
func (a *Adapter) Reboot(ctx context.Context, deviceID string) error {
	return errs.EForbid.WithMsg("RTSP 源不支持远程重启")
}
func (a *Adapter) Diagnose(ctx context.Context, deviceID string) ([]map[string]any, error) {
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", deviceID).Error; err != nil {
		return nil, errs.ENotFound
	}
	uri, _ := dev.Identity["url"].(string)
	_, err := Probe(uri, 5*time.Second)
	ok := err == nil
	return []map[string]any{
		{"item": "rtsp", "ok": ok, "cost": 0, "msg": map[bool]string{true: "", false: err.Error()}[ok]},
	}, nil
}
func (a *Adapter) Transfer(ctx context.Context, deviceID, projectID string) error { return nil }
func (a *Adapter) Unbind(ctx context.Context, deviceID string) error              { return nil }

// statusWatcher 30s 周期探测。
func (a *Adapter) statusWatcher(ctx context.Context) {
	t := time.NewTicker(30 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			var devs []models.Device
			store.DB.Where("source = ? AND status IN ('online','offline') AND deleted_at = 0", Source).Find(&devs)
			for _, d := range devs {
				uri, _ := d.Identity["url"].(string)
				_, err := Probe(uri, 4*time.Second)
				status := "online"
				if err != nil {
					status = "offline"
				}
				if status != d.Status {
					store.DB.Model(&models.Device{}).Where("id = ?", d.ID).
						Updates(map[string]any{"status": status, "last_seen_at": models.NowMilli()})
				} else {
					store.DB.Model(&models.Device{}).Where("id = ?", d.ID).Update("last_seen_at", models.NowMilli())
				}
			}
		}
	}
}
