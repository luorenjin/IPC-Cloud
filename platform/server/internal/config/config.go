package config

import (
	"os"
	"strconv"
)

// Config 全部来自环境变量，默认值面向 docker-compose 单机部署。
type Config struct {
	HTTPAddr        string // REST/WS 监听地址
	DBDSN           string // PostgreSQL DSN
	RedisAddr       string
	JWTSecret       string
	DataDir         string // 快照/录像等本地存储根目录
	EncryptionKey   string // 凭据 AES-256-GCM 密钥（32 字节 hex 或原文）
	AccessTTLMin    int
	RefreshTTLDays  int
	StreamIdleSec   int // 无人观看停流等待（默认 30s）
	PlayTokenTTLMin int // 播放 token 有效期 ≤10min

	MQTTBroker   string // IDP Broker (e.g. tcp://emqx:1883)
	MQTTUsername string
	MQTTPassword string
	MQTTTLS      bool

	SIPIP       string // SIP 服务器对外 IP（SDP c= 行使用节点公网地址，此项为平台自身监听绑定）
	SIPPort     int    // 5060
	SIPServerID string // 20 位平台国标编号（行业编码 200）
	SIPDomain   string // 默认取 SIPServerID 前 10 位
	SIPRealm    string

	// 首次启动引导：自动创建的超级管理员
	AdminUser     string
	AdminPassword string
}

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func getenvInt(k string, def int) int {
	if v := os.Getenv(k); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func Load() *Config {
	c := &Config{
		HTTPAddr:        getenv("HTTP_ADDR", ":8080"),
		DBDSN:           getenv("DB_DSN", "host=postgres user=ipccloud password=ipccloud dbname=ipccloud port=5432 sslmode=disable TimeZone=Asia/Shanghai"),
		RedisAddr:       getenv("REDIS_ADDR", "redis:6379"),
		JWTSecret:       getenv("JWT_SECRET", "change-me-in-production"),
		DataDir:         getenv("DATA_DIR", "./data"),
		EncryptionKey:   getenv("ENCRYPTION_KEY", "ipccloud-default-encryption-key-32b"),
		AccessTTLMin:    getenvInt("ACCESS_TTL_MIN", 120),
		RefreshTTLDays:  getenvInt("REFRESH_TTL_DAYS", 7),
		StreamIdleSec:   getenvInt("STREAM_IDLE_SEC", 30),
		PlayTokenTTLMin: getenvInt("PLAY_TOKEN_TTL_MIN", 10),
		MQTTBroker:      getenv("MQTT_BROKER", "tcp://emqx:1883"),
		MQTTUsername:    getenv("MQTT_USERNAME", ""),
		MQTTPassword:    getenv("MQTT_PASSWORD", ""),
		MQTTTLS:         getenv("MQTT_TLS", "false") == "true",
		SIPIP:           getenv("SIP_IP", ""),
		SIPPort:         getenvInt("SIP_PORT", 5060),
		SIPServerID:     getenv("SIP_SERVER_ID", "34020000002000000001"),
		SIPRealm:        getenv("SIP_REALM", "3402000000"),
		AdminUser:       getenv("ADMIN_USER", "admin"),
		AdminPassword:   getenv("ADMIN_PASSWORD", "Admin@12345"),
	}
	if c.SIPDomain == "" {
		if len(c.SIPServerID) >= 10 {
			c.SIPDomain = c.SIPServerID[:10]
		}
	}
	return c
}
