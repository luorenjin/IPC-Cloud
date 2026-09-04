/**
 * @file hal_crypto.h
 * @brief HAL v1 —— 安全存储与设备身份（证书/私钥/验证码/随机数）
 *
 * 可选模块：hal_ops_t.crypto 可为 NULL，此时 core 层退化为文件系统权限保护的软存储，
 * 能力清单须声明 security.hw_secure=false。有 OTP/eFuse/加密引擎的平台应实现本接口。
 *
 * 私钥不出设备：sign() 在 HAL 内完成，core 层只拿到签名。
 */
#ifndef IPC_HAL_CRYPTO_H
#define IPC_HAL_CRYPTO_H

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 安全存储预定义键 */
#define HAL_SEC_KEY_DEVICE_ID     "device_id"      /**< 17 位 DeviceID */
#define HAL_SEC_KEY_VERIFY_CODE   "verify_code"    /**< 6 位验证码 */
#define HAL_SEC_KEY_DEVICE_CERT   "device_cert"    /**< PEM */
#define HAL_SEC_KEY_DEVICE_KEY    "device_key"     /**< 私钥（仅 HAL 内可读） */
#define HAL_SEC_KEY_PSK           "psk"
#define HAL_SEC_KEY_LOCAL_USER    "local_user"     /**< 本地账号哈希 */

#define HAL_SEC_VALUE_MAX 4096

typedef enum {
    HAL_SIGN_ECDSA_P256_SHA256 = 0,
    HAL_SIGN_RSA2048_SHA256 = 1
} hal_sign_alg_t;

typedef struct {
    bool hw_secure_storage;   /**< OTP/eFuse/TEE */
    bool hw_rng;
    bool hw_sign;
    uint32_t sign_algs_mask;  /**< bit(HAL_SIGN_x) */
} hal_crypto_caps_t;

typedef struct hal_crypto_ops {
    hal_err_t (*get_caps)(hal_crypto_caps_t *caps);

    /* 安全存储：key 为 ASCII 名称；HAL_SEC_KEY_DEVICE_KEY 禁止 read（返回 HAL_ENOTSUP） */
    hal_err_t (*secure_read)(const char *key, uint8_t *buf, size_t cap, size_t *len);
    hal_err_t (*secure_write)(const char *key, const uint8_t *data, size_t len);
    hal_err_t (*secure_delete)(const char *key);
    hal_err_t (*secure_exists)(const char *key, bool *exists);

    /* 身份 */
    hal_err_t (*get_device_cert)(char *pem, size_t cap, size_t *len);
    hal_err_t (*sign)(hal_sign_alg_t alg, const uint8_t *digest, size_t digest_len,
                      uint8_t *sig, size_t sig_cap, size_t *sig_len);

    /* 随机数 */
    hal_err_t (*random)(uint8_t *buf, size_t len);
} hal_crypto_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_CRYPTO_H */
