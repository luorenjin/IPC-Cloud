/**
 * @file mock_sys.c
 * @brief mock 平台 —— 系统信息、时钟、看门狗、OTA 双分区（文件模拟）、安全存储（文件模拟）
 *
 * OTA 槽位：./mock_state/slot_A.bin、slot_B.bin、ota_state.txt
 * 安全存储：./mock_state/secure_<key>.bin
 */
#include "hal/hal.h"
#include "mock_os.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define MKDIR(p) mkdir(p, 0700)
#endif

#define STATE_DIR "mock_state"

static uint64_t g_boot_us;
static bool g_wdt_on;
static uint32_t g_wdt_timeout_s;
static uint64_t g_wdt_last_feed;

/* ---- OTA 状态 ---- */
typedef struct {
    int  current_slot;
    bool pending;
    char ver[2][HAL_NAME_MAX];
} ota_state_file_t;

static ota_state_file_t g_ota = { 0, false, { "1.0.0-mock", "" } };
static FILE *g_ota_fp;
static int   g_ota_slot = -1;
static uint32_t g_ota_total, g_ota_written;

static void ensure_dir(void) { MKDIR(STATE_DIR); }

static void ota_state_save(void)
{
    FILE *fp;
    ensure_dir();
    fp = fopen(STATE_DIR "/ota_state.txt", "w");
    if (!fp) return;
    fprintf(fp, "%d %d %s %s\n", g_ota.current_slot, g_ota.pending ? 1 : 0,
            g_ota.ver[0][0] ? g_ota.ver[0] : "-", g_ota.ver[1][0] ? g_ota.ver[1] : "-");
    fclose(fp);
}

static void ota_state_load(void)
{
    FILE *fp = fopen(STATE_DIR "/ota_state.txt", "r");
    int pend = 0;
    if (!fp) return;
    if (fscanf(fp, "%d %d %63s %63s", &g_ota.current_slot, &pend, g_ota.ver[0], g_ota.ver[1]) == 4) {
        g_ota.pending = pend != 0;
        if (strcmp(g_ota.ver[0], "-") == 0) g_ota.ver[0][0] = 0;
        if (strcmp(g_ota.ver[1], "-") == 0) g_ota.ver[1][0] = 0;
    }
    fclose(fp);
}

void mock_sys_boot(void)
{
    g_boot_us = mock_os_monotonic_us();
    ota_state_load();
}

static hal_err_t s_get_info(hal_sys_info_t *info)
{
    if (!info) return HAL_EINVAL;
    memset(info, 0, sizeof(*info));
    strncpy(info->platform_id, "mock", HAL_NAME_MAX - 1);
    strncpy(info->chip_id, "MOCK00000000CAFE", HAL_NAME_MAX - 1);
    strncpy(info->soc_name, "x86-mock", HAL_NAME_MAX - 1);
    info->cpu_mhz = 2400; info->cpu_cores = 4;
    info->mem_total_kb = 64 * 1024;   /* 模拟 V200 预算 */
    info->flash_total_kb = 128 * 1024;
    info->hal_version = HAL_API_VERSION;
    return HAL_OK;
}

static hal_err_t s_get_stats(hal_sys_stats_t *st)
{
    if (!st) return HAL_EINVAL;
    memset(st, 0, sizeof(*st));
    st->mem_total_kb = 64 * 1024;
    st->mem_free_kb = 20 * 1024;
    st->mem_avail_kb = 24 * 1024;
    st->cpu_usage_pct = 12;
    st->temp_milli_c = 45000;
    st->uptime_s = (mock_os_monotonic_us() - g_boot_us) / 1000000ULL;
    return HAL_OK;
}

static hal_err_t s_boot_reason(hal_boot_reason_t *r) { if (!r) return HAL_EINVAL; *r = HAL_BOOT_POWER_ON; return HAL_OK; }
static uint64_t  s_monotonic_us(void) { return mock_os_monotonic_us(); }
static hal_err_t s_set_wallclock(int64_t utc) { (void)utc; return HAL_OK; }
static hal_err_t s_reboot(void) { printf("[mock] reboot requested\n"); return HAL_OK; }
static hal_err_t s_factory_reset(bool keep_net) { printf("[mock] factory reset keep_net=%d\n", keep_net); return HAL_OK; }

static hal_err_t s_wdt_enable(uint32_t t) { if (t == 0) return HAL_EINVAL; g_wdt_on = true; g_wdt_timeout_s = t; g_wdt_last_feed = mock_os_monotonic_us(); return HAL_OK; }
static hal_err_t s_wdt_feed(void) { if (!g_wdt_on) return HAL_ESTATE; g_wdt_last_feed = mock_os_monotonic_us(); return HAL_OK; }
static hal_err_t s_wdt_disable(void) { g_wdt_on = false; return HAL_OK; }

static hal_err_t s_ota_get_state(hal_ota_state_t *st)
{
    if (!st) return HAL_EINVAL;
    memset(st, 0, sizeof(*st));
    st->current_slot = g_ota.current_slot;
    st->other_slot = 1 - g_ota.current_slot;
    st->pending_confirm = g_ota.pending;
    strncpy(st->current_version, g_ota.ver[g_ota.current_slot], HAL_NAME_MAX - 1);
    strncpy(st->other_version, g_ota.ver[1 - g_ota.current_slot], HAL_NAME_MAX - 1);
    st->slot_size_kb = 40 * 1024;
    return HAL_OK;
}

static hal_err_t s_ota_begin(int slot, uint32_t total)
{
    char path[64];
    if (slot < 0 || slot > 1 || total == 0) return HAL_EINVAL;
    if (slot == g_ota.current_slot) return HAL_EINVAL;      /* 不允许写当前运行分区 */
    if (total > 40u * 1024u * 1024u) return HAL_ENOMEM;
    if (g_ota_fp) return HAL_EBUSY;
    ensure_dir();
    snprintf(path, sizeof(path), STATE_DIR "/slot_%c.bin", slot == 0 ? 'A' : 'B');
    g_ota_fp = fopen(path, "w+b");   /* 读写：ota_end 需回读整段镜像校验 */
    if (!g_ota_fp) return HAL_EIO;
    g_ota_slot = slot; g_ota_total = total; g_ota_written = 0;
    return HAL_OK;
}

static hal_err_t s_ota_write(const void *data, uint32_t len)
{
    if (!g_ota_fp) return HAL_ESTATE;
    if (!data || len == 0) return HAL_EINVAL;
    if (g_ota_written + len > g_ota_total) return HAL_EINVAL;
    if (fwrite(data, 1, len, g_ota_fp) != len) return HAL_EIO;
    g_ota_written += len;
    return HAL_OK;
}

/* 简化：mock 不实现 SHA-256，用 FNV-1a 折叠到 32 字节做一致性校验（真实平台必须用 SHA-256） */
static void mock_digest(FILE *fp, uint8_t out[32])
{
    uint64_t h = 1469598103934665603ULL;
    int c;
    rewind(fp);
    while ((c = fgetc(fp)) != EOF) {
        h ^= (uint8_t)c;
        h *= 1099511628211ULL;
    }
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h >> ((i % 8) * 8));
}

void mock_ota_expected_digest(const void *data, size_t len, uint8_t out[32])
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h >> ((i % 8) * 8));
}

static hal_err_t s_ota_end(const uint8_t sha[32])
{
    uint8_t got[32];
    if (!g_ota_fp) return HAL_ESTATE;
    if (!sha) return HAL_EINVAL;
    if (g_ota_written != g_ota_total) { fclose(g_ota_fp); g_ota_fp = NULL; return HAL_ECORRUPT; }
    fflush(g_ota_fp);
    mock_digest(g_ota_fp, got);
    fclose(g_ota_fp); g_ota_fp = NULL;
    if (memcmp(got, sha, 32) != 0) return HAL_ECORRUPT;
    snprintf(g_ota.ver[g_ota_slot], HAL_NAME_MAX, "staged-%u", g_ota_total);
    ota_state_save();
    return HAL_OK;
}

static hal_err_t s_ota_switch(int slot)
{
    if (slot < 0 || slot > 1) return HAL_EINVAL;
    if (slot == g_ota.current_slot) return HAL_EINVAL;
    if (!g_ota.ver[slot][0]) return HAL_ESTATE;   /* 目标槽未写入 */
    g_ota.current_slot = slot;
    g_ota.pending = true;
    ota_state_save();
    return HAL_OK;
}

static hal_err_t s_ota_confirm(void)
{
    if (!g_ota.pending) return HAL_ESTATE;
    g_ota.pending = false;
    ota_state_save();
    return HAL_OK;
}

static hal_err_t s_ota_abort(void)
{
    if (g_ota_fp) { fclose(g_ota_fp); g_ota_fp = NULL; }
    g_ota_slot = -1;
    return HAL_OK;
}

const hal_sys_ops_t mock_sys_ops = {
    s_get_info, s_get_stats, s_boot_reason, s_monotonic_us, s_set_wallclock,
    s_reboot, s_factory_reset, s_wdt_enable, s_wdt_feed, s_wdt_disable,
    s_ota_get_state, s_ota_begin, s_ota_write, s_ota_end, s_ota_switch, s_ota_confirm, s_ota_abort
};

/* ---- 安全存储（文件模拟）---- */

static void sec_path(const char *key, char *out, size_t cap)
{
    snprintf(out, cap, STATE_DIR "/secure_%s.bin", key);
}

static hal_err_t c_get_caps(hal_crypto_caps_t *caps)
{
    if (!caps) return HAL_EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->hw_secure_storage = false; caps->hw_rng = false; caps->hw_sign = false;
    caps->sign_algs_mask = (1u << HAL_SIGN_ECDSA_P256_SHA256);
    return HAL_OK;
}

static hal_err_t c_read(const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    char path[128]; FILE *fp; size_t n;
    if (!key || !buf || !len) return HAL_EINVAL;
    if (strcmp(key, HAL_SEC_KEY_DEVICE_KEY) == 0) return HAL_ENOTSUP;
    sec_path(key, path, sizeof(path));
    fp = fopen(path, "rb");
    if (!fp) return HAL_ENODEV;
    n = fread(buf, 1, cap, fp);
    fclose(fp);
    *len = n;
    return HAL_OK;
}

static hal_err_t c_write(const char *key, const uint8_t *data, size_t len)
{
    char path[128]; FILE *fp;
    if (!key || !data || len == 0 || len > HAL_SEC_VALUE_MAX) return HAL_EINVAL;
    ensure_dir();
    sec_path(key, path, sizeof(path));
    fp = fopen(path, "wb");
    if (!fp) return HAL_EIO;
    if (fwrite(data, 1, len, fp) != len) { fclose(fp); return HAL_EIO; }
    fclose(fp);
    return HAL_OK;
}

static hal_err_t c_delete(const char *key)
{
    char path[128];
    if (!key) return HAL_EINVAL;
    sec_path(key, path, sizeof(path));
    return remove(path) == 0 ? HAL_OK : HAL_ENODEV;
}

static hal_err_t c_exists(const char *key, bool *ex)
{
    char path[128]; FILE *fp;
    if (!key || !ex) return HAL_EINVAL;
    sec_path(key, path, sizeof(path));
    fp = fopen(path, "rb");
    *ex = fp != NULL;
    if (fp) fclose(fp);
    return HAL_OK;
}

static hal_err_t c_get_cert(char *pem, size_t cap, size_t *len)
{
    size_t n;
    hal_err_t rc = c_read(HAL_SEC_KEY_DEVICE_CERT, (uint8_t *)pem, cap ? cap - 1 : 0, &n);
    if (rc != HAL_OK) return rc;
    pem[n] = 0;
    if (len) *len = n;
    return HAL_OK;
}

static hal_err_t c_sign(hal_sign_alg_t alg, const uint8_t *digest, size_t dlen, uint8_t *sig, size_t cap, size_t *slen)
{
    if (alg != HAL_SIGN_ECDSA_P256_SHA256 || !digest || dlen != 32 || !sig || !slen) return HAL_EINVAL;
    if (cap < 64) return HAL_ENOMEM;
    /* 伪签名：mock 无私钥，输出 digest 重复两次；仅供链路联调，平台校验端需配置 mock 公钥跳过 */
    memcpy(sig, digest, 32); memcpy(sig + 32, digest, 32);
    *slen = 64;
    return HAL_OK;
}

static hal_err_t c_random(uint8_t *buf, size_t len)
{
    static bool seeded;
    if (!buf || len == 0) return HAL_EINVAL;
    if (!seeded) { srand((unsigned)(mock_os_monotonic_us() & 0xFFFFFFFFu)); seeded = true; }
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(rand() & 0xFF);
    return HAL_OK;
}

const hal_crypto_ops_t mock_crypto_ops = {
    c_get_caps, c_read, c_write, c_delete, c_exists, c_get_cert, c_sign, c_random
};
