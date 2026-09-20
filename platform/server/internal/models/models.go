// Package models 数据模型（PRD §7）。除行级时间戳外，业务时间一律为 UTC 毫秒时间戳。
package models

import (
	"database/sql/driver"
	"encoding/json"
	"errors"
	"strings"
	"time"
)

// JSONB 通用 jsonb 映射。
type JSONB map[string]any

func (j JSONB) Value() (driver.Value, error) {
	if j == nil {
		return "{}", nil
	}
	b, err := json.Marshal(j)
	return string(b), err
}

func (j *JSONB) Scan(v any) error {
	switch x := v.(type) {
	case []byte:
		return json.Unmarshal(x, &j)
	case string:
		return json.Unmarshal([]byte(x), &j)
	case nil:
		*j = JSONB{}
		return nil
	}
	return errors.New("unsupported jsonb type")
}

// StringSlice text[] 映射。
type StringSlice []string

// encodePGTextArray 编码为 Postgres 数组字面量（{"a","b"}，含引号/反斜杠转义）。
func encodePGTextArray(items []string) string {
	var b strings.Builder
	b.WriteByte('{')
	for i, v := range items {
		if i > 0 {
			b.WriteByte(',')
		}
		b.WriteByte('"')
		for _, r := range v {
			if r == '"' || r == '\\' {
				b.WriteByte('\\')
			}
			b.WriteRune(r)
		}
		b.WriteByte('"')
	}
	b.WriteByte('}')
	return b.String()
}

// decodePGTextArray 解析 Postgres 数组字面量；兼容 JSON 数组格式。
func decodePGTextArray(s string) ([]string, error) {
	s = strings.TrimSpace(s)
	if s == "" || s == "{}" || s == "NULL" {
		return nil, nil
	}
	if s[0] != '{' {
		var out []string
		if err := json.Unmarshal([]byte(s), &out); err != nil {
			return nil, err
		}
		return out, nil
	}
	inner := s[1 : len(s)-1]
	var out []string
	var cur strings.Builder
	inQuote, escaped := false, false
	for i := 0; i < len(inner); i++ {
		ch := inner[i]
		switch {
		case escaped:
			cur.WriteByte(ch)
			escaped = false
		case ch == '\\':
			escaped = true
		case ch == '"':
			inQuote = !inQuote
		case ch == ',' && !inQuote:
			out = append(out, cur.String())
			cur.Reset()
		default:
			cur.WriteByte(ch)
		}
	}
	out = append(out, cur.String())
	return out, nil
}

func (s StringSlice) Value() (driver.Value, error) {
	if s == nil {
		return "{}", nil
	}
	return encodePGTextArray(s), nil
}

func (s *StringSlice) Scan(v any) error {
	switch x := v.(type) {
	case []byte:
		parsed, err := decodePGTextArray(string(x))
		if err != nil {
			return err
		}
		*s = parsed
		return nil
	case string:
		parsed, err := decodePGTextArray(x)
		if err != nil {
			return err
		}
		*s = parsed
		return nil
	case nil:
		*s = nil
		return nil
	}
	return errors.New("unsupported text[] type")
}

func NowMilli() int64 { return time.Now().UnixMilli() }

// ---------- 组织与权限 ----------

type Tenant struct {
	ID   string `gorm:"primaryKey;size:40" json:"id"`
	Name string `gorm:"size:128" json:"name"`
}

type Project struct {
	ID        string `gorm:"primaryKey;size:40" json:"id"`
	TenantID  string `gorm:"size:40" json:"tenantId"`
	Name      string `gorm:"size:128" json:"name"`
	TZ        string `gorm:"size:64;default:Asia/Shanghai" json:"tz"`
	Settings  JSONB  `gorm:"type:jsonb" json:"settings"`
	Enabled   bool   `gorm:"default:true" json:"enabled"`
	CreatedAt int64  `json:"createdAt"`
	UpdatedAt int64  `json:"updatedAt"`
	SetupDone bool   `gorm:"default:false" json:"setupDone"` // ACC-02 首次向导是否完成
}

type DeviceGroup struct {
	ID        string `gorm:"primaryKey;size:40" json:"id"`
	ProjectID string `gorm:"index;size:40" json:"projectId"`
	ParentID  string `gorm:"size:40;default:" json:"parentId"`
	Name      string `gorm:"size:128" json:"name"`
	Sort      int    `gorm:"default:0" json:"sort"`
	CreatedAt int64  `json:"createdAt"`
}

// Role 权限矩阵 + 资源范围（PRD ACC-05）。
type Role struct {
	ID        string `gorm:"primaryKey;size:40" json:"id"`
	ProjectID string `gorm:"index;size:40" json:"projectId"`
	Name      string `gorm:"size:64" json:"name"`
	Builtin   bool   `gorm:"default:false" json:"builtin"` // 超级管理员不可删改
	Perms     JSONB  `gorm:"type:jsonb" json:"perms"`      // {"menus":[],"actions":[]}
	Scope     JSONB  `gorm:"type:jsonb" json:"scope"`      // {"groups":[],"channels":[]} 空=全部
	CreatedAt int64  `json:"createdAt"`
	UpdatedAt int64  `json:"updatedAt"`
}

type User struct {
	ID        string `gorm:"primaryKey;size:40" json:"id"`
	TenantID  string `gorm:"size:40" json:"tenantId"`
	Username  string `gorm:"uniqueIndex;size:64" json:"username"`
	PwdHash   string `gorm:"size:256" json:"-"`
	Name      string `gorm:"size:64" json:"name"`
	Contact   string `gorm:"size:128" json:"contact"`
	Status    string `gorm:"size:16;default:active" json:"status"` // active/disabled
	CreatedAt int64  `json:"createdAt"`
	UpdatedAt int64  `json:"updatedAt"`
}

type UserRole struct {
	UserID    string `gorm:"primaryKey;size:40" json:"userId"`
	RoleID    string `gorm:"primaryKey;size:40" json:"roleId"`
	ProjectID string `gorm:"primaryKey;size:40" json:"projectId"`
}

// ---------- 设备与通道 ----------

// Device 设备（Source: idp/gb28181/onvif/rtsp）。
type Device struct {
	ID             string      `gorm:"primaryKey;size:40" json:"id"`
	ProjectID      string      `gorm:"index;size:40" json:"projectId"`
	GroupID        string      `gorm:"size:40" json:"groupId"`
	Source         string      `gorm:"size:16;index" json:"source"` // 主来源
	AltSources     StringSlice `gorm:"type:text[]" json:"altSources"`
	Name           string      `gorm:"size:128" json:"name"`
	Model          string      `gorm:"size:64" json:"model"`
	Vendor         string      `gorm:"size:64" json:"vendor"`
	Fw             string      `gorm:"size:64" json:"fw"`
	Hw             string      `gorm:"size:64" json:"hw"`
	Identity       JSONB       `gorm:"type:jsonb" json:"identity"` // {deviceId|gbId|ip,port}
	Status         string      `gorm:"size:16;index;default:offline" json:"status"`
	Error          JSONB       `gorm:"type:jsonb" json:"error"`
	LastSeenAt     int64       `json:"lastSeenAt"`
	Capabilities   StringSlice `gorm:"type:text[]" json:"capabilities"`
	Meta           JSONB       `gorm:"type:jsonb" json:"meta"`
	CredentialsEnc string      `gorm:"text" json:"-"` // AES-256-GCM 密文
	Location       string      `gorm:"size:128" json:"location"`
	Remark         string      `gorm:"size:256" json:"remark"`
	DeletedAt      int64       `gorm:"index" json:"deletedAt,omitempty"`
	CreatedAt      int64       `json:"createdAt"`
	UpdatedAt      int64       `json:"updatedAt"`
}

// CapabilityMissing 判断设备是否缺少某能力。
func (d *Device) CapabilityMissing(cap string) bool {
	for _, c := range d.Capabilities {
		if c == cap {
			return false
		}
	}
	return true
}

// HasCapability 是否具备能力。
func (d *Device) HasCapability(cap string) bool { return !d.CapabilityMissing(cap) }

type Channel struct {
	ID           string      `gorm:"primaryKey;size:40" json:"id"`
	DeviceID     string      `gorm:"index;size:40" json:"deviceId"`
	ProjectID    string      `gorm:"index;size:40" json:"projectId"`
	Idx          int         `gorm:"default:1" json:"idx"`
	Name         string      `gorm:"size:128" json:"name"`
	Enabled      bool        `gorm:"default:true" json:"enabled"`
	StreamState  string      `gorm:"size:16;default:idle" json:"streamState"` // idle/starting/streaming/error
	Profiles     JSONB       `gorm:"type:jsonb" json:"profiles"`
	CoverURL     string      `gorm:"size:256" json:"coverUrl"`
	Capabilities StringSlice `gorm:"type:text[]" json:"capabilities"`
	Meta         JSONB       `gorm:"type:jsonb" json:"meta"`
	CreatedAt    int64       `json:"createdAt"`
	UpdatedAt    int64       `json:"updatedAt"`
}

// ---------- 媒体节点与流 ----------

type MediaNode struct {
	ID            string `gorm:"primaryKey;size:40" json:"id"`
	Name          string `gorm:"size:64" json:"name"`
	APIURL        string `gorm:"size:256" json:"apiUrl"`
	SecretEnc     string `gorm:"text" json:"-"`
	PublicHost    string `gorm:"size:128" json:"publicHost"`
	RTMPPort      int    `gorm:"default:1936" json:"rtmpPort"`
	HTTPPort      int    `gorm:"default:80" json:"httpPort"`
	HTTPSPort     int    `gorm:"default:443" json:"httpsPort"`
	RTPRange      string `gorm:"size:64;default:30000-30100" json:"rtpRange"`
	MaxStreams    int    `gorm:"default:200" json:"maxStreams"`
	Streams       int    `gorm:"default:0" json:"streams"`
	Playing       int    `gorm:"default:0" json:"playing"`      // SYS-01 当前播放路数
	BwIn          int64  `gorm:"default:0" json:"bwIn"`         // SYS-01 入带宽 B/s
	BwOut         int64  `gorm:"default:0" json:"bwOut"`        // SYS-01 出带宽 B/s
	Version       string `gorm:"size:128" json:"version"`       // SYS-01 ZLM 版本（Server 头）
	Disabled      bool   `gorm:"default:false" json:"disabled"` // 禁用（不参与调度）
	Weight        int    `gorm:"default:100" json:"weight"`
	Status        string `gorm:"size:16;default:offline" json:"status"`
	LastKeepalive int64  `json:"lastKeepalive"`
	CreatedAt     int64  `json:"createdAt"`
}

type StreamSession struct {
	ID        string `gorm:"primaryKey;size:48" json:"id"`
	ChannelID string `gorm:"index;size:40" json:"channelId"`
	Profile   string `gorm:"size:16" json:"profile"`
	NodeID    string `gorm:"size:40" json:"nodeId"`
	App       string `gorm:"size:32" json:"app"`
	Stream    string `gorm:"size:128" json:"stream"`
	Kind      string `gorm:"size:16" json:"kind"` // live/playback/record
	StartedAt int64  `json:"startedAt"`
	EndedAt   int64  `json:"endedAt"`
	Bytes     int64  `json:"bytes"`
}

// ---------- 录像 ----------

type RecordTemplate struct {
	ID        string `gorm:"primaryKey;size:40" json:"id"`
	ProjectID string `gorm:"index;size:40" json:"projectId"`
	Name      string `gorm:"size:64" json:"name"`
	Kind      string `gorm:"size:16" json:"kind"` // timer/event
	Schedule  JSONB  `gorm:"type:jsonb" json:"schedule"`
	Builtin   bool   `gorm:"default:false" json:"builtin"`
	CreatedAt int64  `json:"createdAt"`
	UpdatedAt int64  `json:"updatedAt"`
}

type RecordPlan struct {
	ID         string `gorm:"primaryKey;size:40" json:"id"`
	ChannelID  string `gorm:"index;size:40" json:"channelId"`
	TemplateID string `gorm:"size:40" json:"templateId"`
	Profile    string `gorm:"size:16;default:main" json:"profile"`
	Enabled    bool   `gorm:"default:true" json:"enabled"`
	CreatedAt  int64  `json:"createdAt"`
	UpdatedAt  int64  `json:"updatedAt"`
}

type RecordIndex struct {
	ID        string `gorm:"primaryKey;size:48" json:"id"`
	ChannelID string `gorm:"index;size:40" json:"channelId"`
	Source    string `gorm:"size:16" json:"source"` // device|platform
	StartTs   int64  `json:"startTs"`
	EndTs     int64  `json:"endTs"`
	Type      string `gorm:"size:16" json:"type"` // timer|event|manual
	Path      string `gorm:"size:256" json:"path"`
	Size      int64  `json:"size"`
}

// ---------- 告警 ----------

type AlarmTemplate struct {
	ID        string `gorm:"primaryKey;size:40" json:"id"`
	ProjectID string `gorm:"index;size:40" json:"projectId"`
	Name      string `gorm:"size:64" json:"name"`
	Schedule  JSONB  `gorm:"type:jsonb" json:"schedule"`
	Builtin   bool   `gorm:"default:false" json:"builtin"`
	CreatedAt int64  `json:"createdAt"`
	UpdatedAt int64  `json:"updatedAt"`
}

type AlarmRule struct {
	ID         string      `gorm:"primaryKey;size:40" json:"id"`
	ProjectID  string      `gorm:"index;size:40" json:"projectId"`
	ChannelID  string      `gorm:"index;size:40" json:"channelId"`
	Kinds      StringSlice `gorm:"type:text[]" json:"kinds"`
	TemplateID string      `gorm:"size:40" json:"templateId"`
	Enabled    bool        `gorm:"default:true" json:"enabled"`
	CreatedAt  int64       `json:"createdAt"`
	UpdatedAt  int64       `json:"updatedAt"`
}

type AlarmEvent struct {
	ID          string `gorm:"primaryKey;size:48" json:"id"`
	ProjectID   string `gorm:"index;size:40" json:"projectId"`
	DeviceID    string `gorm:"index;size:40" json:"deviceId"`
	ChannelID   string `gorm:"index;size:40" json:"channelId"`
	Kind        string `gorm:"size:32;index" json:"kind"`
	Level       string `gorm:"size:16;default:info" json:"level"`
	Ts          int64  `gorm:"index" json:"ts"`
	Data        JSONB  `gorm:"type:jsonb" json:"data"`
	SnapshotURL string `gorm:"size:256" json:"snapshotUrl"`
	Read        bool   `gorm:"default:false" json:"read"`
}

// ---------- 运维 ----------

type AuditLog struct {
	ID        string `gorm:"primaryKey;size:48" json:"id"`
	UserID    string `gorm:"index;size:40" json:"userId"`
	Username  string `gorm:"size:64" json:"username"`
	ProjectID string `gorm:"index;size:40" json:"projectId"`
	Action    string `gorm:"size:64;index" json:"action"`
	Target    string `gorm:"size:128" json:"target"`
	Result    string `gorm:"size:16" json:"result"`
	IP        string `gorm:"size:64" json:"ip"`
	Detail    JSONB  `gorm:"type:jsonb" json:"detail"`
	Ts        int64  `gorm:"index" json:"ts"`
}

// Task 任务中心记录（P-18）。ProjectID 为项目隔离依据，Title/Detail 供前端直接展示。
type Task struct {
	ID        string `gorm:"primaryKey;size:48" json:"id"`
	ProjectID string `gorm:"index;size:40" json:"projectId"`
	Type      string `gorm:"size:32;index" json:"type"`
	Title     string `gorm:"size:128" json:"title"`
	Detail    string `gorm:"size:255" json:"detail"`
	Status    string `gorm:"size:16;index" json:"status"` // pending/running/success/partial/failed/canceled
	Progress  int    `json:"progress"`
	Result    JSONB  `gorm:"type:jsonb" json:"result"`
	CreatedBy string `gorm:"size:40" json:"createdBy"`
	CreatedAt int64  `gorm:"index" json:"createdAt"`
	UpdatedAt int64  `json:"updatedAt"`
}

// IdpPreadd IDP 预添加记录（7 天有效）。
type IdpPreadd struct {
	ID         string `gorm:"primaryKey;size:48" json:"id"`
	ProjectID  string `gorm:"index;size:40" json:"projectId"`
	DeviceID   string `gorm:"index;size:32" json:"deviceId"`
	VerifyHmac string `gorm:"size:128" json:"-"`
	GroupID    string `gorm:"size:40" json:"groupId"`
	Name       string `gorm:"size:128" json:"name"`
	Location   string `gorm:"size:128" json:"location"`
	ExpiresAt  int64  `json:"expiresAt"`
	State      string `gorm:"size:16;default:pending" json:"state"` // pending/activated/expired/failed
	CreatedAt  int64  `json:"createdAt"`
}

// GbWhitelist 国标白名单：预填即自动入组。
type GbWhitelist struct {
	ID        string `gorm:"primaryKey;size:48" json:"id"`
	ProjectID string `gorm:"index;size:40" json:"projectId"`
	GbID      string `gorm:"index;size:32" json:"gbId"`
	PwdEnc    string `gorm:"text" json:"-"`
	GroupID   string `gorm:"size:40" json:"groupId"`
	Name      string `gorm:"size:128" json:"name"`
	CreatedAt int64  `json:"createdAt"`
}

// GbPending 待确认的注册设备。
type GbPending struct {
	ID        string `gorm:"primaryKey;size:48" json:"id"`
	ProjectID string `gorm:"index;size:40" json:"projectId"`
	GbID      string `gorm:"index;size:32" json:"gbId"`
	IP        string `gorm:"size:64" json:"ip"`
	Vendor    string `gorm:"size:64" json:"vendor"`
	Model     string `gorm:"size:64" json:"model"`
	FirstSeen int64  `json:"firstSeen"`
}

// Setting 全局/项目级设置（key-value）。
type Setting struct {
	Scope string `gorm:"primaryKey;size:40" json:"scope"` // "global" 或 projectId
	Key   string `gorm:"primaryKey;size:64" json:"key"`
	Value JSONB  `gorm:"type:jsonb" json:"value"`
}

// TableName 统一小写复数（GORM 默认，显式声明避免复数歧义）。
func (Tenant) TableName() string      { return "tenants" }
func (IdpPreadd) TableName() string   { return "idp_preadd" }
func (GbWhitelist) TableName() string { return "gb_whitelist" }
func (GbPending) TableName() string   { return "gb_pending" }
