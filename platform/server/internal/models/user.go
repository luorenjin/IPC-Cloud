package models

import (
	"crypto/rand"
	"encoding/base64"

	"github.com/jetscam/ipccloud/server/internal/crypto"
)

// NewUser 创建用户（Argon2id 哈希）。
func NewUser(tenantID, username, password, name string) (*User, error) {
	return &User{
		TenantID: tenantID, Username: username,
		PwdHash: crypto.HashPassword(password), Name: name, Status: "active",
		CreatedAt: NowMilli(), UpdatedAt: NowMilli(),
	}, nil
}

// NewID 生成资源 ID（短随机，带可读前缀由调用方拼接）。
func NewID() string {
	b := make([]byte, 9)
	rand.Read(b)
	return base64.RawURLEncoding.EncodeToString(b)
}
