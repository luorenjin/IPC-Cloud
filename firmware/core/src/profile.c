/**
 * @file profile.c
 * @brief 能力清单加载：JSON → profile_t，关键字段校验，能力字符串导出
 */
#include "core/profile.h"
#include "core/json.h"
#include "core/os.h"
#include "core/log.h"
#include "hal/hal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MOD "profile"

static profile_t g_p;
static bool g_loaded;
static char *g_raw;

static void cpy(char *dst, size_t cap, const char *s) { if (!s) s = ""; strncpy(dst, s, cap - 1); dst[cap - 1] = 0; }

static hal_codec_t codec_of(const char *s)
{
    if (!s) return HAL_CODEC_NONE;
    if (strcmp(s, "h264") == 0) return HAL_CODEC_H264;
    if (strcmp(s, "h265") == 0) return HAL_CODEC_H265;
    if (strcmp(s, "mjpeg") == 0) return HAL_CODEC_MJPEG;
    if (strcmp(s, "g711a") == 0) return HAL_CODEC_G711A;
    if (strcmp(s, "g711u") == 0) return HAL_CODEC_G711U;
    if (strcmp(s, "aac") == 0) return HAL_CODEC_AAC;
    if (strcmp(s, "pcm") == 0) return HAL_CODEC_PCM_S16LE;
    return HAL_CODEC_NONE;
}
static hal_rc_mode_t rc_of(const char *s)
{
    if (s && strcmp(s, "cbr") == 0) return HAL_RC_CBR;
    if (s && strcmp(s, "avbr") == 0) return HAL_RC_AVBR;
    if (s && strcmp(s, "fixqp") == 0) return HAL_RC_FIXQP;
    return HAL_RC_VBR;
}
static uint32_t codec_mask(const json_t *arr)
{
    uint32_t m = 0;
    for (size_t i = 0; i < json_size(arr); i++) {
        hal_codec_t c = codec_of(json_string(json_at(arr, i), NULL));
        if (c != HAL_CODEC_NONE) m |= 1u << c;
    }
    return m;
}
static bool arr_has(const json_t *arr, const char *s)
{
    for (size_t i = 0; i < json_size(arr); i++) if (strcmp(json_string(json_at(arr, i), ""), s) == 0) return true;
    return false;
}

static hal_err_t require(const json_t *root, const char *path, const char *what)
{
    if (!json_path(root, path)) { LOGE(MOD, "missing required field: %s", what); return HAL_EINVAL; }
    return HAL_OK;
}

/* fill 写入调用者提供的临时结构，校验全部通过后才提交到全局，
   避免失败的 profile_load 破坏上一次成功加载的配置。
   注意：out 命名不可与函数内局部变量冲突（g_p 宏展开为 (*out)）。 */
#define g_p (*out)

static hal_err_t fill(const json_t *r, profile_t *out)
{
    const json_t *v, *arr;
    memset(out, 0, sizeof(*out));

    if (json_int(json_get(r, "schema_version"), 0) != 1) { LOGE(MOD, "schema_version must be 1"); return HAL_EINVAL; }
    if (require(r, "identity.model", "identity.model") || require(r, "identity.platform", "identity.platform") ||
        require(r, "video.channels", "video.channels") || require(r, "video.lens.type", "video.lens.type") ||
        require(r, "protocols", "protocols") || require(r, "limits.frame_bus_consumers", "limits.frame_bus_consumers"))
        return HAL_EINVAL;

    /* identity */
    v = json_get(r, "identity");
    cpy(g_p.model, sizeof(g_p.model), json_string(json_get(v, "model"), ""));
    cpy(g_p.vendor, sizeof(g_p.vendor), json_string(json_get(v, "vendor"), ""));
    cpy(g_p.hw, sizeof(g_p.hw), json_string(json_get(v, "hw"), ""));
    cpy(g_p.platform, sizeof(g_p.platform), json_string(json_get(v, "platform"), ""));
    cpy(g_p.gb_manufacturer, sizeof(g_p.gb_manufacturer), json_string(json_get(v, "gb_manufacturer"), g_p.vendor));

    /* video */
    v = json_get(r, "video");
    arr = json_get(v, "sensors");
    for (size_t i = 0; i < json_size(arr) && i < PROFILE_MAX_SENSORS; i++) {
        cpy(g_p.sensors[i], PROFILE_STR, json_string(json_at(arr, i), ""));
        g_p.sensor_count++;
    }
    {
        const json_t *lens = json_get(v, "lens");
        g_p.lens_motorized = strcmp(json_string(json_get(lens, "type"), "fixed"), "motorized") == 0;
        g_p.lens_af = json_bool(json_get(lens, "af"), false);
        g_p.lens_zoom = json_bool(json_get(lens, "zoom"), false);
        cpy(g_p.lens_driver, PROFILE_STR, json_string(json_get(lens, "driver"), ""));
        if (g_p.lens_motorized && !g_p.lens_driver[0]) { LOGE(MOD, "motorized lens requires video.lens.driver"); return HAL_EINVAL; }
    }
    arr = json_get(v, "channels");
    if (json_size(arr) == 0 || json_size(arr) > PROFILE_MAX_CHANNELS) { LOGE(MOD, "video.channels count invalid"); return HAL_EINVAL; }
    for (size_t i = 0; i < json_size(arr); i++) {
        const json_t *c = json_at(arr, i), *mx = json_get(c, "max"), *df = json_get(c, "default");
        profile_channel_t *pc = &g_p.channels[g_p.channel_count++];
        pc->ch = (int)json_int(json_get(c, "ch"), (int64_t)i);
        cpy(pc->name, sizeof(pc->name), json_string(json_get(c, "name"), i == 0 ? "main" : "sub"));
        pc->codecs_mask = codec_mask(json_get(c, "codecs"));
        pc->max_w = (uint32_t)json_int(json_get(mx, "w"), 0); pc->max_h = (uint32_t)json_int(json_get(mx, "h"), 0);
        pc->max_fps = (uint32_t)json_int(json_get(mx, "fps"), 0);
        pc->def_codec = codec_of(json_string(json_get(df, "codec"), NULL));
        pc->def_w = (uint32_t)json_int(json_get(df, "w"), 0); pc->def_h = (uint32_t)json_int(json_get(df, "h"), 0);
        pc->def_fps = (uint32_t)json_int(json_get(df, "fps"), 0); pc->def_kbps = (uint32_t)json_int(json_get(df, "kbps"), 0);
        pc->def_gop = (uint32_t)json_int(json_get(df, "gop"), pc->def_fps * 2);
        pc->def_rc = rc_of(json_string(json_get(df, "rc"), "vbr"));
        if (!pc->max_w || !pc->max_h || !pc->max_fps || !pc->def_w || !pc->def_h || !pc->def_fps || pc->def_codec == HAL_CODEC_NONE) {
            LOGE(MOD, "video.channels[%zu] incomplete", i); return HAL_EINVAL;
        }
        if (pc->def_w > pc->max_w || pc->def_h > pc->max_h || pc->def_fps > pc->max_fps) {
            LOGE(MOD, "video.channels[%zu] default exceeds max", i); return HAL_EINVAL;
        }
        if (!(pc->codecs_mask & (1u << pc->def_codec))) { LOGE(MOD, "video.channels[%zu] default codec not in codecs", i); return HAL_EINVAL; }
    }
    {
        const json_t *isp = json_get(v, "isp");
        g_p.isp_wdr = json_bool(json_get(isp, "wdr"), false);
        g_p.isp_hdr = json_bool(json_get(isp, "hdr"), false);
        g_p.isp_3dnr = json_bool(json_get(isp, "3dnr"), false);
        cpy(g_p.daynight, PROFILE_STR, json_string(json_get(isp, "daynight"), "none"));
        g_p.snapshot_w = (uint32_t)json_int(json_path(v, "snapshot.max.w"), g_p.channels[0].max_w);
        g_p.snapshot_h = (uint32_t)json_int(json_path(v, "snapshot.max.h"), g_p.channels[0].max_h);
    }

    /* audio */
    v = json_get(r, "audio");
    g_p.audio_in_mic = arr_has(json_get(v, "in"), "mic");
    g_p.audio_in_line = arr_has(json_get(v, "in"), "line");
    g_p.audio_out = json_size(json_get(v, "out")) > 0;
    g_p.audio_codecs_mask = codec_mask(json_get(v, "codecs"));
    g_p.audio_aec = json_bool(json_get(v, "aec"), false);

    /* ivs */
    v = json_get(r, "ivs");
    cpy(g_p.ivs_engine, PROFILE_STR, json_string(json_get(v, "engine"), "none"));
    if (strcmp(g_p.ivs_engine, "none") != 0) {
        if (json_bool(json_get(v, "motion"), false))    g_p.ivs_kinds_mask |= 1u << HAL_IVS_MOTION;
        if (json_bool(json_get(v, "humanoid"), false))  g_p.ivs_kinds_mask |= 1u << HAL_IVS_HUMANOID;
        if (json_bool(json_get(v, "intrusion"), false)) g_p.ivs_kinds_mask |= 1u << HAL_IVS_INTRUSION;
        if (json_bool(json_get(v, "linecross"), false)) g_p.ivs_kinds_mask |= 1u << HAL_IVS_LINECROSS;
        if (json_bool(json_get(v, "tamper"), false))    g_p.ivs_kinds_mask |= 1u << HAL_IVS_TAMPER;
    }
    g_p.ivs_max_regions = (uint32_t)json_int(json_get(v, "max_regions"), 4);

    /* storage */
    v = json_get(r, "storage");
    g_p.tf = json_bool(json_get(v, "tf"), false);
    g_p.tf_max_gb = (uint32_t)json_int(json_get(v, "max_gb"), 256);
    g_p.segment_s = (uint32_t)json_int(json_get(v, "segment_s"), 60);
    g_p.reserve_pct = (uint32_t)json_int(json_get(v, "reserve_pct"), 5);

    /* network */
    v = json_get(r, "network");
    g_p.eth = json_bool(json_get(v, "eth"), true);
    {
        const json_t *w = json_get(v, "wifi");
        g_p.wifi = w != NULL;
        cpy(g_p.wifi_module, PROFILE_STR, json_string(json_get(w, "module"), ""));
        g_p.wifi_5g = arr_has(json_get(w, "bands"), "5g");
        g_p.wifi_wpa3 = arr_has(json_get(w, "security"), "wpa3");
    }

    /* protocols */
    v = json_get(r, "protocols");
    {
        const json_t *p;
        p = json_get(v, "idp");
        g_p.idp_enabled = json_bool(json_get(p, "enabled"), false);
        cpy(g_p.idp_broker, sizeof(g_p.idp_broker), json_string(json_get(p, "broker"), ""));
        g_p.idp_psk = strcmp(json_string(json_get(p, "auth"), "cert"), "psk") == 0;
        g_p.idp_keepalive_s = (uint32_t)json_int(json_get(p, "keepalive_s"), 60);
        p = json_get(v, "rtsp");
        g_p.rtsp_enabled = json_bool(json_get(p, "enabled"), false);
        g_p.rtsp_port = (uint32_t)json_int(json_get(p, "port"), 554);
        g_p.rtsp_max_sessions = (uint32_t)json_int(json_get(p, "max_sessions"), 2);
        p = json_get(v, "onvif");
        g_p.onvif_enabled = json_bool(json_get(p, "enabled"), false);
        g_p.onvif_port = (uint32_t)json_int(json_get(p, "port"), 80);
        cpy(g_p.onvif_events, PROFILE_STR, json_string(json_get(p, "events"), "on_demand"));
        g_p.onvif_discovery = json_bool(json_get(p, "discovery"), true);
        p = json_get(v, "gb28181");
        g_p.gb_enabled = json_bool(json_get(p, "enabled"), false);
        g_p.gb_tcp = strcmp(json_string(json_get(p, "transport"), "tcp"), "tcp") == 0;
        g_p.gb_playback = json_bool(json_get(p, "playback"), true);
        g_p.gb_alarm = json_bool(json_get(p, "alarm"), true);
    }

    /* limits */
    v = json_get(r, "limits");
    g_p.frame_bus_consumers = (uint32_t)json_int(json_get(v, "frame_bus_consumers"), 4);
    g_p.playback_sessions = (uint32_t)json_int(json_get(v, "playback_sessions"), 1);
    g_p.mem_budget_mb = (uint32_t)json_int(json_get(v, "mem_budget_mb"), 56);
    g_p.cpu_budget_pct = (uint32_t)json_int(json_get(v, "cpu_budget_pct"), 70);
    if (g_p.frame_bus_consumers < 2) { LOGE(MOD, "limits.frame_bus_consumers must be >= 2"); return HAL_EINVAL; }

    /* security / ota */
    v = json_get(r, "security");
    g_p.hw_secure = json_bool(json_get(v, "hw_secure"), false);
    g_p.force_password_change = json_bool(json_get(v, "force_password_change"), true);
    g_p.telnet = json_bool(json_get(v, "telnet"), false);
    g_p.ssh = json_bool(json_get(v, "ssh"), false);
    v = json_get(r, "ota");
    g_p.ota_ab = json_bool(json_get(v, "ab"), true);
    g_p.ota_slot_mb = (uint32_t)json_int(json_get(v, "slot_size_mb"), 40);
    g_p.ota_confirm_timeout_s = (uint32_t)json_int(json_get(v, "confirm_timeout_s"), 60);
    return HAL_OK;
}
#undef g_p

hal_err_t profile_load_from_string(const char *json)
{
    char err[128];
    json_t *root;
    profile_t tmp;
    hal_err_t rc;
    if (!json) return HAL_EINVAL;
    root = json_parse(json, 0, err, sizeof(err));
    if (!root) { LOGE(MOD, "parse error: %s", err); return HAL_EINVAL; }
    rc = fill(root, &tmp);
    json_free(root);
    if (rc != HAL_OK) return rc;
    {
        char *dup = (char *)malloc(strlen(json) + 1);
        if (!dup) return HAL_ENOMEM;
        strcpy(dup, json);
        free(g_raw);
        g_raw = dup;
    }
    g_p = tmp;
    g_loaded = true;
    LOGI(MOD, "loaded model=%s platform=%s channels=%u", g_p.model, g_p.platform, g_p.channel_count);
    return HAL_OK;
}

hal_err_t profile_load(const char *path)
{
    char *txt = os_file_read_all(path, NULL);
    hal_err_t rc;
    if (!txt) { LOGE(MOD, "cannot read %s", path ? path : "(null)"); return HAL_ENODEV; }
    rc = profile_load_from_string(txt);
    free(txt);
    return rc;
}

const profile_t *profile_get(void) { return g_loaded ? &g_p : NULL; }
const char *profile_raw_json(void) { return g_loaded ? g_raw : NULL; }

const profile_channel_t *profile_channel_by_name(const char *name)
{
    if (!g_loaded || !name) return NULL;
    for (uint32_t i = 0; i < g_p.channel_count; i++) if (strcmp(g_p.channels[i].name, name) == 0) return &g_p.channels[i];
    return NULL;
}

size_t profile_capabilities(const char **out, size_t max)
{
    size_t n = 0;
#define ADD(s) do { if (n < max) out[n] = (s); n++; } while (0)
    if (!g_loaded || !out) return 0;
    ADD("live.main");
    if (g_p.channel_count > 1) ADD("live.sub");
    if (g_p.channels[0].codecs_mask & (1u << HAL_CODEC_H265)) ADD("live.h265");
    ADD("snapshot");
    if (g_p.tf) { ADD("record.device.query"); ADD("record.device.play"); ADD("record.device.speed"); ADD("record.device.seek"); }
    if (g_p.lens_motorized && g_p.lens_zoom) ADD("ptz.zoom");
    if (g_p.lens_motorized && g_p.lens_af) ADD("focus");
    if (g_p.audio_out && (g_p.audio_in_mic || g_p.audio_in_line)) ADD("audio.talk");
    if (g_p.ivs_kinds_mask & (1u << HAL_IVS_MOTION))    ADD("event.motion");
    if (g_p.ivs_kinds_mask & (1u << HAL_IVS_HUMANOID))  ADD("event.humanoid");
    if (g_p.ivs_kinds_mask & (1u << HAL_IVS_INTRUSION)) ADD("event.intrusion");
    if (g_p.ivs_kinds_mask & (1u << HAL_IVS_LINECROSS)) ADD("event.linecross");
    if (g_p.ivs_kinds_mask & (1u << HAL_IVS_TAMPER))    ADD("event.tamper");
    ADD("status.metrics");
    ADD("config.remote");
    if (g_p.ota_ab) ADD("ota");
    ADD("reboot");
#undef ADD
    return n;
}
