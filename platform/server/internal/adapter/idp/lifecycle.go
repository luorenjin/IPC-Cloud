package idp

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"time"

	"github.com/jetscam/ipccloud/server/internal/auth"
	"github.com/jetscam/ipccloud/server/internal/bus"
	"github.com/jetscam/ipccloud/server/internal/crypto"
	"github.com/jetscam/ipccloud/server/internal/devsvc"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// errCode 设备回执码 → 平台错误码（§10）。
func errCode(code int) string {
	switch code {
	case 0:
		return "ok"
	case 403:
		return "E0403"
	case 404:
		return "E0400"
	default:
		return fmt.Sprintf("E%d%03d", code/1000, code%1000)
	}
}

func (a *Adapter) markOnline(deviceID string) {
	_ = store.KVSet("idp:online:"+deviceID, "1", offlineAfter+time.Minute)
}

// isRevoked SET-02：设备是否在 CRL 吊销列表中（吊销的设备拒绝 hello/绑定）。
func isRevoked(deviceID string) bool {
	_, err := store.KVGet("idp:crl:" + strings.ToUpper(strings.TrimSpace(deviceID)))
	return err == nil
}

func (a *Adapter) markOffline(deviceID string) {
	_ = store.KVDel("idp:online:" + deviceID)
	var dev models.Device
	if store.DB.First(&dev, "id = ?", deviceID).Error != nil {
		return
	}
	if dev.Status == "online" {
		devsvc.SetDeviceStatus(deviceID, "offline", nil)
	}
}

// offlineWatcher 3×60s 无消息判离线（§3.2）。
func (a *Adapter) offlineWatcher(ctx context.Context) {
	t := time.NewTicker(30 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			var devs []models.Device
			store.DB.Where("source = ? AND status = 'online' AND deleted_at = 0", Source).Find(&devs)
			for _, d := range devs {
				if _, err := store.KVGet("idp:online:" + d.ID); err != nil {
					devsvc.SetDeviceStatus(d.ID, "offline", nil)
				}
			}
		}
	}
}

func (a *Adapter) handleStatus(deviceID string, env Envelope) {
	switch env.Type {
	case "status.hello":
		a.handleHello(deviceID, env)
	case "status.report":
		a.handleReport(deviceID, env)
	case "lwt":
		a.markOffline(deviceID)
	}
}

func (a *Adapter) handleHello(deviceID string, env Envelope) {
	a.markOnline(deviceID)
	d := env.Data
	model, _ := d["model"].(string)
	vendor, _ := d["vendor"].(string)
	fw, _ := d["fw"].(string)
	hw, _ := d["hw"].(string)

	chRaw, _ := json.Marshal(d["channels"])
	var channels []devsvc.HelloChannel
	_ = json.Unmarshal(chRaw, &channels)
	capsRaw, _ := json.Marshal(d["capabilities"])
	var caps []string
	_ = json.Unmarshal(capsRaw, &caps)

	var dev models.Device
	err := store.DB.First(&dev, "id = ?", deviceID).Error
	if err != nil {
		dev = models.Device{
			ID: deviceID, Source: Source, Name: model + "-" + deviceID[len(deviceID)-4:],
			Model: model, Vendor: vendor, Fw: fw, Hw: hw,
			Identity:     models.JSONB{"deviceId": deviceID},
			Status:       "offline",
			Capabilities: models.StringSlice(caps),
			CreatedAt:    models.NowMilli(), UpdatedAt: models.NowMilli(),
		}
		if err := store.DB.Create(&dev).Error; err != nil {
			log.Printf("[idp] create device %s: %v", deviceID, err)
			return
		}
	} else {
		dev.Model, dev.Vendor, dev.Fw, dev.Hw = model, vendor, fw, hw
		dev.Capabilities = models.StringSlice(caps)
		dev.UpdatedAt = models.NowMilli()
		store.DB.Save(&dev)
	}

	if dev.ProjectID == "" {
		a.tryPreadd(&dev, channels, caps)
		return
	}
	devsvc.UpsertChannels(deviceID, dev.ProjectID, caps, channels)
	devsvc.SetDeviceStatus(deviceID, "online", nil)
}

func (a *Adapter) handleReport(deviceID string, env Envelope) {
	a.markOnline(deviceID)
	devsvc.TouchDevice(deviceID)
	var dev models.Device
	if store.DB.First(&dev, "id = ?", deviceID).Error == nil && dev.ProjectID != "" {
		if dev.Meta == nil {
			dev.Meta = models.JSONB{}
		}
		dev.Meta["metrics"] = env.Data
		store.DB.Model(&dev).Update("meta", dev.Meta)
	}
}

// tryPreadd 预添加自动绑定（§5.5.4）。
func (a *Adapter) tryPreadd(dev *models.Device, channels []devsvc.HelloChannel, caps []string) {
	var pre models.IdpPreadd
	if store.DB.First(&pre, "device_id = ? AND state = 'pending'", dev.ID).Error != nil {
		return
	}
	if pre.ExpiresAt > 0 && pre.ExpiresAt < models.NowMilli() {
		store.DB.Model(&pre).Update("state", "expired")
		return
	}
	if pre.VerifyHmac == "" {
		return // 未含验证码：待管理员在前端输入完成绑定
	}
	if err := a.sendBind(dev.ID, pre.ProjectID, pre.GroupID, pre.Name); err == nil {
		dev.ProjectID = pre.ProjectID
		dev.GroupID = pre.GroupID
		dev.Name = pre.Name
		store.DB.Save(dev)
		devsvc.UpsertChannels(dev.ID, dev.ProjectID, caps, channels)
		devsvc.SetDeviceStatus(dev.ID, "online", nil)
		store.DB.Model(&pre).Update("state", "activated")
	}
}

// Bind 绑定（§5.5.3，供 API 层调用）。
func (a *Adapter) Bind(projectID, groupID, name, deviceID, verifyCode string) error {
	if isRevoked(deviceID) {
		return errs.EForbid.WithMsg("设备已被吊销（CRL），拒绝绑定")
	}
	if !a.isOnline(deviceID) {
		return errs.EDeviceNotOnline
	}
	cnt, _ := store.KVIncr("bindfail:"+deviceID, time.Hour)
	_ = cnt

	var dev models.Device
	if store.DB.First(&dev, "id = ?", deviceID).Error != nil {
		// 设备尚未经 hello 建档，拒绝绑定避免生成脏记录
		return errs.ENotFound
	}
	if dev.ProjectID != "" && dev.ProjectID != projectID {
		return errs.EAlreadyBound
	}

	expectHmac := ""
	var pre models.IdpPreadd
	if store.DB.First(&pre, "device_id = ?", deviceID).Error == nil {
		expectHmac = pre.VerifyHmac
	}
	if expectHmac == "" {
		if v, err := store.KVGet("idp:vc:" + deviceID); err == nil {
			expectHmac = v
		}
	}
	if expectHmac != "" {
		got := crypto.HMACSHA256Hex(a.cfg.JWTSecret+":vc", strings.ToUpper(verifyCode))
		if got != expectHmac {
			return errs.EVerifyCode
		}
	} else {
		_ = store.KVSet("idp:vc:"+deviceID,
			crypto.HMACSHA256Hex(a.cfg.JWTSecret+":vc", strings.ToUpper(verifyCode)), 365*24*time.Hour)
	}

	if err := a.sendBind(deviceID, projectID, groupID, name); err != nil {
		return err
	}

	if dev.ID == "" {
		store.DB.First(&dev, "id = ?", deviceID)
	}
	dev.ProjectID = projectID
	if groupID != "" {
		dev.GroupID = groupID
	}
	if name != "" {
		dev.Name = name
	}
	store.DB.Save(&dev)
	devsvc.SetDeviceStatus(deviceID, "online", nil)
	if pre.ID != "" {
		store.DB.Model(&pre).Update("state", "activated")
	}
	return nil
}

// sendBind 下发 cmd.bind 并等待 ack(0)。
func (a *Adapter) sendBind(deviceID, projectID, groupID, name string) error {
	bindToken, err := auth.Sign("bind", "", projectID, deviceID, 5*time.Minute)
	if err != nil {
		return err
	}
	_, err = a.request(deviceID, "cmd", "cmd.bind", map[string]any{
		"projectId": projectID, "bindToken": bindToken, "deviceName": name, "time": models.NowMilli(),
	})
	return err
}

// Unbind cmd.unbind（§5.5.5）。
func (a *Adapter) Unbind(ctx context.Context, deviceID string) error {
	_, err := a.request(deviceID, "cmd", "cmd.unbind", map[string]any{})
	if err == nil {
		store.DB.Model(&models.Device{}).Where("id = ?", deviceID).
			Updates(map[string]any{"project_id": "", "group_id": "", "status": "offline"})
	}
	return err
}

// Transfer cmd.transfer。
func (a *Adapter) Transfer(ctx context.Context, deviceID, projectID string) error {
	_, err := a.request(deviceID, "cmd", "cmd.transfer", map[string]any{"projectId": projectID})
	return err
}

// ---------- 事件 ----------

func (a *Adapter) handleEvent(deviceID string, env Envelope) {
	a.markOnline(deviceID)
	var dev models.Device
	if store.DB.First(&dev, "id = ?", deviceID).Error != nil || dev.ProjectID == "" {
		return
	}
	kind := strings.TrimPrefix(env.Type, "event.")
	if kind == "" {
		return
	}
	data := env.Data
	if ch, ok := data["ch"].(float64); ok {
		var chRow models.Channel
		if store.DB.First(&chRow, "device_id = ? AND idx = ?", deviceID, int(ch)).Error == nil {
			data["channelId"] = chRow.ID
		}
	}
	switch kind {
	case "rebooted", "bind_state", "ota_state", "tf_inserted", "tf_removed":
	default:
		chID, _ := data["channelId"].(string)
		bus.Default.Publish(bus.Event{
			Type: "alarm." + kind, DeviceID: deviceID, ChannelID: chID,
			Source: Source, ProjectID: dev.ProjectID, Data: data,
		})
	}
}

func (a *Adapter) handleOtaProgress(deviceID string, env Envelope) {
	var dev models.Device
	if store.DB.First(&dev, "id = ?", deviceID).Error != nil {
		return
	}
	bus.Default.Publish(bus.Event{Type: "ota.progress", DeviceID: deviceID,
		ProjectID: dev.ProjectID, Data: env.Data})
}

// StartOTA 下发 ota.start（§5.10）。
func (a *Adapter) StartOTA(deviceID string, opt map[string]any) error {
	_, err := a.request(deviceID, "ota", "ota.start", opt)
	return err
}

// Snapshot cmd.snapshot。
func (a *Adapter) Snapshot(ctx context.Context, channelID, uploadURL string) (string, error) {
	var ch models.Channel
	if err := store.DB.First(&ch, "id = ?", channelID).Error; err != nil {
		return "", errs.ENotFound
	}
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", ch.DeviceID).Error; err != nil {
		return "", errs.ENotFound
	}
	data, err := a.request(dev.ID, "cmd", "cmd.snapshot", map[string]any{
		"ch": ch.Idx, "profile": "main", "uploadUrl": uploadURL,
	})
	if err != nil {
		return "", err
	}
	url, _ := data["url"].(string)
	return url, nil
}