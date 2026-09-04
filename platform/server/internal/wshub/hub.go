// Package wshub WebSocket 事件推送（WS /ws/v1/events）。
package wshub

import (
	"log"
	"net/http"
	"sync"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"

	"github.com/jetscam/ipccloud/server/internal/auth"
	"github.com/jetscam/ipccloud/server/internal/bus"
)

type client struct {
	conn      *websocket.Conn
	userID    string
	tenantID  string
	projectID string
	send      chan any
}

type Hub struct {
	mu      sync.RWMutex
	clients map[*client]struct{}
}

var upgrader = websocket.Upgrader{
	CheckOrigin: func(*http.Request) bool { return true },
}

func New() *Hub { return &Hub{clients: map[*client]struct{}{}} }

// Handler GET /ws/v1/events?token=<jwt>&projectId=xxx
func (h *Hub) Handler(c *gin.Context) {
	token := c.Query("token")
	if token == "" {
		ah := c.GetHeader("Authorization")
		if len(ah) > 7 {
			token = ah[7:]
		}
	}
	claims, err := auth.Parse(token)
	if err != nil || claims.Kind != "access" {
		c.AbortWithStatus(http.StatusUnauthorized)
		return
	}
	ws, err := upgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		return
	}
	cl := &client{
		conn: ws, userID: claims.UserID, tenantID: claims.TenantID,
		projectID: c.Query("projectId"), send: make(chan any, 256),
	}
	h.mu.Lock()
	h.clients[cl] = struct{}{}
	h.mu.Unlock()

	// 读泵：仅处理关闭
	go func() {
		defer func() {
			h.mu.Lock()
			delete(h.clients, cl)
			h.mu.Unlock()
			close(cl.send)
			ws.Close()
		}()
		ws.SetReadLimit(4096)
		for {
			if _, _, err := ws.ReadMessage(); err != nil {
				return
			}
		}
	}()
	// 写泵
	go func() {
		ping := time.NewTicker(25 * time.Second)
		defer ping.Stop()
		for {
			msg, ok := <-cl.send
			if !ok {
				return
			}
			if err := ws.WriteJSON(msg); err != nil {
				return
			}
			ws.SetWriteDeadline(time.Now().Add(10 * time.Second))
		}
	}()
	cl.send <- map[string]any{"type": "connected", "ts": time.Now().UnixMilli()}
}

// Push 向项目内（或全租户）客户端推送。
func (h *Hub) Push(projectID string, msg any) {
	h.mu.RLock()
	targets := make([]*client, 0, len(h.clients))
	for cl := range h.clients {
		if projectID == "" || cl.projectID == "" || cl.projectID == projectID {
			targets = append(targets, cl)
		}
	}
	h.mu.RUnlock()
	for _, cl := range targets {
		select {
		case cl.send <- msg:
		default:
			log.Printf("ws send buffer full for user %s", cl.userID)
		}
	}
}

// Bridge 把事件总线桥接到 WS。
func (h *Hub) Bridge(b *bus.Bus) {
	b.Subscribe("*", func(ev bus.Event) {
		h.Push(ev.ProjectID, ev)
	})
}
