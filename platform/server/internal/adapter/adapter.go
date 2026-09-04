// Package adapter 协议适配器接口与注册表。适配器对上只产出统一模型与统一事件（接入规范 §1.3）。
package adapter

import (
	"context"
	"sync"

	"github.com/jetscam/ipccloud/server/internal/models"
)

// StartOptions 起流参数。
type StartOptions struct {
	ChannelID string
	Profile   string // main/sub
	App       string // live | record
	Stream    string // 流名（§8.3）
	PushURL   string // 设备推流目标（IDP rtmp）
	PushToken string
	TTLSec    int
	Node      *models.MediaNode // 已选节点（GB INVITE 用）
	RtpPort   int               // ZLM openRtpServer 返回端口
}

// StopOptions 停流参数。
type StopOptions struct {
	ChannelID string
	App       string
	Stream    string
	Reason    string // none_reader | user_stop | device_offline
}

// RecordQuery 设备端录像检索。
type RecordQuery struct {
	ChannelID string
	Start     int64
	End       int64
	Types     []string
	Page      int
	PageSize  int
}

// PlaybackStart 回放起播。
type PlaybackStart struct {
	ChannelID string
	SessionID string
	Start     int64
	End       int64
	Speed     float64
	PushURL   string
	PushToken string
	Node      *models.MediaNode // 已选节点
	RtpPort   int               // ZLM 收流端口
}

// PlaybackCtrl 回放控制。
type PlaybackCtrl struct {
	ChannelID string
	SessionID string
	Op        string // pause|resume|seek|speed
	Speed     float64
	SeekTs    int64
	BaseTs    int64 // 回放起始时间（seek 换算基准）
}

// Adapter 统一适配器接口。所有协议差异限制在实现内。
type Adapter interface {
	Source() string // idp|gb28181|onvif|rtsp
	Start(ctx context.Context) error
	Stop()

	// 媒体
	StartStream(ctx context.Context, opt StartOptions) error
	StopStream(opt StopOptions)
	Snapshot(ctx context.Context, channelID, uploadURL string) (string, error)

	// 录像（不支持的实现返回 false, nil）
	QueryRecords(ctx context.Context, q RecordQuery) (segments []map[string]any, total int, ok bool, err error)
	StartPlayback(ctx context.Context, p PlaybackStart) error
	PlaybackCtrl(ctx context.Context, p PlaybackCtrl) error
	StopPlayback(ctx context.Context, channelID, sessionID string)

	// 设备管理
	Reboot(ctx context.Context, deviceID string) error
	Diagnose(ctx context.Context, deviceID string) ([]map[string]any, error)
	Transfer(ctx context.Context, deviceID, projectID string) error
	Unbind(ctx context.Context, deviceID string) error
}

var (
	mu       sync.RWMutex
	registry = map[string]Adapter{}
)

func Register(a Adapter) {
	mu.Lock()
	defer mu.Unlock()
	registry[a.Source()] = a
}

func Get(source string) Adapter {
	mu.RLock()
	defer mu.RUnlock()
	return registry[source]
}

func All() []Adapter {
	mu.RLock()
	defer mu.RUnlock()
	out := make([]Adapter, 0, len(registry))
	for _, a := range registry {
		out = append(out, a)
	}
	return out
}

// StopAll 停止全部适配器。
func StopAll() {
	for _, a := range All() {
		a.Stop()
	}
}