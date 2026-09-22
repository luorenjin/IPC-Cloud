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
#include <errno.h>
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

/* 逐级创建 dir 的每一段路径。真机 rootfs 打包时 /etc/ipc/sec 已预建
   （Task 11 的 mkdir -p），但 hal_conformance 等测试直接以默认路径
   "/etc/ipc/sec" 跑在一次性环境里，父目录 /etc/ipc 未必存在；
   单层 mkdir 遇到缺失的父目录会直接失败，之后的 fopen 也会连带失败
   （ENOENT），c_write 会把本应能成功的写入错误地报成 HAL_EIO。 */
static void ensure_dir(void)
{
    char buf[HAL_PATH_MAX];
    char *p;
    size_t n = snprintf(buf, sizeof(buf), "%s", sec_dir());
    if (n == 0 || n >= sizeof(buf)) return;

    for (p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
#ifdef _WIN32
            _mkdir(buf);
#else
            mkdir(buf, 0700);
#endif
            *p = '/';
        }
    }
#ifdef _WIN32
    _mkdir(buf);
#else
    mkdir(buf, 0700);
#endif
}

static bool sec_path(const char *key, char *out, size_t cap)
{
    int n;
    if (!key_is_safe(key)) return false;
    n = snprintf(out, cap, "%s/%s", sec_dir(), key);
    /* snprintf 返回“若缓冲区足够本应写入的长度”；截断（>= cap）不会引入新的
       “..”，不构成目录逃逸，但会把两个仅后缀不同的超长键悄悄拼成同一路径，
       造成键混淆/互相覆盖——必须当失败处理。负数同样视为失败（与
       console_auth.c 的 fmt_safe 用同一判定方式）。 */
    if (n < 0 || (size_t)n >= cap) return false;
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

    errno = 0;
    fp = fopen(path, "rb");
    if (!fp) {
        /* 评审 Ruling 19（C-2）：区分"文件从未创建过"（ENOENT，上层据此判定
           "从未配置"，允许 /api/v1/auth/activate 首次激活）与"文件存在但打不开"
           （权限被改、同名目录、磁盘故障等，HAL_EIO）。此前两者一律折叠成
           HAL_ENODEV，攻击者只需让 fopen 以任何方式失败（例如把凭据文件替换成
           同名目录，触发 EISDIR）就能让一台已激活设备重新呈现"从未配置"，
           进而用自己的口令重新激活——fail-open。errno 必须紧跟 fopen 失败
           立即读取，不能在其后插入任何可能改写 errno 的调用。 */
        return (errno == ENOENT) ? HAL_ENODEV : HAL_EIO;
    }
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
    /* fflush/fsync/fclose 任一失败都说明数据未必已落盘（介质写满、I/O 错误等），
       绝不能继续走 rename——否则会把“写失败”报成 HAL_OK，掉电后凭据静默丢失
       且调用方毫无察觉。对照 core/src/os.c 的 os_file_write_atomic，同样显式
       检查 fflush 返回值。 */
    if (fflush(fp) != 0) { fclose(fp); remove(tmp); return HAL_EIO; }
#ifndef _WIN32
    if (fsync(fileno(fp)) != 0) { fclose(fp); remove(tmp); return HAL_EIO; }
#endif
    if (fclose(fp) != 0) { remove(tmp); return HAL_EIO; }

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
