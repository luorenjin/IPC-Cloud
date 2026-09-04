package store

import (
	"context"
	"log"
	"sync"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/jetscam/ipccloud/server/internal/config"
)

// KV 通用缓存接口：Redis 可用则用 Redis，否则退化为进程内 map（开发/单机场景）。
type KV interface {
	Set(ctx context.Context, key string, val any, ttl time.Duration) error
	Get(ctx context.Context, key string) (string, error)
	Del(ctx context.Context, key string) error
	IncrTTL(ctx context.Context, key string, ttl time.Duration) (int64, error) // 计数器
}

type redisKV struct{ r *redis.Client }

func (k redisKV) Set(c context.Context, key string, val any, ttl time.Duration) error {
	return k.r.Set(c, key, val, ttl).Err()
}
func (k redisKV) Get(c context.Context, key string) (string, error) { return k.r.Get(c, key).Result() }
func (k redisKV) Del(c context.Context, key string) error          { return k.r.Del(c, key).Err() }
func (k redisKV) IncrTTL(c context.Context, key string, ttl time.Duration) (int64, error) {
	n, err := k.r.Incr(c, key).Result()
	if err == nil && n == 1 {
		k.r.Expire(c, key, ttl)
	}
	return n, err
}

type memKV struct {
	mu sync.Mutex
	m  map[string]memEntry
}
type memEntry struct {
	val    string
	expire time.Time
}

func (k *memKV) Set(_ context.Context, key string, val any, ttl time.Duration) error {
	k.mu.Lock()
	defer k.mu.Unlock()
	k.m[key] = memEntry{val: val.(string), expire: time.Now().Add(ttl)}
	return nil
}
func (k *memKV) Get(_ context.Context, key string) (string, error) {
	k.mu.Lock()
	defer k.mu.Unlock()
	e, ok := k.m[key]
	if !ok || time.Now().After(e.expire) {
		delete(k.m, key)
		return "", redis.Nil
	}
	return e.val, nil
}
func (k *memKV) Del(_ context.Context, key string) error {
	k.mu.Lock()
	defer k.mu.Unlock()
	delete(k.m, key)
	return nil
}
func (k *memKV) IncrTTL(_ context.Context, key string, ttl time.Duration) (int64, error) {
	k.mu.Lock()
	defer k.mu.Unlock()
	e, ok := k.m[key]
	if !ok || time.Now().After(e.expire) {
		k.m[key] = memEntry{val: "1", expire: time.Now().Add(ttl)}
		return 1, nil
	}
	var n int64
	for _, ch := range e.val {
		if ch >= '0' && ch <= '9' {
			n = n*10 + int64(ch-'0')
		}
	}
	n++
	e.val = itoa(n)
	k.m[key] = e
	return n, nil
}
func itoa(n int64) string {
	if n == 0 {
		return "0"
	}
	var b [20]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	return string(b[i:])
}

var KVImpl KV

// KVGet/KVSet/KVDel/KVIncr 便捷封装。
func KVGet(key string) (string, error) { return KVImpl.Get(context.Background(), key) }
func KVSet(key string, val string, ttl time.Duration) error {
	return KVImpl.Set(context.Background(), key, val, ttl)
}
func KVDel(key string) error { return KVImpl.Del(context.Background(), key) }
func KVIncr(key string, ttl time.Duration) (int64, error) {
	return KVImpl.IncrTTL(context.Background(), key, ttl)
}

func OpenRedis(cfg *config.Config) {
	r := redis.NewClient(&redis.Options{Addr: cfg.RedisAddr, PoolSize: 32})
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := r.Ping(ctx).Err(); err != nil {
		log.Printf("[store] redis unavailable (%v), fallback to in-memory KV", err)
		KVImpl = &memKV{m: map[string]memEntry{}}
		return
	}
	log.Printf("[store] redis connected %s", cfg.RedisAddr)
	KVImpl = redisKV{r: r}
}
