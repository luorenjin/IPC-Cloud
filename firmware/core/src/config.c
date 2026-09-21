/**
 * @file config.c
 * @brief 配置中心：规则校验、点分键、原子持久化、变更事件
 *
 * 值来源优先级：profile 默认（不持久化）< 持久化用户配置。
 * 持久化格式为扁平 JSON 对象 {"a.b.c": value}，写入采用 .tmp + 原子替换。
 * 规则字符串（key_pattern / enum_csv）在登记时复制到堆，deinit 统一释放。
 */
#include "core/config.h"
#include "core/json.h"
#include "core/os.h"
#include "core/log.h"
#include "core/event_bus.h"
#include "core/profile.h"
#include "hal/hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MOD "cfg"
#define RULES_MAX 128

typedef struct {
    char       key[CFG_KEY_MAX];
    cfg_type_t type;
    bool       dirty;      /**< 属于用户配置，需持久化 */
    bool       set;
    union {
        bool    b;
        int64_t i;
        char   *s;         /**< STR 与 JSON 共用 */
    } v;
} entry_t;

static struct {
    bool        inited;
    entry_t    *items;
    size_t      count, cap;
    cfg_rule_t  rules[RULES_MAX];
    size_t      rule_count;
    char        path[HAL_PATH_MAX];
    os_mutex_t *mu;
} g;

static char *dup_str(const char *s)
{
    char *d;
    if (!s) return NULL;
    d = (char *)malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

/* ---------------- 键匹配 ---------------- */

static int split_path(const char *s, char out[16][32])
{
    int n = 0; size_t i = 0, start = 0;
    for (;;) {
        if (s[i] == '.' || s[i] == 0) {
            size_t len = i - start;
            if (len == 0 || len >= 32 || n >= 16) return -1;
            memcpy(out[n], s + start, len); out[n][len] = 0; n++;
            if (s[i] == 0) break;
            start = ++i;
        } else i++;
    }
    return n;
}

/** 模式匹配：'*' 恰好匹配一段 */
static bool key_matches(const char *pattern, const char *key)
{
    char pa[16][32], ka[16][32];
    int pn = split_path(pattern, pa);
    int kn = split_path(key, ka);
    if (pn < 0 || kn < 0 || pn != kn) return false;
    for (int i = 0; i < pn; i++) {
        if (strcmp(pa[i], "*") != 0 && strcmp(pa[i], ka[i]) != 0) return false;
    }
    return true;
}

static const cfg_rule_t *rule_for(const char *key)
{
    for (size_t i = 0; i < g.rule_count; i++) {
        if (key_matches(g.rules[i].key_pattern, key)) return &g.rules[i];
    }
    return NULL;
}

static entry_t *find_entry(const char *key)
{
    for (size_t i = 0; i < g.count; i++) if (strcmp(g.items[i].key, key) == 0) return &g.items[i];
    return NULL;
}

static entry_t *ensure_entry(const char *key)
{
    entry_t *e = find_entry(key);
    if (e) return e;
    if (g.count == g.cap) {
        size_t ncap = g.cap ? g.cap * 2 : 32;
        entry_t *ni = (entry_t *)realloc(g.items, ncap * sizeof(*ni));
        if (!ni) return NULL;
        g.items = ni; g.cap = ncap;
    }
    e = &g.items[g.count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->key, key, CFG_KEY_MAX - 1);
    return e;
}

static void notify_changed(const char *key)
{
    event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EVT_CONFIG_CHANGED; e.ch = -1;
    strncpy(e.payload.str, key, sizeof(e.payload.str) - 1);
    event_bus_publish(&e);
}

static hal_err_t persist_locked(void)
{
    json_t *root = json_new_object();
    char *txt;
    int rc;
    if (!root) return HAL_ENOMEM;
    for (size_t i = 0; i < g.count; i++) {
        const entry_t *e = &g.items[i];
        const cfg_rule_t *r = rule_for(e->key);
        json_t *v = NULL;
        if (!e->dirty || !e->set) continue;
        /* 只写键（口令类）不落盘：持久化文件是要能被导出/备份的，
           而凭据进备份等于把口令写在卡片上（接入规范 §11）。 */
        if (r && r->write_only) continue;
        switch (e->type) {
        case CFG_T_BOOL: v = json_new_bool(e->v.b); break;
        case CFG_T_INT:  v = json_new_int(e->v.i); break;
        case CFG_T_STR:  v = json_new_string(e->v.s ? e->v.s : ""); break;
        default:         v = json_parse(e->v.s ? e->v.s : "null", 0, NULL, 0); if (!v) v = json_new_null(); break;
        }
        if (!v || json_object_set(root, e->key, v) != 0) { json_free(v); json_free(root); return HAL_ENOMEM; }
    }
    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = os_file_write_atomic(g.path, txt, strlen(txt));
    free(txt);
    return rc == 0 ? HAL_OK : HAL_EIO;
}

/* ---------------- 校验与写入 ---------------- */

static hal_err_t validate(const cfg_rule_t *r, cfg_type_t type, int64_t i, const char *s, const char **why)
{
    if (r->type != type) { *why = "type"; return HAL_EINVAL; }
    if (type == CFG_T_INT && (i < r->min || i > r->max)) { *why = "range"; return HAL_EINVAL; }
    /* STR 的 min/max 在规则表里表示字节长度上下限，min>0 才生效：
       现有字符串键全是 min=max=0，此项对它们无影响；密码类键则靠它把
       “8–63 位”真的卡在固件侧，而不是只写在注释里（口径与本地控制台一致）。*/
    if (type == CFG_T_STR && r->min > 0) {
        size_t len = s ? strlen(s) : 0;
        if ((int64_t)len < r->min || (r->max > 0 && (int64_t)len > r->max)) {
            *why = "length"; return HAL_EINVAL;
        }
    }
    if ((type == CFG_T_STR || type == CFG_T_JSON) && r->enum_csv) {
        char list[CFG_STR_MAX * 4];
        size_t n = strlen(r->enum_csv);
        bool ok = false;
        char *tok, *save = NULL;
        if (n >= sizeof(list)) { *why = "enum"; return HAL_EINVAL; }
        memcpy(list, r->enum_csv, n + 1);
        for (tok = list; tok && *tok; ) {
            char *comma = strchr(tok, ',');
            if (comma) *comma = 0;
            if (s && strcmp(tok, s) == 0) { ok = true; }
            if (!comma) break;
            tok = comma + 1;
        }
        (void)save;
        if (!ok) { *why = "enum"; return HAL_EINVAL; }
    }
    return HAL_OK;
}

/** 调用者持锁。persist=true 时写文件并标 dirty */
static hal_err_t set_value(const char *key, cfg_type_t type, int64_t i, const char *s,
                           bool persist, const char **why)
{
    const cfg_rule_t *r;
    entry_t *e;
    hal_err_t rc;

    *why = "";
    if (!key || strlen(key) >= CFG_KEY_MAX) return HAL_EINVAL;
    r = rule_for(key);
    if (!r) { *why = "unknown_key"; return HAL_EINVAL; }
    rc = validate(r, type, i, s, why);
    if (rc != HAL_OK) return rc;

    e = ensure_entry(key);
    if (!e) return HAL_ENOMEM;
    if (e->set && e->type != type) {
        if ((e->type == CFG_T_STR || e->type == CFG_T_JSON) && e->v.s) free(e->v.s);
        e->v.s = NULL;
    }
    e->type = type;
    switch (type) {
    case CFG_T_BOOL: e->v.b = (i != 0); break;
    case CFG_T_INT:  e->v.i = i; break;
    default: {
        char *dup = dup_str(s ? s : "");
        if (!dup) return HAL_ENOMEM;
        if (e->v.s) free(e->v.s);
        e->v.s = dup;
        break;
    }
    }
    e->set = true;
    if (persist) {
        e->dirty = true;
        rc = persist_locked();
        if (rc != HAL_OK) LOGE(MOD, "persist failed: %s", hal_strerror(rc));
    }
    notify_changed(key);
    return HAL_OK;
}

/* ---------------- 规则登记 ---------------- */

hal_err_t cfg_register_rules(const cfg_rule_t *rules, size_t n)
{
    if (!g.inited) return HAL_ESTATE;
    if (!rules) return HAL_EINVAL;
    os_mutex_lock(g.mu);
    for (size_t i = 0; i < n; i++) {
        cfg_rule_t dst;
        if (!rules[i].key_pattern) continue;
        if (g.rule_count >= RULES_MAX) { os_mutex_unlock(g.mu); return HAL_ENOMEM; }
        dst = rules[i];
        dst.key_pattern = dup_str(rules[i].key_pattern);
        dst.enum_csv = rules[i].enum_csv ? dup_str(rules[i].enum_csv) : NULL;
        if (!dst.key_pattern || (rules[i].enum_csv && !dst.enum_csv)) {
            free((void *)dst.key_pattern); free((void *)dst.enum_csv);
            os_mutex_unlock(g.mu);
            return HAL_ENOMEM;
        }
        g.rules[g.rule_count++] = dst;
    }
    os_mutex_unlock(g.mu);
    return HAL_OK;
}

/* ---------------- profile 默认值与规则播种 ---------------- */

static void seed_channel(const profile_channel_t *c)
{
    static const char *rc_names[] = { "cbr", "vbr", "avbr", "fixqp" };
    char codecs[64] = {0}, rcs[64] = {0};
    char pat[CFG_KEY_MAX];
    size_t off = 0;
    const char *why;

    if (c->codecs_mask & (1u << HAL_CODEC_H264)) off += (size_t)snprintf(codecs + off, sizeof(codecs) - off, "h264,");
    if (c->codecs_mask & (1u << HAL_CODEC_H265)) off += (size_t)snprintf(codecs + off, sizeof(codecs) - off, "h265,");
    if (c->codecs_mask & (1u << HAL_CODEC_MJPEG)) snprintf(codecs + off, sizeof(codecs) - off, "mjpeg");
    for (int k = 0; k < 4; k++) { size_t o = strlen(rcs); snprintf(rcs + o, sizeof(rcs) - o, "%s%s", k ? "," : "", rc_names[k]); }

#define RULE(k, t, lo, hi, en, rb) do { \
        cfg_rule_t r; memset(&r, 0, sizeof(r)); \
        r.key_pattern = (k); r.type = (t); r.min = (lo); r.max = (hi); r.enum_csv = (en); r.reboot_required = (rb); \
        cfg_register_rules(&r, 1); \
    } while (0)
#define KEY(fmt, name) snprintf(pat, sizeof(pat), fmt, c->ch, (name))
    /* cfg_register_rules 会复制字符串，pat/codecs/rcs 可为栈缓冲 */
    KEY("video.%d.%s.codec", c->name); RULE(pat, CFG_T_STR, 0, 0, codecs, false);
    KEY("video.%d.%s.w", c->name);     RULE(pat, CFG_T_INT, 64, (int64_t)c->max_w, NULL, true);
    KEY("video.%d.%s.h", c->name);     RULE(pat, CFG_T_INT, 64, (int64_t)c->max_h, NULL, true);
    KEY("video.%d.%s.fps", c->name);   RULE(pat, CFG_T_INT, 1, (int64_t)c->max_fps, NULL, false);
    KEY("video.%d.%s.kbps", c->name);  RULE(pat, CFG_T_INT, 32, 16384, NULL, false);
    KEY("video.%d.%s.gop", c->name);   RULE(pat, CFG_T_INT, 1, 300, NULL, false);
    KEY("video.%d.%s.rc", c->name);    RULE(pat, CFG_T_STR, 0, 0, rcs, false);
#undef RULE

    {
        const char *codec = (c->def_codec == HAL_CODEC_H265) ? "h265"
                          : (c->def_codec == HAL_CODEC_MJPEG) ? "mjpeg" : "h264";
        KEY("video.%d.%s.codec", c->name); set_value(pat, CFG_T_STR, 0, codec, false, &why);
        KEY("video.%d.%s.w", c->name);     set_value(pat, CFG_T_INT, (int64_t)c->def_w, NULL, false, &why);
        KEY("video.%d.%s.h", c->name);     set_value(pat, CFG_T_INT, (int64_t)c->def_h, NULL, false, &why);
        KEY("video.%d.%s.fps", c->name);   set_value(pat, CFG_T_INT, (int64_t)c->def_fps, NULL, false, &why);
        KEY("video.%d.%s.kbps", c->name);  set_value(pat, CFG_T_INT, (int64_t)c->def_kbps, NULL, false, &why);
        KEY("video.%d.%s.gop", c->name);   set_value(pat, CFG_T_INT, (int64_t)c->def_gop, NULL, false, &why);
        KEY("video.%d.%s.rc", c->name);    set_value(pat, CFG_T_STR, 0, rc_names[c->def_rc], false, &why);
#undef KEY
    }
}

static void register_common_rules(void)
{
    static const struct { const char *k; cfg_type_t t; int64_t lo, hi; const char *en; bool rb; bool wo; } common[] = {
        { "record.enabled",          CFG_T_BOOL, 0, 1, NULL, false },
        { "record.mode",             CFG_T_STR,  0, 0, "continuous,event,schedule", false },
        { "record.channel",          CFG_T_INT,  0, 2, NULL, false },
        { "record.retention_days",   CFG_T_INT,  1, 365, NULL, false },
        { "image.brightness",        CFG_T_INT,  0, 100, NULL, false },
        { "image.contrast",          CFG_T_INT,  0, 100, NULL, false },
        { "image.saturation",        CFG_T_INT,  0, 100, NULL, false },
        { "image.sharpness",         CFG_T_INT,  0, 100, NULL, false },
        { "image.flip",              CFG_T_INT,  0, 1, NULL, false },
        { "image.mirror",            CFG_T_INT,  0, 1, NULL, false },
        { "time.timezone",           CFG_T_STR,  0, 0, NULL, false },
        { "time.ntp.enable",         CFG_T_BOOL, 0, 1, NULL, false },
        { "time.ntp.server",         CFG_T_STR,  0, 0, NULL, false },
        { "net.dhcp",                CFG_T_BOOL, 0, 1, NULL, true },
        { "net.ip",                  CFG_T_STR,  0, 0, NULL, true },
        /* 静态地址四件套的其余三项：与 dhcp 一样改动后需重启网络栈才生效。
         * 设备侧不做“dhcp 开启时忽略静态值”的联动校验——那是平台面的交互职责，
         * 固件只负责逐键校验类型与范围（与 cfg_apply_json 的逐键语义一致）。 */
        { "net.mask",                CFG_T_STR,  0, 0, NULL, true },
        { "net.gw",                  CFG_T_STR,  0, 0, NULL, true },
        { "net.dns",                 CFG_T_STR,  0, 0, NULL, true },
        { "localUser.name",          CFG_T_STR,  0, 0, NULL, false },
        /* 本地账户口令（接入规范 §5.7 的 cfg 最小集）：只写键——可下发、卡长度，
         * 但不落盘、不进 cfg_dump_json，值只留在内存等认证模块取走转成哈希。
         * 8–63 位与 modules/console/console_internal.h 的口令长度限制、
         * 以及平台侧 pwdPolicyViolation() 三处口径一致；该处另要求含字母+数字。 */
        { "localUser.password",      CFG_T_STR,  8, 63, NULL, false, true },
        { "osd.channelName.enable",  CFG_T_BOOL, 0, 1, NULL, false },
        { "osd.time.enable",         CFG_T_BOOL, 0, 1, NULL, false },
        /* 固定叠加项（通道名/时间）的字号：像普通整型键那样卡区间，不必靠平台与模拟器兜底。
         * 12–72 与自定义文字 font_px、平台界面的滑块三处同值；单位是主码流分辨率下的像素高度。*/
        { "osd.channelName.fontPx",  CFG_T_INT,  12, 72, NULL, false },
        { "osd.time.fontPx",         CFG_T_INT,  12, 72, NULL, false },
        /* OSD 叠加位置与自定义文字（画面上可拖拽定位，PRD MGR-09）。
         * 位置用归一化二元组 [x, y]，含义是**文字区域左上角**在画面中的比例坐标，
         * 与 alarm.motion.regions 同属 CFG_T_JSON（固件只校验“是合法 JSON”，
         * 取值范围由平台界面与模拟器负责——逐层校验强度不同是有意的：
         * 设备端不接受的是「不是 JSON」，越界坐标在渲染时会被 HAL 夹到画面内）。*/
        { "osd.channelName.pos",     CFG_T_JSON, 0, 0, NULL, false },
        { "osd.time.pos",            CFG_T_JSON, 0, 0, NULL, false },
        /* 自定义文字叠加：**变长列表**——每条一个 OSD 区域，故用一个 JSON 数组而不是
         * 若干扁平键（同 alarm.motion.regions 的取舍）。元素形状：
         *   {"text": "东门仓库", "x": 0.02, "y": 0.5, "font_px": 32}
         * x/y 是文字区域左上角的归一化坐标（0–1）；font_px 是**主码流分辨率**下的像素高度，
         * 与 hal_osd_cfg_t.font_px 同名同义。text 上限 HAL_OSD_TEXT_MAX(64 字节)。
         * 条数受 HAL 的 hal_osd_caps_t.max_regions_per_channel 约束（参考实现为 4/通道，
         * 通道名与时间各占 1 个区域）——固件此处只校验“是合法 JSON”，
         * 条数与各字段范围由平台界面与模拟器把关，设备侧渲染时按区域上限截断。*/
        { "osd.text.regions",        CFG_T_JSON, 0, 0, NULL, false },
        { "alarm.motion.enable",     CFG_T_BOOL, 0, 1, NULL, false },
        { "alarm.motion.sensitivity",CFG_T_INT,  0, 100, NULL, false },
        { "alarm.motion.regions",    CFG_T_JSON, 0, 0, NULL, false },
        { "led.enable",              CFG_T_BOOL, 0, 1, NULL, false }
    };
    for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
        cfg_rule_t r;
        memset(&r, 0, sizeof(r));
        r.key_pattern = common[i].k; r.type = common[i].t;
        r.min = common[i].lo; r.max = common[i].hi;
        r.enum_csv = common[i].en; r.reboot_required = common[i].rb;
        r.write_only = common[i].wo;
        cfg_register_rules(&r, 1);
    }
}

/* ---------------- 生命周期 ---------------- */

hal_err_t cfg_init(const char *profile_json, const char *persist_path)
{
    hal_err_t rc;
    char *txt;
    const char *why;

    if (g.inited) return HAL_ESTATE;
    memset(&g, 0, sizeof(g));
    g.mu = os_mutex_create();
    if (!g.mu) return HAL_ENOMEM;
    if (profile_json) profile_load_from_string(profile_json);   /* 失败则 profile_get() 返回 NULL */
    strncpy(g.path, persist_path ? persist_path : "ipc_config.json", sizeof(g.path) - 1);
    g.inited = true;

    register_common_rules();
    {
        const profile_t *p = profile_get();
        if (p) for (uint32_t i = 0; i < p->channel_count; i++) seed_channel(&p->channels[i]);
    }

    txt = os_file_read_all(g.path, NULL);
    if (txt) {
        char err[128];
        json_t *root = json_parse(txt, 0, err, sizeof(err));
        if (root && json_is(root, JSON_OBJECT)) {
            for (size_t i = 0; i < json_size(root); i++) {
                const json_t *v = json_at(root, i);
                const char *key = json_key_at(root, i);
                if (!key) continue;
                switch (json_type(v)) {
                case JSON_BOOL:   rc = set_value(key, CFG_T_BOOL, json_bool(v, false) ? 1 : 0, NULL, false, &why); break;
                case JSON_NUMBER: rc = set_value(key, CFG_T_INT, json_int(v, 0), NULL, false, &why); break;
                case JSON_STRING: rc = set_value(key, CFG_T_STR, 0, json_string(v, ""), false, &why); break;
                default: {
                    char *dump = json_dump(v, false);
                    rc = set_value(key, CFG_T_JSON, 0, dump ? dump : "null", false, &why);
                    free(dump);
                    break;
                }
                }
                if (rc == HAL_OK) {
                    entry_t *e = find_entry(key);
                    if (e) e->dirty = true;
                } else {
                    LOGW(MOD, "discard persisted '%s': %s", key, why);
                }
            }
        } else if (root) {
            LOGE(MOD, "config file is not a JSON object");
        } else {
            LOGE(MOD, "config file corrupt: %s", err);
        }
        json_free(root);
        free(txt);
    }
    LOGI(MOD, "ready: %zu entries, %zu rules", g.count, g.rule_count);
    return HAL_OK;
}

hal_err_t cfg_deinit(void)
{
    os_mutex_t *mu;
    if (!g.inited) return HAL_ESTATE;
    mu = g.mu;
    os_mutex_lock(mu);
    for (size_t i = 0; i < g.count; i++) {
        entry_t *e = &g.items[i];
        if ((e->type == CFG_T_STR || e->type == CFG_T_JSON) && e->v.s) free(e->v.s);
    }
    free(g.items);
    for (size_t i = 0; i < g.rule_count; i++) {
        free((void *)g.rules[i].key_pattern);
        free((void *)g.rules[i].enum_csv);
    }
    g.items = NULL; g.count = g.cap = g.rule_count = 0;
    g.inited = false;
    os_mutex_unlock(mu);
    os_mutex_destroy(mu);
    g.mu = NULL;
    return HAL_OK;
}

/* ---------------- 读 ---------------- */

hal_err_t cfg_get_bool(const char *key, bool *v)
{
    hal_err_t rc = HAL_ENODEV;
    if (!g.inited || !key || !v) return key && v ? HAL_ESTATE : HAL_EINVAL;
    os_mutex_lock(g.mu);
    {
        const entry_t *e = find_entry(key);
        if (e && e->set) { if (e->type == CFG_T_BOOL) { *v = e->v.b; rc = HAL_OK; } else rc = HAL_EINVAL; }
    }
    os_mutex_unlock(g.mu);
    return rc;
}

hal_err_t cfg_get_int(const char *key, int64_t *v)
{
    hal_err_t rc = HAL_ENODEV;
    if (!g.inited || !key || !v) return key && v ? HAL_ESTATE : HAL_EINVAL;
    os_mutex_lock(g.mu);
    {
        const entry_t *e = find_entry(key);
        if (e && e->set) { if (e->type == CFG_T_INT) { *v = e->v.i; rc = HAL_OK; } else rc = HAL_EINVAL; }
    }
    os_mutex_unlock(g.mu);
    return rc;
}

hal_err_t cfg_get_str(const char *key, char *buf, size_t cap)
{
    hal_err_t rc = HAL_ENODEV;
    if (!g.inited || !key || !buf || !cap) return HAL_EINVAL;
    os_mutex_lock(g.mu);
    {
        const entry_t *e = find_entry(key);
        if (e && e->set) {
            if (e->type == CFG_T_STR) { strncpy(buf, e->v.s ? e->v.s : "", cap - 1); buf[cap - 1] = 0; rc = HAL_OK; }
            else rc = HAL_EINVAL;
        }
    }
    os_mutex_unlock(g.mu);
    return rc;
}

hal_err_t cfg_get_json(const char *key, char *buf, size_t cap)
{
    hal_err_t rc = HAL_ENODEV;
    if (!g.inited || !key || !buf || !cap) return HAL_EINVAL;
    os_mutex_lock(g.mu);
    {
        const entry_t *e = find_entry(key);
        if (e && e->set) {
            if (e->type == CFG_T_JSON) { strncpy(buf, e->v.s ? e->v.s : "null", cap - 1); buf[cap - 1] = 0; rc = HAL_OK; }
            else rc = HAL_EINVAL;
        } else {
            /* 前缀聚合：把 key.* 组合成对象，便于模块按子树读取 */
            size_t klen = strlen(key);
            json_t *obj = json_new_object();
            bool any = false;
            for (size_t i = 0; obj && i < g.count; i++) {
                const entry_t *c = &g.items[i];
                json_t *v = NULL;
                if (!c->set || strncmp(c->key, key, klen) != 0 || c->key[klen] != '.') continue;
                switch (c->type) {
                case CFG_T_BOOL: v = json_new_bool(c->v.b); break;
                case CFG_T_INT:  v = json_new_int(c->v.i); break;
                case CFG_T_STR:  v = json_new_string(c->v.s); break;
                default:         v = json_parse(c->v.s, 0, NULL, 0); break;
                }
                if (v && json_object_set(obj, c->key + klen + 1, v) == 0) any = true;
                else json_free(v);
            }
            if (any) {
                char *txt = json_dump(obj, false);
                if (txt) { strncpy(buf, txt, cap - 1); buf[cap - 1] = 0; rc = HAL_OK; free(txt); }
            }
            json_free(obj);
        }
    }
    os_mutex_unlock(g.mu);
    return rc;
}

/* ---------------- 写 ---------------- */

static hal_err_t guarded_set(const char *key, cfg_type_t t, int64_t i, const char *s)
{
    const char *why;
    hal_err_t rc;
    if (!g.inited) return HAL_ESTATE;
    os_mutex_lock(g.mu);
    rc = set_value(key, t, i, s, true, &why);
    os_mutex_unlock(g.mu);
    if (rc != HAL_OK) LOGD(MOD, "set '%s' rejected: %s", key, why);
    return rc;
}

hal_err_t cfg_set_bool(const char *key, bool v) { return guarded_set(key, CFG_T_BOOL, v ? 1 : 0, NULL); }
hal_err_t cfg_set_int(const char *key, int64_t v) { return guarded_set(key, CFG_T_INT, v, NULL); }

hal_err_t cfg_set_str(const char *key, const char *v)
{
    if (!v || strlen(v) >= CFG_STR_MAX) return HAL_EINVAL;
    return guarded_set(key, CFG_T_STR, 0, v);
}

hal_err_t cfg_set_json(const char *key, const char *json)
{
    char err[128];
    json_t *probe;
    if (!json) return HAL_EINVAL;
    probe = json_parse(json, 0, err, sizeof(err));
    if (!probe) return HAL_EINVAL;
    json_free(probe);
    return guarded_set(key, CFG_T_JSON, 0, json);
}

int cfg_apply_json(const char *values_json, cfg_reject_t *rejects, size_t max_rejects)
{
    char err[128];
    json_t *root;
    int rejected = 0;
    if (!g.inited || !values_json) return -1;
    root = json_parse(values_json, 0, err, sizeof(err));
    if (!root || !json_is(root, JSON_OBJECT)) { json_free(root); return -1; }

    os_mutex_lock(g.mu);
    for (size_t i = 0; i < json_size(root); i++) {
        const json_t *v = json_at(root, i);
        const char *key = json_key_at(root, i);
        const char *why = "";
        hal_err_t rc;
        if (!key) continue;
        switch (json_type(v)) {
        case JSON_BOOL:   rc = set_value(key, CFG_T_BOOL, json_bool(v, false) ? 1 : 0, NULL, true, &why); break;
        case JSON_NUMBER: rc = set_value(key, CFG_T_INT, json_int(v, 0), NULL, true, &why); break;
        case JSON_STRING: rc = set_value(key, CFG_T_STR, 0, json_string(v, ""), true, &why); break;
        default: {
            char *dump = json_dump(v, false);
            rc = set_value(key, CFG_T_JSON, 0, dump ? dump : "null", true, &why);
            free(dump);
            break;
        }
        }
        if (rc != HAL_OK) {
            rejected++;
            if (rejects && (size_t)rejected <= max_rejects) {
                strncpy(rejects[rejected - 1].key, key, CFG_KEY_MAX - 1);
                strncpy(rejects[rejected - 1].reason, why[0] ? why : "invalid", 63);
            }
        }
    }
    os_mutex_unlock(g.mu);
    json_free(root);
    return rejected;
}

hal_err_t cfg_dump_json(char *buf, size_t cap)
{
    hal_err_t rc = HAL_OK;
    if (!g.inited || !buf || !cap) return HAL_EINVAL;
    os_mutex_lock(g.mu);
    {
        json_t *root = json_new_object();
        char *txt;
        for (size_t i = 0; root && i < g.count; i++) {
            const entry_t *e = &g.items[i];
            const cfg_rule_t *r = rule_for(e->key);
            json_t *v = NULL;
            if (!e->set) continue;
            /* 只写键不进导出：导出常用于排查/交付，口令不该出现在里面 */
            if (r && r->write_only) continue;
            switch (e->type) {
            case CFG_T_BOOL: v = json_new_bool(e->v.b); break;
            case CFG_T_INT:  v = json_new_int(e->v.i); break;
            case CFG_T_STR:  v = json_new_string(e->v.s); break;
            default:         v = json_parse(e->v.s, 0, NULL, 0); if (!v) v = json_new_string(e->v.s); break;
            }
            if (v && json_object_set(root, e->key, v) != 0) json_free(v);
        }
        txt = root ? json_dump(root, false) : NULL;
        json_free(root);
        if (!txt) rc = HAL_ENOMEM;
        else { strncpy(buf, txt, cap - 1); buf[cap - 1] = 0; free(txt); }
    }
    os_mutex_unlock(g.mu);
    return rc;
}

hal_err_t cfg_reset(const char *const *keep_keys, size_t n)
{
    hal_err_t rc;
    if (!g.inited) return HAL_ESTATE;
    os_mutex_lock(g.mu);
    for (size_t i = 0; i < g.count; i++) {
        entry_t *e = &g.items[i];
        bool keep = false;
        if (!e->dirty) continue;
        for (size_t k = 0; k < n; k++) if (keep_keys[k] && strcmp(keep_keys[k], e->key) == 0) { keep = true; break; }
        if (keep) continue;
        /* 回到出厂：丢弃用户值，恢复为未设置（profile 默认仍保留） */
        if ((e->type == CFG_T_STR || e->type == CFG_T_JSON) && e->v.s) { free(e->v.s); e->v.s = NULL; }
        e->dirty = false;
        e->set = false;
    }
    rc = persist_locked();
    os_mutex_unlock(g.mu);
    notify_changed("*");
    return rc;
}
