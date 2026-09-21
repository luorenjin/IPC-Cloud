/**
 * @file gk_crypto.c
 * @brief GK7205V200 文件型安全存储与随机数
 *
 * GK7205V200 无硬件安全存储，用受限权限的文件代替（0600/0700）。
 * 写入经临时文件 + rename 原子替换，避免掉电产生半截凭据文件。
 *
 * random 的两条路径不对称，务必分清：
 *   - Linux（目标平台）：只读 /dev/urandom，读不满即返回错误，绝不退化为
 *     rand()——鉴权挑战值的不可预测性完全依赖它，可预测的随机数等于没有鉴权。
 *   - Windows（仅用于本机联调/单元测试，非目标平台）：用 rand_s()（底层
 *     RtlGenRandom，CSPRNG 强度足够测试可信），因为 Windows 上不存在
 *     /dev/urandom。这只是让测试能在开发机上全过，不代表 Linux 路径可以
 *     放宽——Linux 分支永远只认 /dev/urandom。
 */
#ifdef _WIN32
#define _CRT_RAND_S     /* 必须在 <stdlib.h> 之前定义，才能拿到 rand_s */
#endif

#include "hal/hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define SEC_DIR_DEFAULT "/etc/ipc/sec"

static const char *sec_dir(void)
{
    const char *d = getenv("IPC_SEC_DIR");
    return (d && d[0]) ? d : SEC_DIR_DEFAULT;
}

/* key 必须是纯文件名：拒绝分隔符与 ".."，否则可写到存储目录之外 */
static bool key_is_safe(const char *key)
{
    if (!key || !key[0]) return false;
    if (strchr(key, '/') || strchr(key, '\\')) return false;
    if (strcmp(key, ".") == 0 || strcmp(key, "..") == 0) return false;
    if (strstr(key, "..") != NULL) return false;
    return true;
}

static void ensure_dir(void)
{
#ifdef _WIN32
    _mkdir(sec_dir());
#else
    mkdir(sec_dir(), 0700);
#endif
}

static bool sec_path(const char *key, char *out, size_t cap)
{
    if (!key_is_safe(key)) return false;
    snprintf(out, cap, "%s/%s", sec_dir(), key);
    return true;
}

static hal_err_t c_get_caps(hal_crypto_caps_t *caps)
{
    if (!caps) return HAL_EINVAL;
    memset(caps, 0, sizeof(*caps));
    /* 无硬件安全单元：软件实现，如实声明 */
    caps->hw_secure_storage = false;
    caps->hw_rng = false;
    caps->hw_sign = false;
    caps->sign_algs_mask = 0;
    return HAL_OK;
}

static hal_err_t c_read(const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    char path[256];
    FILE *fp;
    size_t n;

    if (!key || !buf || !len) return HAL_EINVAL;
    /* 设备私钥不可读（hal_crypto.h 契约） */
    if (strcmp(key, HAL_SEC_KEY_DEVICE_KEY) == 0) return HAL_ENOTSUP;
    if (!sec_path(key, path, sizeof(path))) return HAL_EINVAL;

    fp = fopen(path, "rb");
    if (!fp) return HAL_ENODEV;
    n = fread(buf, 1, cap, fp);
    fclose(fp);
    *len = n;
    return HAL_OK;
}

static hal_err_t c_write(const char *key, const uint8_t *data, size_t len)
{
    char path[256], tmp[300];
    FILE *fp;

    if (!key || !data || len == 0 || len > HAL_SEC_VALUE_MAX) return HAL_EINVAL;
    if (!sec_path(key, path, sizeof(path))) return HAL_EINVAL;

    ensure_dir();
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    fp = fopen(tmp, "wb");
    if (!fp) return HAL_EIO;
    if (fwrite(data, 1, len, fp) != len) { fclose(fp); remove(tmp); return HAL_EIO; }
    fflush(fp);
#ifndef _WIN32
    fsync(fileno(fp));
#endif
    fclose(fp);

#ifndef _WIN32
    chmod(tmp, 0600);
#endif
    /* Windows 的 rename 不覆盖已存在文件，先删再换 */
#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp, path) != 0) { remove(tmp); return HAL_EIO; }
    return HAL_OK;
}

static hal_err_t c_delete(const char *key)
{
    char path[256];
    if (!key) return HAL_EINVAL;
    if (!sec_path(key, path, sizeof(path))) return HAL_EINVAL;
    return remove(path) == 0 ? HAL_OK : HAL_ENODEV;
}

static hal_err_t c_exists(const char *key, bool *ex)
{
    char path[256];
    FILE *fp;
    if (!key || !ex) return HAL_EINVAL;
    if (!sec_path(key, path, sizeof(path))) return HAL_EINVAL;
    fp = fopen(path, "rb");
    *ex = (fp != NULL);
    if (fp) fclose(fp);
    return HAL_OK;
}

static hal_err_t c_get_cert(char *pem, size_t cap, size_t *len)
{
    size_t n = 0;
    hal_err_t rc;
    if (!pem || cap == 0) return HAL_EINVAL;
    rc = c_read(HAL_SEC_KEY_DEVICE_CERT, (uint8_t *)pem, cap - 1, &n);
    if (rc != HAL_OK) return rc;
    pem[n] = '\0';
    if (len) *len = n;
    return HAL_OK;
}

static hal_err_t c_sign(hal_sign_alg_t alg, const uint8_t *digest, size_t dlen,
                        uint8_t *sig, size_t cap, size_t *slen)
{
    (void)alg; (void)digest; (void)dlen; (void)sig; (void)cap; (void)slen;
    /* 无设备证书体系，不提供伪签名——伪签名会让上层误以为已具备身份认证 */
    return HAL_ENOTSUP;
}

static hal_err_t c_random(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return HAL_EINVAL;

#ifdef _WIN32
    /* 仅用于 Windows 开发机联调/单测：/dev/urandom 在 Windows 上不存在。
       rand_s() 底层是 RtlGenRandom，CSPRNG 强度足够作为本地测试环境的
       随机源。目标平台是 Linux，见下面 #else 分支——那里绝不允许退化。 */
    {
        size_t i = 0;
        while (i < len) {
            unsigned int v;
            size_t take;
            if (rand_s(&v) != 0) return HAL_EIO;
            take = (len - i < sizeof(v)) ? (len - i) : sizeof(v);
            memcpy(buf + i, &v, take);
            i += take;
        }
        return HAL_OK;
    }
#else
    /* 目标平台路径：只认 /dev/urandom，读不满即失败，绝不退化为 rand()。
       鉴权挑战值的不可预测性完全依赖这里，可预测的随机数等于没有鉴权。 */
    {
        FILE *fp = fopen("/dev/urandom", "rb");
        size_t n;
        if (!fp) return HAL_ENODEV;
        n = fread(buf, 1, len, fp);
        fclose(fp);
        return (n == len) ? HAL_OK : HAL_EIO;
    }
#endif
}

const hal_crypto_ops_t gk_crypto_ops = {
    c_get_caps,
    c_read,
    c_write,
    c_delete,
    c_exists,
    c_get_cert,
    c_sign,
    c_random
};
