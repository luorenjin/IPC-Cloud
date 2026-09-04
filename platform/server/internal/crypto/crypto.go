// Package crypto 凭据加密与口令哈希（接入规范 §11：AES-256-GCM、Argon2id）。
package crypto

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"

	"golang.org/x/crypto/argon2"
)

// Encrypt AES-256-GCM；key 任意长度，内部派生 32 字节。
func Encrypt(key, plain string) string {
	k := sha256.Sum256([]byte(key))
	block, _ := aes.NewCipher(k[:])
	gcm, _ := cipher.NewGCM(block)
	nonce := make([]byte, gcm.NonceSize())
	rand.Read(nonce)
	out := gcm.Seal(nil, nonce, []byte(plain), nil)
	return base64.StdEncoding.EncodeToString(append(nonce, out...))
}

func Decrypt(key, enc string) (string, error) {
	raw, err := base64.StdEncoding.DecodeString(enc)
	if err != nil {
		return "", err
	}
	k := sha256.Sum256([]byte(key))
	block, _ := aes.NewCipher(k[:])
	gcm, _ := cipher.NewGCM(block)
	if len(raw) < gcm.NonceSize() {
		return "", errors.New("ciphertext too short")
	}
	plain, err := gcm.Open(nil, raw[:gcm.NonceSize()], raw[gcm.NonceSize():], nil)
	if err != nil {
		return "", err
	}
	return string(plain), nil
}

// HashPassword Argon2id：格式 $argon2id$v=19$m,t,p$salt$hash。
func HashPassword(password string) string {
	salt := make([]byte, 16)
	rand.Read(salt)
	const timeCost, memKiB, threads = 2, 64 * 1024, 4
	h := argon2.IDKey([]byte(password), salt, timeCost, memKiB, uint8(threads), 32)
	return fmt.Sprintf("$argon2id$v=19$m=%d,t=%d,p=%d$%s$%s", memKiB, timeCost, threads,
		base64.RawStdEncoding.EncodeToString(salt), base64.RawStdEncoding.EncodeToString(h))
}

func VerifyPassword(password, encoded string) bool {
	parts := strings.Split(encoded, "$")
	if len(parts) != 6 || parts[1] != "argon2id" {
		return false
	}
	var m, t uint32
	var p uint8
	fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &m, &t, &p)
	salt, err1 := base64.RawStdEncoding.DecodeString(parts[4])
	want, err2 := base64.RawStdEncoding.DecodeString(parts[5])
	if err1 != nil || err2 != nil {
		return false
	}
	got := argon2.IDKey([]byte(password), salt, t, m, p, uint32(len(want)))
	return hmac.Equal(got, want)
}

// HMACSHA256Hex 平台侧 VerifyCode 存储：HMAC-SHA256(key, code)。
func HMACSHA256Hex(key, data string) string {
	m := hmac.New(sha256.New, []byte(key))
	m.Write([]byte(data))
	return hex.EncodeToString(m.Sum(nil))
}

// RandomHex 随机十六进制。
func RandomHex(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return hex.EncodeToString(b)
}
