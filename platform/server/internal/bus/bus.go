// Package bus 进程内统一事件总线（接入规范 §3.5）。适配器产出统一事件，WS/告警/媒体订阅消费。
package bus

import (
	"log"
	"sync"
	"time"
)

type Event struct {
	Type      string         `json:"type"`
	DeviceID  string         `json:"deviceId,omitempty"`
	ChannelID string         `json:"channelId,omitempty"`
	Source    string         `json:"source,omitempty"`
	ProjectID string         `json:"projectId,omitempty"`
	Ts        int64          `json:"ts"`
	Data      map[string]any `json:"data,omitempty"`
}

type Handler func(Event)

type Bus struct {
	mu      sync.RWMutex
	subs    map[string][]Handler
	queue   chan Event
	stopped bool
}

// Default 进程级默认总线。
var Default = New()

func New() *Bus {
	b := &Bus{subs: map[string][]Handler{}, queue: make(chan Event, 4096)}
	for i := 0; i < 4; i++ {
		go b.worker()
	}
	return b
}

func (b *Bus) worker() {
	for ev := range b.queue {
		b.mu.RLock()
		handlers := append([]Handler{}, b.subs[ev.Type]...)
		handlers = append(handlers, b.subs["*"]...)
		b.mu.RUnlock()
		for _, h := range handlers {
			func() {
				defer func() {
					if r := recover(); r != nil {
						log.Printf("bus handler panic: %v", r)
					}
				}()
				h(ev)
			}()
		}
	}
}

func (b *Bus) Subscribe(typ string, h Handler) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.subs[typ] = append(b.subs[typ], h)
}

func (b *Bus) Publish(ev Event) {
	if ev.Ts == 0 {
		ev.Ts = time.Now().UnixMilli()
	}
	if b.stopped {
		return
	}
	select {
	case b.queue <- ev:
	default:
		log.Printf("bus queue full, drop event %s", ev.Type)
	}
}

func (b *Bus) Close() { b.stopped = true; close(b.queue) }
