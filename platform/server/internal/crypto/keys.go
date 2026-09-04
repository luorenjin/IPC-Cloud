package crypto

// 主加密密钥（启动时注入，来自 ENCRYPTION_KEY）。
var masterKey string

func SetMasterKey(k string) { masterKey = k }

// Enc/Dec 使用主密钥加解密。
func Enc(plain string) string { return Encrypt(masterKey, plain) }
func Dec(enc string) (string, error) {
	if masterKey == "" {
		return "", ErrNoKey
	}
	return Decrypt(masterKey, enc)
}

type keyError string

func (e keyError) Error() string { return string(e) }

const ErrNoKey keyError = "master key not set"
