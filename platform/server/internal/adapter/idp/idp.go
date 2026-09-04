// Package idp IDP v1 MQTT 适配器（接入规范 §5）。
package idp

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/config"
	"github.com/jetscam/ipccloud/server/internal/store"
)

const (
	Source = "idp"

	keepalivePeriod = 60 * time.Second
	offlineAfter    = 3 * keepalivePeriod
	ackTimeout      = 10 * time.Second
)

const codeE0400 = 400

// Envelope IDP 消息信封（规范 §5.4）。
type Envelope struct {
	V     int            `json:"v"`
	Type  string         `json:"type"`
	MsgID string         `json:"msgId"`
	Ts    int64          `json:"ts"`
	Code  int            `json:"code,omitempty"`
	Msg   string         `json:"msg,omitempty"`
	Data  map[string]any `json:"data"`
}

// Adapter IDP 适配器实现。
type Adapter struct {
	cfg  *config.Config
	cli  mqtt.Client
	mu   sync.Mutex
	acks map[string]chan Envelope
	ctx  context.Context
}

func New(cfg *config.Config) *Adapter {
	return &Adapter{cfg: cfg, acks: map[string]chan Envelope{}}
}

func (a *Adapter) Source() string { return Source }

func (a *Adapter) Start(ctx context.Context) error {
	a.ctx = ctx
	opts := mqtt.NewClientOptions().
		AddBroker(a.cfg.MQTTBroker).
		SetClientID("ipccloud-server-" + fmt.Sprint(time.Now().UnixMilli())).
		SetAutoReconnect(true).SetConnectRetry(true).SetConnectRetryInterval(3 * time.Second).
		SetKeepAlive(30 * time.Second)
	if a.cfg.MQTTUsername != "" {
		opts = opts.SetUsername(a.cfg.MQTTUsername).SetPassword(a.cfg.MQTTPassword)
	}
	opts.OnConnect = func(c mqtt.Client) {
		if tok := c.Subscribe("idp/v1/+/up/#", 1, a.onMessage); tok.Wait() && tok.Error() != nil {
			log.Printf("[idp] subscribe fail: %v", tok.Error())
		} else {
			log.Printf("[idp] subscribed idp/v1/+/up/#")
		}
	}
	opts.OnConnectionLost = func(mqtt.Client, error) { log.Printf("[idp] connection lost") }
	a.cli = mqtt.NewClient(opts)
	if tok := a.cli.Connect(); tok.WaitTimeout(10*time.Second) && tok.Error() != nil {
		log.Printf("[idp] broker connect error (will retry in background): %v", tok.Error())
	}
	go a.offlineWatcher(ctx)
	return nil
}

func (a *Adapter) Stop() {
	if a.cli != nil && a.cli.IsConnectionOpen() {
		a.cli.Disconnect(500)
	}
}

func (a *Adapter) onMessage(_ mqtt.Client, msg mqtt.Message) {
	parts := strings.Split(msg.Topic(), "/")
	if len(parts) < 5 {
		return
	}
	deviceID := parts[2]
	var env Envelope
	if err := json.Unmarshal(msg.Payload(), &env); err != nil {
		log.Printf("[idp] %s bad payload: %v", msg.Topic(), err)
		return
	}
	a.mu.Lock()
	if ch, ok := a.acks[env.MsgID]; ok {
		delete(a.acks, env.MsgID)
		a.mu.Unlock()
		select {
		case ch <- env:
		default:
		}
		return
	}
	a.mu.Unlock()

	switch {
	case strings.HasSuffix(msg.Topic(), "/up/status"):
		a.handleStatus(deviceID, env)
	case strings.HasSuffix(msg.Topic(), "/up/event"):
		a.handleEvent(deviceID, env)
	case strings.HasSuffix(msg.Topic(), "/up/ota"):
		a.handleOtaProgress(deviceID, env)
	default:
		a.reply(deviceID, "cmd", env.MsgID, codeE0400, "unknown type", nil)
	}
}

// waitAck 发送命令并等待 ack（10s 超时重发 1 次，§5.4）。
func (a *Adapter) waitAck(topic string, payload any) (Envelope, error) {
	env, ok := payload.(Envelope)
	if !ok {
		b, _ := json.Marshal(payload)
		env = Envelope{}
		if err := json.Unmarshal(b, &env); err != nil {
			return Envelope{}, err
		}
	}
	if env.MsgID == "" {
		env.MsgID = "m_" + strings.ReplaceAll(fmt.Sprint(time.Now().UnixNano()), "-", "")
	}
	if env.V == 0 {
		env.V = 1
	}
	ch := make(chan Envelope, 1)
	a.mu.Lock()
	a.acks[env.MsgID] = ch
	a.mu.Unlock()
	defer func() {
		a.mu.Lock()
		delete(a.acks, env.MsgID)
		a.mu.Unlock()
	}()
	tok := a.cli.Publish(topic, 1, false, mustJSON(env))
	if !tok.WaitTimeout(5*time.Second) || tok.Error() != nil {
		return Envelope{}, fmt.Errorf("E1001 mqtt publish failed")
	}
	select {
	case ack := <-ch:
		return ack, nil
	case <-time.After(ackTimeout):
		a.cli.Publish(topic, 1, false, mustJSON(env))
		select {
		case ack := <-ch:
			return ack, nil
		case <-time.After(ackTimeout):
			return Envelope{}, fmt.Errorf("E7003 DEVICE_TIMEOUT")
		}
	}
}

// request 发送命令并要求回执。
func (a *Adapter) request(deviceID, domain string, typ string, data map[string]any) (map[string]any, error) {
	env := Envelope{Type: typ, Data: data}
	ack, err := a.waitAck(downTopic(deviceID, domain), env)
	if err != nil {
		return nil, err
	}
	if ack.Code != 0 {
		return nil, fmt.Errorf("%s %s", errCode(ack.Code), ack.Msg)
	}
	return ack.Data, nil
}

func (a *Adapter) reply(deviceID, domain, msgID string, code int, msg string, data map[string]any) {
	env := Envelope{Type: "cmd", MsgID: msgID, Code: code, Msg: msg, Data: data, Ts: time.Now().UnixMilli()}
	a.cli.Publish(downTopic(deviceID, domain), 1, false, mustJSON(env))
}

func (a *Adapter) publishTo(deviceID, domain string, env Envelope) {
	if env.V == 0 {
		env.V = 1
	}
	if env.Ts == 0 {
		env.Ts = time.Now().UnixMilli()
	}
	a.cli.Publish(downTopic(deviceID, domain), 1, false, mustJSON(env))
}

func downTopic(deviceID, domain string) string {
	return fmt.Sprintf("idp/v1/%s/down/%s", deviceID, domain)
}

// DeviceOnline 查询设备 MQTT 在线状态（KV）。
func (a *Adapter) DeviceOnline(deviceID string) bool {
	v, err := store.KVGet("idp:online:" + deviceID)
	return err == nil && v == "1"
}

func (a *Adapter) isOnline(deviceID string) bool { return a.DeviceOnline(deviceID) }

func mustJSON(v any) []byte {
	b, _ := json.Marshal(v)
	return b
}

var _ = adapter.Adapter(nil)