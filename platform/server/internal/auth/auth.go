// Package auth JWT 会话与登录锁定（ACC-01：access 2h / refresh 7d，5 次失败锁 15 分钟）。
package auth

import (
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/golang-jwt/jwt/v5"

	"github.com/jetscam/ipccloud/server/internal/config"
)

type Claims struct {
	UserID   string `json:"uid"`
	TenantID string `json:"tid"`
	Kind     string `json:"kind"` // access | refresh | play | push
	Extra    string `json:"extra,omitempty"`
	jwt.RegisteredClaims
}

var secret []byte

func Init(cfg *config.Config) { secret = []byte(cfg.JWTSecret) }

func Sign(kind, userID, tenantID, extra string, ttl time.Duration) (string, error) {
	c := Claims{
		UserID: userID, TenantID: tenantID, Kind: kind, Extra: extra,
		RegisteredClaims: jwt.RegisteredClaims{
			IssuedAt:  jwt.NewNumericDate(time.Now()),
			ExpiresAt: jwt.NewNumericDate(time.Now().Add(ttl)),
			ID:        fmt.Sprintf("%d", time.Now().UnixNano()),
		},
	}
	return jwt.NewWithClaims(jwt.SigningMethodHS256, c).SignedString(secret)
}

func Parse(token string) (*Claims, error) {
	t, err := jwt.ParseWithClaims(token, &Claims{}, func(*jwt.Token) (any, error) { return secret, nil })
	if err != nil {
		return nil, err
	}
	c, ok := t.Claims.(*Claims)
	if !ok || !t.Valid {
		return nil, errors.New("invalid token")
	}
	return c, nil
}

// ---------- 登录失败锁定 ----------

type lockEntry struct {
	fails    int
	until    time.Time
	lastFail time.Time
}

var (
	lockMu    sync.Mutex
	lockState = map[string]*lockEntry{}
)

// CheckLocked 返回剩余锁定秒数（0 表示未锁）。
func CheckLocked(key string) int {
	lockMu.Lock()
	defer lockMu.Unlock()
	e, ok := lockState[key]
	if !ok || time.Now().After(e.until) {
		return 0
	}
	return int(time.Until(e.until).Seconds()) + 1
}

// RecordFail 记录失败，返回本次是否触发锁定。
func RecordFail(key string) bool {
	lockMu.Lock()
	defer lockMu.Unlock()
	e := lockState[key]
	if e == nil {
		e = &lockEntry{}
		lockState[key] = e
	}
	now := time.Now()
	if e.fails > 0 && now.Sub(e.lastFail) > time.Hour {
		e.fails = 0
	}
	e.fails++
	e.lastFail = now
	if e.fails >= 5 {
		e.until = now.Add(15 * time.Minute)
		return true
	}
	return false
}

// RecordSuccess 清除失败计数。
func RecordSuccess(key string) {
	lockMu.Lock()
	defer lockMu.Unlock()
	delete(lockState, key)
}
