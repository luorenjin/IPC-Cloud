/**
 * @file main.c
 * @brief HAL 一致性测试 —— 任何平台实现必须全部通过
 *
 * 用法：hal_conformance [profile.json]
 * 退出码：0 全部通过；非 0 为失败用例数。
 * 用例编号对应《固件平台化架构》§7 S2~S5 与《设备接入规范》§12.1 的 HAL 前置条件。
 */
#include "hal/hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define SECTION(name) printf("== %s\n", name)

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n; char *buf;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    buf[n] = 0;
    fclose(fp);
    return buf;
}

/* ---------------- HAL-01 初始化与版本 ---------------- */
static void test_init(const char *profile)
{
    const hal_ops_t *h;
    SECTION("HAL-01 init/version");
    CHECK(hal() == NULL, "hal() must be NULL before init");
    CHECK(hal_init(profile) == HAL_OK, "hal_init");
    h = hal();
    CHECK(h != NULL, "hal() after init");
    if (!h) return;
    CHECK((h->version >> 16) == HAL_API_VERSION_MAJOR, "major version");
    CHECK(h->platform_id && h->platform_id[0], "platform_id");
    CHECK(hal_init(profile) == HAL_ESTATE, "double init must fail with ESTATE");
    CHECK(hal_has(HAL_MOD_VIDEO) && hal_has(HAL_MOD_SYS) && hal_has(HAL_MOD_NET) &&
          hal_has(HAL_MOD_GPIO) && hal_has(HAL_MOD_STORAGE), "mandatory modules present");
}

/* ---------------- HAL-02 sys ---------------- */
static void test_sys(void)
{
    const hal_sys_ops_t *s = hal()->sys;
    hal_sys_info_t info; hal_sys_stats_t st; hal_boot_reason_t br;
    uint64_t t1, t2;
    SECTION("HAL-02 sys");
    CHECK(s->get_info(&info) == HAL_OK, "get_info");
    CHECK(info.platform_id[0] && strcmp(info.platform_id, hal()->platform_id) == 0, "platform_id consistent");
    CHECK(info.chip_id[0], "chip_id non-empty");
    CHECK(info.mem_total_kb > 0, "mem_total");
    CHECK(s->get_stats(&st) == HAL_OK && st.mem_total_kb > 0, "get_stats");
    CHECK(s->get_boot_reason(&br) == HAL_OK, "boot reason");
    t1 = s->monotonic_us();
    t2 = s->monotonic_us();
    CHECK(t2 >= t1, "monotonic non-decreasing");
    CHECK(s->get_info(NULL) == HAL_EINVAL, "get_info NULL -> EINVAL");
    CHECK(s->wdt_enable(0) == HAL_EINVAL, "wdt_enable(0) -> EINVAL");
    CHECK(s->wdt_enable(30) == HAL_OK, "wdt_enable");
    CHECK(s->wdt_feed() == HAL_OK, "wdt_feed");
    CHECK(s->wdt_disable() == HAL_OK, "wdt_disable");
}

/* ---------------- HAL-03 OTA A/B ---------------- */
void mock_ota_expected_digest(const void *data, size_t len, uint8_t out[32]); /* mock 专用；真实平台用 SHA-256 */

static void test_ota(void)
{
    const hal_sys_ops_t *s = hal()->sys;
    hal_ota_state_t st0, st1;
    uint8_t img[4096], digest[32], bad[32];
    int other;
    SECTION("HAL-03 ota");
    CHECK(s->ota_get_state(&st0) == HAL_OK, "ota_get_state");
    other = st0.other_slot;
    for (size_t i = 0; i < sizeof(img); i++) img[i] = (uint8_t)(i * 7);

    CHECK(s->ota_begin(st0.current_slot, sizeof(img)) == HAL_EINVAL, "write to current slot rejected");
    CHECK(s->ota_begin(other, sizeof(img)) == HAL_OK, "ota_begin");
    CHECK(s->ota_write(img, 1024) == HAL_OK, "ota_write 1");
    CHECK(s->ota_write(img + 1024, sizeof(img) - 1024) == HAL_OK, "ota_write 2");
    CHECK(s->ota_write(img, 1) == HAL_EINVAL, "ota_write overflow rejected");

    memset(bad, 0, sizeof(bad));
    /* 错误摘要必须失败 */
    CHECK(s->ota_end(bad) == HAL_ECORRUPT, "ota_end bad digest -> ECORRUPT");

    /* 重新写入并用正确摘要结束 */
    CHECK(s->ota_begin(other, sizeof(img)) == HAL_OK, "ota_begin again");
    CHECK(s->ota_write(img, sizeof(img)) == HAL_OK, "ota_write full");
    if (strcmp(hal()->platform_id, "mock") == 0) {
        mock_ota_expected_digest(img, sizeof(img), digest);
    } else {
        /* 真实平台：此处应计算 SHA-256；测试框架后续接入 sha256 实现 */
        memset(digest, 0, sizeof(digest));
    }
    CHECK(s->ota_end(digest) == HAL_OK, "ota_end good digest");
    CHECK(s->ota_switch_slot(other) == HAL_OK, "ota_switch_slot");
    CHECK(s->ota_get_state(&st1) == HAL_OK && st1.current_slot == other && st1.pending_confirm, "state after switch: pending");
    CHECK(s->ota_confirm() == HAL_OK, "ota_confirm");
    CHECK(s->ota_get_state(&st1) == HAL_OK && !st1.pending_confirm, "pending cleared");
    CHECK(s->ota_confirm() == HAL_ESTATE, "double confirm -> ESTATE");
    /* 切回原槽，保持测试可重复 */
    CHECK(s->ota_switch_slot(st0.current_slot) == HAL_OK && s->ota_confirm() == HAL_OK, "switch back");
}

/* ---------------- HAL-04 video ---------------- */
static void test_video(void)
{
    const hal_video_ops_t *v = hal()->video;
    hal_video_caps_t caps; hal_sensor_info_t si; hal_enc_cfg_t cfg, back;
    hal_frame_t f;
    uint64_t last_pts = 0; uint32_t last_seq = 0; int keys = 0, got = 0, cfg_flags = 0;
    SECTION("HAL-04 video");
    CHECK(v->open() == HAL_OK, "open");
    CHECK(v->open() == HAL_ESTATE, "double open -> ESTATE");
    CHECK(v->probe_sensor(&si) == HAL_OK && si.name[0], "probe_sensor");
    CHECK(v->get_caps(&caps) == HAL_OK && caps.channels >= 2, "get_caps channels>=2");
    CHECK(caps.codecs_mask & ((1u << HAL_CODEC_H264) | (1u << HAL_CODEC_H265)), "h264 or h265");

    memset(&cfg, 0, sizeof(cfg));
    cfg.codec = (caps.codecs_mask & (1u << HAL_CODEC_H265)) ? HAL_CODEC_H265 : HAL_CODEC_H264;
    cfg.profile = HAL_H265_MAIN; cfg.width = 1280; cfg.height = 720; cfg.fps = 25;
    cfg.rc = HAL_RC_VBR; cfg.bitrate_kbps = 1024; cfg.gop = 10;

    CHECK(v->get_frame(0, &f, 10) == HAL_ESTATE, "get_frame before start -> ESTATE");
    CHECK(v->set_encoder(0, &cfg) == HAL_OK, "set_encoder ch0");
    CHECK(v->get_encoder(0, &back) == HAL_OK && back.width == 1280 && back.fps == 25, "get_encoder roundtrip");
    cfg.fps = 0;
    CHECK(v->set_encoder(0, &cfg) == HAL_EINVAL, "fps=0 rejected");
    cfg.fps = 25;
    CHECK(v->start(0) == HAL_OK, "start ch0");
    CHECK(v->start(0) == HAL_EBUSY, "double start -> EBUSY");

    /* 取 30 帧：PTS 单调、seq 递增、首帧关键、GOP 内关键帧数 ≥ 3 */
    for (int i = 0; i < 30; i++) {
        hal_err_t rc = v->get_frame(0, &f, 500);
        if (rc != HAL_OK) { CHECK(0, "get_frame #%d rc=%d", i, rc); break; }
        got++;
        if (i == 0) CHECK(f.flags & HAL_FRAME_FLAG_KEY, "first frame is key");
        if (i > 0) { CHECK(f.pts_us > last_pts, "pts increasing at #%d", i); CHECK(f.seq == last_seq + 1, "seq contiguous at #%d", i); }
        CHECK(f.data && f.size > 4, "frame data");
        CHECK(f.data[0] == 0 && f.data[1] == 0 && f.data[2] == 0 && f.data[3] == 1, "annex-b start code");
        if (f.flags & HAL_FRAME_FLAG_KEY) keys++;
        if (f.flags & HAL_FRAME_FLAG_CONFIG) cfg_flags++;
        last_pts = f.pts_us; last_seq = f.seq;
        CHECK(v->release_frame(&f) == HAL_OK, "release #%d", i);
    }
    CHECK(got == 30, "got 30 frames");
    CHECK(keys >= 3, "keyframes per GOP(10) over 30 frames >= 3 (got %d)", keys);
    CHECK(cfg_flags >= 1, "config flag present on key frames");

    /* request_idr 后下一帧必须是关键帧 */
    CHECK(v->request_idr(0) == HAL_OK, "request_idr");
    CHECK(v->get_frame(0, &f, 500) == HAL_OK && (f.flags & HAL_FRAME_FLAG_KEY), "idr after request");
    v->release_frame(&f);

    /* 动态改码率允许，改分辩率需 stop */
    cfg.bitrate_kbps = 512;
    CHECK(v->set_encoder(0, &cfg) == HAL_OK, "runtime bitrate change");
    cfg.width = 1920; cfg.height = 1080;
    CHECK(v->set_encoder(0, &cfg) == HAL_EBUSY, "resolution change while running -> EBUSY");

    /* 第二通道并行 */
    cfg.width = 640; cfg.height = 360; cfg.codec = HAL_CODEC_H264; cfg.fps = 15; cfg.gop = 15;
    CHECK(v->set_encoder(1, &cfg) == HAL_OK && v->start(1) == HAL_OK, "ch1 start");
    CHECK(v->get_frame(1, &f, 500) == HAL_OK && f.ch == 1 && f.codec == HAL_CODEC_H264, "ch1 frame");
    v->release_frame(&f);

    /* 抓图 */
    CHECK(v->snapshot(0, 80, &f) == HAL_OK && f.codec == HAL_CODEC_MJPEG && f.size > 2, "snapshot");
    CHECK(f.data[0] == 0xFF && f.data[1] == 0xD8, "jpeg SOI");
    CHECK(v->release_frame(&f) == HAL_OK, "release snapshot");

    /* ISP / 图像 / 日夜 */
    {
        hal_isp_mode_t m; hal_image_t img, rd; hal_daynight_t dn; bool night;
        CHECK(v->set_isp_mode(HAL_ISP_WDR) == HAL_OK && v->get_isp_mode(&m) == HAL_OK && m == HAL_ISP_WDR, "isp mode");
        memset(&img, -1, sizeof(img)); img.brightness = 70;
        CHECK(v->set_image(&img) == HAL_OK && v->get_image(&rd) == HAL_OK && rd.brightness == 70, "image brightness");
        img.brightness = 101;
        CHECK(v->set_image(&img) == HAL_EINVAL, "brightness>100 rejected");
        CHECK(v->set_daynight(HAL_DAYNIGHT_NIGHT) == HAL_OK && v->get_daynight(&dn, &night) == HAL_OK && dn == HAL_DAYNIGHT_NIGHT, "daynight");
    }

    /* 镜头：可选 */
    if (v->lens_focus) {
        CHECK(v->lens_focus(1, 50) == HAL_OK || v->lens_focus(1, 50) == HAL_ENOTSUP, "lens_focus ok or notsup");
    }

    CHECK(v->stop(0) == HAL_OK && v->stop(1) == HAL_OK, "stop");
    CHECK(v->stop(0) == HAL_ESTATE, "double stop -> ESTATE");
    CHECK(v->close() == HAL_OK, "close");
}

/* ---------------- HAL-05 audio（可选） ---------------- */
static void test_audio(void)
{
    const hal_audio_ops_t *a;
    hal_audio_caps_t caps; hal_audio_cfg_t cfg; hal_frame_t f;
    SECTION("HAL-05 audio (optional)");
    if (!hal_has(HAL_MOD_AUDIO)) { printf("  skipped\n"); return; }
    a = hal()->audio;
    CHECK(a->get_caps(&caps) == HAL_OK, "caps");
    if (!caps.capture) return;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = 8000; cfg.channels = 1; cfg.codec = HAL_CODEC_G711A; cfg.frame_ms = 20; cfg.input = HAL_AUDIO_IN_MIC;
    CHECK(a->capture_open(&cfg) == HAL_OK, "capture_open");
    CHECK(a->capture_read(&f, 500) == HAL_OK && f.size == 160, "g711a 20ms = 160 bytes (got %u)", f.size);
    CHECK(a->capture_release(&f) == HAL_OK, "release");
    CHECK(a->capture_close() == HAL_OK, "capture_close");
    CHECK(a->set_capture_gain(101) == HAL_EINVAL, "gain>100 rejected");
}

/* ---------------- HAL-06 osd / ivs（可选） ---------------- */
static void test_osd_ivs(void)
{
    SECTION("HAL-06 osd/ivs (optional)");
    if (hal_has(HAL_MOD_OSD)) {
        const hal_osd_ops_t *o = hal()->osd;
        hal_osd_cfg_t c; int id = -1;
        memset(&c, 0, sizeof(c));
        c.kind = HAL_OSD_TEXT; c.pos.x = 0.02f; c.pos.y = 0.9f; c.font_px = 24; c.color_argb = 0xFFFFFFFF;
        strncpy(c.text, "IPC", HAL_OSD_TEXT_MAX - 1);
        CHECK(o->create_region(0, &c, &id) == HAL_OK && id >= 0, "osd create");
        CHECK(o->update_text(id, "Cam-01") == HAL_OK, "osd text");
        CHECK(o->set_enable(id, false) == HAL_OK, "osd disable");
        CHECK(o->destroy_region(id) == HAL_OK, "osd destroy");
        CHECK(o->destroy_region(id) == HAL_EINVAL, "osd double destroy -> EINVAL");
    } else printf("  osd skipped\n");

    if (hal_has(HAL_MOD_IVS)) {
        const hal_ivs_ops_t *i = hal()->ivs;
        hal_ivs_caps_t caps; hal_ivs_cfg_t c; hal_ivs_event_t e; hal_err_t rc;
        CHECK(i->get_caps(&caps) == HAL_OK && caps.kinds_mask, "ivs caps");
        memset(&c, 0, sizeof(c));
        c.enable = true; c.sensitivity = 50; c.region_count = 1;
        c.regions[0].x = 0; c.regions[0].y = 0; c.regions[0].w = 1; c.regions[0].h = 1;
        CHECK(i->configure(HAL_IVS_MOTION, &c) == HAL_OK, "ivs configure motion");
        c.sensitivity = 101;
        CHECK(i->configure(HAL_IVS_MOTION, &c) == HAL_EINVAL, "sensitivity>100 rejected");
        rc = i->poll_event(&e, 100);
        CHECK(rc == HAL_OK || rc == HAL_EAGAIN, "poll_event returns OK or EAGAIN (rc=%d)", rc);
    } else printf("  ivs skipped\n");
}

/* ---------------- HAL-07 gpio / net / storage ---------------- */
static void test_gpio_net_storage(void)
{
    const hal_gpio_ops_t *g = hal()->gpio;
    const hal_net_ops_t *n = hal()->net;
    const hal_storage_ops_t *st = hal()->storage;
    uint32_t mask; bool lv; uint8_t mac[6]; hal_netif_status_t ns; hal_storage_stat_t ss;
    hal_net_event_t ne; hal_storage_event_t se; hal_key_event_t ke; hal_err_t rc;
    SECTION("HAL-07 gpio/net/storage");
    CHECK(g->get_mapped_mask(&mask) == HAL_OK, "gpio mask");
    if (mask & (1u << HAL_PIN_STATUS_LED)) {
        CHECK(g->set(HAL_PIN_STATUS_LED, true) == HAL_OK && g->get(HAL_PIN_STATUS_LED, &lv) == HAL_OK && lv, "led set/get");
    }
    CHECK(g->set(HAL_PIN_COUNT, true) == HAL_EINVAL, "invalid pin -> EINVAL");
    rc = g->wait_event(&ke, 20);
    CHECK(rc == HAL_OK || rc == HAL_EAGAIN, "gpio wait_event");

    CHECK(n->get_mac(HAL_NETIF_ETH, mac) == HAL_OK, "eth mac");
    CHECK(n->get_status(HAL_NETIF_ETH, &ns) == HAL_OK && ns.ifname[0], "eth status");
    /* ip 是尽力而为字段（hal_net.h 的字段注释：未获取到地址时约定为空串），
       不强制要求非空——不同平台的以太网在 HAL 查询这一刻是否已经拿到地址
       是运行期状态，不是 HAL 契约能保证的东西。这里只核对缓冲区语义本身：
       必须是 HAL_IP_MAX 缓冲区内 NUL 结尾的字符串，不能是未终止的溢出写入
       （否则 console 侧的 "%s" 格式化会读出界）。 */
    CHECK(memchr(ns.ip, '\0', sizeof(ns.ip)) != NULL,
          "eth status.ip 必须是缓冲区内 NUL 结尾的字符串（空串合法，表示尚未获取地址）");
    rc = n->poll_event(&ne, 20);
    CHECK(rc == HAL_OK || rc == HAL_EAGAIN, "net poll_event");

    CHECK(st->stat(&ss) == HAL_OK, "storage stat");
    if (ss.present) {
        if (!ss.mounted) CHECK(st->mount() == HAL_OK, "mount");
        CHECK(st->stat(&ss) == HAL_OK && ss.mounted && ss.mount_path[0], "mounted with path");
        CHECK(st->format(HAL_FS_EXFAT) == HAL_EBUSY, "format while mounted -> EBUSY");
        CHECK(st->umount() == HAL_OK, "umount");
    }
    rc = st->poll_event(&se, 20);
    CHECK(rc == HAL_OK || rc == HAL_EAGAIN, "storage poll_event");

    /* AP 模式（可选能力：不支持须返回 HAL_ENOTSUP，且 caps.wifi_ap 为 false） */
    {
        hal_net_caps_t caps;
        bool has_ap = (hal()->net->wifi_ap_start != NULL);
        CHECK(hal()->net->get_caps(&caps) == HAL_OK, "net get_caps");
        if (has_ap) {
            CHECK(caps.wifi_ap == true, "声明了 wifi_ap_start 则 caps.wifi_ap 必须为 true");
            CHECK(hal()->net->wifi_ap_start("IPC-TEST", "12345678", 6) == HAL_OK, "wifi_ap_start");
            CHECK(hal()->net->wifi_ap_start("IPC-TEST", "12345678", 6) == HAL_EBUSY,
                  "重复 start 返回 EBUSY");
            CHECK(hal()->net->wifi_ap_stop() == HAL_OK, "wifi_ap_stop");
            CHECK(hal()->net->wifi_ap_stop() == HAL_ESTATE, "未启动时 stop 返回 ESTATE");
            /* PSK 过短须拒绝：WPA2 要求 8~63 字符 */
            CHECK(hal()->net->wifi_ap_start("IPC-TEST", "123", 6) == HAL_EINVAL, "PSK 过短");
        } else {
            CHECK(caps.wifi_ap == false, "未实现 AP 则 caps.wifi_ap 必须为 false");
        }
    }
}

/* ---------------- HAL-08 crypto（可选） ---------------- */
static void test_crypto(void)
{
    const hal_crypto_ops_t *c;
    uint8_t r1[16], r2[16], buf[64]; size_t n; bool ex;
    SECTION("HAL-08 crypto (optional)");
    if (!hal_has(HAL_MOD_CRYPTO)) { printf("  skipped\n"); return; }
    c = hal()->crypto;
    CHECK(c->random(r1, 16) == HAL_OK && c->random(r2, 16) == HAL_OK && memcmp(r1, r2, 16) != 0, "random differs");
    CHECK(c->secure_write("test_key", (const uint8_t *)"hello", 5) == HAL_OK, "secure_write");
    CHECK(c->secure_exists("test_key", &ex) == HAL_OK && ex, "secure_exists");
    CHECK(c->secure_read("test_key", buf, sizeof(buf), &n) == HAL_OK && n == 5 && memcmp(buf, "hello", 5) == 0, "secure_read");
    CHECK(c->secure_read(HAL_SEC_KEY_DEVICE_KEY, buf, sizeof(buf), &n) == HAL_ENOTSUP, "private key not readable");
    CHECK(c->secure_delete("test_key") == HAL_OK, "secure_delete");
    CHECK(c->secure_exists("test_key", &ex) == HAL_OK && !ex, "deleted");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "profiles/mock-x86.json";
    char *profile = read_file(path);
    if (!profile) {
        /* 允许无 profile 运行（平台自带默认） */
        profile = (char *)malloc(3); strcpy(profile, "{}");
        printf("(profile %s not found, using {})\n", path);
    }

    test_init(profile);
    if (!hal()) { printf("hal_init failed, abort\n"); return 1; }
    test_sys();
    test_ota();
    test_video();
    test_audio();
    test_osd_ivs();
    test_gpio_net_storage();
    test_crypto();

    SECTION("HAL-09 deinit");
    CHECK(hal_deinit() == HAL_OK, "hal_deinit");
    CHECK(hal() == NULL, "hal() NULL after deinit");

    printf("\nRESULT: platform=%s pass=%d fail=%d\n",
           hal_platform_get()->platform_id, g_pass, g_fail);
    free(profile);
    return g_fail;
}
