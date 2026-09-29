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
#include "core/os.h"
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

/**
 * 核对结论（复核后修正，供后续维护者确认——不要在没有重新核对 core/config.c
 * 的情况下往这里加回 video.* 规则）：
 *
 * core/config.c 的 cfg_init() 必然早于本函数被调用（cfg_register_rules
 * 要求 g.inited 已为真），且它在 register_common_rules() 之后，已经为
 * profile 的每一个通道调用了内部的 seed_channel()（core/src/config.c:245-286），
 * 逐条登记：
 *   video.<ch>.<name>.codec —— 枚举取自 channels[].codecs_mask
 *   video.<ch>.<name>.w     —— 上界取自 channels[].max_w
 *   video.<ch>.<name>.h     —— 上界取自 channels[].max_h
 *   video.<ch>.<name>.fps   —— 上界取自 channels[].max_fps
 *   video.<ch>.<name>.kbps  —— 固定 32~16384
 *   video.<ch>.<name>.gop   —— 固定 1~300
 *   video.<ch>.<name>.rc    —— 枚举 cbr/vbr/avbr/fixqp
 * register_common_rules()（core/src/config.c:288 起）则登记了 image.*、
 * record.*、time.*、net.*、osd.*、alarm.*、led.* 等全部通用键。
 *
 * brief 要求登记的 video.0.main.{w,h,fps,kbps,gop,codec}、video.1.sub.codec
 * 以及"image.* 与 record.* 同理"这句话所指的键，经逐条比对，**全部**已被
 * 上面两处覆盖——
 * 上下界/枚举同样来自 profile（与 R3 的要求完全一致），语义与本模块原本
 * 打算登记的没有任何差异。cfg_register_rules 底层的 rule_for()
 * （core/src/config.c 的 rule_for，一个线性查找，按注册顺序返回第一个
 * 匹配）只认第一条命中的规则，在 cfg_init 已经注册过之后本函数若再登记
 * 同名规则，会成为占用 RULES_MAX 配额、却永远不会被命中的死代码——
 * 比"边界改紧了但没生效"更糟：维护者会误以为改这里能收紧边界，改了却
 * 发现毫无效果，白排查。
 *
 * 因此本函数当前**不注册任何规则**；保留函数本身（供 console_api_init
 * 调用、供测试直接调用、供未来出现 console 独有且 core 未覆盖的键时
 * 使用），只做一次 profile 就绪的防御性检查。"配置校验的上下界从 profile
 * 动态生成"这条需求由 core/config.c 的 seed_channel 实际满足，console
 * 端点（cfg_apply_json/cfg_dump_json）直接复用其结果——这正是"配置统一走
 * core/config，不重写校验"的字面含义，而不是在这里重复一遍。
 */
hal_err_t console_api_register_rules(void)
{
    static const char *const module_keys[] = {
        "module.idp.enabled", "module.rtsp.enabled", "module.onvif.enabled",
        "module.gb28181.enabled", "module.recorder.enabled", "module.ota.enabled",
        "module.ivs.enabled", "module.snapshot.enabled"
    };
    size_t i;

    if (!profile_get()) return HAL_ESTATE;   /* profile 未加载：不应发生，纯防御 */

    /* 模块级运行时开关：在 profile 允许范围内由产线/售后本机启停。
     * 显式列出键名（而非 module.*.enabled 通配），避免误开未知模块键。
     * reboot_required=true：协议栈启停涉及端口/线程，默认重启生效（R1 策略）。
     * 允许重复登记：rule_for 命中首条即可；cfg 重新 init 后规则表清空，
     * 必须能再次登记（测试里会多次 cfg_init）。 */
    for (i = 0; i < sizeof(module_keys) / sizeof(module_keys[0]); i++) {
        cfg_rule_t r;
        memset(&r, 0, sizeof(r));
        r.key_pattern = module_keys[i];
        r.type = CFG_T_BOOL;
        r.min = 0; r.max = 1;
        r.reboot_required = true;
        if (cfg_register_rules(&r, 1) != HAL_OK) return HAL_ENOMEM;
    }
    return HAL_OK;
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

/** 运行时配置布尔键：未写入时用默认值（cfg_get_bool 对未设置键返回 ENODEV） */
static bool cfg_flag(const char *key, bool def)
{
    bool v = def;
    if (cfg_get_bool(key, &v) != HAL_OK) return def;
    return v;
}

static bool cap_audio_in(const profile_t *p)
{
    return p && (p->audio_in_mic || p->audio_in_line);
}

static bool cap_ivs(const profile_t *p, uint32_t kind)
{
    return p && (p->ivs_kinds_mask & (1u << kind)) != 0;
}

static bool cap_ivs_smart(const profile_t *p)
{
    return cap_ivs(p, HAL_IVS_HUMANOID) || cap_ivs(p, HAL_IVS_INTRUSION) || cap_ivs(p, HAL_IVS_LINECROSS);
}

static bool mod_up(const char *name) { return module_state(name) != MOD_STATE_UNLOADED; }

/** 视频真在出流（通道 0 已 set_encoder/start）。GK 视频 HAL 未落地时恒 false。 */
/**
 * 视频是否**可用**（能力，而不是"此刻是否正在出流"）。
 *
 * 判据必须是 HAL 的函数指针是否齐全，**不能**用"get_encoder 能否返回参数"：
 * 编码器只在真正有预览消费者时才 open/start（没人看就不出流，省电是刻意的），
 * 空闲态下 get_encoder 返回 HAL_ESTATE 是正确行为——若拿它当能力判据，预览
 * 入口会在空闲时消失，用户永远进不去（自锁）。真出不出得了图，由预览连接
 * 自己发现（收不到帧就提示），而不是靠能力位替它预判。
 */
static bool video_live(void)
{
    return hal_has(HAL_MOD_VIDEO) && hal()->video &&
           hal()->video->open && hal()->video->start &&
           hal()->video->get_frame && hal()->video->release_frame;
}

/** 图像调节能力：按 HAL 是否**实现**接口判断，不按「当前能否读到值」。
 * @note 与 video_live() 同一个坑：若用 get_image()==HAL_OK 当判据，设备空闲时
 * （视频尚未打开）能力恒为 false，图像设置模块会被一直隐藏，用户也就永远没
 * 机会打开它。图像参数本身是**配置**，未出流时也有值。 */
static bool image_ok(void)
{
    return hal_has(HAL_MOD_VIDEO) && hal()->video &&
           hal()->video->get_image && hal()->video->set_image;
}

/**
 * 构建能力清单的 json_t 对象，调用者持有并负责 json_free。
 * console_caps_json 与 ep_system_info 共用，避免"序列化→再解析"往返。
 * caps 描述「硬件/编译/型号有什么」；「现在开不开」见 build_features_object。
 * 返回 NULL 表示 profile 未加载或内存不足。
 */
static json_t *build_caps_object(void)
{
    const profile_t *p = profile_get();
    json_t *obj;

    if (!p) return NULL;
    obj = json_new_object();
    if (!obj) return NULL;
    if (json_object_set(obj, "model", json_new_string(p->model)) != 0 ||
        json_object_set(obj, "vendor", json_new_string(p->vendor)) != 0 ||
        json_object_set(obj, "wifi", json_new_bool(cap_wifi(p))) != 0 ||
        json_object_set(obj, "wifi_ap", json_new_bool(cap_wifi_ap())) != 0 ||
        json_object_set(obj, "eth", json_new_bool(p->eth)) != 0 ||
        json_object_set(obj, "tf", json_new_bool(p->tf)) != 0 ||
        json_object_set(obj, "h265", json_new_bool(cap_h265())) != 0 ||
        json_object_set(obj, "playback", json_new_bool(cap_playback())) != 0 ||
        json_object_set(obj, "audio_in", json_new_bool(cap_audio_in(p))) != 0 ||
        json_object_set(obj, "audio_out", json_new_bool(p->audio_out)) != 0 ||
        json_object_set(obj, "audio_talk", json_new_bool(p->audio_out && cap_audio_in(p))) != 0 ||
        json_object_set(obj, "ivs_motion", json_new_bool(cap_ivs(p, HAL_IVS_MOTION))) != 0 ||
        json_object_set(obj, "ivs_tamper", json_new_bool(cap_ivs(p, HAL_IVS_TAMPER))) != 0 ||
        json_object_set(obj, "ivs_smart", json_new_bool(cap_ivs_smart(p))) != 0 ||
        json_object_set(obj, "wdr", json_new_bool(p->isp_wdr)) != 0 ||
        json_object_set(obj, "daynight", json_new_bool(strcmp(p->daynight, "none") != 0)) != 0 ||
        json_object_set(obj, "idp", json_new_bool(p->idp_enabled)) != 0 ||
        json_object_set(obj, "rtsp", json_new_bool(p->rtsp_enabled)) != 0 ||
        json_object_set(obj, "onvif", json_new_bool(p->onvif_enabled)) != 0 ||
        json_object_set(obj, "gb", json_new_bool(p->gb_enabled)) != 0 ||
        json_object_set(obj, "ota_ab", json_new_bool(p->ota_ab)) != 0) {
        json_free(obj);
        return NULL;
    }
    return obj;
}

/**
 * 功能映射（UI 功能层）：feature = 编译包含 ∧ profile 硬件能力 ∧ 运行时配置。
 * 任一为假时前端必须隐藏/禁用对应入口，API 对该功能返回 HAL_ENOTSUP。
 * 键名与 firmware/web 的导航过滤表、开发计划 §8 对齐，新增键须同步前端。
 */
static json_t *build_features_object(void)
{
    const profile_t *p = profile_get();
    json_t *obj;
    bool storage_record, preview_playback, event_smart, event_any, event_motion, event_tamper;
    bool np_idp, np_rtsp, np_onvif, np_gb, live, img, ivs, audio;

    if (!p) return NULL;
    obj = json_new_object();
    if (!obj) return NULL;

    /* 功能 = profile 有 ∧ 固件实现能跑通；只看 profile 会把未落地的模块报成可用 */
    live = video_live();
    img = p->channel_count > 0 && image_ok();
    ivs = hal_has(HAL_MOD_IVS);
    audio = hal_has(HAL_MOD_AUDIO);
    event_motion = ivs && cap_ivs(p, HAL_IVS_MOTION);
    event_tamper = ivs && cap_ivs(p, HAL_IVS_TAMPER);
    event_smart = ivs && cap_ivs_smart(p);
    event_any = event_motion || event_tamper || event_smart;
    storage_record = p->tf && cfg_flag("record.enabled", true) && mod_up("recorder");
#ifdef IPC_CONSOLE_PLAYBACK
    preview_playback = p->tf && mod_up("recorder") && live;
#else
    preview_playback = false;
#endif
    np_idp = p->idp_enabled && mod_up("idp");
    np_rtsp = p->rtsp_enabled && mod_up("rtsp");
    np_onvif = p->onvif_enabled && mod_up("onvif");
    np_gb = p->gb_enabled && mod_up("gb28181");

    if (json_object_set(obj, "preview.live", json_new_bool(live)) != 0 ||
        json_object_set(obj, "preview.playback", json_new_bool(preview_playback)) != 0 ||
        json_object_set(obj, "tools.download", json_new_bool((p->tf || cap_ivs(p, HAL_IVS_MOTION)) && live)) != 0 ||
        json_object_set(obj, "image.basic", json_new_bool(img)) != 0 ||
        json_object_set(obj, "image.daynight", json_new_bool(img && strcmp(p->daynight, "none") != 0)) != 0 ||
        json_object_set(obj, "image.wdr", json_new_bool(img && p->isp_wdr)) != 0 ||
        json_object_set(obj, "event.motion", json_new_bool(event_motion)) != 0 ||
        json_object_set(obj, "event.tamper", json_new_bool(event_tamper)) != 0 ||
        json_object_set(obj, "event.smart", json_new_bool(event_smart)) != 0 ||
        json_object_set(obj, "event.any", json_new_bool(event_any)) != 0 ||
        json_object_set(obj, "event.alarm", json_new_bool(event_any)) != 0 ||
        json_object_set(obj, "storage.tf", json_new_bool(p->tf)) != 0 ||
        json_object_set(obj, "storage.record", json_new_bool(storage_record)) != 0 ||
        json_object_set(obj, "storage.manage", json_new_bool(p->tf)) != 0 ||
        json_object_set(obj, "storage.format", json_new_bool(false)) != 0 ||
        json_object_set(obj, "network.eth", json_new_bool(p->eth)) != 0 ||
        json_object_set(obj, "network.config", json_new_bool(p->eth)) != 0 ||
        /* 端口页已落地（port.http/port.rtsp + POST /system/port/apply），如实上报 */
        json_object_set(obj, "network.ports", json_new_bool(p->eth)) != 0 ||
        json_object_set(obj, "network.wifi", json_new_bool(cap_wifi(p))) != 0 ||
        json_object_set(obj, "network.wifi_ap", json_new_bool(cap_wifi_ap())) != 0 ||
        json_object_set(obj, "netplatform.idp", json_new_bool(np_idp)) != 0 ||
        json_object_set(obj, "netplatform.rtsp", json_new_bool(np_rtsp)) != 0 ||
        json_object_set(obj, "netplatform.onvif", json_new_bool(np_onvif)) != 0 ||
        json_object_set(obj, "netplatform.gb", json_new_bool(np_gb)) != 0 ||
        json_object_set(obj, "netplatform.any", json_new_bool(np_idp || np_rtsp || np_onvif || np_gb)) != 0 ||
        json_object_set(obj, "audio.in", json_new_bool(audio && cap_audio_in(p))) != 0 ||
        json_object_set(obj, "audio.out", json_new_bool(audio && p->audio_out)) != 0 ||
        json_object_set(obj, "system.ota", json_new_bool(mod_up("ota"))) != 0 ||
        json_object_set(obj, "system.users", json_new_bool(true)) != 0 ||
        json_object_set(obj, "system.time", json_new_bool(true)) != 0 ||
        json_object_set(obj, "system.log", json_new_bool(true)) != 0 ||
        json_object_set(obj, "system.device", json_new_bool(true)) != 0 ||
        /* 配置导入导出（cfg_dump_json/PUT /config 已具备）与定时重启
           （system.reboot.plan.* + console_maint 的调度）本轮已实现，如实上报 */
        json_object_set(obj, "system.cfgfile", json_new_bool(true)) != 0 ||
        json_object_set(obj, "system.reboot_plan", json_new_bool(true)) != 0 ||
        json_object_set(obj, "system.diag", json_new_bool(true)) != 0 ||
        json_object_set(obj, "module.admin", json_new_bool(true)) != 0 ||
        json_object_set(obj, "cloud.bind", json_new_bool(np_idp)) != 0) {
        json_free(obj);
        return NULL;
    }
    return obj;
}

/**
 * 固件模块目录（产品层元数据，与 module.h 的 mod_* 声明对应）。
 * 未注册的模块也出现在管理页：说明「SKU 是否允许 / 配置是否打开」，
 * 而不是等模块落地后才看得到开关。state 仍以 module_loader 为准。
 */
typedef struct {
    const char *name;
    const char *title;
    const char *cfg_key;     /**< NULL = 固件必备，不可开关 */
    const char *depends;     /**< 依赖模块名，无则 NULL */
    bool        reboot_required;
} module_meta_t;

static const module_meta_t MODULE_CATALOG[] = {
    { "console",   "本地控制台",   NULL,                     NULL,   false },
    { "idp",       "IDP 云接入",   "module.idp.enabled",     NULL,   true  },
    { "rtsp",      "RTSP 服务",    "module.rtsp.enabled",    NULL,   true  },
    { "onvif",     "ONVIF 服务",   "module.onvif.enabled",   "rtsp", true  },
    { "gb28181",   "GB28181 接入", "module.gb28181.enabled", NULL,   true  },
    { "recorder",  "本地录像",     "module.recorder.enabled",NULL,   true  },
    { "ota",       "固件升级",     "module.ota.enabled",     NULL,   true  },
    { "ivs",       "智能分析",     "module.ivs.enabled",     NULL,   true  },
    { "snapshot",  "抓图服务",     "module.snapshot.enabled",NULL,   true  },
};

static bool profile_allows_module(const profile_t *p, const char *name)
{
    if (!p) return false;
    if (strcmp(name, "console") == 0) return true;
    if (strcmp(name, "idp") == 0)       return p->idp_enabled;
    if (strcmp(name, "rtsp") == 0)      return p->rtsp_enabled;
    if (strcmp(name, "onvif") == 0)     return p->onvif_enabled;
    if (strcmp(name, "gb28181") == 0)   return p->gb_enabled;
    if (strcmp(name, "recorder") == 0)  return p->tf;
    if (strcmp(name, "ivs") == 0)       return strcmp(p->ivs_engine, "none") != 0;
    if (strcmp(name, "ota") == 0)       return true;
    if (strcmp(name, "snapshot") == 0)  return p->channel_count > 0;
    return false;
}

static const char *module_state_str(module_state_t state)
{
    switch (state) {
    case MOD_STATE_INITED:  return "inited";
    case MOD_STATE_RUNNING: return "running";
    case MOD_STATE_STOPPED: return "stopped";
    case MOD_STATE_FAILED:  return "failed";
    default:                return "unloaded";
    }
}

/**
 * 模块清单：目录全量（含未注册）+ 运行态 + profile/config/effective 三层判定。
 * source：always / profile / config —— 以「谁能让它关掉」为准。
 */
static json_t *build_modules_array(void)
{
    const profile_t *p = profile_get();
    json_t *arr;
    size_t i;

    if (!p) return NULL;
    arr = json_new_array();
    if (!arr) return NULL;

    for (i = 0; i < sizeof(MODULE_CATALOG) / sizeof(MODULE_CATALOG[0]); i++) {
        const module_meta_t *meta = &MODULE_CATALOG[i];
        bool profile_ok = profile_allows_module(p, meta->name);
        bool config_on = true;
        module_state_t st = module_state(meta->name);
        bool registered = (st != MOD_STATE_UNLOADED);
        bool effective;
        const char *source;
        json_t *m;
        if (meta->cfg_key) {
            bool v = true;
            if (cfg_get_bool(meta->cfg_key, &v) == HAL_OK) config_on = v;
        }
        /* 依赖未满足时 effective 为假（管理页展示影响面） */
        effective = profile_ok && config_on;
        if (effective && meta->depends) {
            bool dep_cfg = true;
            const char *dep_key = NULL;
            size_t k;
            for (k = 0; k < sizeof(MODULE_CATALOG) / sizeof(MODULE_CATALOG[0]); k++) {
                if (strcmp(MODULE_CATALOG[k].name, meta->depends) == 0) {
                    dep_key = MODULE_CATALOG[k].cfg_key;
                    if (!profile_allows_module(p, meta->depends)) effective = false;
                    break;
                }
            }
            if (effective && dep_key && cfg_get_bool(dep_key, &dep_cfg) == HAL_OK && !dep_cfg)
                effective = false;
        }

        if (!meta->cfg_key) source = "always";
        else if (!profile_ok) source = "profile";
        else if (!config_on) source = "config";
        else source = "profile";

        m = json_new_object();
        if (!m) continue;
        json_object_set(m, "name", json_new_string(meta->name));
        json_object_set(m, "title", json_new_string(meta->title));
        json_object_set(m, "state", json_new_string(module_state_str(st)));
        json_object_set(m, "registered", json_new_bool(registered));
        json_object_set(m, "profile_ok", json_new_bool(profile_ok));
        json_object_set(m, "config_enabled", json_new_bool(config_on));
        json_object_set(m, "user_enabled", json_new_bool(effective));
        json_object_set(m, "effective", json_new_bool(effective));
        /* 只有固件里真正注册了的模块，开关才会被读取生效；未落地的模块开关是假开关 */
        json_object_set(m, "toggleable", json_new_bool(meta->cfg_key != NULL && profile_ok && registered));
        json_object_set(m, "reboot_required", json_new_bool(meta->reboot_required));
        json_object_set(m, "source", json_new_string(source));
        if (meta->cfg_key) json_object_set(m, "cfg_key", json_new_string(meta->cfg_key));
        if (meta->depends) json_object_set(m, "depends", json_new_string(meta->depends));
        json_array_push(arr, m);
    }
    return arr;
}

/**
 * 功能明细（能力管理页）：在 features 布尔之外补充来源与中文名。
 * source：hardware（profile/HAL）/ compile（裁剪宏）/ config（运行时开关）/ mixed。
 */
static json_t *build_feature_detail_array(void)
{
    const profile_t *p = profile_get();
    json_t *arr;
    bool rec_cfg = true;

    if (!p) return NULL;
    arr = json_new_array();
    if (!arr) return NULL;
    (void)cfg_get_bool("record.enabled", &rec_cfg);

#define FEAT(id, title, on, src, detail) do { \
    json_t *f = json_new_object(); \
    if (!f) break; \
    json_object_set(f, "id", json_new_string(id)); \
    json_object_set(f, "title", json_new_string(title)); \
    json_object_set(f, "enabled", json_new_bool(on)); \
    json_object_set(f, "source", json_new_string(src)); \
    json_object_set(f, "detail", json_new_string(detail)); \
    json_array_push(arr, f); \
} while (0)

    {
    bool live = video_live();
    bool img = p->channel_count > 0 && image_ok();
    bool ivs = hal_has(HAL_MOD_IVS);
    bool audio = hal_has(HAL_MOD_AUDIO);
    bool ev_any = ivs && (cap_ivs(p, HAL_IVS_MOTION) || cap_ivs(p, HAL_IVS_TAMPER) || cap_ivs_smart(p));

    FEAT("preview.live", "实时预览", live, "mixed", "依赖视频模块出流");
    FEAT("preview.playback", "本地回放",
         cap_playback() && p->tf && live, "mixed",
         cap_playback() ? "依赖 TF 卡、recorder 模块与视频出流" : "固件未编入回放代码路径");
    FEAT("tools.download", "图片下载", (p->tf || cap_ivs(p, HAL_IVS_MOTION)) && live, "mixed",
         "依赖视频出流与 TF 或移动侦测抓图");
    FEAT("image.basic", "图像调节", img, "mixed", "依赖视频通道与 ISP 图像接口");
    FEAT("image.daynight", "日夜切换", img && strcmp(p->daynight, "none") != 0, "mixed",
         p->daynight[0] ? p->daynight : "none");
    FEAT("image.wdr", "宽动态", img && p->isp_wdr, "mixed", "ISP 能力");
    FEAT("event.motion", "移动侦测", ivs && cap_ivs(p, HAL_IVS_MOTION), "mixed", "IVS 能力位 + IVS 模块");
    FEAT("event.tamper", "视频遮挡", ivs && cap_ivs(p, HAL_IVS_TAMPER), "mixed", "IVS 能力位 + IVS 模块");
    FEAT("event.smart", "智能检测", ivs && cap_ivs_smart(p), "mixed", "人形/入侵/越界任一 + IVS 模块");
    FEAT("event.alarm", "报警设备", ev_any, "mixed", "依赖任一侦测事件");
    FEAT("storage.tf", "存储卡", p->tf, "hardware", "profile.storage.tf");
    FEAT("storage.record", "本地录像", p->tf && rec_cfg && mod_up("recorder"), "mixed",
         rec_cfg ? "硬件 TF + recorder 模块" : "record.enabled 已关闭");
    FEAT("storage.manage", "存储管理", p->tf, "hardware", "profile.storage.tf");
    FEAT("storage.format", "存储卡格式化", false, "always", "本期未实现");
    FEAT("network.eth", "以太网", p->eth, "hardware", "profile.network.eth");
    FEAT("network.config", "网络设置", p->eth, "hardware", "有线 DHCP/静态地址");
    FEAT("network.ports", "端口", p->eth, "hardware",
         "port.http/port.rtsp，保存即重绑监听（POST /system/port/apply）");
    FEAT("network.wifi", "WiFi", cap_wifi(p), "hardware", "需 WiFi 模组与 HAL");
    FEAT("network.wifi_ap", "AP 配网", cap_wifi_ap(), "hardware", "需 AP 能力");
    FEAT("netplatform.idp", "IDP 接入", p->idp_enabled && mod_up("idp"), "mixed", "protocols.idp + idp 模块");
    FEAT("netplatform.rtsp", "RTSP 接入", p->rtsp_enabled && mod_up("rtsp"), "mixed", "protocols.rtsp + rtsp 模块");
    FEAT("netplatform.onvif", "ONVIF 接入", p->onvif_enabled && mod_up("onvif"), "mixed", "protocols.onvif + onvif 模块");
    FEAT("netplatform.gb", "GB28181 接入", p->gb_enabled && mod_up("gb28181"), "mixed", "protocols.gb28181 + gb28181 模块");
    FEAT("audio.in", "音频输入", audio && cap_audio_in(p), "mixed", "拾音/线性输入 + 音频模块");
    FEAT("audio.out", "音频输出", audio && p->audio_out, "mixed", "扬声器/线性输出 + 音频模块");
    FEAT("system.ota", "固件升级", mod_up("ota"), "mixed", "依赖 ota 模块");
    FEAT("system.users", "用户管理", true, "always", "本地账号");
    FEAT("system.time", "时间设置", true, "always", "NTP / 手动校时");
    FEAT("system.log", "系统日志", true, "always", "内存环形日志");
    FEAT("system.device", "设备名称", true, "always", "device.name");
    FEAT("system.cfgfile", "配置导入导出", true, "always", "cfg_dump_json / PUT /config");
    FEAT("system.reboot_plan", "定时重启", true, "always", "system.reboot.plan.*");
    FEAT("system.diag", "网络诊断", true, "always", "Ping / Tracert");
    FEAT("module.admin", "模块管理", true, "always", "本页");
    FEAT("cloud.bind", "云服务绑定", p->idp_enabled && mod_up("idp"), "mixed", "依赖 IDP 模块");
    }

#undef FEAT
    return arr;
}

/**
 * GET /api/v1/system/capabilities：能力管理页数据源。
 * 响应：{"code":0,"features":[{id,title,enabled,source,detail}...],
 *        "modules":[{name,title,state,registered,profile_ok,config_enabled,
 *                    effective,toggleable,reboot_required,source,cfg_key,depends}...]}
 */
static hal_err_t ep_system_capabilities(char *out, size_t out_cap)
{
    json_t *root, *feats, *mods;
    char *txt;
    hal_err_t rc;

    if (!profile_get()) return HAL_ESTATE;
    feats = build_feature_detail_array();
    mods = build_modules_array();
    if (!feats || !mods) {
        json_free(feats);
        json_free(mods);
        return HAL_ENOMEM;
    }
    root = json_new_object();
    if (!root) {
        json_free(feats);
        json_free(mods);
        return HAL_ENOMEM;
    }
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "features", feats);
    json_object_set(root, "modules", mods);
    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

hal_err_t console_caps_json(char *buf, size_t cap)
{
    json_t *obj;
    char *txt;

    if (!buf || cap == 0) return HAL_EINVAL;
    if (!profile_get()) return HAL_ESTATE;

    obj = build_caps_object();
    if (!obj) return HAL_ENOMEM;

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
    /* 必须清零：core/config.c 的 validate() 只拒绝 strlen(key) >= CFG_KEY_MAX
       （96），一个恰好 95 字符的键会被 strncpy(dst, src, 95) 拷满 0..94
       且不补 NUL——rejects[i].key[95] 与整条 reason 都可能停留在未初始化
       状态，被下面的 json_new_string 当字符串处理时读出栈残留、甚至越读进
       同一数组里下一个元素。与本任务较早前修的 http_query 用法 bug
       同一类问题（http_server.c 头注释明确禁止"栈残留"）。 */
    cfg_reject_t rejects[8] = {0};
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
    /* rejected_total 是准确的被拒总数（cfg_apply_json 对每个被拒键都计数，
       与是否还有空位记录详情无关）；rejected[] 数组本身受限于上面固定的
       容量 8，超过 8 条时静默截断——没有 rejected_total，前端按
       rejected.length 展示会少报"到底拒了多少个"。 */
    json_object_set(root, "rejected_total", json_new_int(rejected));
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

/** 读安全存储里的短文本键（出厂烧录的 DeviceID/SN/验证码）：
 *  没有该模块/没这个键/读失败 → 返回 false 且 out 为空串（调用方显示「未烧录」，
 *  **不编造、不回落到派生值**）。读到后统一剥尾部换行与空白。 */
static bool read_sec_str(const char *key, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    if (!hal_has(HAL_MOD_CRYPTO) || !hal()->crypto->secure_read) return false;
    if (hal()->crypto->secure_read(key, (uint8_t *)out, cap - 1, &len) != HAL_OK) {
        out[0] = '\0';
        return false;
    }
    if (len >= cap) len = cap - 1;
    out[len] = '\0';
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                       out[len - 1] == ' '  || out[len - 1] == '\t')) out[--len] = '\0';
    return out[0] != '\0';
}

/** 设备序列号（唯一身份，即序列码）：安全存储里的 17 位 DeviceID；未烧录时留空。 */
static void device_serial(char *out, size_t cap)
{
    if (!read_sec_str(HAL_SEC_KEY_DEVICE_ID, out, cap)) out[0] = '\0';
}

/**
 * GET /api/v1/system/info：型号/序列号/固件版本/运行时长 + caps/features/modules。
 * 响应：{"code":0,"model":..,"vendor":..,"serial":..,"qr_content":..,
 *        "fw_version":..,"uptime_s":..,"caps":{...},"features":{...},"modules":[...]}
 * serial = 17 位 DeviceID（**唯一身份，同时就是序列码**，单码体系见
 * Docs/PRD/IpcCloud设备序列号生成规则_v1.0.md）；未烧录时为空串，前端显示
 * 「未烧录」（不编造）。qr_content 仅在 DeviceID 与验证码都已烧录时给出：
 * IPC1:<DeviceID>:<VerifyCode>:<Model>（接入规范 QR 条目）。
 * 唯一在强制改密期间仍可访问的业务端点（console_auth_check 已豁免），
 * 前端在改密页也要能读到型号与能力清单渲染页面外壳。
 * features/modules 为能力驱动 UI 的权威来源，前端不得另算一套。
 */
static hal_err_t ep_system_info(char *out, size_t out_cap)
{
    const profile_t *p = profile_get();
    hal_sys_stats_t st;
    hal_ota_state_t ota;
    char serial[64], verify[16] = "", qr[160] = "";
    json_t *root, *caps_obj, *feat_obj, *mods_arr;
    char *txt;
    hal_err_t rc;

    if (!p) return HAL_ESTATE;
    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->get_stats || hal()->sys->get_stats(&st) != HAL_OK)
        return HAL_EIO;

    memset(&ota, 0, sizeof(ota));
    if (hal_has(HAL_MOD_SYS) && hal()->sys->ota_get_state) hal()->sys->ota_get_state(&ota);
    device_serial(serial, sizeof(serial));
    read_sec_str(HAL_SEC_KEY_VERIFY_CODE, verify, sizeof(verify));
    if (serial[0] && verify[0])
        snprintf(qr, sizeof(qr), "IPC1:%s:%s:%s", serial, verify, p->model);

    caps_obj = build_caps_object();
    feat_obj = build_features_object();
    mods_arr = build_modules_array();
    if (!caps_obj || !feat_obj || !mods_arr) {
        json_free(caps_obj);
        json_free(feat_obj);
        json_free(mods_arr);
        return HAL_ENOMEM;
    }

    root = json_new_object();
    if (!root) {
        json_free(caps_obj);
        json_free(feat_obj);
        json_free(mods_arr);
        return HAL_ENOMEM;
    }
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "model", json_new_string(p->model));
    json_object_set(root, "vendor", json_new_string(p->vendor));
    {
        char name[64];
        hal_netif_status_t ns;
        char mac[18] = "";
        if (cfg_get_str("device.name", name, sizeof(name)) != HAL_OK || !name[0])
            snprintf(name, sizeof(name), "%s", p->model);
        json_object_set(root, "device_name", json_new_string(name));
        memset(&ns, 0, sizeof(ns));
        if (hal_has(HAL_MOD_NET) && hal()->net->get_status &&
            hal()->net->get_status(HAL_NETIF_ETH, &ns) == HAL_OK) {
            snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                     ns.mac[0], ns.mac[1], ns.mac[2], ns.mac[3], ns.mac[4], ns.mac[5]);
        }
        json_object_set(root, "mac", json_new_string(mac));
        json_object_set(root, "ip", json_new_string(ns.ip));
    }
    json_object_set(root, "serial", json_new_string(serial));
    if (qr[0]) json_object_set(root, "qr_content", json_new_string(qr));
    json_object_set(root, "fw_version", json_new_string(ota.current_version));
    json_object_set(root, "uptime_s", json_new_int((int64_t)st.uptime_s));
    json_object_set(root, "caps", caps_obj);       /* 接管所有权 */
    json_object_set(root, "features", feat_obj);   /* 接管所有权 */
    json_object_set(root, "modules", mods_arr);    /* 接管所有权 */

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
    if (st.temp_milli_c != INT32_MIN)   /* HAL 约定：无传感器填 INT32_MIN，此时省略字段 */
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

#define CONSOLE_LOG_DUMP_MAX   (64 * 1024)
#define CONSOLE_LOG_LINES_DEF  200
#define CONSOLE_LOG_LINES_MAX  1000

/**
 * GET /api/v1/system/log?lines=N：内存环形日志的最后 N 行（默认 200，上限 1000）。
 * 响应：{"code":0,"total":<返回行数>,"lines":["...", ...]}（旧→新）。
 * 响应体受 out_cap 限制：从最新一行往回取，按 JSON 转义最坏情况估算
 * （控制字符转成 6 字节的 \u00XX），放不下就停——保留最新的行，丢最旧的。
 */
static hal_err_t ep_system_log(const http_req_t *req, char *out, size_t out_cap)
{
    char qbuf[16];
    const char *q = http_query(req, "lines", qbuf, sizeof(qbuf), "");
    long want = q[0] ? strtol(q, NULL, 10) : CONSOLE_LOG_LINES_DEF;
    size_t budget = out_cap > 256 ? out_cap - 256 : 0;   /* 预留外层字段 */
    size_t n, used = 0;
    char *dump, *start, *end, *p, *txt;
    long count = 0;
    json_t *root, *arr;
    hal_err_t rc;

    if (want <= 0) want = CONSOLE_LOG_LINES_DEF;
    if (want > CONSOLE_LOG_LINES_MAX) want = CONSOLE_LOG_LINES_MAX;

    dump = (char *)malloc(CONSOLE_LOG_DUMP_MAX);
    if (!dump) return HAL_ENOMEM;
    n = log_ring_dump(dump, CONSOLE_LOG_DUMP_MAX);
    if (n >= CONSOLE_LOG_DUMP_MAX) n = CONSOLE_LOG_DUMP_MAX - 1;
    dump[n] = '\0';

    /* 单趟从尾部往回：每行累计最坏转义长度，行数或预算到限即停（线性时间） */
    end = dump + n;
    while (end > dump && end[-1] == '\n') end--;
    *end = '\0';
    start = end;
    while (start > dump && count < want) {
        char *ls = start;
        size_t cost = 4;                                   /* 引号 + 逗号 */
        if (ls < end) ls--;                                /* 跳到上一行末尾的换行符之前 */
        while (ls > dump && ls[-1] != '\n') ls--;
        for (p = ls; p < end && *p != '\n'; p++)
            cost += ((unsigned char)*p < 0x20) ? 6 : (*p == '"' || *p == '\\') ? 2 : 1;
        if (used + cost > budget) break;
        used += cost;
        count++;
        start = ls;
    }

    root = json_new_object();
    arr = json_new_array();
    if (!root || !arr) { json_free(root); json_free(arr); free(dump); return HAL_ENOMEM; }

    count = 0;
    for (p = start; *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (*p) { json_array_push(arr, json_new_string(p)); count++; }
        if (!nl) break;
        p = nl + 1;
    }
    free(dump);

    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "total", json_new_int(count));
    /*
     * 两个时间锚点：行首是**单调毫秒**，要显示成年月日必须同时知道「此刻的
     * 墙钟」与「此刻的单调钟」，前端按 utc −(mono_ms − 行ms) 换算。设备无
     * RTC，未校时（NTP 还没同步）时 utc 仍停在 1970，前端据此显示 "--"，
     * 不硬算一个看起来像真的假时间。
     */
    {
        int64_t utc = 0;
        if (hal_has(HAL_MOD_SYS) && hal()->sys->get_wallclock &&
            hal()->sys->get_wallclock(&utc) == HAL_OK)
            json_object_set(root, "utc", json_new_int(utc));
        json_object_set(root, "mono_ms", json_new_int((int64_t)(os_monotonic_us() / 1000ULL)));
    }
    json_object_set(root, "lines", arr);
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
       http_respond_json 失败导致不登记动作时出现悬空的堆块需要回收。

       契约豁免（如实记录）：http_server.h 对 http_conn_defer_after_flush
       的回调契约第 2 条明令"不可阻塞——不做磁盘 I/O"，而 factory_reset
       在真实平台上要擦写 flash，是彻头彻尾的阻塞磁盘操作，字面上违反了
       这条契约。这里刻意豁免：紧跟着的 reboot() 让整个进程/事件循环
       随后立即终止，阻塞事件循环不再有"后续请求排队等待"这个代价——
       契约要保护的东西（不让其他连接被饿死）在"即将重启"这个前提下不
       成立。仍然遵守第 1、3 条（同步、不重入本连接）。这是本模块唯一
       用到这条豁免的回调；新增"以终态收尾"的回调前，请先确认是否真的
       满足"随后立即重启/关机、不再服务任何请求"这个前提。 */
    bool keep_network = (bool)(intptr_t)arg;
    /* 清配置与凭据放在这里（响应已送出、紧接着重启），而不是 handler 里：
       响应没送达、延后动作被放弃时设备保持原状，不会出现"已回到未激活、
       却没重启"的窗口——那个窗口里局域网内任何人都能抢先激活设备 */
    cfg_reset(NULL, 0);
    if (console_auth_wipe() != HAL_OK)
        LOGE(MOD, "恢复出厂：凭据清除失败，设备重启后仍保留原密码");
    if (!hal_has(HAL_MOD_SYS)) return;
    if (hal()->sys->factory_reset) hal()->sys->factory_reset(keep_network);
    if (hal()->sys->reboot) hal()->sys->reboot();
}

/** NTP 服务器名只允许主机名/IPv4 字符，避免拼进平台命令时被注入 */
static bool ntp_host_ok(const char *s)
{
    size_t n = 0;
    for (; *s; s++, n++) {
        char c = *s;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '.' || c == '-')) return false;
    }
    return n > 0 && n <= CONSOLE_NTP_HOST_MAX;
}

/**
 * GET /api/v1/system/time →
 * {"code":0,"utc":秒,"timezone":"CST-8","ntp_enable":bool,"ntp_server":"..","ntp_synced":bool}
 * ntp_synced：平台能否确认 NTP 已完成同步（不支持查询时恒 false，前端据此显示"同步中"）。
 */
static hal_err_t ep_time_get(char *out, size_t out_cap)
{
    int64_t utc = 0;
    bool en = false, synced = false;
    char server[CONSOLE_NTP_HOST_MAX + 1] = "", tz[64] = "";
    json_t *root;
    char *txt;
    hal_err_t rc;

    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->get_wallclock) return HAL_ENOTSUP;
    if (hal()->sys->get_wallclock(&utc) != HAL_OK) return HAL_EIO;
    /* 与 console_apply_time 同源读取：cfg 未写入时报「默认开启 + 缺省服务器」，
       否则页面会显示 NTP 关闭、而设备其实正按默认值在跑 ntpd（或反过来）。 */
    console_ntp_read(&en, server, sizeof(server));
    if (cfg_get_str("time.timezone", tz, sizeof(tz)) != HAL_OK || !tz[0])
        snprintf(tz, sizeof(tz), CONSOLE_TZ_DEFAULT);   /* 与 console_maint 的定时重启同一缺省 */
    if (en && hal()->sys->ntp_synced) synced = hal()->sys->ntp_synced();

    /* 用 json_* 构造：配置里的字符串不能直接拼进 JSON */
    root = json_new_object();
    if (!root) return HAL_ENOMEM;
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "utc", json_new_int(utc));
    json_object_set(root, "timezone", json_new_string(tz));
    json_object_set(root, "ntp_enable", json_new_bool(en));
    json_object_set(root, "ntp_server", json_new_string(server));
    json_object_set(root, "ntp_synced", json_new_bool(synced));
    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/**
 * PUT /api/v1/system/time，二选一：
 *   {"ntp_enable":true,"ntp_server":"pool.ntp.org"}  → 保存并启动 NTP
 *   {"ntp_enable":false,"utc":1790000000}            → 停 NTP 并手动设墙钟
 */
static hal_err_t ep_time_put(const http_req_t *req, char *out, size_t out_cap)
{
    json_t *j;
    bool en;
    hal_err_t rc = HAL_OK;

    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->apply_ntp || !hal()->sys->set_wallclock) return HAL_ENOTSUP;
    if (!req->body || req->body_len == 0) return HAL_EINVAL;
    j = json_parse(req->body, req->body_len, NULL, 0);
    if (!j || !json_is(j, JSON_OBJECT)) { json_free(j); return HAL_EINVAL; }
    en = json_bool(json_get(j, "ntp_enable"), false);

    if (en) {
        const char *server = json_string(json_get(j, "ntp_server"), "");
        if (!ntp_host_ok(server)) { json_free(j); return HAL_EINVAL; }
        if (cfg_set_str("time.ntp.server", server) != HAL_OK ||
            cfg_set_bool("time.ntp.enable", true) != HAL_OK) rc = HAL_EIO;
    } else {
        const json_t *u = json_get(j, "utc");
        int64_t utc = json_int(u, -1);
        if (!u || utc < 0) { json_free(j); return HAL_EINVAL; }
        if (cfg_set_bool("time.ntp.enable", false) != HAL_OK) rc = HAL_EIO;
        if (rc == HAL_OK && hal()->sys->set_wallclock(utc) != HAL_OK) rc = HAL_EIO;
    }
    json_free(j);
    if (rc != HAL_OK) return rc;
    if (console_apply_time() != HAL_OK) return HAL_EIO;
    /* 开启 NTP 只是启动了同步进程，是否成功要等 ntpd 实际校时；如实说明 */
    return fmt_safe(out, out_cap, en
        ? "{\"code\":0,\"msg\":\"已开启 NTP 自动校时，正在与服务器同步\"}"
        : "{\"code\":0,\"msg\":\"时间设置已生效\"}");
}

static void do_apply_net(void *arg)
{
    (void)arg;
    console_apply_net();
}

/**
 * POST /api/v1/system/net/apply：按已保存的 net.* 应用网络。
 * 应用会改变本机地址，必须先把响应发出去（延后动作），前端据 new_ip 跳转。
 * 响应：{"code":0,"msg":"...","new_ip":"<静态 IP；DHCP 时为空>"}
 */
static void body_str_copy(const json_t *j, const char *key, char *dst, size_t cap)
{
    snprintf(dst, cap, "%s", json_string(json_get(j, key), ""));
}

/**
 * 请求体（可选）：{"dhcp":bool,"ip":"","mask":"","gw":"","dns":""}。
 * 带请求体时先校验、通过后才写入 net.* —— 被拒的配置不得落盘，否则设备与
 * 控制台/平台看到的配置不一致。不带请求体时应用已保存的 net.*。
 */
static hal_err_t ep_net_apply(const http_req_t *req, char *out, size_t out_cap,
                              void (**dfn)(void *), void **darg)
{
    console_net_cfg_t c;
    const char *why;
    hal_err_t rc;

    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->apply_net) return HAL_ENOTSUP;
    if (req->body && req->body_len > 0) {
        json_t *j = json_parse(req->body, req->body_len, NULL, 0);
        bool has_mtu;
        if (!j || !json_is(j, JSON_OBJECT)) { json_free(j); return HAL_EINVAL; }
        memset(&c, 0, sizeof(c));
        c.dhcp = json_bool(json_get(j, "dhcp"), true);
        body_str_copy(j, "ip", c.ip, sizeof(c.ip));
        body_str_copy(j, "mask", c.mask, sizeof(c.mask));
        body_str_copy(j, "gw", c.gw, sizeof(c.gw));
        body_str_copy(j, "dns", c.dns, sizeof(c.dns));
        /* mtu 可选：不带 = 本次不动 MTU（向后兼容旧调用方），带了才校验与写入 */
        has_mtu = json_get(j, "mtu") != NULL;
        c.mtu = has_mtu ? (int)json_int(json_get(j, "mtu"), -1) : -1;
        json_free(j);
        if (has_mtu && (c.mtu < 576 || c.mtu > 1500)) {
            fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"MTU 范围应为 576-1500\"}", (int)HAL_EINVAL);
            return HAL_EINVAL;
        }
    } else {
        console_net_read(&c);
    }
    why = console_net_check(&c);
    if (why) {
        LOGW(MOD, "拒绝应用网络设置：%s", why);
        fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"%s\"}", (int)HAL_EINVAL, why);
        return HAL_EINVAL;
    }
    if (req->body && req->body_len > 0) {
        console_net_cfg_t cur;
        console_net_read(&cur);
        /* 与已保存配置相同：不重启网络（否则无谓断网，DHCP 下还可能换地址）。
         * mtu 未随本次提交（-1）或与已保存值一致，都算“没变”。 */
        if (cur.dhcp == c.dhcp && (c.mtu == -1 || c.mtu == cur.mtu) &&
            (c.dhcp || (strcmp(cur.ip, c.ip) == 0 && strcmp(cur.mask, c.mask) == 0 &&
                        strcmp(cur.gw, c.gw) == 0 && strcmp(cur.dns, c.dns) == 0)))
            return fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"网络设置未变化\",\"new_ip\":\"%s\",\"unchanged\":true}",
                            c.dhcp ? "" : c.ip);
        if (cfg_set_bool("net.dhcp", c.dhcp) != HAL_OK) return HAL_EIO;
        if (!c.dhcp &&
            (cfg_set_str("net.ip", c.ip) != HAL_OK || cfg_set_str("net.mask", c.mask) != HAL_OK ||
             cfg_set_str("net.gw", c.gw) != HAL_OK || cfg_set_str("net.dns", c.dns) != HAL_OK))
            return HAL_EIO;
        if (c.mtu != -1 && cfg_set_int("net.mtu", c.mtu) != HAL_OK) return HAL_EIO;
    }
    rc = fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"网络设置即将生效\",\"new_ip\":\"%s\"}",
                  c.dhcp ? "" : c.ip);
    if (rc != HAL_OK) return rc;
    *dfn = do_apply_net;
    *darg = NULL;
    return HAL_OK;
}

/* ---- 端口（PRD LC-NET-02）：HTTP 保存后立即重绑监听，RTSP 只落盘 ---- */

#define CONSOLE_HTTP_DEFAULT_PORT 8080   /**< 与 app/main.c 的 DEFAULT_PORT 一致 */
#define CONSOLE_RTSP_DEFAULT_PORT 554    /**< 与 profile protocols.rtsp.port 默认一致 */

/* do_apply_port 要用的切换参数：端口是人手改的，同时只可能有一次在途切换 */
static int s_port_new, s_port_old;

/** 响应发出后（after_flush 回调，http_server 线程内）把重绑任务投给维护线程 */
static void do_apply_port(void *arg)
{
    (void)arg;
    if (console_maint_post_port(s_port_new, s_port_old) != HAL_OK)
        LOGE(MOD, "端口切换任务投递失败：仍监听旧端口 %d", s_port_old);
}

/**
 * POST /api/v1/system/port/apply：保存 HTTP/RTSP 端口（对齐实机「网络设置→端口」）。
 * 请求体 {"http":int,"rtsp":int}（均需 1-65535）。
 *  - 先做**占用预检**，把「端口被占用」挡在配置落盘之前；
 *  - HTTP 端口变化 → 响应带 new_port，*dfn 在响应发出后投递重绑（维护线程
 *    stop+start，失败回滚旧端口）；前端按 new_port 倒计时跳转；
 *  - RTSP 只落盘：固件暂无 RTSP 服务，值供 RTSP 模块/平台后续使用。
 * 响应：{"code":0,"msg":..,"new_port":N[,"unchanged":true]}
 */
static hal_err_t ep_port_apply(const http_req_t *req, char *out, size_t out_cap,
                               void (**dfn)(void *), void **darg)
{
    json_t *j;
    int64_t hv, rv, v;
    int cur_http, cur_rtsp;

    (void)darg;
    if (!req->body || req->body_len <= 0) {
        fmt_safe(out, out_cap, "{\"code\":-1,\"msg\":\"缺少请求体\"}");
        return HAL_EINVAL;
    }
    j = json_parse(req->body, req->body_len, NULL, 0);
    if (!j || !json_is(j, JSON_OBJECT)) {
        json_free(j);
        fmt_safe(out, out_cap, "{\"code\":-1,\"msg\":\"请求体不是合法 JSON\"}");
        return HAL_EINVAL;
    }
    hv = json_int(json_get(j, "http"), -1);
    rv = json_int(json_get(j, "rtsp"), -1);
    json_free(j);
    if (hv < 1 || hv > 65535) {
        fmt_safe(out, out_cap, "{\"code\":-1,\"msg\":\"HTTP 端口需为 1-65535\"}");
        return HAL_EINVAL;
    }
    if (rv < 1 || rv > 65535) {
        fmt_safe(out, out_cap, "{\"code\":-1,\"msg\":\"RTSP 端口需为 1-65535\"}");
        return HAL_EINVAL;
    }

    /* 当前值：HTTP 优先取**正在监听的端口**（开机 --port 可覆盖 cfg）；服务
       未运行（单测桩）时退而取已保存配置，缺省 8080。RTSP 没有服务在跑，
       以已保存配置为准（未写入按默认 554 比较）。 */
    cur_http = (int)http_server_port();
    if (cur_http <= 0)
        cur_http = (cfg_get_int("port.http", &v) == HAL_OK) ? (int)v : CONSOLE_HTTP_DEFAULT_PORT;
    cur_rtsp = (cfg_get_int("port.rtsp", &v) == HAL_OK) ? (int)v : CONSOLE_RTSP_DEFAULT_PORT;

    if ((int)hv == cur_http && (int)rv == cur_rtsp)
        return fmt_safe(out, out_cap,
                        "{\"code\":0,\"msg\":\"端口未变化\",\"new_port\":%d,\"unchanged\":true}", cur_http);

    if ((int)hv != cur_http && http_port_available((uint16_t)hv) != HAL_OK) {
        fmt_safe(out, out_cap, "{\"code\":-1,\"msg\":\"HTTP 端口 %d 已被占用\"}", (int)hv);
        return HAL_EINVAL;
    }
    if (cfg_set_int("port.http", hv) != HAL_OK) return HAL_EIO;
    if (cfg_set_int("port.rtsp", rv) != HAL_OK) return HAL_EIO;

    if ((int)hv != cur_http) {
        s_port_new = (int)hv;
        s_port_old = cur_http;
        *dfn = do_apply_port;
        return fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"端口即将生效\",\"new_port\":%d}", (int)hv);
    }
    return fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"端口已保存\",\"new_port\":%d}", (int)hv);
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

    /* 出厂 = 清配置 + 清凭据（回到"未激活"）+ 重启，全部在 do_reset 里、
       响应发出之后执行。平台连凭据都删不了时直接拒绝，不假装会成功。 */
    if (hal_has(HAL_MOD_CRYPTO) && hal()->crypto->secure_read && hal()->crypto->secure_write &&
        !hal()->crypto->secure_delete)
        return HAL_ENOTSUP;

    rc = fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"设备将恢复出厂设置并重启\"}");
    if (rc != HAL_OK) return rc;
    *dfn = do_reset;
    *darg = (void *)(intptr_t)keep_network;
    return HAL_OK;
}

/**
 * POST /api/v1/system/config/reset：对齐实机「配置管理 → 恢复默认值 → 简单恢复」。
 *
 * 语义：把**除网络与管理员账号之外**的参数全部丢弃，回到 profile 默认值。
 * 保留表的两条理由：net.* 不保留 → 恢复完设备就换地址、当前页面直接失联；
 * localUser.* 不保留 → 用户把自己锁在门外（完全恢复本来就要重走激活，那是
 * 另一个端点 /system/reset 的语义，两者刻意并存、不合并成带 scope 的端点——
 * 差别正在"要不要重置账号与凭据"）。
 *
 * 不触发重启：多数键读取时即生效；被标 reboot_required 的键（net.* 五件套 + MTU）
 * 恰好都在保留表里，因此也用不着重启。配置落盘与 PUT /api/v1/config 一样，
 * 属于本模块"handler 内做一次性磁盘 I/O"的既有例外。
 */
static hal_err_t ep_config_reset(char *out, size_t out_cap)
{
    static const char *keep[] = {
        "net.dhcp", "net.ip", "net.mask", "net.gw", "net.dns", "net.mtu",
        "localUser.name", "localUser.password"
    };
    hal_err_t rc = cfg_reset(keep, sizeof(keep) / sizeof(keep[0]));
    if (rc != HAL_OK) return rc;
    LOGI(MOD, "配置管理：简单恢复已执行（网络与管理员账号保留）");
    return fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"已恢复默认参数，网络与管理员账号保持不变\"}");
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

/* ---- 图像参数（PRD 图像调节）：GET 读当前值，POST 部分更新。
 *
 * 字段名与配置键 image.* 的后缀一致，前端可直接与配置页共用一套命名。
 * POST 采用「部分更新」：未出现的字段不动（HAL 契约里 -1 = 不修改），
 * 因为界面上的滑块是逐个提交的，不能要求每次带上全部字段。 */

static void image_fill_json(json_t *o, const hal_image_t *img)
{
    json_object_set(o, "brightness", json_new_int(img->brightness));
    json_object_set(o, "contrast",   json_new_int(img->contrast));
    json_object_set(o, "saturation", json_new_int(img->saturation));
    json_object_set(o, "sharpness",  json_new_int(img->sharpness));
    json_object_set(o, "hue",        json_new_int(img->hue));
    json_object_set(o, "flip",       json_new_int(img->flip));
    json_object_set(o, "mirror",     json_new_int(img->mirror));
    /* 区域补偿：0/1，配置键 image.blc（本页唯一一个真接了后端的"补光设置"类项） */
    json_object_set(o, "blc",        json_new_bool(img->backlight_comp != 0));
}

/** 曝光组/白平衡/补光组的"枚举 ↔ 字符串"映射。
 *
 * 这几个项**没有 HAL 读回入口**（hal_image_t 只有 set_image，没有对应的 get_ext），
 * 所以 GET 一律以 **cfg** 为准——cfg 就是用户选择的持久源头，HAL 只是执行器。
 * 这样做同时保证"保存后重启"回读一致：重启时 image_apply_cfg() 拿同一批 cfg
 * 重新下发（见该函数注释：不回放的话界面会"保存成功但重启回默认"）。 */
static const char *const IMG_EXPO_MODES[] = { "auto", "manual" };
static const char *const IMG_FLICKERS[]   = { "off", "50hz", "60hz" };
static const char *const IMG_AWB_MODES[]  = { "auto", "indoor", "outdoor" };
static const char *const IMG_IR_MODES[]   = { "auto", "off", "on" };
/* 日夜配置三态：与 cfg 键 image.daynight 的字符集、前端 camera.js 的 DN_TO_API
   逐字一致，也必须与实机 dayNightMode 的三项一一对应（日夜通用/日夜定时切换/
   日夜自动切换）。timed 目前只落 cfg —— HAL 只有 day/auto 两态（LEG-UI-13）。 */
static const char *const IMG_DN_MODES[]   = { "common", "timed", "auto" };

/** 按枚举表取 n 元组中的第 idx 个；越界/未知一律回第 0 个（= 默认档），不编造 */
static const char *name_of(const char *const *tab, int n, int idx)
{
    return (idx >= 0 && idx < n) ? tab[idx] : tab[0];
}

/** 在枚举表里找 s 的下标；找不到回 -1 */
static int idx_of(const char *const *tab, int n, const char *s)
{
    for (int i = 0; i < n; i++) if (strcmp(tab[i], s) == 0) return i;
    return -1;
}

static hal_err_t ep_image_get(char *out, size_t out_cap)
{
    hal_image_t img;
    json_t *root;
    char *txt;
    hal_err_t rc;

    if (!image_ok()) return HAL_ENOTSUP;
    if (hal()->video->get_image(&img) != HAL_OK) return HAL_EIO;

    root = json_new_object();
    if (!root) return HAL_ENOMEM;
    json_object_set(root, "code", json_new_int(0));
    image_fill_json(root, &img);
    /* 中性值由后端告知：免得前端把 50 这种魔法数字再写一遍 */
    json_object_set(root, "neutral", json_new_int(50));

    /* 日夜与宽动态跟图像参数同页，一并返回，前端一次请求拿全 */
    {
        bool night = false;
        hal_isp_mode_t isp = HAL_ISP_LINEAR;
        char s[16];
        int m = 2;   /* 出厂默认 auto（日夜自动切换），与 HAL 初值一致 */

        /* daynight 以 **cfg 为准**，不从 HAL 读：三态里的 timed（日夜定时切换）
           在 HAL 里根本没有对应状态，从 HAL 读回来只能得到 auto —— 界面会把
           用户选的「日夜定时切换」显示成「日夜自动切换」，看着就是“没保存”。
           HAL 只用来取“此刻是不是夜视”，供预览上的状态点用。 */
        if (cfg_get_str("image.daynight", s, sizeof(s)) == HAL_OK) {
            int i = idx_of(IMG_DN_MODES, 3, s);
            if (i >= 0) m = i;
        }
        if (hal()->video->get_daynight) {
            hal_daynight_t dn = HAL_DAYNIGHT_AUTO;
            if (hal()->video->get_daynight(&dn, &night) == HAL_OK)
                json_object_set(root, "night_now", json_new_bool(night));
        }
        json_object_set(root, "daynight", json_new_string(IMG_DN_MODES[m]));
        if (hal()->video->get_isp_mode &&
            hal()->video->get_isp_mode(&isp) == HAL_OK)
            json_object_set(root, "wdr", json_new_bool(isp == HAL_ISP_WDR));
    }

    /* 图像页其它项：HAL 无读回入口，以 cfg 为准（见 IMG_EXPO_MODES 注释）。
     * cfg 里没写过的键用**与 HAL 初值一致的默认档**：曝光 auto/0 档、防闪烁 off、
     * 白平衡 auto、补光灯 auto/灵敏度 4/延迟 5。 */
    {
        char s[16];
        int64_t iv;
        int m = 0;

        if (cfg_get_str("image.exposure.mode", s, sizeof(s)) == HAL_OK)
            m = idx_of(IMG_EXPO_MODES, 2, s);
        json_object_set(root, "exposure", json_new_string(name_of(IMG_EXPO_MODES, 2, m < 0 ? 0 : m)));

        json_object_set(root, "exposure_level",
            json_new_int(cfg_get_int("image.exposure.level", &iv) == HAL_OK ? (int)iv : 0));

        m = 0;
        if (cfg_get_str("image.antiflicker", s, sizeof(s)) == HAL_OK)
            m = idx_of(IMG_FLICKERS, 3, s);
        json_object_set(root, "antiflicker", json_new_string(name_of(IMG_FLICKERS, 3, m < 0 ? 0 : m)));

        m = 0;
        if (cfg_get_str("image.awb", s, sizeof(s)) == HAL_OK)
            m = idx_of(IMG_AWB_MODES, 3, s);
        json_object_set(root, "awb", json_new_string(name_of(IMG_AWB_MODES, 3, m < 0 ? 0 : m)));

        m = 0;
        if (cfg_get_str("image.ir.mode", s, sizeof(s)) == HAL_OK)
            m = idx_of(IMG_IR_MODES, 3, s);
        json_object_set(root, "ir_mode", json_new_string(name_of(IMG_IR_MODES, 3, m < 0 ? 0 : m)));

        json_object_set(root, "ir_sensitivity",
            json_new_int(cfg_get_int("image.ir.sensitivity", &iv) == HAL_OK ? (int)iv : 4));
        json_object_set(root, "ir_delay",
            json_new_int(cfg_get_int("image.ir.delay", &iv) == HAL_OK ? (int)iv : 5));
    }

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

static hal_err_t ep_image_set(const http_req_t *req, char *out, size_t out_cap)
{
    json_t *j;
    hal_image_t img;
    hal_err_t r;

    if (!image_ok()) return HAL_ENOTSUP;
    if (!req->body || req->body_len <= 0)
        return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"请求体为空\"}", (int)HAL_EINVAL);

    j = json_parse(req->body, req->body_len, NULL, 0);
    if (!j || !json_is(j, JSON_OBJECT)) { json_free(j); return HAL_EINVAL; }

    /* memset(-1)：本次未带上的字段一律"不改"（HAL 契约） */
    memset(&img, -1, sizeof(img));
#define IMG_TAKE(fld) do { json_t *n = json_get(j, #fld); if (n) img.fld = (int)json_int(n, -1); } while (0)
    IMG_TAKE(brightness); IMG_TAKE(contrast); IMG_TAKE(saturation);
    IMG_TAKE(sharpness);  IMG_TAKE(hue);
    IMG_TAKE(flip);       IMG_TAKE(mirror);
#undef IMG_TAKE
    /* 区域补偿是**布尔**（前端开关发 true/false，cfg 也是 CFG_T_BOOL）：
       不能走 IMG_TAKE 的 json_int——JSON true 会被读成兜底 -1（=不修改），
       表现为"保存成功但值没变"。与下面 wdr 的读法保持一致。 */
    {
        json_t *blc = json_get(j, "blc");
        if (blc) img.backlight_comp = json_bool(blc, false) ? 1 : 0;
    }

    /* 曝光组 / 白平衡 / 补光组：枚举传字符串、数值传整数（同 cfg 键的取值）。
       越界**当场报错**——用户要知道是哪一项超了，不能靠 set_image 的通用 EINVAL。
       `img_*` 临时变量在 set_image 之后写给 cfg（顺序：先下发后落盘，
       与 daynight/wdr 一致，避免"cfg 已改但 HAL 拒了"的不一致态）。

       ⚠️ “未提供”的哨兵必须用 **-999**，不能用 -1：exposure_level 的**合法**
       范围就含 -1/-2/-3（实机 expLevelSel 是 -3..3），用 -1 当哨兵会把
       "把曝光等级改成 -2" 当成"没传这个字段"而静默丢掉（写测试时当场踩到）。 */
    int img_expo_mode = -1, img_expo_lvl = -999, img_flicker = -1;
    int img_awb = -1, img_ir_mode = -1, img_ir_sens = -1, img_ir_delay = -1;
    {
        json_t *n;
        const char *s;

        if ((n = json_get(j, "exposure")) != NULL) {
            s = json_string(n, NULL);
            img_expo_mode = s ? idx_of(IMG_EXPO_MODES, 2, s) : -1;
            if (img_expo_mode < 0) {
                json_free(j);
                return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"曝光模式只支持 auto/manual\"}",
                                (int)HAL_EINVAL);
            }
            img.exposure_mode = img_expo_mode;
        }
        if ((n = json_get(j, "exposure_level")) != NULL) {
            img_expo_lvl = (int)json_int(n, 999);
            if (img_expo_lvl < -3 || img_expo_lvl > 3) {
                json_free(j);
                return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"曝光等级范围应为 -3~3\"}",
                                (int)HAL_EINVAL);
            }
            img.exposure_level = img_expo_lvl;
        }
        if ((n = json_get(j, "antiflicker")) != NULL) {
            s = json_string(n, NULL);
            img_flicker = s ? idx_of(IMG_FLICKERS, 3, s) : -1;
            if (img_flicker < 0) {
                json_free(j);
                return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"防闪烁只支持 off/50hz/60hz\"}",
                                (int)HAL_EINVAL);
            }
            img.antiflicker = img_flicker;
        }
        if ((n = json_get(j, "awb")) != NULL) {
            s = json_string(n, NULL);
            img_awb = s ? idx_of(IMG_AWB_MODES, 3, s) : -1;
            if (img_awb < 0) {
                json_free(j);
                return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"白平衡只支持 auto/indoor/outdoor\"}",
                                (int)HAL_EINVAL);
            }
            img.awb_mode = img_awb;
        }
        if ((n = json_get(j, "ir_mode")) != NULL) {
            s = json_string(n, NULL);
            img_ir_mode = s ? idx_of(IMG_IR_MODES, 3, s) : -1;
            if (img_ir_mode < 0) {
                json_free(j);
                return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"补光灯只支持 auto/off/on\"}",
                                (int)HAL_EINVAL);
            }
            img.ir_mode = img_ir_mode;
        }
        if ((n = json_get(j, "ir_sensitivity")) != NULL) {
            img_ir_sens = (int)json_int(n, 999);
            if (img_ir_sens < 0 || img_ir_sens > 7) {
                json_free(j);
                return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"灵敏度范围应为 0~7\"}",
                                (int)HAL_EINVAL);
            }
            img.ir_sensitivity = img_ir_sens;
        }
        if ((n = json_get(j, "ir_delay")) != NULL) {
            img_ir_delay = (int)json_int(n, 999);
            if (img_ir_delay < 5 || img_ir_delay > 60) {
                json_free(j);
                return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"切换延迟范围应为 5~60 秒\"}",
                                (int)HAL_EINVAL);
            }
            img.ir_delay_s = img_ir_delay;
        }
    }

    /* 日夜与宽动态：与图像参数同一端点提交。两者都可能因硬件缺失而不可用
       （IRCUT 未映射 / sensor 不支持），ENOTSUP 如实上报而不是默默忽略。

       ⚠️ 这一段**必须**排在下面 `set_image(&img)` 之前，且两者要能叠加：
       夜视要去色（CSC 饱和度压到 0），而 set_image 恒用缓存里的 saturation
       重算 CSC。两处各算各的就会"谁后调用谁赢"——2026-09-29 实测的真 bug：
       切「夜晚」时 daynight_apply 刚写下去色，紧随其后的 set_image 立刻按
       saturation=50 写回彩色，设备自报 night_now=true 而画面 chroma 纹丝不动
       （31.5→31.6），用户侧看到的就是"调整日夜配置没有任何变化"。
       现在 gk_video.c 用**同一个** csc_saturation() 计算，顺序先后都幂等。 */
    {
        json_t *dn = json_get(j, "daynight");
        if (dn) {
            const char *sv = json_string(dn, NULL);
            int di = sv ? idx_of(IMG_DN_MODES, 3, sv) : -1;
            if (di < 0) {
                json_free(j);
                return fmt_safe(out, out_cap,
                    "{\"code\":%d,\"msg\":\"日夜配置只支持 common/timed/auto"
                    "（日夜通用/日夜定时切换/日夜自动切换）\"}", (int)HAL_EINVAL);
            }
            /* 三态 → HAL：common 落到"不分昼夜、不切夜视"（DAY = 保持彩色），
               auto 落到自动判定；**timed 在 HAL 里没有对应档**，需要时段表才
               能实现，所以只记 cfg、不下发（遗留清单 LEG-UI-13）。 */
            int hm = (di == 0) ? (int)HAL_DAYNIGHT_DAY
                   : (di == 2) ? (int)HAL_DAYNIGHT_AUTO : -1;
            if (hm >= 0 && hal()->video->set_daynight) {
                hal_err_t e2 = hal()->video->set_daynight((hal_daynight_t)hm);
                if (e2 != HAL_OK && e2 != HAL_ENOTSUP) { json_free(j); return e2; }
            }
            cfg_set_str("image.daynight", IMG_DN_MODES[di]);
        }
        if (json_get(j, "wdr")) {
            bool on = json_bool(json_get(j, "wdr"), false);
            if (hal()->video->set_isp_mode) {
                hal_err_t e2 = hal()->video->set_isp_mode(on ? HAL_ISP_WDR : HAL_ISP_LINEAR);
                if (e2 == HAL_ENOTSUP) {
                    json_free(j);
                    return fmt_safe(out, out_cap,
                        "{\"code\":%d,\"msg\":\"本传感器不支持宽动态（线性 sensor 无多帧合成）\"}",
                        (int)HAL_ENOTSUP);
                }
                if (e2 != HAL_OK) { json_free(j); return e2; }
            }
            cfg_set_bool("image.wdr", on);
        }
    }

    json_free(j);

    /* 亮度/对比度/饱和度/锐度/翻转交给 HAL，HAL 内部会做范围校验 */
    r = hal()->video->set_image(&img);
    if (r == HAL_EINVAL)
        return fmt_safe(out, out_cap,
                        "{\"code\":%d,\"msg\":\"参数超出范围（0-100，翻转/镜像 0-1，曝光等级 -3~3，灵敏度 0-7，切换延迟 5-60）\"}",
                        (int)HAL_EINVAL);
    if (r != HAL_OK) return r;

    /* 落盘：参数已生效，写失败不影响本次响应（下次重启会回到旧值），但必须留痕 */
    if (img.brightness >= 0 && cfg_set_int("image.brightness", img.brightness) != HAL_OK)
        LOGW(MOD, "image.brightness 落盘失败");
    if (img.contrast >= 0 && cfg_set_int("image.contrast", img.contrast) != HAL_OK)
        LOGW(MOD, "image.contrast 落盘失败");
    if (img.saturation >= 0 && cfg_set_int("image.saturation", img.saturation) != HAL_OK)
        LOGW(MOD, "image.saturation 落盘失败");
    if (img.sharpness >= 0 && cfg_set_int("image.sharpness", img.sharpness) != HAL_OK)
        LOGW(MOD, "image.sharpness 落盘失败");
    if (img.flip >= 0 && cfg_set_int("image.flip", img.flip) != HAL_OK)
        LOGW(MOD, "image.flip 落盘失败");
    if (img.mirror >= 0 && cfg_set_int("image.mirror", img.mirror) != HAL_OK)
        LOGW(MOD, "image.mirror 落盘失败");
    if (img.backlight_comp >= 0 && cfg_set_bool("image.blc", img.backlight_comp != 0) != HAL_OK)
        LOGW(MOD, "image.blc 落盘失败");
    /* 曝光组/白平衡/补光组：枚举按下标写回字符串（只用**直接下发的值**回写，
       没传的项（-1）不动——否则一次部分提交会把其它项刷成默认档） */
    if (img_expo_mode >= 0 && cfg_set_str("image.exposure.mode", IMG_EXPO_MODES[img_expo_mode]) != HAL_OK)
        LOGW(MOD, "image.exposure.mode 落盘失败");
    if (img_expo_lvl != -999 && cfg_set_int("image.exposure.level", img_expo_lvl) != HAL_OK)
        LOGW(MOD, "image.exposure.level 落盘失败");
    if (img_flicker >= 0 && cfg_set_str("image.antiflicker", IMG_FLICKERS[img_flicker]) != HAL_OK)
        LOGW(MOD, "image.antiflicker 落盘失败");
    if (img_awb >= 0 && cfg_set_str("image.awb", IMG_AWB_MODES[img_awb]) != HAL_OK)
        LOGW(MOD, "image.awb 落盘失败");
    if (img_ir_mode >= 0 && cfg_set_str("image.ir.mode", IMG_IR_MODES[img_ir_mode]) != HAL_OK)
        LOGW(MOD, "image.ir.mode 落盘失败");
    if (img_ir_sens >= 0 && cfg_set_int("image.ir.sensitivity", img_ir_sens) != HAL_OK)
        LOGW(MOD, "image.ir.sensitivity 落盘失败");
    if (img_ir_delay >= 0 && cfg_set_int("image.ir.delay", img_ir_delay) != HAL_OK)
        LOGW(MOD, "image.ir.delay 落盘失败");

    return fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"图像参数已生效\"}");
}

/* ---- 开机把 image.* 从 cfg 落实到 HAL（与 osd_apply_all 同一模式）。
 *
 * 没有这一步：保存只写 cfg，HAL 停在出厂默认，**重启后画面回默认、界面回读
 * 也回默认**（实测：cfg 里 brightness=88/blc=true，重启后 HAL 回 50/false），
 * 用户会认定“保存是假的”。
 *
 * 时机选在 console_api_init（模块启动、出流之前）是安全的：
 *   · set_image / set_isp_mode 在未 open 时**只写缓存**，v_open 建通路后统一下发
 *     （gk_video.c 明确写了这条契约，mock 则立即生效）；
 *   · set_daynight 会先把 s_dn_mode 记下，此刻那次 daynight_apply 因 ISP 未起
 *     会失败，但 v_open 会按 s_dn_mode 重放，所以这里不把失败当错误。
 * auto 是 HAL 默认值，就不必白跑一次 set_daynight。 */
static void image_apply_cfg(void)
{
    hal_image_t img;
    int64_t v;
    bool b;
    char s[16];

    if (!image_ok()) return;
    /* memset(-1)：cfg 里没写过的键一律不动，保留 HAL 出厂默认 */
    memset(&img, -1, sizeof(img));
    if (cfg_get_int("image.brightness", &v) == HAL_OK) img.brightness = (int)v;
    if (cfg_get_int("image.contrast",   &v) == HAL_OK) img.contrast   = (int)v;
    if (cfg_get_int("image.saturation", &v) == HAL_OK) img.saturation = (int)v;
    if (cfg_get_int("image.sharpness",  &v) == HAL_OK) img.sharpness  = (int)v;
    if (cfg_get_int("image.flip",       &v) == HAL_OK) img.flip       = (int)v;
    if (cfg_get_int("image.mirror",     &v) == HAL_OK) img.mirror     = (int)v;
    if (cfg_get_bool("image.blc", &b) == HAL_OK) img.backlight_comp = b ? 1 : 0;
    /* 图像页其它项（曝光组/白平衡/补光组）：同样只在 cfg 里有值时下发，
       没设过就保留 HAL 初值。枚举值非法（键被外部手改）时按"不改"处理，
       不让一个坏值把设备带进未知档位。 */
    if (cfg_get_str("image.exposure.mode", s, sizeof(s)) == HAL_OK) {
        int i = idx_of(IMG_EXPO_MODES, 2, s);
        if (i >= 0) img.exposure_mode = i;
    }
    if (cfg_get_int("image.exposure.level", &v) == HAL_OK && v >= -3 && v <= 3)
        img.exposure_level = (int)v;
    if (cfg_get_str("image.antiflicker", s, sizeof(s)) == HAL_OK) {
        int i = idx_of(IMG_FLICKERS, 3, s);
        if (i >= 0) img.antiflicker = i;
    }
    if (cfg_get_str("image.awb", s, sizeof(s)) == HAL_OK) {
        int i = idx_of(IMG_AWB_MODES, 3, s);
        if (i >= 0) img.awb_mode = i;
    }
    if (cfg_get_str("image.ir.mode", s, sizeof(s)) == HAL_OK) {
        int i = idx_of(IMG_IR_MODES, 3, s);
        if (i >= 0) img.ir_mode = i;
    }
    if (cfg_get_int("image.ir.sensitivity", &v) == HAL_OK && v >= 0 && v <= 7)
        img.ir_sensitivity = (int)v;
    if (cfg_get_int("image.ir.delay", &v) == HAL_OK && v >= 5 && v <= 60)
        img.ir_delay_s = (int)v;
    if (hal()->video->set_image(&img) != HAL_OK)
        LOGW(MOD, "开机落实 image.*（亮度/翻转/区域补偿/曝光/白平衡/补光）失败");

    if (hal()->video->set_isp_mode && cfg_get_bool("image.wdr", &b) == HAL_OK) {
        if (hal()->video->set_isp_mode(b ? HAL_ISP_WDR : HAL_ISP_LINEAR) != HAL_OK)
            LOGW(MOD, "开机落实 image.wdr 失败");
    }
    if (hal()->video->set_daynight && cfg_get_str("image.daynight", s, sizeof(s)) == HAL_OK) {
        /* common（日夜通用）= 不切夜视；auto = HAL 默认，不必白跑一次；
           timed 需要时段表才能落实，本轮跳过（LEG-UI-13）。 */
        if (strcmp(s, "common") == 0) {
            if (hal()->video->set_daynight(HAL_DAYNIGHT_DAY) != HAL_OK)
                LOGI(MOD, "开机落实 image.daynight=common：尚未出流，交给 v_open 重放");
        }
    }
}

#ifdef IPC_TESTING
/** 测试桩：模拟“开机回放一次 image.*”（console_api_init 里那一步） */
void console_api_test_image_apply_cfg(void) { image_apply_cfg(); }
#endif

/* ---- OSD 叠加（PRD 的 OSD 页）：配置落 osd.* 键，同时下发到 HAL。
 *
 * 两个固定项（通道名、时间）各占一个 HAL 区域；区域内只在**真的变了**时才
 * 调 set_pos——那是一个 destroy+attach 的较重操作，每次提交都调会让 OSD 闪。 */

#define OSD_POS_NAME 0
#define OSD_POS_TIME 1

static int   s_osd_id[2] = { -1, -1 };      /* region_id：[0]=通道名 [1]=时间 */
static float s_osd_pos[2][2];               /* 已下发的归一化位置 */
static bool  s_osd_pos_valid[2];
static int   osd_scratch_int;               /* 宏用的暂存：避免在宏里重复解析 JSON 节点 */

static bool osd_ok(void)
{
    return hal_has(HAL_MOD_OSD) && hal()->osd && hal()->osd->create_region &&
           hal()->osd->update_text && hal()->osd->set_pos &&
           hal()->osd->set_enable && hal()->osd->destroy_region;
}

/** 读 "osd.time.pos" 这类 JSON 二元组 [x,y]；缺失或非法就用传入的默认位置。
 *  设备端只保证"是合法 JSON"（见 config.c 该键的注释），所以这里按最简形状解析，
 *  解析不了就退回默认，不因为一个坐标把 OSD 整体关掉。 */
static void osd_read_pos(const char *key, float *x, float *y, float def_x, float def_y)
{
    char buf[96];
    const char *p;
    float a, b;

    *x = def_x;
    *y = def_y;
    if (cfg_get_str(key, buf, sizeof(buf)) != HAL_OK || buf[0] == '\0') return;
    p = strchr(buf, '[');
    if (!p) return;
    if (sscanf(p, "[%f , %f", &a, &b) != 2) return;
    if (a >= 0.0f && a <= 1.0f) *x = a;
    if (b >= 0.0f && b <= 1.0f) *y = b;
}

/** 把一个固定叠加项按当前配置建/改/删。slot：OSD_POS_NAME / OSD_POS_TIME */
static hal_err_t osd_apply_one(int slot, const char *enable_key, const char *pos_key,
                               const char *font_key, hal_osd_kind_t kind,
                               const char *text)
{
    const hal_osd_ops_t *o = hal()->osd;
    hal_osd_cfg_t cfg;
    bool en = false;
    int font = 24, id;
    float x, y;
    hal_err_t e;

    if (!osd_ok()) return HAL_ENOTSUP;
    cfg_get_bool(enable_key, &en);
    if (cfg_get_int(font_key, &font) != HAL_OK) font = 24;
    if (font < 12) font = 12;
    if (font > 72) font = 72;
    osd_read_pos(pos_key, &x, &y, 0.02f, (kind == HAL_OSD_TEXT_TIME) ? 0.90f : 0.02f);

    if (!en) {
        if (s_osd_id[slot] >= 0) {
            o->destroy_region(s_osd_id[slot]);
            s_osd_id[slot] = -1;
            s_osd_pos_valid[slot] = false;
        }
        return HAL_OK;
    }

    if (s_osd_id[slot] < 0) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.kind = kind;
        cfg.pos.x = x; cfg.pos.y = y;
        cfg.font_px = (uint32_t)font;
        cfg.color_argb = 0xFFFFFFFFu;      /* 白色，对齐实机默认 */
        snprintf(cfg.text, sizeof(cfg.text), "%s", text);
        id = -1;
        e = o->create_region(0, &cfg, &id);
        if (e != HAL_OK) return e;
        s_osd_id[slot] = id;
        s_osd_pos[slot][0] = x;
        s_osd_pos[slot][1] = y;
        s_osd_pos_valid[slot] = true;
        return HAL_OK;
    }

    /* 已存在：文本每次都同步（通道名可能改过），位置只在变了才动 */
    o->update_text(s_osd_id[slot], text);
    if (!s_osd_pos_valid[slot] || s_osd_pos[slot][0] != x || s_osd_pos[slot][1] != y) {
        hal_rect_t pos;
        pos.x = x; pos.y = y; pos.w = 0.0f; pos.h = 0.0f;
        if (o->set_pos(s_osd_id[slot], &pos) == HAL_OK) {
            s_osd_pos[slot][0] = x;
            s_osd_pos[slot][1] = y;
            s_osd_pos_valid[slot] = true;
        }
    }
    return HAL_OK;
}

/** 按当前配置重新落实所有 OSD 项（启动时与每次提交后各调一次） */
static hal_err_t osd_apply_all(void)
{
    char name[64] = "IPC";

    if (!osd_ok()) return HAL_ENOTSUP;
    /* 设备名为空时给个固定串：整块空白在画面上看不出原因，用户会误判成
       "OSD 没生效" */
    if (cfg_get_str("device.name", name, sizeof(name)) != HAL_OK || name[0] == '\0')
        snprintf(name, sizeof(name), "IPC");
    osd_apply_one(OSD_POS_NAME, "osd.channelName.enable", "osd.channelName.pos",
                  "osd.channelName.fontPx", HAL_OSD_TEXT, name);
    /* 时间项：HAL 按 strftime 格式串每秒自刷新（见 hal_osd.h） */
    osd_apply_one(OSD_POS_TIME, "osd.time.enable", "osd.time.pos",
                  "osd.time.fontPx", HAL_OSD_TEXT_TIME, "%Y-%m-%d %H:%M:%S");
    return HAL_OK;
}

static hal_err_t ep_osd_get(char *out, size_t out_cap)
{
    json_t *root;
    char *txt;
    char name[64] = "";
    bool t_en = false, n_en = false;
    int t_font = 24, n_font = 24;
    float tx, ty, nx, ny;
    hal_err_t rc;

    if (!osd_ok()) return HAL_ENOTSUP;
    cfg_get_bool("osd.time.enable", &t_en);
    cfg_get_bool("osd.channelName.enable", &n_en);
    if (cfg_get_int("osd.time.fontPx", &t_font) != HAL_OK) t_font = 24;
    if (cfg_get_int("osd.channelName.fontPx", &n_font) != HAL_OK) n_font = 24;
    osd_read_pos("osd.time.pos", &tx, &ty, 0.02f, 0.90f);
    osd_read_pos("osd.channelName.pos", &nx, &ny, 0.02f, 0.02f);
    cfg_get_str("device.name", name, sizeof(name));

    root = json_new_object();
    if (!root) return HAL_ENOMEM;
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "time_enable", json_new_bool(t_en));
    json_object_set(root, "time_font_px", json_new_int(t_font));
    json_object_set(root, "time_x", json_new_int((int)(tx * 100.0f + 0.5f)));
    json_object_set(root, "time_y", json_new_int((int)(ty * 100.0f + 0.5f)));
    json_object_set(root, "name_enable", json_new_bool(n_en));
    json_object_set(root, "name_font_px", json_new_int(n_font));
    json_object_set(root, "name_x", json_new_int((int)(nx * 100.0f + 0.5f)));
    json_object_set(root, "name_y", json_new_int((int)(ny * 100.0f + 0.5f)));
    json_object_set(root, "name", json_new_string(name));

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/** 写一个 [x,y] 位置键（百分比：0~100，前端用整数百分比更好操作） */
static void osd_write_pos(const char *key, int pct_x, int pct_y)
{
    char buf[64];
    if (pct_x < 0) pct_x = 0;
    if (pct_x > 100) pct_x = 100;
    if (pct_y < 0) pct_y = 0;
    if (pct_y > 100) pct_y = 100;
    fmt_safe(buf, sizeof(buf), "[%.2f,%.2f]", (double)pct_x / 100.0, (double)pct_y / 100.0);
    /* 这两个键登记为 CFG_T_JSON，必须走 cfg_set_json：用 cfg_set_str 会因类型
     * 不符被拒（表现为"提交成功但回读还是默认位置"）。 */
    cfg_set_json(key, buf);
}

static hal_err_t ep_osd_set(const http_req_t *req, char *out, size_t out_cap)
{
    json_t *j;

    if (!osd_ok()) return HAL_ENOTSUP;
    if (!req->body || req->body_len <= 0)
        return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"请求体为空\"}", (int)HAL_EINVAL);

    j = json_parse(req->body, req->body_len, NULL, 0);
    if (!j || !json_is(j, JSON_OBJECT)) { json_free(j); return HAL_EINVAL; }
#define OSD_TAKE_BOOL(fld, key) do { if (json_get(j, fld)) cfg_set_bool(key, json_bool(json_get(j, fld), false)); } while (0)
#define OSD_TAKE_INT(fld, lo, hi) do { \
        if (json_get(j, fld)) { \
            int _v = (int)json_int(json_get(j, fld), lo); \
            if (_v < (lo) || _v > (hi)) { json_free(j); \
                return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"%s 超出范围 %d-%d\"}", \
                                (int)HAL_EINVAL, fld, lo, hi); } \
            osd_scratch_int = _v; \
        } } while (0)
    /* 逐字段可选：只提交变化的那几项（前端各控件各自提交） */
    OSD_TAKE_BOOL("time_enable", "osd.time.enable");
    OSD_TAKE_BOOL("name_enable", "osd.channelName.enable");

    OSD_TAKE_INT("time_font_px", 12, 72);
    if (json_get(j, "time_font_px")) cfg_set_int("osd.time.fontPx", osd_scratch_int);
    OSD_TAKE_INT("name_font_px", 12, 72);
    if (json_get(j, "name_font_px")) cfg_set_int("osd.channelName.fontPx", osd_scratch_int);
#undef OSD_TAKE_BOOL
#undef OSD_TAKE_INT

    /* 位置：time_x/time_y 与 name_x/name_y 必须成对出现，否则只改一半会让
       文字跑到奇怪的地方；成对时两个键一起写。 */
    if (json_get(j, "time_x") && json_get(j, "time_y"))
        osd_write_pos("osd.time.pos", (int)json_int(json_get(j, "time_x"), 2),
                      (int)json_int(json_get(j, "time_y"), 90));
    if (json_get(j, "name_x") && json_get(j, "name_y"))
        osd_write_pos("osd.channelName.pos", (int)json_int(json_get(j, "name_x"), 2),
                      (int)json_int(json_get(j, "name_y"), 2));

    json_free(j);

    if (osd_apply_all() != HAL_OK)
        return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"OSD 下发失败\"}", (int)HAL_EIO);
    return fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"OSD 设置已生效\"}");
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
 * *dfn/*darg 非 NULL 表示该端点要求"响应发出后再执行"的动作（网络/端口应用、
 * 重启、恢复出厂等，见各端点注释）。
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
    if (strcmp(req->path, "/api/v1/system/capabilities") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_system_capabilities(out, out_cap) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/status") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_system_status(out, out_cap) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/time") == 0) {
        if (strcmp(req->method, "GET") == 0) return ep_time_get(out, out_cap);
        if (strcmp(req->method, "PUT") == 0) return ep_time_put(req, out, out_cap);
        return HAL_EINVAL;
    }
    if (strcmp(req->path, "/api/v1/system/net/apply") == 0)
        return strcmp(req->method, "POST") == 0 ? ep_net_apply(req, out, out_cap, dfn, darg) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/port/apply") == 0)
        return strcmp(req->method, "POST") == 0 ? ep_port_apply(req, out, out_cap, dfn, darg) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/log") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_system_log(req, out, out_cap) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/reboot") == 0)
        return strcmp(req->method, "POST") == 0 ? ep_system_reboot(out, out_cap, dfn, darg) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/reset") == 0)
        return strcmp(req->method, "POST") == 0 ? ep_system_reset(req, out, out_cap, dfn, darg) : HAL_EINVAL;
    /* 配置管理（简单恢复）与网络诊断：见 ep_config_reset 与 console_maint.c */
    if (strcmp(req->path, "/api/v1/system/config/reset") == 0)
        return strcmp(req->method, "POST") == 0 ? ep_config_reset(out, out_cap) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/system/diag") == 0) {
        if (strcmp(req->method, "GET") == 0) return console_maint_diag_state(out, out_cap);
        if (strcmp(req->method, "POST") == 0) return console_maint_diag(req, out, out_cap);
        return HAL_EINVAL;
    }
    if (strcmp(req->path, "/api/v1/video/params") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_video_params(out, out_cap) : HAL_EINVAL;
    if (strcmp(req->path, "/api/v1/image/params") == 0) {
        if (strcmp(req->method, "GET") == 0) return ep_image_get(out, out_cap);
        if (strcmp(req->method, "POST") == 0) return ep_image_set(req, out, out_cap);
        return HAL_EINVAL;
    }
    if (strcmp(req->path, "/api/v1/osd") == 0) {
        if (strcmp(req->method, "GET") == 0) return ep_osd_get(out, out_cap);
        if (strcmp(req->method, "POST") == 0) return ep_osd_set(req, out, out_cap);
        return HAL_EINVAL;
    }
    if (strcmp(req->path, "/api/v1/storage/info") == 0)
        return strcmp(req->method, "GET") == 0 ? ep_storage_info(out, out_cap) : HAL_EINVAL;

    return HAL_ENODEV;
}

#ifdef IPC_TESTING
/** 测试可见的计数器：正常调用顺序（先 http_respond_json 入队、再
 *  http_conn_defer_after_flush 登记）下应恒为 0。颠倒这两行的调用顺序会
 *  让登记那一刻发送队列已空（c->slen==c->soff），http_conn_defer_after_flush
 *  据此返回 HAL_ESTATE 而不是静默成功或同步执行——这正是它存在的意义，
 *  见 http_server.h 的声明注释。这个计数器只用来让"顺序反了"这件事在
 *  测试里可观察：console_api_handler 本身收到失败后只能记日志（响应
 *  已经发出，来不及回头改响应内容），日志不适合被测试断言。 */
static unsigned s_defer_register_fail_count;
#endif

static int console_api_handler(http_req_t *req, void *user)
{
    char *body;
    void (*dfn)(void *) = NULL;
    void *darg = NULL;
    hal_err_t e;

    (void)user;
    body = (char *)malloc(CONSOLE_API_BODY_MAX);
    if (!body) return console_reply_err(req->conn, HAL_ENOMEM);

    body[0] = '\0';
    e = api_dispatch(req, body, CONSOLE_API_BODY_MAX, &dfn, &darg);
    if (e != HAL_OK) {
        /* 端点可在失败时写入带具体原因的错误体（以 {"code": 开头），优先使用 */
        if (strncmp(body, "{\"code\":", 8) == 0) {
            int r = http_respond_json(req->conn, console_http_status(e), body) == HAL_OK ? 0 : -1;
            free(body);
            return r;
        }
        free(body);
        return console_reply_err(req->conn, e);
    }

    /* 调用顺序是硬约束：必须先让响应真正入队，才能登记"响应发出后"的动作。
       http_conn_defer_after_flush 用返回值强制这一点——若这两行被颠倒，
       登记时发送队列还是空的，它会返回 HAL_ESTATE 而不是替我们决定要不要
       同步执行（见 http_server.h 声明注释）。 */
    e = http_respond_json(req->conn, 200, body);
    free(body);
    if (e != HAL_OK) return console_reply_err(req->conn, e);   /* 入队失败：不登记动作 */

    if (dfn && http_conn_defer_after_flush(req->conn, dfn, darg) != HAL_OK) {
        /* 正常调用顺序下不会走到这里（响应刚入队，发送队列必然非空）。
           响应已经发出，不能退回来同步执行动作（那样会破坏"响应先发出"
           这条约束本身），只能记日志留痕 + 让测试能观察到（见上面
           s_defer_register_fail_count 的注释）。 */
#ifdef IPC_TESTING
        s_defer_register_fail_count++;
#endif
        LOGE(MOD, "延后动作登记失败：响应已发出但设备不会自动执行该动作，请重试");
    }
    return 0;
}

hal_err_t console_api_init(void)
{
    hal_err_t e = console_api_register_rules();
    if (e != HAL_OK) return e;
    /* 把已存的 OSD 配置落实一次：视频通路没起来时 HAL 会把区域先登记，
       等首次 open 再真正建出来（见 gk_osd.c），所以这里不会因为
       "此刻还没出流"而丢掉 OSD。 */
    osd_apply_all();
    /* 图像参数同理：不回放的话重启就回出厂默认（详见 image_apply_cfg 注释） */
    image_apply_cfg();
    return http_route("/api/v1/", console_api_handler, NULL);
}

#ifdef IPC_TESTING
/* 测试：记住最近一次登记的延后动作，供 console_api_test_run_deferred 执行 */
static void (*s_test_last_dfn)(void *);
static void *s_test_last_darg;

hal_err_t console_api_test_dispatch(const http_req_t *req, char *body, size_t body_cap,
                                    bool *deferred_out)
{
    void (*dfn)(void *) = NULL;
    void *darg = NULL;
    hal_err_t rc = api_dispatch(req, body, body_cap, &dfn, &darg);
    if (deferred_out) *deferred_out = (rc == HAL_OK) && (dfn != NULL);
    s_test_last_dfn = (rc == HAL_OK) ? dfn : NULL;
    s_test_last_darg = darg;
    return rc;
}

hal_err_t console_api_test_run_deferred(void)
{
    void (*fn)(void *) = s_test_last_dfn;
    if (!fn) return HAL_ESTATE;
    s_test_last_dfn = NULL;
    fn(s_test_last_darg);
    return HAL_OK;
}

int console_api_test_full_handler(http_req_t *req)
{
    return console_api_handler(req, NULL);
}

unsigned console_api_test_defer_fail_count(void)
{
    return s_defer_register_fail_count;
}
#endif
