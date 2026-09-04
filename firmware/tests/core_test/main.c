/**
 * @file main.c
 * @brief core 层单元测试（x86 + mock 平台）
 *
 * 覆盖：json / os 文件原子写 / log 环形缓冲与脱敏 / profile 加载与能力导出 /
 *       event_bus 发布订阅 / config 规则校验与持久化 / frame_bus 多消费者与按需起停 /
 *       module_loader 拓扑排序与失败传播。
 * 退出码 = 失败数。
 */
#include "core/json.h"
#include "core/os.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/event_bus.h"
#include "core/config.h"
#include "core/frame_bus.h"
#include "core/module.h"
#include "core/log.h"
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

/* ================= JSON ================= */
static void test_json(void)
{
    char err[128];
    json_t *j = json_parse("{\"a\":{\"b\":[1,2.5,\"x\",true,null]},\"c\":\"\\u4e2d\\n\"}", 0, err, sizeof(err));
    char *dump;
    json_t *clone;

    SECTION("json");
    CHECK(j != NULL, "parse: %s", err);
    if (!j) return;
    CHECK(json_type(json_path(j, "a.b.0")) == JSON_NUMBER && json_int(json_path(j, "a.b.0"), -1) == 1, "path a.b.0");
    CHECK(json_number(json_path(j, "a.b.1"), 0) == 2.5, "float");
    CHECK(strcmp(json_string(json_path(j, "a.b.2"), ""), "x") == 0, "string in array");
    CHECK(json_bool(json_path(j, "a.b.3"), false) == true, "bool");
    CHECK(json_type(json_path(j, "a.b.4")) == JSON_NULL, "null");
    CHECK(strcmp(json_string(json_path(j, "c"), ""), "中\n") == 0, "unicode escape");
    CHECK(json_path(j, "a.b.9") == NULL && json_path(j, "zz") == NULL, "missing path NULL");

    dump = json_dump(j, false);
    clone = json_parse(dump, 0, err, sizeof(err));
    CHECK(clone != NULL, "roundtrip parse");
    CHECK(json_size(json_get(clone, "a")) == 1, "roundtrip size");
    free(dump);
    json_free(clone);

    /* 构建 */
    {
        json_t *o = json_new_object();
        json_t *arr = json_new_array();
        json_array_push(arr, json_new_int(7));
        json_object_set(o, "arr", arr);
        json_object_set(o, "s", json_new_string("hi"));
        dump = json_dump(o, false);
        CHECK(strcmp(dump, "{\"arr\":[7],\"s\":\"hi\"}") == 0, "build dump: %s", dump);
        free(dump);
        json_free(o);
    }
    /* 失败样本 */
    CHECK(json_parse("{\"a\":", 0, err, sizeof(err)) == NULL, "truncated fails");
    CHECK(json_parse("{} x", 0, err, sizeof(err)) == NULL, "trailing fails");
    json_free(j);
}

/* ================= os ================= */
static void test_os(void)
{
    const char *path = "core_test_file.txt";
    char *txt;
    uint64_t t1, t2;

    SECTION("os");
    CHECK(os_file_write_atomic(path, "hello", 5) == 0, "atomic write");
    txt = os_file_read_all(path, NULL);
    CHECK(txt && strcmp(txt, "hello") == 0, "read back");
    free(txt);
    CHECK(os_file_write_atomic(path, "again", 5) == 0, "overwrite");
    txt = os_file_read_all(path, NULL);
    CHECK(txt && strcmp(txt, "again") == 0, "read back 2");
    free(txt);
    remove(path);
    CHECK(os_file_read_all(path, NULL) == NULL, "missing file NULL");

    t1 = os_monotonic_us();
    os_sleep_ms(15);
    t2 = os_monotonic_us();
    CHECK(t2 > t1 && t2 - t1 >= 10000, "monotonic advances");
    CHECK(os_wallclock_ms() > 1700000000000LL, "wallclock sane");
}

/* ================= log ================= */
static void test_log(void)
{
    char buf[2048];
    size_t n;
    SECTION("log");
    log_init(LOG_DEBUG, 16);
    LOGI("t", "hello %d", 1);
    LOGE("t", "boom");
    n = log_ring_dump(buf, sizeof(buf));
    CHECK(n > 0 && strstr(buf, "hello 1") && strstr(buf, "boom"), "ring contains lines");
    CHECK(strcmp(log_mask("abcdefgh"), "ab****gh") == 0, "mask: %s", log_mask("abcdefgh"));
    CHECK(strcmp(log_mask("ab"), "****") == 0, "short mask");
}

/* ================= profile ================= */
static const char *g_profiles_dir = "profiles";

static void test_profile(void)
{
    const profile_t *p;
    const char *caps[32];
    size_t n;
    char path[512];
    SECTION("profile");
    snprintf(path, sizeof(path), "%s/mock-x86.json", g_profiles_dir);
    CHECK(profile_load(path) == HAL_OK, "load %s", path);
    p = profile_get();
    CHECK(p && strcmp(p->platform, "mock") == 0, "platform");
    CHECK(p->channel_count == 2, "channels");
    CHECK(p->channels[0].def_codec == HAL_CODEC_H265, "main h265");
    CHECK(strcmp(p->ivs_engine, "sw") == 0, "ivs engine");
    CHECK(p->idp_enabled && p->rtsp_enabled && p->onvif_enabled && p->gb_enabled, "protocols on");
    CHECK(p->frame_bus_consumers == 8, "consumers");
    CHECK(profile_channel_by_name("sub") == &p->channels[1], "channel by name");
    n = profile_capabilities(caps, 32);
    CHECK(n >= 10, "capabilities count=%zu", n);

    /* 非法 profile：default 超过 max */
    CHECK(profile_load_from_string("{\"schema_version\":1,\"identity\":{\"model\":\"x\",\"vendor\":\"v\",\"hw\":\"1\",\"platform\":\"mock\"},"
        "\"video\":{\"sensors\":[\"a\"],\"lens\":{\"type\":\"fixed\",\"af\":false,\"zoom\":false},"
        "\"channels\":[{\"ch\":0,\"name\":\"main\",\"codecs\":[\"h264\"],\"max\":{\"w\":640,\"h\":360,\"fps\":30},"
        "\"default\":{\"codec\":\"h264\",\"w\":1920,\"h\":1080,\"fps\":25,\"kbps\":1000,\"rc\":\"vbr\",\"gop\":50}}],"
        "\"isp\":{\"wdr\":false,\"hdr\":false,\"3dnr\":false,\"daynight\":\"none\"},\"snapshot\":{\"codec\":\"mjpeg\",\"max\":{\"w\":640,\"h\":360}}},"
        "\"audio\":{\"in\":[],\"out\":[],\"codecs\":[\"pcm\"]},\"storage\":{\"tf\":false},\"network\":{\"eth\":true},"
        "\"gpio_map\":{},\"protocols\":{\"idp\":{\"enabled\":false},\"rtsp\":{\"enabled\":false},\"onvif\":{\"enabled\":false},\"gb28181\":{\"enabled\":false}},"
        "\"limits\":{\"frame_bus_consumers\":4,\"rtsp_sessions\":1,\"playback_sessions\":0,\"mem_budget_mb\":64}}") == HAL_EINVAL,
        "default>max rejected");
}

/* ================= event_bus ================= */
static volatile int g_motion_hits, g_sys_hits;
static void on_motion(const event_t *e, void *u) { (void)e; (void)u; g_motion_hits++; }
static void on_all(const event_t *e, void *u) { (void)e; (void)u; if (EVT_DOMAIN(e->type) == EVT_DOM_SYS) g_sys_hits++; }

static void test_event_bus(void)
{
    event_sub_t *s1, *s2;
    SECTION("event_bus");
    CHECK(event_bus_init(64) == HAL_OK, "init");
    CHECK(event_bus_subscribe(1u << EVT_DOM_IVS, on_motion, NULL, &s1) == HAL_OK, "sub ivs");
    CHECK(event_bus_subscribe(0, on_all, NULL, &s2) == HAL_OK, "sub all");
    for (int i = 0; i < 10; i++) CHECK(event_bus_emit(EVT_MAKE(EVT_DOM_IVS, 1), 0) == HAL_OK, "emit %d", i);
    CHECK(event_bus_emit(EVT_SYS_STARTED, -1) == HAL_OK, "emit sys");
    os_sleep_ms(50);
    CHECK(g_motion_hits == 10, "motion hits=%d", g_motion_hits);
    CHECK(g_sys_hits == 1, "sys hits=%d", g_sys_hits);
    CHECK(event_bus_unsubscribe(s1) == HAL_OK, "unsub");
    CHECK(event_bus_emit(EVT_MAKE(EVT_DOM_IVS, 1), 0) == HAL_OK, "emit after unsub");
    os_sleep_ms(30);
    CHECK(g_motion_hits == 10, "no more motion hits");
    CHECK(event_bus_unsubscribe(s2) == HAL_OK, "unsub 2");
    CHECK(event_bus_deinit() == HAL_OK, "deinit");
    CHECK(event_bus_emit(EVT_SYS_STARTED, -1) == HAL_ESTATE, "emit after deinit");
}

/* ================= config ================= */
static void test_config(void)
{
    bool b; int64_t i; char s[128];
    cfg_reject_t rej[4];
    int n;
    SECTION("config");
    remove("core_test_cfg.json");
    CHECK(cfg_init(NULL, "core_test_cfg.json") == HAL_OK, "init (profile already loaded)");

    /* profile 播种的默认值 */
    CHECK(cfg_get_str("video.0.main.codec", s, sizeof(s)) == HAL_OK && strcmp(s, "h265") == 0, "seeded codec");
    CHECK(cfg_get_int("video.0.main.w", &i) == HAL_OK && i == 1920, "seeded w");

    /* 规则校验 */
    CHECK(cfg_set_int("image.brightness", 55) == HAL_OK, "set brightness");
    CHECK(cfg_get_int("image.brightness", &i) == HAL_OK && i == 55, "get brightness");
    CHECK(cfg_set_int("image.brightness", 101) == HAL_EINVAL, "range rejected");
    CHECK(cfg_set_bool("image.brightness", true) == HAL_EINVAL, "type rejected");
    CHECK(cfg_set_str("record.mode", "event") == HAL_OK, "enum ok");
    CHECK(cfg_set_str("record.mode", "bogus") == HAL_EINVAL, "enum rejected");
    CHECK(cfg_set_int("no.such.key", 1) == HAL_EINVAL, "unknown key rejected");

    /* 批量 + 拒绝列表 */
    n = cfg_apply_json("{\"image.contrast\":60,\"image.brightness\":999,\"record.enabled\":true}", rej, 4);
    CHECK(n == 1 && strcmp(rej[0].key, "image.brightness") == 0, "apply rejected=%d", n);
    CHECK(cfg_get_int("image.contrast", &i) == HAL_OK && i == 60, "apply ok key");

    /* 持久化 + 重载 */
    CHECK(cfg_set_bool("record.enabled", true) == HAL_OK, "persist set");
    CHECK(cfg_deinit() == HAL_OK, "deinit");
    CHECK(cfg_init(NULL, "core_test_cfg.json") == HAL_OK, "re-init");
    CHECK(cfg_get_bool("record.enabled", &b) == HAL_OK && b, "persisted bool");
    CHECK(cfg_get_int("image.brightness", &i) == HAL_OK && i == 55, "persisted int");

    /* dump / reset */
    {
        char big[4096];
        CHECK(cfg_dump_json(big, sizeof(big)) == HAL_OK && strstr(big, "record.enabled"), "dump");
    }
    {
        const char *keep[] = { "record.enabled" };
        CHECK(cfg_reset(keep, 1) == HAL_OK, "reset");
        CHECK(cfg_get_bool("record.enabled", &b) == HAL_OK && b, "kept");
        CHECK(cfg_get_int("image.contrast", &i) == HAL_ENODEV, "reset cleared");
    }
    cfg_deinit();
    remove("core_test_cfg.json");
}

/* ================= frame_bus ================= */
typedef struct {
    frame_sub_t *sub;
    int          got;
    int          keys;
    volatile bool stop;
} reader_arg_t;

static void reader_thread(void *arg)
{
    reader_arg_t *r = (reader_arg_t *)arg;
    hal_frame_t f;
    while (!r->stop) {
        hal_err_t rc = frame_bus_pull(r->sub, &f, 50);
        if (rc == HAL_OK) {
            r->got++;
            if (f.flags & HAL_FRAME_FLAG_KEY) r->keys++;
            frame_bus_release(r->sub, &f);
            os_sleep_ms(1);   /* 慢消费者，触发丢帧路径 */
        }
    }
}

static volatile int g_started, g_stopped;
static void on_stream(const event_t *e, void *u)
{
    (void)u;
    if (e->type == EVT_STREAM_STARTED) g_started++;
    if (e->type == EVT_STREAM_STOPPED) g_stopped++;
}

static void test_frame_bus(void)
{
    frame_bus_t *bus;
    frame_sub_t *s1;
    hal_frame_t f;
    frame_bus_stats_t st;
    reader_arg_t ra;
    os_thread_t *slow;
    uint64_t dropped;

    SECTION("frame_bus");
    CHECK(hal_init(profile_raw_json()) == HAL_OK, "hal init");
    CHECK(hal()->video->open() == HAL_OK, "video open");
    {
        hal_enc_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.codec = HAL_CODEC_H265; cfg.width = 640; cfg.height = 360; cfg.fps = 50;
        cfg.rc = HAL_RC_VBR; cfg.bitrate_kbps = 512; cfg.gop = 10;
        CHECK(hal()->video->set_encoder(0, &cfg) == HAL_OK, "set_encoder");
    }
    {
        frame_bus_cfg_t bc;
        memset(&bc, 0, sizeof(bc));
        bc.max_consumers = 3; bc.queue_depth = 8; bc.start_on_demand = true; bc.idle_stop_ms = 0;
        CHECK(frame_bus_create(0, &bc, &bus) == HAL_OK, "create");
        CHECK(frame_bus_create(0, &bc, NULL) == HAL_EINVAL, "create NULL out");
        CHECK(frame_bus_get(0) == bus, "get(0)");
    }

    CHECK(event_bus_init(128) == HAL_OK, "evt init");
    event_sub_t *sev;
    event_bus_subscribe(1u << EVT_DOM_STREAM, on_stream, NULL, &sev);

    /* 未订阅：不跑 */
    CHECK(frame_bus_stats(bus, &st) == HAL_OK && !st.running, "idle not running");

    CHECK(frame_bus_subscribe(bus, "fast", true, &s1) == HAL_OK, "sub1");
    os_sleep_ms(30);                                   /* 事件总线异步投递 */
    CHECK(g_started == 1, "start event");
    CHECK(frame_bus_pull(s1, &f, 500) == HAL_OK, "pull1");
    CHECK((f.flags & HAL_FRAME_FLAG_KEY) != 0, "first frame is IDR");
    CHECK(f.data && f.size > 4, "frame payload");
    CHECK(frame_bus_release(s1, &f) == HAL_OK, "release1");

    /* 第二个消费者共享同一帧数据（零拷贝：指针相同） */
    {
        frame_sub_t *probe;
        hal_frame_t a, b2;
        CHECK(frame_bus_subscribe(bus, "probe", false, &probe) == HAL_OK, "sub2");
        /* 排空 s1 中 probe 订阅前入队的旧帧，保证双方取到同一帧 */
        while (frame_bus_pull(s1, &a, 0) == HAL_OK) frame_bus_release(s1, &a);
        CHECK(frame_bus_pull(s1, &a, 500) == HAL_OK, "pull s1");
        CHECK(frame_bus_pull(probe, &b2, 500) == HAL_OK, "pull probe");
        CHECK(a.data == b2.data && a.seq == b2.seq, "shared buffer zero-copy");
        frame_bus_release(s1, &a);
        frame_bus_release(probe, &b2);
        frame_bus_unsubscribe(probe);
    }

    /* 慢消费者丢帧但不阻塞快消费者 */
    memset(&ra, 0, sizeof(ra));
    ra.stop = false;
    CHECK(frame_bus_subscribe(bus, "slow", false, &ra.sub) == HAL_OK, "sub slow");
    slow = os_thread_create(reader_thread, &ra, "slowrd", 64);
    os_sleep_ms(300);
    ra.stop = true;
    os_thread_join(slow);
    CHECK(ra.got > 5, "slow reader got=%d", ra.got);
    CHECK(ra.keys >= 1, "slow reader saw keyframes=%d", ra.keys);
    frame_bus_stats(bus, &st);
    CHECK(st.frames_published > 10, "published=%llu", (unsigned long long)st.frames_published);
    CHECK(st.consumers == 2, "consumers=%u", st.consumers);
    frame_bus_sub_dropped(ra.sub, &dropped);
    (void)dropped;

    /* 超限：max_consumers=3 */
    {
        frame_sub_t *x, *y;
        CHECK(frame_bus_subscribe(bus, "x1", false, &x) == HAL_OK, "sub 3rd ok");
        CHECK(frame_bus_subscribe(bus, "x2", false, &y) == HAL_EBUSY, "4th rejected (max 3)");
        frame_bus_unsubscribe(x);
    }

    /* 全部退订 -> idle stop */
    frame_bus_unsubscribe(ra.sub);
    frame_bus_unsubscribe(s1);
    os_sleep_ms(80);
    CHECK(frame_bus_stats(bus, &st) == HAL_OK && !st.running, "idle stopped");
    CHECK(g_stopped >= 1, "stop event");

    /* 重新订阅恢复 */
    CHECK(frame_bus_subscribe(bus, "again", true, &s1) == HAL_OK, "resubscribe");
    CHECK(frame_bus_pull(s1, &f, 500) == HAL_OK, "frames flow again");
    frame_bus_release(s1, &f);
    frame_bus_unsubscribe(s1);

    CHECK(frame_bus_destroy(bus) == HAL_OK, "destroy");
    event_bus_unsubscribe(sev);
    event_bus_deinit();
    hal()->video->close();
    hal_deinit();
}

/* ================= module_loader ================= */
static int init_order[8];
static int init_n;
static bool m_a_fail_start, m_c_health_bad;

static bool en_true(void) { return true; }
static bool en_false(void) { return false; }
static hal_err_t m_init(void *tag) { init_order[init_n < 8 ? init_n++ : 7] = (int)(intptr_t)tag; return HAL_OK; }
static hal_err_t m_i_a(void) { return m_init((void *)1); }
static hal_err_t m_i_b(void) { return m_init((void *)2); }
static hal_err_t m_i_c(void) { return m_init((void *)3); }
static hal_err_t m_s_a(void) { return m_a_fail_start ? HAL_EIO : HAL_OK; }
static hal_err_t m_s_ok(void) { return HAL_OK; }
static hal_err_t m_stop(void) { return HAL_OK; }
static hal_err_t m_health_c(char *d, size_t c) { (void)d; (void)c; return m_c_health_bad ? HAL_EIO : HAL_OK; }

static const char *deps_b[] = { "a", NULL };
static const char *deps_c[] = { "b", NULL };
static const module_desc_t mA = { "a", 1, NULL, { 100, 1 }, en_true, m_i_a, m_s_a, m_stop, m_i_a, NULL };
static const module_desc_t mB = { "b", 1, deps_b, { 200, 1 }, en_true, m_i_b, m_s_ok, m_stop, m_i_b, NULL };
static const module_desc_t mC = { "c", 1, deps_c, { 300, 2 }, en_true, m_i_c, m_s_ok, m_stop, m_i_c, m_health_c };
static const module_desc_t mD = { "d", 1, NULL, { 0, 0 }, en_false, m_i_a, m_s_ok, m_stop, m_i_a, NULL };

static void test_module_loader(void)
{
    SECTION("module_loader");
    init_n = 0;
    CHECK(module_register(&mA) == HAL_OK, "reg a");
    CHECK(module_register(&mB) == HAL_OK, "reg b");
    CHECK(module_register(&mC) == HAL_OK, "reg c");
    CHECK(module_register(&mD) == HAL_OK, "reg d (disabled)");
    CHECK(module_register(&mA) == HAL_EBUSY, "dup rejected");

    CHECK(module_start_all() == HAL_OK, "start_all");
    CHECK(init_n == 3 && init_order[0] == 1 && init_order[1] == 2 && init_order[2] == 3, "topo order a<b<c");
    CHECK(module_state("b") == MOD_STATE_RUNNING, "state running");
    CHECK(module_state("d") == MOD_STATE_UNLOADED, "disabled never started");

    m_c_health_bad = true;
    for (int i = 0; i < 3; i++) module_health_check(NULL, 0);
    CHECK(module_state("c") == MOD_STATE_RUNNING, "c restarted (health ok again after restart)");
    m_c_health_bad = false;

    CHECK(module_stop_all() == HAL_OK, "stop_all");
    CHECK(module_state("a") == MOD_STATE_UNLOADED, "state unloaded after stop");

    /* 失败传播：a start 失败 -> 整体失败 */
    m_a_fail_start = true;
    init_n = 0;
    CHECK(module_start_all() == HAL_EIO, "start failure propagates");
    CHECK(module_state("a") == MOD_STATE_FAILED, "a failed");
    m_a_fail_start = false;
    CHECK(module_start_all() == HAL_OK, "restart ok");
    module_stop_all();
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    log_init(LOG_INFO, 32);
    test_json();
    test_os();
    test_log();
    test_profile();
    test_event_bus();
    test_config();
    test_frame_bus();
    test_module_loader();
    printf("\nRESULT: core pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
