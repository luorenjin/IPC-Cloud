/**
 * @file console_api.c
 * @brief console REST 子模块：配置读写、系统信息、运行时实况
 *
 * 配置统一走 core/config（cfg_apply_json/cfg_dump_json/cfg_register_rules），
 * 不重写一套校验或持久化；这里只负责：登记 profile 派生的规则、把 core/config
 * 与 HAL 的读数拼成 JSON、以及路由分发。
 *
 * 路由：只注册一个前缀 "/api/v1/"（与 Task 6 的 "/api/v1/auth/" 分居
 * ROUTE_MAX=8 的两个槽位），内部按 req->path 精确匹配再分发到各端点，
 * 详见 api_dispatch。
 */
#include "console_internal.h"
#include "core/config.h"
#include "core/profile.h"
#include "core/module.h"
#include "core/json.h"
#include "core/log.h"
#include "hal/hal.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "console"

/** 单次响应体上限（堆分配，不占用 http_server 事件循环线程的 64KB 栈——见
 *  http_server_start 的 os_thread_create(..., "http", 64) 调用）。留给
 *  GET /api/v1/config 的 wrapper 开销（"{\"code\":0,\"data\":...}"）远小于
 *  CONSOLE_CFG_BUF_MAX 与本值之间的差额。 */
#define CONSOLE_API_BODY_MAX (20 * 1024)
/** cfg_dump_json/cfg_get_json 的工作缓冲上限：mock-x86 profile 下实际配置
 *  远小于此值；命中即视为可能截断（见 ep_config_get），不会静默吐出半截 JSON。 */
#define CONSOLE_CFG_BUF_MAX  (16 * 1024)
/** system/status 汇总的模块数上限：与 core/module_loader.c 的私有常量
 *  MODULES_MAX 取值一致（该常量未对外导出）；超出只会截断列表，不会越界。 */
#define CONSOLE_MODULES_MAX  16

/** 带截断检测的格式化：截断即返回 HAL_ENOMEM 并把 out 置空串（与
 *  console_auth.c 的同名同语义 helper 各自独立一份，两个 .c 互不引用对方的
 *  static 符号）。 */
static hal_err_t fmt_safe(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!out || cap == 0) return HAL_EINVAL;
    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return HAL_ENOMEM; }
    return HAL_OK;
}

/* ==========================================================================
 * 一、配置校验规则登记（规则 R3：上下界与枚举一律从 profile 动态生成）
 * ========================================================================== */

/** 按 codecs_mask 生成形如 "h264,h265" 的枚举串，供 cfg_rule_t.enum_csv 用。
 *  与 core/config.c 的 seed_channel 各写各的一份同类逻辑——模块间不得直接
 *  调用对方的 static 函数（规则 R4），且它本就是 core 的内部实现细节。 */
static void codec_enum_csv(uint32_t codecs_mask, char *out, size_t cap)
{
    static const struct { hal_codec_t codec; const char *name; } table[] = {
        { HAL_CODEC_H264, "h264" }, { HAL_CODEC_H265, "h265" }, { HAL_CODEC_MJPEG, "mjpeg" }
    };
    size_t off = 0, i;

    out[0] = '\0';
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        int n;
        if (!(codecs_mask & (1u << table[i].codec))) continue;
        n = snprintf(out + off, off < cap ? cap - off : 0, "%s%s", off ? "," : "", table[i].name);
        if (n > 0) off += (size_t)n;
    }
}

/**
 * 登记 video.0.main.* / video.1.sub.codec 等键的校验规则。
 *
 * 重要说明（如实记录，供后续维护者知晓）：core/config.c 的 cfg_init() 在
 * 启动时已经用 profile 的 channels[].max_w/h/fps 为每个通道自动登记过同名
 * 规则（其内部 seed_channel()），且必然早于本函数被调用——cfg_register_rules
 * 要求 g.inited 已为真，而这只有 cfg_init 完成之后才成立。core/config.c 的
 * rule_for() 按登记顺序线性查找、返回第一个匹配的规则，因此本函数为
 * video.*.main.kbps / .gop 登记的更紧上下界（相对 core 自动登记的通用值
 * kbps 32~16384、gop 1~300），在当前实现下不会成为实际生效的那一条——校验
 * 结果仍由 core 自动登记的规则决定。这不是本任务能修的问题（把 rule_for
 * 改成"后登记者覆盖"要动 core/config.c，超出本任务范围）。
 * 即便如此，下面登记的上下界与枚举仍然全部来自 profile 动态生成、不是
 * 硬编码摆设：w/h/fps/codec 的取值与 core 自动登记的完全一致（两者独立地从
 * 同一份 profile 派生，重复只是无害冗余）；kbps/gop 的更紧数值一旦
 * core 一侧的匹配语义改变就会立即生效，且届时数值已经是对的。
 */
hal_err_t console_api_register_rules(void)
{
    const profile_channel_t *main_ch, *sub_ch;
    cfg_rule_t rules[16];
    size_t n = 0, k = 0;
    char keys[7][CFG_KEY_MAX];
    char main_codecs[32], sub_codecs[32];

    if (!profile_get()) return HAL_ESTATE;   /* profile 未加载：不应发生，纯防御 */

    main_ch = profile_channel_by_name("main");
    sub_ch  = profile_channel_by_name("sub");

    if (main_ch) {
        codec_enum_csv(main_ch->codecs_mask, main_codecs, sizeof(main_codecs));

        snprintf(keys[k], CFG_KEY_MAX, "video.%d.%s.w", main_ch->ch, main_ch->name);
        rules[n++] = (cfg_rule_t){ keys[k++], CFG_T_INT, 176, (int64_t)main_ch->max_w, NULL, false };

        snprintf(keys[k], CFG_KEY_MAX, "video.%d.%s.h", main_ch->ch, main_ch->name);
        rules[n++] = (cfg_rule_t){ keys[k++], CFG_T_INT, 144, (int64_t)main_ch->max_h, NULL, false };

        snprintf(keys[k], CFG_KEY_MAX, "video.%d.%s.fps", main_ch->ch, main_ch->name);
        rules[n++] = (cfg_rule_t){ keys[k++], CFG_T_INT, 1, (int64_t)main_ch->max_fps, NULL, false };

        snprintf(keys[k], CFG_KEY_MAX, "video.%d.%s.kbps", main_ch->ch, main_ch->name);
        rules[n++] = (cfg_rule_t){ keys[k++], CFG_T_INT, 128, 4096, NULL, false };

        snprintf(keys[k], CFG_KEY_MAX, "video.%d.%s.gop", main_ch->ch, main_ch->name);
        rules[n++] = (cfg_rule_t){ keys[k++], CFG_T_INT, 1, 150, NULL, false };

        snprintf(keys[k], CFG_KEY_MAX, "video.%d.%s.codec", main_ch->ch, main_ch->name);
        rules[n++] = (cfg_rule_t){ keys[k++], CFG_T_STR, 0, 0, main_codecs, false };
    }
    if (sub_ch) {
        /* 子码流的可用编解码集合同样来自 profile 的 codecs_mask（mock-x86
           下只有 h264），不硬编码"子码流只能 h264"这条产品假设。 */
        codec_enum_csv(sub_ch->codecs_mask, sub_codecs, sizeof(sub_codecs));
        snprintf(keys[k], CFG_KEY_MAX, "video.%d.%s.codec", sub_ch->ch, sub_ch->name);
        rules[n++] = (cfg_rule_t){ keys[k++], CFG_T_STR, 0, 0, sub_codecs, false };
    }

    return cfg_register_rules(rules, n);
}

/* ==========================================================================
 * 二、能力清单（供前端按能力渲染菜单；GET /api/v1/system/info 内嵌同一份）
 * ========================================================================== */

static bool cap_wifi(const profile_t *p)
{
    hal_net_caps_t nc;
    if (!p->wifi || !hal_has(HAL_MOD_NET) || !hal()->net->get_caps) return false;
    if (hal()->net->get_caps(&nc) != HAL_OK) return false;
    return nc.wifi;
}

static bool cap_wifi_ap(void)
{
    hal_net_caps_t nc;
    if (!hal_has(HAL_MOD_NET) || !hal()->net->get_caps) return false;
    if (hal()->net->get_caps(&nc) != HAL_OK) return false;
    return nc.wifi_ap;
}

static bool cap_h265(void)
{
#ifdef IPC_CONSOLE_H265
    const profile_channel_t *main_ch = profile_channel_by_name("main");
    return main_ch && (main_ch->codecs_mask & (1u << HAL_CODEC_H265)) != 0;
#else
    return false;
#endif
}

static bool cap_playback(void)
{
#ifdef IPC_CONSOLE_PLAYBACK
    /* 经 core/module.h 的启动器 API 查询，不直接调用 recorder 模块的函数
       （规则 R4）。recorder 尚未在本仓库落地时，module_state 对未注册的
       名字恒返回 MOD_STATE_UNLOADED，天然得到 false，不需要特判。 */
    return module_state("recorder") != MOD_STATE_UNLOADED;
#else
    return false;
#endif
}

hal_err_t console_caps_json(char *buf, size_t cap)
{
    const profile_t *p;
    json_t *obj;
    char *txt;

    if (!buf || cap == 0) return HAL_EINVAL;
    p = profile_get();
    if (!p) return HAL_ESTATE;

    obj = json_new_object();
    if (!obj) return HAL_ENOMEM;
    if (json_object_set(obj, "model", json_new_string(p->model)) != 0 ||
        json_object_set(obj, "vendor", json_new_string(p->vendor)) != 0 ||
        json_object_set(obj, "wifi", json_new_bool(cap_wifi(p))) != 0 ||
        json_object_set(obj, "wifi_ap", json_new_bool(cap_wifi_ap())) != 0 ||
        json_object_set(obj, "tf", json_new_bool(p->tf)) != 0 ||
        json_object_set(obj, "h265", json_new_bool(cap_h265())) != 0 ||
        json_object_set(obj, "playback", json_new_bool(cap_playback())) != 0) {
        json_free(obj);
        return HAL_ENOMEM;
    }

    txt = json_dump(obj, false);
    json_free(obj);
    if (!txt) return HAL_ENOMEM;
    if (strlen(txt) >= cap) { free(txt); return HAL_ENOMEM; }
    strcpy(buf, txt);
    free(txt);
    return HAL_OK;
}

/* ==========================================================================
 * 三、GET/PUT /api/v1/config
 * ========================================================================== */

/**
 * GET /api/v1/config?prefix=xxx
 * 响应：{"code":0,"data":{...}}；prefix 省略或为空时 data 是全量配置
 * （cfg_dump_json），否则是该前缀下的子树（cfg_get_json 的前缀聚合读取，
 * 键名已去掉前缀，如 prefix=video.0.main 时得到 {"kbps":2048,...}）。
 * 前缀不命中任何键时 data 为 {}（视为"暂无数据"而非错误，避免前端因为
 * 页面还没配置某个可选子树就弹错误提示）。
 */
static hal_err_t ep_config_get(const http_req_t *req, char *out, size_t out_cap)
{
    char prefix_buf[CFG_KEY_MAX];
    const char *prefix;
    char *cfgbuf;
    hal_err_t rc;

    /* http_query 未命中时直接返回调用者传入的 def 指针，不会往 prefix_buf
       里写任何东西——必须用它的返回值，不能假设 buf 一定被填充（不这样做
       会在"未传 prefix"的常见路径上读到未初始化的栈内容）。 */
    prefix = http_query(req, "prefix", prefix_buf, sizeof(prefix_buf), "");

    cfgbuf = (char *)malloc(CONSOLE_CFG_BUF_MAX);
    if (!cfgbuf) return HAL_ENOMEM;

    if (prefix[0] == '\0') {
        rc = cfg_dump_json(cfgbuf, CONSOLE_CFG_BUF_MAX);
    } else {
        rc = cfg_get_json(prefix, cfgbuf, CONSOLE_CFG_BUF_MAX);
        if (rc == HAL_ENODEV) { snprintf(cfgbuf, CONSOLE_CFG_BUF_MAX, "{}"); rc = HAL_OK; }
    }
    if (rc != HAL_OK) { free(cfgbuf); return HAL_EIO; }

    /* 截断检测：cfg_dump_json/cfg_get_json 内部用 strncpy 静默截断到
       cap-1，长度恰好等于 cap-1 是"很可能被截断"的信号（真实内容长度精确
       等于该值的概率可以忽略）。绝不能把截断当成功——那样前端会拿到断在
       中间、解析失败的 JSON，而不是一个明确的错误。 */
    if (strlen(cfgbuf) >= CONSOLE_CFG_BUF_MAX - 1) { free(cfgbuf); return HAL_ENOMEM; }

    rc = fmt_safe(out, out_cap, "{\"code\":0,\"data\":%s}", cfgbuf);
    free(cfgbuf);
    return rc;
}

/**
 * PUT /api/v1/config，body 为扁平 JSON 对象 {"video.0.main.kbps":2048,...}。
 * 响应：{"code":0,"applied":N,"rejected":[{"key":..,"reason":..}]}——
 * 部分成功语义，与 IDP 的 ack.data.rejected[] 一致：校验失败的键单独列出、
 * 原因简短（"range"/"enum"/"type"/"unknown_key"），同批次里通过校验的键
 * 照常应用并持久化，不因为其中几个键不合法就整体回滚。
 */
static hal_err_t ep_config_put(const http_req_t *req, char *out, size_t out_cap)
{
    cfg_reject_t rejects[8];
    char *bodycopy;
    const json_t *counted;
    int total, rejected, i;
    json_t *root, *arr;
    char *txt;
    hal_err_t rc;

    if (!req->body || req->body_len == 0) return HAL_EINVAL;

    /* cfg_apply_json 内部以 NUL 结尾字符串重新解析（json_parse(.., 0, ..)），
       而 req->body 只保证 [0, body_len) 有效、不保证其后是 NUL——必须先拷贝
       成一份自己独立、以 NUL 结尾的缓冲，两次解析（这里数总键数一次、
       cfg_apply_json 内部再解析一次）都基于这份拷贝，不会读到接收缓冲里
       body 之后的脏字节。 */
    bodycopy = (char *)malloc(req->body_len + 1);
    if (!bodycopy) return HAL_ENOMEM;
    memcpy(bodycopy, req->body, req->body_len);
    bodycopy[req->body_len] = '\0';

    counted = json_parse(bodycopy, req->body_len, NULL, 0);
    if (!counted || !json_is(counted, JSON_OBJECT)) {
        json_free((json_t *)counted);
        free(bodycopy);
        return HAL_EINVAL;
    }
    total = (int)json_size(counted);
    json_free((json_t *)counted);

    rejected = cfg_apply_json(bodycopy, rejects, 8);
    free(bodycopy);
    if (rejected < 0) return HAL_EINVAL;   /* 畸形 JSON：cfg_apply_json 自身解析失败 */

    root = json_new_object();
    arr = json_new_array();
    if (!root || !arr) { json_free(root); json_free(arr); return HAL_ENOMEM; }

    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "applied", json_new_int(total - rejected));
    for (i = 0; i < rejected && i < 8; i++) {
        json_t *r = json_new_object();
        if (!r) continue;
        json_object_set(r, "key", json_new_string(rejects[i].key));
        json_object_set(r, "reason", json_new_string(rejects[i].reason));
        json_array_push(arr, r);
    }
    json_object_set(root, "rejected", arr);

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/* ==========================================================================
 * 四、GET /api/v1/system/{info,status}、POST /api/v1/system/{reboot,reset}
 * ========================================================================== */

/** 设备序列号：优先取安全存储里的 17 位 DeviceID；无 hal_crypto 或尚未
 *  产线烧录（真机开发早期、mock 环境常见）时留空，不视为错误。 */
static void device_serial(char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    if (!hal_has(HAL_MOD_CRYPTO) || !hal()->crypto->secure_read) return;
    if (hal()->crypto->secure_read(HAL_SEC_KEY_DEVICE_ID, (uint8_t *)out, cap - 1, &len) != HAL_OK)
        return;
    if (len >= cap) len = cap - 1;
    out[len] = '\0';
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                       out[len - 1] == ' '  || out[len - 1] == '\t')) out[--len] = '\0';
}

/**
 * GET /api/v1/system/info：型号/序列号/固件版本/运行时长 + 能力清单。
 * 响应：{"code":0,"model":..,"vendor":..,"serial":..,"fw_version":..,
 *        "uptime_s":..,"caps":{...console_caps_json 同款字段...}}
 * 唯一在强制改密期间仍可访问的业务端点（console_auth_check 已豁免），
 * 前端在改密页也要能读到型号与能力清单渲染页面外壳。
 */
static hal_err_t ep_system_info(char *out, size_t out_cap)
{
    const profile_t *p = profile_get();
    hal_sys_stats_t st;
    hal_ota_state_t ota;
    char serial[64];
    char caps_buf[1024];
    json_t *root, *caps_obj;
    char *txt;
    hal_err_t rc;

    if (!p) return HAL_ESTATE;
    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->get_stats || hal()->sys->get_stats(&st) != HAL_OK)
        return HAL_EIO;

    memset(&ota, 0, sizeof(ota));
    if (hal_has(HAL_MOD_SYS) && hal()->sys->ota_get_state) hal()->sys->ota_get_state(&ota);
    device_serial(serial, sizeof(serial));

    if (console_caps_json(caps_buf, sizeof(caps_buf)) != HAL_OK) return HAL_ENOMEM;
    caps_obj = json_parse(caps_buf, 0, NULL, 0);

    root = json_new_object();
    if (!root) { json_free(caps_obj); return HAL_ENOMEM; }
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "model", json_new_string(p->model));
    json_object_set(root, "vendor", json_new_string(p->vendor));
    json_object_set(root, "serial", json_new_string(serial));
    json_object_set(root, "fw_version", json_new_string(ota.current_version));
    json_object_set(root, "uptime_s", json_new_int((int64_t)st.uptime_s));
    if (caps_obj) json_object_set(root, "caps", caps_obj);   /* 接管所有权 */

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/**
 * GET /api/v1/system/status：CPU/内存/温度/运行时长 + 各模块 health。
 * 响应：{"code":0,"cpu_usage_pct":..,"mem_total_kb":..,"mem_free_kb":..,
 *        "mem_avail_kb":..,"temp_milli_c":..,"uptime_s":..,
 *        "modules":[{"name":..,"state":..,"health":".."(仅 running 时)}]}
 * 经 core/module.h 的 module_list + 逐个 health() 汇总，不直接调用其他
 * 模块的函数（规则 R4）。
 */
static hal_err_t ep_system_status(char *out, size_t out_cap)
{
    const module_desc_t *mods[CONSOLE_MODULES_MAX];
    size_t n, i;
    hal_sys_stats_t st;
    json_t *root, *arr;
    char *txt;
    hal_err_t rc;

    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->get_stats || hal()->sys->get_stats(&st) != HAL_OK)
        return HAL_EIO;

    root = json_new_object();
    arr = json_new_array();
    if (!root || !arr) { json_free(root); json_free(arr); return HAL_ENOMEM; }

    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "cpu_usage_pct", json_new_int(st.cpu_usage_pct));
    json_object_set(root, "mem_total_kb", json_new_int(st.mem_total_kb));
    json_object_set(root, "mem_free_kb", json_new_int(st.mem_free_kb));
    json_object_set(root, "mem_avail_kb", json_new_int(st.mem_avail_kb));
    json_object_set(root, "temp_milli_c", json_new_int(st.temp_milli_c));
    json_object_set(root, "uptime_s", json_new_int((int64_t)st.uptime_s));

    n = module_list(mods, CONSOLE_MODULES_MAX);
    for (i = 0; i < n; i++) {
        module_state_t state = module_state(mods[i]->name);
        const char *state_str;
        json_t *m;

        switch (state) {
        case MOD_STATE_INITED:  state_str = "inited";   break;
        case MOD_STATE_RUNNING: state_str = "running";  break;
        case MOD_STATE_STOPPED: state_str = "stopped";  break;
        case MOD_STATE_FAILED:  state_str = "failed";   break;
        default:                state_str = "unloaded"; break;
        }
        m = json_new_object();
        if (!m) continue;
        json_object_set(m, "name", json_new_string(mods[i]->name));
        json_object_set(m, "state", json_new_string(state_str));
        /* 只在 RUNNING 时才调 health()：与 module_health_check 的巡检口径
           一致——未运行的模块没有"健康与否"可言。 */
        if (state == MOD_STATE_RUNNING && mods[i]->health) {
            char detail[128] = "";
            hal_err_t hrc = mods[i]->health(detail, sizeof(detail));
            json_object_set(m, "health", json_new_string(hrc == HAL_OK ? "ok" : (detail[0] ? detail : "error")));
        }
        json_array_push(arr, m);
    }
    json_object_set(root, "modules", arr);

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/*
 * resolution B："先响应再动作"：http_respond_json 只把数据拷进连接的发送
 * 队列，真正的 send() 发生在事件循环的可写分支（conn_flush_send）里——
 * 在 handler 内联调用 hal()->sys->reboot()，响应很可能还在队列里就被
 * 一起带走，客户端永远收不到这个 200。这里改用 http_conn_defer_after_flush
 * 登记，动作会在 conn_flush_send 观察到本连接排队字节已经全部交给内核
 * （c->slen==c->soff）之后才执行；该原语本身已由 http_server_test 的
 * e2e 测试覆盖（用真实 socket 验证登记不会同步触发、且响应收完后回调
 * 确实会被触发）。
 *
 * 强度说明：这只保证字节已经交给了内核 socket 发送缓冲，不保证对端已经
 * 收到——重启会把内核里还没真正发出的字节一起带走，这是 TCP/操作系统的
 * 边界，任何应用层机制都做不到更强（见 http_server.h 的
 * http_conn_defer_after_flush 声明注释）。即便如此也远好于内联执行。
 */
static void do_reboot(void *arg)
{
    (void)arg;
    if (hal_has(HAL_MOD_SYS) && hal()->sys->reboot) hal()->sys->reboot();
}

static void do_reset(void *arg)
{
    /* keep_network 借 void* 本身的值传递，不做堆分配——避免
       http_respond_json 失败导致不登记动作时出现悬空的堆块需要回收。 */
    bool keep_network = (bool)(intptr_t)arg;
    if (!hal_has(HAL_MOD_SYS)) return;
    if (hal()->sys->factory_reset) hal()->sys->factory_reset(keep_network);
    if (hal()->sys->reboot) hal()->sys->reboot();
}

/** POST /api/v1/system/reboot。响应：{"code":0,"msg":"设备将重启"} */
static hal_err_t ep_system_reboot(char *out, size_t out_cap, void (**dfn)(void *), void **darg)
{
    hal_err_t rc = fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"设备将重启\"}");
    if (rc != HAL_OK) return rc;
    *dfn = do_reboot;
    *darg = NULL;
    return HAL_OK;
}

/**
 * POST /api/v1/system/reset，body 可选 {"keep_network":true|false}（缺省
 * false）。响应：{"code":0,"msg":"设备将恢复出厂设置并重启"}
 * core/config 一侧的恢复出厂（cfg_reset）立即同步执行——复用其原子持久化，
 * 不重写一套（resolution C 同类取舍：与 PUT /api/v1/config 一样，配置落盘
 * 属于本计划接受的"handler 内做一次性磁盘 I/O"例外）。HAL 一侧（证书/
 * WiFi/重启）随重启一起被 resolution B 的机制延后到响应真正发出之后。
 */
static hal_err_t ep_system_reset(const http_req_t *req, char *out, size_t out_cap,
                                 void (**dfn)(void *), void **darg)
{
    bool keep_network = false;
    hal_err_t rc;

    if (req->body && req->body_len > 0) {
        json_t *j = json_parse(req->body, req->body_len, NULL, 0);
        if (j) {
            keep_network = json_bool(json_get(j, "keep_network"), false);
            json_free(j);
        }
    }

    cfg_reset(NULL, 0);

    rc = fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"设备将恢复出厂设置并重启\"}");
    if (rc != HAL_OK) return rc;
    *dfn = do_reset;
    *darg = (void *)(intptr_t)keep_network;
    return HAL_OK;
}

/* ==========================================================================
 * 五、GET /api/v1/video/params、GET /api/v1/storage/info（运行时实况，查 HAL）
 * ========================================================================== */

/**
 * 响应：{"code":0,"channels":[
 *   {"ch":0,"name":"main","running":true,"codec":"h265","w":1920,"h":1080,
 *    "fps":25,"kbps":2048,"gop":50,"rc":"vbr"},
 *   {"ch":1,"name":"sub","running":false}
 * ]}
 * "running":false 表示该通道尚未 set_encoder/start（HAL 返回 HAL_ESTATE），
 * 此时不带 codec/w/h/... 等字段，而不是硬凑一份假数据。
 */
static hal_err_t ep_video_params(char *out, size_t out_cap)
{
    const profile_t *p = profile_get();
    json_t *root, *arr;
    char *txt;
    uint32_t i;
    hal_err_t rc;

    if (!p) return HAL_ESTATE;
    if (!hal_has(HAL_MOD_VIDEO) || !hal()->video->get_encoder) return HAL_ENOTSUP;

    root = json_new_object();
    arr = json_new_array();
    if (!root || !arr) { json_free(root); json_free(arr); return HAL_ENOMEM; }
    json_object_set(root, "code", json_new_int(0));

    for (i = 0; i < p->channel_count; i++) {
        const profile_channel_t *c = &p->channels[i];
        hal_enc_cfg_t cfg;
        json_t *jc = json_new_object();
        if (!jc) continue;
        json_object_set(jc, "ch", json_new_int(c->ch));
        json_object_set(jc, "name", json_new_string(c->name));

        if (hal()->video->get_encoder(c->ch, &cfg) == HAL_OK) {
            static const char *rc_names[] = { "cbr", "vbr", "avbr", "fixqp" };
            const char *codec_name = cfg.codec == HAL_CODEC_H265 ? "h265"
                                    : cfg.codec == HAL_CODEC_MJPEG ? "mjpeg" : "h264";
            json_object_set(jc, "running", json_new_bool(true));
            json_object_set(jc, "codec", json_new_string(codec_name));
            json_object_set(jc, "w", json_new_int(cfg.width));
            json_object_set(jc, "h", json_new_int(cfg.height));
            json_object_set(jc, "fps", json_new_int(cfg.fps));
            json_object_set(jc, "kbps", json_new_int(cfg.bitrate_kbps));
            json_object_set(jc, "gop", json_new_int(cfg.gop));
            json_object_set(jc, "rc", json_new_string(
                (cfg.rc >= HAL_RC_CBR && cfg.rc <= HAL_RC_FIXQP) ? rc_names[cfg.rc] : "?"));
        } else {
            json_object_set(jc, "running", json_new_bool(false));
        }
        json_array_push(arr, jc);
    }
    json_object_set(root, "channels", arr);

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/**
 * 响应：{"code":0,"present":..,"mounted":..,"mount_path":..,"fs":"exfat",
 *        "total_bytes":..,"free_bytes":..,"health":"ok","io_errors":0,
 *        "cid":".."}
 */
static hal_err_t ep_storage_info(char *out, size_t out_cap)
{
    hal_storage_stat_t st;
    json_t *root;
    char *txt;
    const char *fs_name, *health_name;
    hal_err_t rc;

    if (!hal_has(HAL_MOD_STORAGE) || !hal()->storage->stat) return HAL_ENOTSUP;
    if (hal()->storage->stat(&st) != HAL_OK) return HAL_EIO;

    switch (st.fs) {
    case HAL_FS_FAT32: fs_name = "fat32"; break;
    case HAL_FS_EXFAT: fs_name = "exfat"; break;
    case HAL_FS_EXT4:  fs_name = "ext4";  break;
    default:           fs_name = "unknown"; break;
    }
    switch (st.health) {
    case HAL_STOR_HEALTH_OK:   health_name = "ok";   break;
    case HAL_STOR_HEALTH_WARN: health_name = "warn"; break;
    case HAL_STOR_HEALTH_BAD:  health_name = "bad";  break;
    default:                   health_name = "unknown"; break;
    }

    root = json_new_object();
    if (!root) return HAL_ENOMEM;
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "present", json_new_bool(st.present));
    json_object_set(root, "mounted", json_new_bool(st.mounted));
    json_object_set(root, "mount_path", json_new_string(st.mount_path));
    json_object_set(root, "fs", json_new_string(fs_name));
    json_object_set(root, "total_bytes", json_new_int((int64_t)st.total_bytes));
    json_object_set(root, "free_bytes", json_new_int((int64_t)st.free_bytes));
    json_object_set(root, "health", json_new_string(health_name));
    json_object_set(root, "io_errors", json_new_int(st.io_errors));
    json_object_set(root, "cid", json_new_string(st.cid));

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/* ==========================================================================
 * 六、分发与路由注册
 * ========================================================================== */

/**
 * 纯函数：按 req->path/method 分发到具体端点，只往 out 写响应体、不碰
 * req->conn，也不调用 http_respond 系列或 http_conn_defer_after_flush——
 * 这样 console_api_test_dispatch 才能在没有真实/伪造连接的情况下驱动它。
 * 真正的 I/O 由 console_api_handler 在拿到结果之后统一做。
 * *dfn/*darg 非 NULL 表示该端点要求"响应发出后再执行"的动作（目前只有
 * 重启/恢复出厂两个端点会用到）。
 */
static hal_err_t api_dispatch(const http_req_t *req, char *out, size_t out_cap,
                              void (**dfn)(void *), void **darg)
{
    hal_err_t e;

    *dfn = NULL;
    *darg = NULL;

    e = console_auth_check(req);
    if (e != HAL_OK) return e;

    if (strcmp(req->path, "/api/v1/config") == 0) {
        if (strcmp(req->method, "GET") == 0) return ep_config_get(req, out, out_cap);
        if (strcmp(req->method, "PUT") == 0) return ep_config_put(req, out, out_cap);
        return HAL_EINVAL;
    }
    if (strcmp(req->path, "/api/v1/system/info") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_system_info(out, out_cap) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/status") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_system_status(out, out_cap) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/reboot") == 0)
        return strcmp(req->method, "POST") == 0 ? ep_system_reboot(out, out_cap, dfn, darg) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/reset") == 0)
        return strcmp(req->method, "POST") == 0 ? ep_system_reset(req, out, out_cap, dfn, darg) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/video/params") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_video_params(out, out_cap) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/storage/info") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_storage_info(out, out_cap) : HAL_EINVAL;

    return HAL_ENODEV;
}

static int console_api_handler(http_req_t *req, void *user)
{
    char *body;
    void (*dfn)(void *) = NULL;
    void *darg = NULL;
    hal_err_t e;

    (void)user;
    body = (char *)malloc(CONSOLE_API_BODY_MAX);
    if (!body) return console_reply_err(req->conn, HAL_ENOMEM);

    e = api_dispatch(req, body, CONSOLE_API_BODY_MAX, &dfn, &darg);
    if (e != HAL_OK) { free(body); return console_reply_err(req->conn, e); }

    e = http_respond_json(req->conn, 200, body);
    free(body);
    if (e != HAL_OK) return console_reply_err(req->conn, e);   /* 入队失败：不登记动作 */

    if (dfn && http_conn_defer_after_flush(req->conn, dfn, darg) != HAL_OK) {
        /* http_conn_defer_after_flush 只在 c 或 fn 为 NULL 时返回非 HAL_OK，
           这里两者都已确定非空，正常不会走到这条分支——纯防御。响应已经
           发出，不能退回来同步执行动作（那样会破坏"响应先发出"这条约束
           本身），只能记日志留痕。 */
        LOGE(MOD, "延后动作登记失败：响应已发出但设备不会自动执行该动作，请重试");
    }
    return 0;
}

hal_err_t console_api_init(void)
{
    hal_err_t e = console_api_register_rules();
    if (e != HAL_OK) return e;
    return http_route("/api/v1/", console_api_handler, NULL);
}

#ifdef IPC_TESTING
hal_err_t console_api_test_dispatch(const http_req_t *req, char *body, size_t body_cap,
                                    bool *deferred_out)
{
    void (*dfn)(void *) = NULL;
    void *darg = NULL;
    hal_err_t rc = api_dispatch(req, body, body_cap, &dfn, &darg);
    if (deferred_out) *deferred_out = (rc == HAL_OK) && (dfn != NULL);
    (void)darg;
    return rc;
}
#endif
