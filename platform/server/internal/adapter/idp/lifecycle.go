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

	// 接入规范 §3.2 的状态机要求 offline → online 的**恢复**边：idp 的离线判定是
	// 「LWT 即时，或 3×Keepalive（默认 180s）无消息」，所以「收到任意上行报文」即等于在线。
	// 仅刷 Redis 键不足以表达这一点——DB 的 status 只有 hello / bind / 预添加命中会写 online，
	// 于是设备一旦错过它那条 hello 就再也回不到 online：
	//   实测冷启动竞态（EMQX 尚未就绪，server 13:22:18 才订阅上，而模拟器 13:22:17 已发 hello，
	//   hello 无 retain 故丢失）：此后设备每 30s 持续上报，Redis 键 TTL 220s 一直存活，
	//   而界面恒显「离线」——同一时刻两个在线表示自相矛盾。
	// 不变量（修复后）：Redis 在线键存在 ⟺ DB status = online；反方向由 offlineWatcher 负责。
	// 幂等且低开销：上报每 30s 一次，因此只在状态不是 online 时才写库（避免每条上报都 Save + 广播）。
	var dev models.Device
	if store.DB.Select("id", "status", "project_id").
		First(&dev, "id = ? AND deleted_at = 0", deviceID).Error != nil {
		return
	}
	// 未绑定设备（project_id 为空）不在此处上线：它尚无归属，其上线时机由绑定/预添加流程决定，
	// 这里不改变既有语义。
	if dev.ProjectID == "" {
		return
	}
	if dev.Status != "online" {
		log.Printf("[idp] %s 上报恢复在线（§3.2 恢复边）", deviceID)
		devsvc.SetDeviceStatus(deviceID, "online", nil)
	}
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

	// 默认密码是否已修改（接入规范 §5.5.2 hello.localUserChanged）。
	// 用 comma-ok 读：字段缺失（非 IDP 来源或未实现该字段的固件）绝不能当成 false，
	// 否则会在平台上凭空报一条「仍在用出厂默认密码」，把正常设备也说成有风险。
	meta := models.JSONB{}
	if pwdChanged, has := d["localUserChanged"].(bool); has {
		meta["localUserChanged"] = pwdChanged
	}

	var dev models.Device
	err := store.DB.First(&dev, "id = ?", deviceID).Error
	if err != nil {
		dev = models.Device{
			ID: deviceID, Source: Source, Name: model + "-" + deviceID[len(deviceID)-4:],
			Model: model, Vendor: vendor, Fw: fw, Hw: hw,
			Identity:     models.JSONB{"deviceId": deviceID},
			Status:       "offline",
			Capabilities: models.StringSlice(caps),
			Meta:         meta,
			CreatedAt:    models.NowMilli(), UpdatedAt: models.NowMilli(),
		}
		if err := store.DB.Create(&dev).Error; err != nil {
			log.Printf("[idp] create device %s: %v", deviceID, err)
			return
		}
	} else {
		dev.Model, dev.Vendor, dev.Fw, dev.Hw = model, vendor, fw, hw
		dev.Capabilities = models.StringSlice(caps)
		// 与报告的 metrics 共存：只覆盖 hello 带来的那几个键，不整个重置 meta
		if dev.Meta == nil {
			dev.Meta = models.JSONB{}
		}
		for k, v := range meta {
			dev.Meta[k] = v
		}
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
