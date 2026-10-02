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

/** 遮挡（区域覆盖）能力：HAL 的 osd 模块实现了遮挡矩形（caps.cover）。
 *  定义在下面的「区域覆盖」小节里，这里先声明——build_features_object 要用它
 *  合成 `osd.cover` 功能位（前端页签的显隐就认这个）。 */
static bool cover_ok(void);

/** 白光补光灯能力：profile 的 gpio_map 里有没有 `white_led`（HAL 把未声明的
 *  逻辑引脚标成"未映射"，本 SKU SP-R1-02 只有 `ir_led` → false）。
 *
 *  为什么按能力位：图像页的「照明模式（白光/红外）」「白光强度」只有存在白光
 *  灯才有意义。参照实机也正是这么做的——2026-09-29 从 172.16.1.180 的
 *  `uci.js` 里挖到 `image_capability`，其中 `smart_white_lamp_supported` /
 *  `supplement_lamp` 就是这几项的显隐开关。前端按 `IPC.feat('image.white_led')`
 *  隐藏，与根 AGENTS「硬件能力强绑定的显隐规则」一致（不是随手藏功能）。 */
static bool cap_white_led(void)
{
    uint32_t mask = 0;
    if (!hal_has(HAL_MOD_GPIO) || !hal()->gpio || !hal()->gpio->get_mapped_mask) return false;
    if (hal()->gpio->get_mapped_mask(&mask) != HAL_OK) return false;
    return (mask & (1u << HAL_PIN_WHITE_LED)) != 0;
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
        /* 区域覆盖（隐私遮挡矩形）：能力位由 HAL 的 caps.cover 决定，与页签显隐同源 */
        json_object_set(obj, "osd.cover", json_new_bool(cover_ok())) != 0 ||
        json_object_set(obj, "image.daynight", json_new_bool(img && strcmp(p->daynight, "none") != 0)) != 0 ||
        json_object_set(obj, "image.wdr", json_new_bool(img && p->isp_wdr)) != 0 ||
        /* 白光补光与人形防过曝：硬件能力位驱动显隐（图像页对应控件未上报即隐藏），
           与实机同一做法（见 cap_white_led 注释）。本 SKU 两项均为 false。 */
        json_object_set(obj, "image.white_led", json_new_bool(cap_white_led())) != 0 ||
        json_object_set(obj, "image.human_exp",
                        json_new_bool(img && cap_ivs(p, HAL_IVS_HUMANOID))) != 0 ||
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
    FEAT("image.white_led", "白光补光", cap_white_led(), "hardware",
         "需 profile.gpio_map.white_led（本 SKU 只有红外补光灯）");
    FEAT("image.human_exp", "人形防过曝", img && cap_ivs(p, HAL_IVS_HUMANOID), "mixed",
         "需人形检测能力（profile.ivs.humanoid）");
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
 * 两种方式都**可另带** `timezone`（POSIX TZ 串，如 "CST-8" / "UTC-5:30"）：校验通过
 * 即落盘并立刻设进本进程（os_set_timezone → setenv+tzset），OSD 时间叠加与页面显示
 * 随即可见，不必重启 ipc_app。不带该字段 = 本次不动时区（旧调用方/模拟器照旧可用）。
 * 注意只收 POSIX 偏移写法：板上无 zoneinfo，`Asia/Shanghai` 这类区名会"存进去不生效"，
 * 故直接拒掉并回可读原因（见 console_tz_ok）。
 */
static hal_err_t ep_time_put(const http_req_t *req, char *out, size_t out_cap)
{
    json_t *j;
    bool en, tz_given = false;
    const char *tz;
    char tzbuf[CONSOLE_TZ_MAX + 1] = "";
    hal_err_t rc = HAL_OK;

    if (!hal_has(HAL_MOD_SYS) || !hal()->sys->apply_ntp || !hal()->sys->set_wallclock) return HAL_ENOTSUP;
    if (!req->body || req->body_len == 0) return HAL_EINVAL;
    j = json_parse(req->body, req->body_len, NULL, 0);
    if (!j || !json_is(j, JSON_OBJECT)) { json_free(j); return HAL_EINVAL; }
    en = json_bool(json_get(j, "ntp_enable"), false);
    tz = json_string(json_get(j, "timezone"), NULL);
    if (tz && tz[0]) {
        if (!console_tz_ok(tz)) {
            json_free(j);
            /* fmt_safe 成功时返回 HAL_OK，必须先写 body 再单独返回错误码，
               否则会变成"200 + 错误 body"（console_test 已覆盖这条） */
            fmt_safe(out, out_cap,
                     "{\"code\":%d,\"msg\":\"时区格式应为 POSIX 偏移写法（如 CST-8、UTC-5:30）\"}",
                     (int)HAL_EINVAL);
            return HAL_EINVAL;
        }
        /* 拷出再 free(j)：tz 指向 JSON 解析出的内存 */
        snprintf(tzbuf, sizeof(tzbuf), "%s", tz);
        tz_given = true;
    }

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
    if (rc == HAL_OK && tz_given && cfg_set_str("time.timezone", tzbuf) != HAL_OK) rc = HAL_EIO;
    json_free(j);
    if (rc != HAL_OK) return rc;
    /* 时区先落地再 apply：console_apply_timezone 自己读 cfg，万一写失败就不会误报 */
    if (tz_given) console_apply_timezone();
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
            json_object_set(jc, "smart_enc", json_new_bool(cfg.smart_enc));
            json_object_set(jc, "bg_interval", json_new_int(cfg.bg_interval));
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
    /* 区域补偿（blc）**不在这里出**：它的生效值要跟「监控场景」一起算
       （场景非普通时由预设接管），由 ep_image_get 的 scene 段统一填，
       以免同一个 JSON 里出现两个同名键。 */
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

/* 监控场景三档：取值**逐字对齐实机** image_scene_mode_common（2026-09-29 从
   参照实机 172.16.1.180 逐个切换抓包：普通=normal / 逆光=back_light /
   车牌=clear_licence）。cfg 键 image.scene 与前端 camera.js 的 SCENE_MAP 同值。 */
static const char *const IMG_SCENES[] = { "normal", "back_light", "clear_licence" };

/**
 * 图像页「监控场景」的实际执行策略（近似映射，务必读完再改）。
 *
 * 实机每个场景各自一套**分区域**的测光/背光补偿与白平衡表（切档时 get/set
 * `common[_back_light|_clear_licence]_{area_compensation,white_balance}_region_info`），
 * 本 SDK 没有测光权重表 / ROI（见 gk_video.c image_apply 的说明与 LEG-FW-13），
 * 所以场景在**整幅**上落成两个已有执行器的组合：
 *   普通 normal        = 交回用户自己的两个开关（宽动态 DRC / 区域补偿 AE 策略）；
 *   逆光 back_light    = DRC 开 + AE 保暗部（LOWLIGHT_PRIOR：把背光下的暗部提起来）；
 *   车牌 clear_licence = DRC 开 + AE 防过曝（HIGHLIGHT_PRIOR：压高光，贴近"车牌"
 *                        这类高反光目标想要的曝光取向）。我方**没有**车牌识别，
 *                        不对车牌做任何识别或增强，只是曝光取向。
 * 结论：这是**近似**，不是实机的"按区域测光"。差异已登记在 Docs/遗留问题清单.md。
 *
 * 场景非普通时两档开关的**生效值**由预设决定（前端据此把两行置灰），cfg 里存的
 * 始终是用户自己的选择，两个概念不能混：`wdr_user`/`blc_user` 是用户的，`wdr`/`blc`
 * 是设备此刻真在跑的。
 */
static void scene_effective(const char *scene, int wdr_cfg, int blc_cfg, int *wdr_out, int *blc_out)
{
    if (scene && strcmp(scene, "back_light") == 0) {
        *wdr_out = 1; *blc_out = 1;
    } else if (scene && strcmp(scene, "clear_licence") == 0) {
        *wdr_out = 1; *blc_out = 0;
    } else {
        *wdr_out = wdr_cfg; *blc_out = blc_cfg;
    }
}

/** "HH:MM" → 0..1439 分钟；非法（长度、冒号位置、时分越界）返回 -1。
 *  与 console_maint.c 的 hhmm_min 同一格式口径（各持一份 static，互不引用）。 */
static int hhmm_min_of(const char *s)
{
    int h, m;
    if (!s || strlen(s) != 5 || s[2] != ':') return -1;
    if (sscanf(s, "%2d:%2d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

/* ── 日夜两套配置（2026-09-29 用户裁定「日夜是两套不同的配置」）────────────
 * 白天套 = image.*（存量键，升级零迁移）；夜晚套 = image.night.*（分套范围
 * 与实机 shedday/shednight 的抓包对照见 config.c 键表注释）。夜晚套键
 * **没写过就回落白天套**：升级前的单套设备行为不变，显式改过夜晚套后才独立。
 *
 * POST /api/v1/image/params 的可选字段 period 决定编辑目标：
 *   day / night / both —— both = 日夜通用（两套同值即共用）；
 *   缺省 cur = 当前时段（预览页顶栏等不带 period 的调用点＝改「此刻所见」）。
 * 只有「目标集合包含当前时段」才把分套字段打进 HAL——编辑另一套时画面不能
 * 跟着变（实机同此：夜晚时段改 shedday.luma 画面纹丝不动）。镜像/防闪烁/
 * 补光组/日夜模式与时刻是全局项，不受 period 影响。 */
#define IMG_PFX_DAY   ""
#define IMG_PFX_NIGHT "night."

static void img_k(char *buf, size_t cap, const char *pfx, const char *name)
{
    snprintf(buf, cap, "image.%s%s", pfx, name);
}

/** 当前时段是不是夜晚：以 HAL 的日夜状态为准（auto=光敏判定、timed=维护线程
 *  刚切的、common=DAY）。取不到按白天处理（回放白天套，与出厂一致）。 */
static bool cur_is_night(void)
{
    bool night = false;
    if (hal_has(HAL_MOD_VIDEO) && hal()->video && hal()->video->get_daynight) {
        hal_daynight_t dn = HAL_DAYNIGHT_AUTO;
        if (hal()->video->get_daynight(&dn, &night) != HAL_OK) night = false;
    }
    return night;
}

/* 分套键读取：夜晚套没写过 → 回落白天套（pfx=day 时就是普通 cfg 读取） */
static bool pfx_cfg_int(const char *pfx, const char *name, int64_t *v)
{
    char k[64];
    img_k(k, sizeof(k), pfx, name);
    if (cfg_get_int(k, v) == HAL_OK) return true;
    if (pfx[0]) {
        img_k(k, sizeof(k), IMG_PFX_DAY, name);
        if (cfg_get_int(k, v) == HAL_OK) return true;
    }
    return false;
}

static bool pfx_cfg_bool(const char *pfx, const char *name, bool *v)
{
    char k[64];
    img_k(k, sizeof(k), pfx, name);
    if (cfg_get_bool(k, v) == HAL_OK) return true;
    if (pfx[0]) {
        img_k(k, sizeof(k), IMG_PFX_DAY, name);
        if (cfg_get_bool(k, v) == HAL_OK) return true;
    }
    return false;
}

static bool pfx_cfg_str(const char *pfx, const char *name, char *out, size_t cap)
{
    char k[64];
    img_k(k, sizeof(k), pfx, name);
    if (cfg_get_str(k, out, cap) == HAL_OK) return true;
    if (pfx[0]) {
        img_k(k, sizeof(k), IMG_PFX_DAY, name);
        if (cfg_get_str(k, out, cap) == HAL_OK) return true;
    }
    return false;
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

    /* 此刻是不是夜视：决定下面"白天套没写过"时能不能拿 HAL 读数顶替。
       取不到就按白天处理（保持旧行为）。 */
    bool cur_night = false;
    bool have_night = false;
    if (hal()->video->get_daynight) {
        hal_daynight_t dn = HAL_DAYNIGHT_AUTO;
        if (hal()->video->get_daynight(&dn, &cur_night) == HAL_OK) have_night = true;
        else cur_night = false;
    }

    /* 顶层四个滑杆 = **白天套**（顶层整体语义 = day，前端切月亮开关时读 night
       对象）。白天套 cfg 写过就以 cfg 为准；没写过时：
         · 此刻跑的是白天套 → HAL 读数就是白天套，照旧用它（升级前两者一致）；
         · 此刻跑的是**夜晚套** → HAL 读数是夜晚套的值，冒充白天套会让
           "改夜晚 → 白天滑杆跟着跳"（2026-09-29 夜间真机 E14 实测复现：
           白天套从没写过，夜晚写 63 后顶层 brightness 也变 63）。
           这种情况白天套 = 出厂未改，报中性值 50。 */
    {
        int64_t v;
        static const char *const n4[] = { "brightness", "contrast", "saturation", "sharpness" };
        for (size_t i = 0; i < 4; i++) {
            char k[64];
            img_k(k, sizeof(k), IMG_PFX_DAY, n4[i]);
            if (cfg_get_int(k, &v) == HAL_OK)
                json_object_set(root, n4[i], json_new_int((int)v));
            else if (have_night && cur_night)
                json_object_set(root, n4[i], json_new_int(50));
        }
    }

    /* 日夜与宽动态跟图像参数同页，一并返回，前端一次请求拿全 */
    {
        char s[16];
        int m = 2;   /* 出厂默认 auto（日夜自动切换），与 HAL 初值一致 */

        /* daynight 以 **cfg 为准**，不从 HAL 读：三态里的 timed（日夜定时切换）
           在 HAL 里根本没有对应状态，从 HAL 读回来只能得到 auto —— 界面会把
           用户选的「日夜定时切换」显示成「日夜自动切换」，看着就是“没保存”。
           “此刻是不是夜视”已在上面取过（cur_night），这里直接回报。 */
        if (cfg_get_str("image.daynight", s, sizeof(s)) == HAL_OK) {
            int i = idx_of(IMG_DN_MODES, 3, s);
            if (i >= 0) m = i;
        }
        if (have_night) json_object_set(root, "night_now", json_new_bool(cur_night));
        json_object_set(root, "daynight", json_new_string(IMG_DN_MODES[m]));
        /* 定时切换的两个时刻：键没写过就用出厂默认（与实机时间轴的两个默认
           指针一致）——前端要拿它渲染控件、用户不改也要能原样保存，所以不能省。 */
        s[0] = '\0';
        if (cfg_get_str("image.daynight.day_start", s, sizeof(s)) != HAL_OK || !s[0])
            snprintf(s, sizeof(s), "%s", "06:00");
        json_object_set(root, "daynight_day_start", json_new_string(s));
        s[0] = '\0';
        if (cfg_get_str("image.daynight.night_start", s, sizeof(s)) != HAL_OK || !s[0])
            snprintf(s, sizeof(s), "%s", "18:00");
        json_object_set(root, "daynight_night_start", json_new_string(s));
    }

    /* 监控场景 / 宽动态 / 区域补偿：三者其实是同一件事的三个视角，必须一起回报：
     *   scene              —— cfg，用户选的档（普通/逆光/车牌）；
     *   wdr / blc          —— **此刻实际下发**的值（场景非普通时由预设决定）；
     *   wdr_user / blc_user—— 用户自己的两个开关（cfg），前端从场景切回普通时要还原；
     *   scene_lock         —— 场景是否正在接管这两个开关（前端据此把两行置灰）。
     * 只回 cfg 会变成"开关显示关着、画面却在跑 DRC"；只回生效值则用户看不出自己
     * 的选择被谁改了。宽动态同样**不读 HAL**：未出流时 v_set_isp_mode 只写缓存，
     * 读回来是旧值，重启后界面会把开着的宽动态显示成关着。 */
    {
        bool bw = false, bb = false;
        char sc[24];
        int wdr_eff, blc_eff;

        sc[0] = '\0';
        if (cfg_get_str("image.scene", sc, sizeof(sc)) != HAL_OK ||
            idx_of(IMG_SCENES, 3, sc) < 0)
            sc[0] = '\0';                    /* 未设/非法 = 普通：用户开关说了算 */
        (void)cfg_get_bool("image.wdr", &bw);
        (void)cfg_get_bool("image.blc", &bb);
        scene_effective(sc[0] ? sc : NULL, bw ? 1 : 0, bb ? 1 : 0, &wdr_eff, &blc_eff);

        /* normal 是**合法存值**，但它不接管：锁判据是 "非普通"，不是 "非空"
           （写成非空时，显式存 normal 也会被前端当成被预设接管而置灰两行）。 */
        json_object_set(root, "scene", json_new_string(sc[0] ? sc : IMG_SCENES[0]));
        json_object_set(root, "scene_lock",
                        json_new_bool(sc[0] != '\0' && strcmp(sc, IMG_SCENES[0]) != 0));
        json_object_set(root, "wdr", json_new_bool(wdr_eff != 0));
        json_object_set(root, "wdr_user", json_new_bool(bw));
        json_object_set(root, "blc", json_new_bool(blc_eff != 0));
        json_object_set(root, "blc_user", json_new_bool(bb));
    }

    /* 夜晚套（night 对象）：字段与顶层同名，前端切月亮开关时用它整体回填。
       字段集 = 分套范围（见 config.c）；夜晚套键没写过 → 回落白天套（与
       image_apply_prefixed 的回放口径一致），再退不出就用与顶层相同的兜底值。 */
    {
        json_t *nw = json_new_object();
        const char *p = IMG_PFX_NIGHT;
        int64_t iv;
        char s[24];
        int m;

        if (nw) {
            int64_t v;
            json_object_set(nw, "brightness",
                json_new_int(pfx_cfg_int(p, "brightness", &v) ? (int)v : img.brightness));
            json_object_set(nw, "contrast",
                json_new_int(pfx_cfg_int(p, "contrast", &v) ? (int)v : img.contrast));
            json_object_set(nw, "saturation",
                json_new_int(pfx_cfg_int(p, "saturation", &v) ? (int)v : img.saturation));
            json_object_set(nw, "sharpness",
                json_new_int(pfx_cfg_int(p, "sharpness", &v) ? (int)v : img.sharpness));

            /* 场景 / 宽动态 / 区域补偿：与顶层同一套 scene_effective 规则，
               只是读夜晚套的键（生效值、用户意图、锁态各自独立）。 */
            {
                bool bw = false, bb = false;
                char sc[24];
                int wdr_eff, blc_eff;

                sc[0] = '\0';
                if (!pfx_cfg_str(p, "scene", sc, sizeof(sc)) || idx_of(IMG_SCENES, 3, sc) < 0)
                    sc[0] = '\0';
                (void)pfx_cfg_bool(p, "wdr", &bw);
                (void)pfx_cfg_bool(p, "blc", &bb);
                scene_effective(sc[0] ? sc : NULL, bw ? 1 : 0, bb ? 1 : 0, &wdr_eff, &blc_eff);
                json_object_set(nw, "scene", json_new_string(sc[0] ? sc : IMG_SCENES[0]));
                json_object_set(nw, "scene_lock",
                                json_new_bool(sc[0] != '\0' && strcmp(sc, IMG_SCENES[0]) != 0));
                json_object_set(nw, "wdr", json_new_bool(wdr_eff != 0));
                json_object_set(nw, "wdr_user", json_new_bool(bw));
                json_object_set(nw, "blc", json_new_bool(blc_eff != 0));
                json_object_set(nw, "blc_user", json_new_bool(bb));
            }

            s[0] = '\0';
            m = 0;
            if (pfx_cfg_str(p, "exposure.mode", s, sizeof(s)))
                m = idx_of(IMG_EXPO_MODES, 2, s);
            json_object_set(nw, "exposure", json_new_string(name_of(IMG_EXPO_MODES, 2, m < 0 ? 0 : m)));
            json_object_set(nw, "exposure_level",
                json_new_int(pfx_cfg_int(p, "exposure.level", &iv) ? (int)iv : 0));

            s[0] = '\0';
            m = 0;
            if (pfx_cfg_str(p, "awb", s, sizeof(s)))
                m = idx_of(IMG_AWB_MODES, 3, s);
            json_object_set(nw, "awb", json_new_string(name_of(IMG_AWB_MODES, 3, m < 0 ? 0 : m)));

            json_object_set(root, "night", nw);   /* root 接管所有权 */
        }
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

    /* period：编辑目标（日夜两套，见文件头「日夜两套配置」注释）。
       tgt[0]=白天套 tgt[1]=夜晚套；apply_period = 目标含当前时段（分套字段
       才进这次 set_image）。在 daynight 块之前算：同请求既改模式又改参数时
       以前端实际行为为准（前端从不合并这两种提交，见 camera.js）。 */
    bool tgt[2] = { true, false };
    bool apply_period, cur_night;
    const char *cur_pfx;
    bool dn_changed = false;
    {
        json_t *pn = json_get(j, "period");
        const char *ps = pn ? json_string(pn, NULL) : NULL;
        if (ps && strcmp(ps, "day") && strcmp(ps, "night") &&
            strcmp(ps, "both") && strcmp(ps, "cur")) {
            json_free(j);
            return fmt_safe(out, out_cap,
                "{\"code\":%d,\"msg\":\"period 只支持 day/night/both/cur\"}", (int)HAL_EINVAL);
        }
        cur_night = cur_is_night();
        if (!ps || strcmp(ps, "cur") == 0) { tgt[0] = !cur_night; tgt[1] = cur_night; }
        else if (strcmp(ps, "day") == 0)   { tgt[0] = true;  tgt[1] = false; }
        else if (strcmp(ps, "night") == 0) { tgt[0] = false; tgt[1] = true; }
        else                                { tgt[0] = true;  tgt[1] = true; }
    }
    apply_period = (tgt[0] && !cur_night) || (tgt[1] && cur_night);
    cur_pfx = cur_night ? IMG_PFX_NIGHT : IMG_PFX_DAY;

    /* memset(-1)：本次未带上的字段一律"不改"（HAL 契约） */
    memset(&img, -1, sizeof(img));
#define IMG_TAKE(fld) do { json_t *n = json_get(j, #fld); if (n) img.fld = (int)json_int(n, -1); } while (0)
    IMG_TAKE(brightness); IMG_TAKE(contrast); IMG_TAKE(saturation);
    IMG_TAKE(sharpness);  IMG_TAKE(hue);
    IMG_TAKE(flip);       IMG_TAKE(mirror);
#undef IMG_TAKE
    /* 区域补偿是**布尔**（前端开关发 true/false，cfg 也是 CFG_T_BOOL）：
       不能走 IMG_TAKE 的 json_int——JSON true 会被读成兜底 -1（=不修改），
       表现为"保存成功但值没变"。与 wdr 的读法保持一致。
       注意：这里拿到的是**用户开关**的值，真正的下发值还要跟「监控场景」合成
       （见下面 scene_effective 那一段），所以先存进 blc_user 变量。 */
    int blc_user = -1;
    {
        json_t *blc = json_get(j, "blc");
        if (blc) blc_user = json_bool(blc, false) ? 1 : 0;
    }

    /* 监控场景与定时切换的两个时刻：与图像参数同一端点提交。
       scene 取值必须与 IMG_SCENES（实机 image_scene_mode_common）逐字一致；
       时段是 "HH:MM"，格式非法当场报错——cfg 规则只能卡字符集与长度，
       卡不了冒号位置（"6:0:0" 这类能进 cfg 但语义是垃圾）。 */
    int img_scene = -1;
    char dn_day[8] = { 0 }, dn_night[8] = { 0 };
    {
        json_t *n;
        const char *s;

        if ((n = json_get(j, "scene")) != NULL) {
            s = json_string(n, NULL);
            img_scene = s ? idx_of(IMG_SCENES, 3, s) : -1;
            if (img_scene < 0) {
                json_free(j);
                return fmt_safe(out, out_cap,
                    "{\"code\":%d,\"msg\":\"监控场景只支持 normal/back_light/clear_licence"
                    "（普通模式/逆光模式/车牌模式）\"}", (int)HAL_EINVAL);
            }
        }
        if ((n = json_get(j, "daynight_day_start")) != NULL) {
            s = json_string(n, NULL);
            if (hhmm_min_of(s) < 0) {
                json_free(j);
                return fmt_safe(out, out_cap,
                    "{\"code\":%d,\"msg\":\"白天开始时间应为 HH:MM（00:00-23:59）\"}",
                    (int)HAL_EINVAL);
            }
            snprintf(dn_day, sizeof(dn_day), "%s", s);
        }
        if ((n = json_get(j, "daynight_night_start")) != NULL) {
            s = json_string(n, NULL);
            if (hhmm_min_of(s) < 0) {
                json_free(j);
                return fmt_safe(out, out_cap,
                    "{\"code\":%d,\"msg\":\"夜晚开始时间应为 HH:MM（00:00-23:59）\"}",
                    (int)HAL_EINVAL);
            }
            snprintf(dn_night, sizeof(dn_night), "%s", s);
        }
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
               auto 落到自动判定；timed 由 console_maint 的时段定时器按设备本地
               时间在 DAY/NIGHT 之间切，这里**立刻**同步一次（否则用户点了
               「日夜定时切换」要等维护线程下一拍，最长 2 秒才看到画面变化）。 */
            int hm = (di == 0) ? (int)HAL_DAYNIGHT_DAY
                   : (di == 2) ? (int)HAL_DAYNIGHT_AUTO : -1;
            if (hm >= 0 && hal()->video->set_daynight) {
                hal_err_t e2 = hal()->video->set_daynight((hal_daynight_t)hm);
                if (e2 != HAL_OK && e2 != HAL_ENOTSUP) { json_free(j); return e2; }
            }
            cfg_set_str("image.daynight", IMG_DN_MODES[di]);
            if (di == 1) console_maint_daynight_sync();
            dn_changed = true;   /* 模式决定“哪套在跑”：末尾按新时段重放一套 */
        }
        /* 宽动态 / 区域补偿 / 监控场景：三者一起算**实际下发值**。
           场景非普通时由预设接管这两个执行器（见 scene_effective），普通时用
           用户开关；cfg 里始终存"用户意图"，回读时再用同一套规则算出生效值，
           两边永远一致（GET 见 ep_image_get 的 scene 段）。 */
        {
            char sc[24];
            int wdr_cfg, blc_cfg, wdr_eff, blc_eff;
            bool wdr_given = json_get(j, "wdr") != NULL;
            bool bw = false, bb = false;

            /* 合成的输入 = **当前时段套**的用户意图（请求覆盖其上）——只有这套
               会打进 HAL；落盘则按 period 写目标集合各自的键（见下方循环）。 */
            (void)pfx_cfg_bool(cur_pfx, "wdr", &bw);
            wdr_cfg = bw ? 1 : 0;
            if (wdr_given) wdr_cfg = json_bool(json_get(j, "wdr"), false) ? 1 : 0;
            (void)pfx_cfg_bool(cur_pfx, "blc", &bb);
            blc_cfg = bb ? 1 : 0;
            if (blc_user >= 0) blc_cfg = blc_user;

            sc[0] = '\0';
            if (img_scene >= 0) snprintf(sc, sizeof(sc), "%s", IMG_SCENES[img_scene]);
            else (void)pfx_cfg_str(cur_pfx, "scene", sc, sizeof(sc));
            if (idx_of(IMG_SCENES, 3, sc) < 0) sc[0] = '\0';
            scene_effective(sc[0] ? sc : NULL, wdr_cfg, blc_cfg, &wdr_eff, &blc_eff);

            if (apply_period) {
                /* 区域补偿随 set_image 一起下发（它就是 hal_image_t 的一个字段），
                   所以要在下面 set_image 之前写进 img。 */
                img.backlight_comp = blc_eff;
                /* 只在本次请求真的碰到 scene / wdr 时才打 ISP：滑块拖动会以 150ms 节流
                   频繁提交亮度等字段，每次都重设 ISP 模式纯属白费（DRC 那一下不便宜），
                   而这两个执行器的值没变时 HAL 里已经是它了。 */
                if (hal()->video->set_isp_mode && (img_scene >= 0 || wdr_given)) {
                    hal_err_t e2 = hal()->video->set_isp_mode(wdr_eff ? HAL_ISP_WDR : HAL_ISP_LINEAR);
                    if (e2 == HAL_ENOTSUP) {
                        json_free(j);
                        return fmt_safe(out, out_cap,
                            "{\"code\":%d,\"msg\":\"本传感器不支持宽动态（线性 sensor 无多帧合成），"
                            "逆光/车牌场景同样需要它\"}", (int)HAL_ENOTSUP);
                    }
                    if (e2 != HAL_OK) { json_free(j); return e2; }
                }
            }
            /* 落盘的是**用户意图**：scene 与两个开关各自的键，按 period 写目标套
               （both 时两套都写＝日夜通用的“共用”语义）。生效值只在下发时算，
               不回写 cfg——否则用户切到「逆光模式」会把自己原来关着的宽动态开关
               悄悄改成开，切回普通模式时再也回不去。 */
            for (int t = 0; t < 2; t++) {
                char k[64];
                const char *p;
                if (!tgt[t]) continue;
                p = t ? IMG_PFX_NIGHT : IMG_PFX_DAY;
                if (img_scene >= 0) {
                    img_k(k, sizeof(k), p, "scene");
                    if (cfg_set_str(k, IMG_SCENES[img_scene]) != HAL_OK)
                        LOGW(MOD, "%s 落盘失败", k);
                }
                if (wdr_given) {
                    img_k(k, sizeof(k), p, "wdr");
                    if (cfg_set_bool(k, wdr_cfg != 0) != HAL_OK)
                        LOGW(MOD, "%s 落盘失败", k);
                }
                if (blc_user >= 0) {
                    img_k(k, sizeof(k), p, "blc");
                    if (cfg_set_bool(k, blc_user != 0) != HAL_OK)
                        LOGW(MOD, "%s 落盘失败", k);
                }
            }
        }
    }

    json_free(j);

    /* 亮度/对比度/饱和度/锐度/翻转交给 HAL，HAL 内部会做范围校验。
       编辑的是**另一时段**的套（period 不含当前时段）时，分套字段不进这次
       set_image——否则改夜晚套会把当前画面改掉（实机同此：夜晚时段改
       shedday.luma 画面纹丝不动）。**在副本上剥字段**：img 必须留着原值，
       下面的按套落盘还要用它（先剥后存会把待写值弄丢，写测试时踩过）。
       全局字段（镜像/防闪烁/补光组）两个路径都带。 */
    {
        hal_image_t for_hal = img;
        if (!apply_period) {
            for_hal.brightness = for_hal.contrast = for_hal.saturation =
            for_hal.sharpness = for_hal.hue = -1;
            for_hal.exposure_mode = for_hal.exposure_level = for_hal.awb_mode = -1;
            for_hal.backlight_comp = -1;
        }
        r = hal()->video->set_image(&for_hal);
    }
    if (r == HAL_EINVAL)
        return fmt_safe(out, out_cap,
                        "{\"code\":%d,\"msg\":\"参数超出范围（0-100，翻转/镜像 0-1，曝光等级 -3~3，灵敏度 0-7，切换延迟 5-60）\"}",
                        (int)HAL_EINVAL);
    if (r != HAL_OK) return r;

    /* 落盘：参数已生效，写失败不影响本次响应（下次重启会回到旧值），但必须留痕。
       分套四滑杆按 period 写目标套各自的键（both = 两套都写）。 */
    for (int t = 0; t < 2; t++) {
        char k[64];
        const char *p;
        if (!tgt[t]) continue;
        p = t ? IMG_PFX_NIGHT : IMG_PFX_DAY;
#define SAVE_IMG_INT(name, val) do {                                                   \
        if ((val) >= 0) {                                                              \
            img_k(k, sizeof(k), p, (name));                                            \
            if (cfg_set_int(k, (val)) != HAL_OK) LOGW(MOD, "%s 落盘失败", k);          \
        }                                                                              \
    } while (0)
        SAVE_IMG_INT("brightness", img.brightness);
        SAVE_IMG_INT("contrast",   img.contrast);
        SAVE_IMG_INT("saturation", img.saturation);
        SAVE_IMG_INT("sharpness",  img.sharpness);
#undef SAVE_IMG_INT
    }
    if (img.flip >= 0 && cfg_set_int("image.flip", img.flip) != HAL_OK)
        LOGW(MOD, "image.flip 落盘失败");
    if (img.mirror >= 0 && cfg_set_int("image.mirror", img.mirror) != HAL_OK)
        LOGW(MOD, "image.mirror 落盘失败");
    /* 区域补偿不在这里落盘：img.backlight_comp 此刻是**生效值**（可能被监控场景
       接管），写进 cfg 会污染用户意图——它已在上面的 scene 段按用户值写过。 */
    /* 定时切换的两个时刻：与图像参数同一端点提交，各自独立（不传就不动） */
    if (dn_day[0] && cfg_set_str("image.daynight.day_start", dn_day) != HAL_OK)
        LOGW(MOD, "image.daynight.day_start 落盘失败");
    if (dn_night[0] && cfg_set_str("image.daynight.night_start", dn_night) != HAL_OK)
        LOGW(MOD, "image.daynight.night_start 落盘失败");
    /* 曝光组/白平衡：枚举按下标写回字符串（只用**直接下发的值**回写，
       没传的项（-1）不动——否则一次部分提交会把其它项刷成默认档）。
       曝光模式/等级/白平衡是**分套项**（与实机 shedday 同口径），按 period 写；
       防闪烁是全局项（电网频率不分昼夜）。 */
    for (int t = 0; t < 2; t++) {
        char k[64];
        const char *p;
        if (!tgt[t]) continue;
        p = t ? IMG_PFX_NIGHT : IMG_PFX_DAY;
        if (img_expo_mode >= 0) {
            img_k(k, sizeof(k), p, "exposure.mode");
            if (cfg_set_str(k, IMG_EXPO_MODES[img_expo_mode]) != HAL_OK) LOGW(MOD, "%s 落盘失败", k);
        }
        if (img_expo_lvl != -999) {
            img_k(k, sizeof(k), p, "exposure.level");
            if (cfg_set_int(k, img_expo_lvl) != HAL_OK) LOGW(MOD, "%s 落盘失败", k);
        }
        if (img_awb >= 0) {
            img_k(k, sizeof(k), p, "awb");
            if (cfg_set_str(k, IMG_AWB_MODES[img_awb]) != HAL_OK) LOGW(MOD, "%s 落盘失败", k);
        }
    }
    if (img_flicker >= 0 && cfg_set_str("image.antiflicker", IMG_FLICKERS[img_flicker]) != HAL_OK)
        LOGW(MOD, "image.antiflicker 落盘失败");
    if (img_ir_mode >= 0 && cfg_set_str("image.ir.mode", IMG_IR_MODES[img_ir_mode]) != HAL_OK)
        LOGW(MOD, "image.ir.mode 落盘失败");
    if (img_ir_sens >= 0 && cfg_set_int("image.ir.sensitivity", img_ir_sens) != HAL_OK)
        LOGW(MOD, "image.ir.sensitivity 落盘失败");
    if (img_ir_delay >= 0 && cfg_set_int("image.ir.delay", img_ir_delay) != HAL_OK)
        LOGW(MOD, "image.ir.delay 落盘失败");

    /* 日夜模式刚改：按新时段把该套参数重放一遍（模式决定“哪套在跑”——
       例如从自动切到通用，画面要回到白天套；上面的 cfg 已全部写完）。 */
    if (dn_changed) console_image_apply_period(cur_is_night());

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
static void image_apply_prefixed(const char *pfx)
{
    hal_image_t img;
    int64_t v;
    char s[16];

    if (!image_ok()) return;
    /* memset(-1)：cfg 里没写过的键一律不动，保留 HAL 出厂默认 */
    memset(&img, -1, sizeof(img));
    /* 分套四滑杆：夜晚套键没写过 → 回落白天套（pfx_cfg_int 内含），两套都
       没写过 → -1 保留 HAL 出厂值（与单套时代的回放行为一致）。 */
    if (pfx_cfg_int(pfx, "brightness", &v)) img.brightness = (int)v;
    if (pfx_cfg_int(pfx, "contrast",   &v)) img.contrast   = (int)v;
    if (pfx_cfg_int(pfx, "saturation", &v)) img.saturation = (int)v;
    if (pfx_cfg_int(pfx, "sharpness",  &v)) img.sharpness  = (int)v;
    /* 全局项（不分套）：镜像 */
    if (cfg_get_int("image.flip",       &v) == HAL_OK) img.flip       = (int)v;
    if (cfg_get_int("image.mirror",     &v) == HAL_OK) img.mirror     = (int)v;
    /* 分套：曝光模式/等级、白平衡；全局：防闪烁、补光组。
       同样只在 cfg 里有值时下发，没设过就保留 HAL 初值。枚举值非法
       （键被外部手改）时按"不改"处理，不让一个坏值把设备带进未知档位。 */
    if (pfx_cfg_str(pfx, "exposure.mode", s, sizeof(s))) {
        int i = idx_of(IMG_EXPO_MODES, 2, s);
        if (i >= 0) img.exposure_mode = i;
    }
    if (pfx_cfg_int(pfx, "exposure.level", &v) && v >= -3 && v <= 3)
        img.exposure_level = (int)v;
    if (cfg_get_str("image.antiflicker", s, sizeof(s)) == HAL_OK) {
        int i = idx_of(IMG_FLICKERS, 3, s);
        if (i >= 0) img.antiflicker = i;
    }
    if (pfx_cfg_str(pfx, "awb", s, sizeof(s))) {
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

    /* 监控场景 / 宽动态 / 区域补偿：与 ep_image_set 用**同一套规则**合成实际
       下发值（scene_effective），否则重启后画面会掉回"只看用户开关"那一档，
       而界面上还显示着场景模式——又变成一次"保存是假的"。读的是 pfx 那套的键。 */
    {
        char sc[24];
        int wdr_cfg = 0, blc_cfg = 0, wdr_eff, blc_eff;
        bool bw = false, bb = false;

        (void)pfx_cfg_bool(pfx, "wdr", &bw);
        (void)pfx_cfg_bool(pfx, "blc", &bb);
        wdr_cfg = bw ? 1 : 0;
        blc_cfg = bb ? 1 : 0;
        sc[0] = '\0';
        if (!pfx_cfg_str(pfx, "scene", sc, sizeof(sc)) || idx_of(IMG_SCENES, 3, sc) < 0)
            sc[0] = '\0';
        scene_effective(sc[0] ? sc : NULL, wdr_cfg, blc_cfg, &wdr_eff, &blc_eff);
        img.backlight_comp = blc_eff;
        if (hal()->video->set_isp_mode &&
            hal()->video->set_isp_mode(wdr_eff ? HAL_ISP_WDR : HAL_ISP_LINEAR) != HAL_OK)
            LOGW(MOD, "开机落实 image.wdr / 监控场景失败");
    }

    if (hal()->video->set_image(&img) != HAL_OK)
        LOGW(MOD, "开机落实 image.*（亮度/翻转/区域补偿/曝光/白平衡/补光）失败");
}

static void image_apply_cfg(void)
{
    char s[16];

    /* 先落实全局的日夜模式（common→DAY），再按**当前时段**选套回放图像参数：
       顺序反了会用旧时段的判定选套。 */
    if (image_ok() && hal()->video->set_daynight &&
        cfg_get_str("image.daynight", s, sizeof(s)) == HAL_OK) {
        /* common（日夜通用）= 不切夜视；auto = HAL 默认，不必白跑一次；
           timed 不在这里落实——它要按设备本地时间判，而开机墙钟还没校时
           （设备无 RTC），判出来必然是假结论。console_maint 的工作线程从
           模块 start 起就跑，NTP 一同步它自会切过去（console_maint_daynight_sync）。 */
        if (strcmp(s, "common") == 0) {
            if (hal()->video->set_daynight(HAL_DAYNIGHT_DAY) != HAL_OK)
                LOGI(MOD, "开机落实 image.daynight=common：尚未出流，交给 v_open 重放");
        }
    }
    image_apply_prefixed(cur_is_night() ? IMG_PFX_NIGHT : IMG_PFX_DAY);
}

void console_image_apply_period(bool night)
{
    if (!image_ok()) return;
    image_apply_prefixed(night ? IMG_PFX_NIGHT : IMG_PFX_DAY);
}

#ifdef IPC_TESTING
/** 测试桩：模拟“开机回放一次 image.*”（console_api_init 里那一步） */
void console_api_test_image_apply_cfg(void) { image_apply_cfg(); }
#endif

/* ---- OSD 叠加（PRD 的 OSD 页；排布对齐实机 OSD 模块） ----
 *
 * 实机的 OSD 有两套排布（cfg `osd.mode`）：
 *   · normal 普通模式：通道名、时间、最多 4 条自定义字符，位置都可自由定位；
 *   · gb 国标模式：通道名与时间**右对齐**（落点由 HAL 按「文字宽度 + 最小边距」
 *     从右边框反算），自定义字符最多 8 条、位置固定。
 * 两者共用：字号、字体颜色、显示效果（闪烁）、通道名文本与「同步修改设备名称」。
 *
 * 保存与开机都走 osd_apply_all() **一次性重建**：区域条数随模式变化，增量维护
 * 很容易留下残影（国标的 8 条切回普通模式会多出 4 条）。保存是低频用户动作，
 * 重建一次的代价可以接受。
 *
 * 时间项的文本是 `[日期] [星期] 时间` 三段拼出来的（实机同一形状：勾了「星期」
 * 就是 `2026-09-29 星期二 22:50:56`）：日期段看 osd.time.date、星期段看
 * osd.time.week，区域本身的开关仍是 osd.time.enable（前端按“任一勾选即开”下发）。
 * 星期由 `%a` 表达，字面翻译成中文星期是**平台层**的活（见 hal_osd.h v1.5）。*/
#define OSD_TIME_FMT_MAX 32

#define OSD_MAX_REGIONS 12    /* 与 hal_osd_caps_t 上报的每通道上限一致 */
#define OSD_CHARS_NORMAL 4    /* 普通模式：4 条自定义字符 */
#define OSD_CHARS_GB 8        /* 国标模式：8 条（实机国标模式的能力位） */
#define OSD_GB_NAME_Y 0.02f   /* 国标排布：通道名贴顶 */
#define OSD_GB_TIME_Y 0.90f   /* 国标排布：时间贴底 */
#define OSD_CHAR_Y0 0.10f     /* 自定义字符固定排布：首行 */
#define OSD_CHAR_DY 0.08f     /* 自定义字符固定排布：行距 */

static int         s_osd_id[OSD_MAX_REGIONS] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };  /* 未建区 = -1（不能留 0，0 是合法 region_id） */
static const char *s_osd_tag[OSD_MAX_REGIONS]; /* 日志用：该槽位是什么 */

static bool osd_ok(void)
{
    return hal_has(HAL_MOD_OSD) && hal()->osd && hal()->osd->create_region &&
           hal()->osd->update_text && hal()->osd->set_pos &&
           hal()->osd->set_enable && hal()->osd->destroy_region;
}

/** 读 "osd.time.pos" 这类 JSON 二元组 [x,y]；缺失或非法就用传入的默认位置。
 *  设备端只保证"是合法 JSON"（见 config.c 该键的注释），所以这里按最简形状解析，
 *  解析不了就退回默认，不因为一个坐标把 OSD 整体关掉。
 *
 *  ⚠ 这几个键登记为 CFG_T_JSON，**必须用 cfg_get_json 读**：cfg_get_str 对
 *  CFG_T_JSON 只会回 HAL_EINVAL（类型不符），于是位置与自定义字符永远静默回默认值
 *  （2026-09-29 实测：控制台改过的位置重启后“又回到默认”，就是这个原因）。 */
static void osd_read_pos(const char *key, float *x, float *y, float def_x, float def_y)
{
    char buf[96];
    const char *p;
    float a, b;

    *x = def_x;
    *y = def_y;
    if (cfg_get_json(key, buf, sizeof(buf)) != HAL_OK || buf[0] == '\0') return;
    p = strchr(buf, '[');
    if (!p) return;
    if (sscanf(p, "[%f , %f", &a, &b) != 2) return;
    if (a >= 0.0f && a <= 1.0f) *x = a;
    if (b >= 0.0f && b <= 1.0f) *y = b;
}

/** 颜色名 → ARGB（实机本机色板：白/黑/红/绿/蓝）。未知值按默认白处理。 */
static uint32_t osd_color_argb(const char *name)
{
    if (!name || !name[0] || strcmp(name, "white") == 0) return 0xFFFFFFFFu;
    if (strcmp(name, "black") == 0) return 0xFF000000u;
    if (strcmp(name, "red") == 0) return 0xFFFF0000u;
    if (strcmp(name, "green") == 0) return 0xFF00FF00u;
    if (strcmp(name, "blue") == 0) return 0xFF0000FFu;
    return 0xFFFFFFFFu;
}

/** 全局字号（主码流分辨率下的像素高度）。实机字号是全局一项，我方两个 fontPx
 *  键由 ep_osd_set 始终同步成同一个值，读哪一个都一样。 */
static int osd_font_px(void)
{
    /* cfg_get_int 的形参是 int64_t*：务必用 int64_t 临时变量，
       传 int* 会让它写 8 字节、打穿 4 字节的栈变量（实测直接崩）。 */
    int64_t v = 32;
    if (cfg_get_int("osd.time.fontPx", &v) != HAL_OK) v = 32;
    if (v < 12) v = 12;
    if (v > 72) v = 72;
    return (int)v;
}

/** 一个 OSD 区域的建区请求（字段多，传结构体比 9 个入参清楚） */
typedef struct {
    bool           enable;
    hal_osd_kind_t kind;
    const char    *text;        /**< TEXT 是内容；TEXT_TIME 是 strftime 格式串 */
    const char    *pos_key;     /**< 非空 = 位置读该 cfg 键的 [x,y]；空 = 用 pos_x/pos_y */
    float          pos_x, pos_y;
    bool           align_right; /**< 国标模式：右对齐（HAL 按最小边距反算落点） */
    int            font_override; /**< 0 = 用全局字号（osd_font_px()） */
    const char    *tag;         /**< 日志用标记 */
} osd_req_t;

/** 按请求建/删一个槽位的区域（先销毁同槽位旧区域，避免残留与类型错配） */
static void osd_put(int idx, const osd_req_t *rq)
{
    const hal_osd_ops_t *o = hal()->osd;
    hal_osd_cfg_t cfg;
    char ctype[20] = "auto", color[16] = "white";
    int font, id = -1;
    int64_t margin = 1;   /* cfg_get_int 要 int64_t*，不能用 int */
    bool flicker = false;
    float x = rq->pos_x, y = rq->pos_y;

    if (idx < 0 || idx >= OSD_MAX_REGIONS) return;
    if (s_osd_id[idx] >= 0) {
        o->destroy_region(s_osd_id[idx]);
        s_osd_id[idx] = -1;
    }
    s_osd_tag[idx] = rq->tag;
    if (!rq->enable || !rq->text || rq->text[0] == '\0') return;

    font = rq->font_override > 0 ? rq->font_override : osd_font_px();
    cfg_get_str("osd.colorType", ctype, sizeof(ctype));
    cfg_get_str("osd.color", color, sizeof(color));
    cfg_get_bool("osd.flicker", &flicker);
    if (cfg_get_int("osd.margin", &margin) != HAL_OK) margin = 1;
    if (margin < 0) margin = 0;
    if (margin > 2) margin = 2;
    if (rq->pos_key) osd_read_pos(rq->pos_key, &x, &y, rq->pos_x, rq->pos_y);
    memset(&cfg, 0, sizeof(cfg));
    cfg.kind = rq->kind;
    cfg.pos.x = x;
    cfg.pos.y = y;
    cfg.font_px = (uint32_t)font;
    /* 字体颜色：auto 恒为默认白；只有 user_defined 才用 osd.color 指定的颜色 */
    cfg.color_argb = osd_color_argb(strcmp(ctype, "user_defined") == 0 ? color : "white");
    cfg.flicker = flicker;
    cfg.align = rq->align_right ? HAL_OSD_ALIGN_RIGHT : HAL_OSD_ALIGN_LEFT;
    /* 「最小边距」只在右对齐（国标模式）有意义；普通模式是自由定位，传 0 */
    cfg.margin_chars = rq->align_right ? (uint32_t)margin : 0u;
    snprintf(cfg.text, sizeof(cfg.text), "%s", rq->text);

    if (o->create_region(0, &cfg, &id) != HAL_OK) {
        LOGW(MOD, "OSD 区域创建失败（%s）", rq->tag ? rq->tag : "?");
        return;
    }
    s_osd_id[idx] = id;
}

/** 读自定义字符表（CFG_T_JSON 数组，元素 {"enabled":bool,"text":str,"x":num,"y":num}）。
 *  返回的 DOM 由调用方 json_free；无配置/形状不对返回 NULL（调用方按空表处理）。 */
static json_t *osd_text_regions(void)
{
    char buf[1024];
    json_t *j;

    /* 同样是 CFG_T_JSON 键 → 只能用 cfg_get_json（见 osd_read_pos 的说明） */
    if (cfg_get_json("osd.text.regions", buf, sizeof(buf)) != HAL_OK || buf[0] == '\0') return NULL;
    j = json_parse(buf, 0, NULL, 0);
    if (!j || !json_is(j, JSON_ARRAY)) {
        json_free(j);
        return NULL;
    }
    return j;
}

/** 拼时间项的 strftime 格式串：`[日期] [星期] 时间`，各段都有可能不出现。
 *  星期用 `%a`，由平台层渲染成中文星期（hal_osd.h v1.5）。最长 20 字节。 */
static void osd_time_format(char *out, size_t cap)
{
    bool date = true, week = false;

    cfg_get_bool("osd.time.date", &date);
    cfg_get_bool("osd.time.week", &week);
    out[0] = '\0';
    if (date && cap > 9) strcpy(out, "%Y-%m-%d");
    if (week && strlen(out) + 4 < cap) strcat(out, out[0] ? " %a" : "%a");
    if (strlen(out) + 10 < cap) strcat(out, out[0] ? " %H:%M:%S" : "%H:%M:%S");
}

/** 按当前配置重建所有 OSD 项（开机与每次保存各调一次） */
static void osd_apply_all(void)
{
    char mode[16] = "normal", name[64] = "", tfmt[OSD_TIME_FMT_MAX];
    bool name_en = false, time_en = false, gb;
    json_t *arr;
    int idx = 0, n, max_chars, i;

    if (!osd_ok()) return;
    cfg_get_str("osd.mode", mode, sizeof(mode));
    gb = (strcmp(mode, "gb") == 0);
    cfg_get_bool("osd.channelName.enable", &name_en);
    cfg_get_bool("osd.time.enable", &time_en);

    /* 通道名：优先 osd.channelName.text；没写过就回落 device.name（历史行为，
       升级零迁移）；再空则给个固定串——整块空白在画面上看不出原因，用户会误判
       成"OSD 没生效" */
    if (cfg_get_str("osd.channelName.text", name, sizeof(name)) != HAL_OK || name[0] == '\0') {
        if (cfg_get_str("device.name", name, sizeof(name)) != HAL_OK || name[0] == '\0')
            snprintf(name, sizeof(name), "IPC");
    }

    /* 1) 时间项：HAL 按 strftime 格式串每秒自刷新（`%a` = 中文星期，见 hal_osd.h v1.5） */
    osd_time_format(tfmt, sizeof(tfmt));
    osd_put(idx++, &(osd_req_t){
        .enable = time_en, .kind = HAL_OSD_TEXT_TIME, .text = tfmt,
        .pos_key = "osd.time.pos", .pos_x = 0.02f,
        .pos_y = gb ? OSD_GB_TIME_Y : 0.90f, .align_right = gb, .tag = "time" });
    /* 2) 通道名 */
    osd_put(idx++, &(osd_req_t){
        .enable = name_en, .kind = HAL_OSD_TEXT, .text = name,
        .pos_key = "osd.channelName.pos", .pos_x = 0.02f,
        .pos_y = gb ? OSD_GB_NAME_Y : 0.02f, .align_right = gb, .tag = "channelName" });

    /* 3) 自定义字符：普通 4 条（位置来自 JSON）、国标 8 条（固定排布） */
    arr = osd_text_regions();
    n = arr ? (int)json_size(arr) : 0;
    max_chars = gb ? OSD_CHARS_GB : OSD_CHARS_NORMAL;
    for (i = 0; i < max_chars; i++) {
        char txt[HAL_OSD_TEXT_MAX] = "";
        float x = 0.02f, y = OSD_CHAR_Y0 + OSD_CHAR_DY * (float)i;
        bool en = false;
        int font_ovr = 0;

        if (i < n) {
            const json_t *e = json_at(arr, i);
            /* enabled 缺省视为**开**：平台「OSD 画面贴合」编辑器写的条目没有这个字段，
               若按缺省关处理，用户在平台加的文字就永远不上屏。控制台自己的勾选框总是显式写。 */
            en = json_bool(json_get(e, "enabled"), true);
            snprintf(txt, sizeof(txt), "%s", json_string(json_get(e, "text"), ""));
            /* font_px 可选覆盖全局字号（平台编辑器按条设字号）；未写则跟全局 */
            font_ovr = (int)json_int(json_get(e, "font_px"), 0);
            if (font_ovr < 12 || font_ovr > 72) font_ovr = 0;
            if (!gb) {   /* 普通模式才吃自定义坐标；国标模式位置固定（实机如此） */
                x = (float)json_number(json_get(e, "x"), 0.02);
                y = (float)json_number(json_get(e, "y"), (double)(OSD_CHAR_Y0 + OSD_CHAR_DY * (float)i));
            }
        }
        osd_put(idx++, &(osd_req_t){
            .enable = en, .kind = HAL_OSD_TEXT, .text = txt, .font_override = font_ovr,
            .pos_x = x, .pos_y = y, .align_right = false, .tag = "customText" });
    }
    json_free(arr);

    /* 4) 多出来的槽位清掉（从国标切回普通时会少 4 条） */
    for (; idx < OSD_MAX_REGIONS; idx++) {
        if (s_osd_id[idx] >= 0) {
            hal()->osd->destroy_region(s_osd_id[idx]);
            s_osd_id[idx] = -1;
        }
        s_osd_tag[idx] = NULL;
    }
}

/** 拼一条自定义字符（回读用）：缺失的源项补成「关 + 空文本 + 默认坐标」。 */
static void osd_add_text_item(json_t *arr, int i, const json_t *src)
{
    double def_y = (double)(OSD_CHAR_Y0 + OSD_CHAR_DY * (float)i);
    json_t *o = json_new_object();
    const char *t = "";
    double x = 0.02;
    double y = def_y;
    bool en = false;
    int font = 0;

    if (!o) return;
    if (src) {
        en = json_bool(json_get(src, "enabled"), true);
        t = json_string(json_get(src, "text"), "");
        x = json_number(json_get(src, "x"), 0.02);
        y = json_number(json_get(src, "y"), def_y);
        font = (int)json_int(json_get(src, "font_px"), 0);
        if (font < 12 || font > 72) font = 0;
    }
    json_object_set(o, "enabled", json_new_bool(en));
    json_object_set(o, "text", json_new_string(t));
    json_object_set(o, "x", json_new_int((int)(x * 100.0 + 0.5)));
    json_object_set(o, "y", json_new_int((int)(y * 100.0 + 0.5)));
    /* font_px = 0 表示跟全局字号（前端据此显示“跟随全局”） */
    json_object_set(o, "font_px", json_new_int(font));
    json_array_push(arr, o);
}

/** 回读：实机 OSD 页的**全部**参数（模式/显示效果/颜色/最小边距/通道名/自定义字符）。
 *  字段名与 POST 对称；前端回填只认这里，缺字段就会出现"设备是国标、界面显示普通"。 */
static hal_err_t ep_osd_get(char *out, size_t out_cap)
{
    json_t *root, *texts, *arr;
    char *txt;
    char name[64] = "", mode[16] = "normal", ctype[20] = "auto", color[16] = "white";
    bool t_en = false, n_en = false, flicker = false, link = false;
    bool t_date = true, t_week = false;
    int font, i, max_chars;
    int64_t margin = 1;   /* cfg_get_int 要 int64_t*，不能用 int */
    float tx, ty, nx, ny;
    hal_err_t rc;

    if (!osd_ok()) return HAL_ENOTSUP;
    cfg_get_bool("osd.time.enable", &t_en);
    cfg_get_bool("osd.time.date", &t_date);
    cfg_get_bool("osd.time.week", &t_week);
    cfg_get_bool("osd.channelName.enable", &n_en);
    cfg_get_bool("osd.flicker", &flicker);
    cfg_get_bool("osd.linkDeviceName", &link);
    cfg_get_str("osd.mode", mode, sizeof(mode));
    cfg_get_str("osd.colorType", ctype, sizeof(ctype));
    cfg_get_str("osd.color", color, sizeof(color));
    if (cfg_get_int("osd.margin", &margin) != HAL_OK) margin = 1;
    font = osd_font_px();
    osd_read_pos("osd.time.pos", &tx, &ty, 0.02f, 0.90f);
    osd_read_pos("osd.channelName.pos", &nx, &ny, 0.02f, 0.02f);
    /* 通道名与 osd_apply_all 同一口径：优先 osd.channelName.text，回落 device.name */
    if (cfg_get_str("osd.channelName.text", name, sizeof(name)) != HAL_OK || name[0] == '\0')
        cfg_get_str("device.name", name, sizeof(name));

    root = json_new_object();
    if (!root) return HAL_ENOMEM;
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "mode", json_new_string(mode));
    json_object_set(root, "flicker", json_new_bool(flicker));
    json_object_set(root, "font_px", json_new_int(font));
    json_object_set(root, "color_type", json_new_string(ctype));
    json_object_set(root, "color", json_new_string(color));
    json_object_set(root, "margin", json_new_int(margin));
    json_object_set(root, "link_device_name", json_new_bool(link));
    json_object_set(root, "time_enable", json_new_bool(t_en));
    /* 日期段/星期段的回读口径 = **上屏时到底有没有那一段**：区域关着（time_enable
       为假）时两个都报 false。否则界面会显示「日期勾着、画面上却没有时间」——正是
       本轮用户在「星期」上踩的那种不一致（勾了看不见东西）。**落盘仍存原始键**，
       所以平台侧单独下发 time_enable 的用法不受影响。 */
    json_object_set(root, "time_date", json_new_bool(t_en && t_date));
    json_object_set(root, "time_week", json_new_bool(t_en && t_week));
    json_object_set(root, "time_font_px", json_new_int(font));
    json_object_set(root, "time_x", json_new_int((int)(tx * 100.0f + 0.5f)));
    json_object_set(root, "time_y", json_new_int((int)(ty * 100.0f + 0.5f)));
    json_object_set(root, "name_enable", json_new_bool(n_en));
    json_object_set(root, "name_font_px", json_new_int(font));
    json_object_set(root, "name_x", json_new_int((int)(nx * 100.0f + 0.5f)));
    json_object_set(root, "name_y", json_new_int((int)(ny * 100.0f + 0.5f)));
    json_object_set(root, "name", json_new_string(name));

    /* 自定义字符：按当前模式给足条数（普通 4 / 国标 8），缺的补空项——前端按
       条数渲染输入框，条数少了就跟实机对不上。 */
    texts = json_new_array();
    arr = osd_text_regions();
    max_chars = (strcmp(mode, "gb") == 0) ? OSD_CHARS_GB : OSD_CHARS_NORMAL;
    for (i = 0; i < max_chars; i++) {
        const json_t *src = (arr && i < (int)json_size(arr)) ? json_at(arr, i) : NULL;
        osd_add_text_item(texts, i, src);
    }
    json_free(arr);
    json_object_set(root, "texts", texts);

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

/** 拒绝一次提交：释放请求 DOM 并回一条带中文原因的 400 */
static hal_err_t osd_reject(json_t *j, char *out, size_t out_cap, const char *msg)
{
    json_free(j);
    return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"%s\"}", (int)HAL_EINVAL, msg);
}

static hal_err_t ep_osd_set(const http_req_t *req, char *out, size_t out_cap)
{
    json_t *j;
    const json_t *v;
    char sv[24];
    bool link = false;

    if (!osd_ok()) return HAL_ENOTSUP;
    if (!req->body || req->body_len <= 0)
        return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"请求体为空\"}", (int)HAL_EINVAL);

    j = json_parse(req->body, req->body_len, NULL, 0);
    if (!j || !json_is(j, JSON_OBJECT)) { json_free(j); return HAL_EINVAL; }

    /* ---- 枚举项逐项白名单：脏值不写进 cfg（写进去就会被 validate 拒，反而更难查） ---- */
    if ((v = json_get(j, "mode")) != NULL) {
        snprintf(sv, sizeof(sv), "%s", json_string(v, ""));
        if (strcmp(sv, "normal") != 0 && strcmp(sv, "gb") != 0)
            return osd_reject(j, out, out_cap, "OSD 模式应为 normal 或 gb");
        cfg_set_str("osd.mode", sv);
    }
    if ((v = json_get(j, "color_type")) != NULL) {
        snprintf(sv, sizeof(sv), "%s", json_string(v, ""));
        if (strcmp(sv, "auto") != 0 && strcmp(sv, "user_defined") != 0)
            return osd_reject(j, out, out_cap, "字体颜色类型应为 auto 或 user_defined");
        cfg_set_str("osd.colorType", sv);
    }
    if ((v = json_get(j, "color")) != NULL) {
        static const char *COLORS[] = { "white", "black", "red", "green", "blue" };
        bool hit = false;
        snprintf(sv, sizeof(sv), "%s", json_string(v, ""));
        for (size_t k = 0; k < sizeof(COLORS) / sizeof(COLORS[0]); k++)
            if (strcmp(sv, COLORS[k]) == 0) hit = true;
        if (!hit) return osd_reject(j, out, out_cap, "颜色应为 white/black/red/green/blue 之一");
        cfg_set_str("osd.color", sv);
    }

    /* ---- 布尔与数值 ---- */
    if (json_get(j, "flicker"))
        cfg_set_bool("osd.flicker", json_bool(json_get(j, "flicker"), false));
    if (json_get(j, "link_device_name"))
        cfg_set_bool("osd.linkDeviceName", json_bool(json_get(j, "link_device_name"), false));
    if (json_get(j, "time_enable"))
        cfg_set_bool("osd.time.enable", json_bool(json_get(j, "time_enable"), false));
    /* 日期段/星期段的开关（缺省：日期开、星期关 = 升级前的行为） */
    if (json_get(j, "time_date"))
        cfg_set_bool("osd.time.date", json_bool(json_get(j, "time_date"), true));
    if (json_get(j, "time_week"))
        cfg_set_bool("osd.time.week", json_bool(json_get(j, "time_week"), false));
    if (json_get(j, "name_enable"))
        cfg_set_bool("osd.channelName.enable", json_bool(json_get(j, "name_enable"), false));
    if (json_get(j, "margin")) {
        int m = (int)json_int(json_get(j, "margin"), 1);
        if (m < 0 || m > 2) return osd_reject(j, out, out_cap, "最小边距应为 0/1/2");
        cfg_set_int("osd.margin", m);
    }
    /* 字号是全局一项（实机 OSD.font.size）：任一侧给了就把两个键一起写成同一个值，
       否则 osd.time.fontPx 与 osd.channelName.fontPx 会不一致，回读就自相矛盾。 */
    if (json_get(j, "time_font_px") || json_get(j, "name_font_px")) {
        const json_t *fv = json_get(j, "time_font_px") ? json_get(j, "time_font_px")
                                                      : json_get(j, "name_font_px");
        int f = (int)json_int(fv, 24);
        if (f < 12 || f > 72) return osd_reject(j, out, out_cap, "字号超出范围 12-72");
        cfg_set_int("osd.time.fontPx", f);
        cfg_set_int("osd.channelName.fontPx", f);
    }

    /* 位置：time_x/time_y 与 name_x/name_y 必须成对出现，否则只改一半会让
       文字跑到奇怪的地方；成对时两个键一起写。 */
    if (json_get(j, "time_x") && json_get(j, "time_y"))
        osd_write_pos("osd.time.pos", (int)json_int(json_get(j, "time_x"), 2),
                      (int)json_int(json_get(j, "time_y"), 90));
    if (json_get(j, "name_x") && json_get(j, "name_y"))
        osd_write_pos("osd.channelName.pos", (int)json_int(json_get(j, "name_x"), 2),
                      (int)json_int(json_get(j, "name_y"), 2));

    /* 通道名文本：开了「同步修改设备名称」就一并写 device.name（实机
       label_info[0].link_chn_to_dev 的语义），平台侧的设备名才会跟着变。 */
    cfg_get_bool("osd.linkDeviceName", &link);
    if ((v = json_get(j, "channel_name")) != NULL) {
        const char *cn = json_string(v, "");
        if (strlen(cn) > 32) return osd_reject(j, out, out_cap, "通道名称最长 32 字节");
        cfg_set_str("osd.channelName.text", cn);
        if (link && cn[0]) cfg_set_str("device.name", cn);
    }

    /* 自定义字符表：规整成统一形状再落盘（前端可能带多余字段或提交 half-empty 项） */
    if ((v = json_get(j, "texts")) != NULL) {
        json_t *norm;
        char *dump;
        int i, n;
        if (!json_is(v, JSON_ARRAY))
            return osd_reject(j, out, out_cap, "自定义字符应为数组");
        n = (int)json_size(v);
        if (n > OSD_CHARS_GB)
            return osd_reject(j, out, out_cap, "自定义字符最多 8 条");
        norm = json_new_array();
        for (i = 0; i < n; i++) {
            const json_t *e = json_at(v, i);
            const char *t = json_string(json_get(e, "text"), "");
            json_t *o = json_new_object();
            int font;  /* 0 = 跟全局字号（本地控制台不按条设字号；平台编辑器会带） */
            if (!o) continue;
            if (strlen(t) >= HAL_OSD_TEXT_MAX) {
                json_free(norm);
                return osd_reject(j, out, out_cap, "自定义字符过长");
            }
            font = (int)json_int(json_get(e, "font_px"), 0);
            if (font != 0 && (font < 12 || font > 72)) {
                json_free(norm);
                return osd_reject(j, out, out_cap, "自定义字符字号超出范围 12-72");
            }
            json_object_set(o, "enabled", json_new_bool(json_bool(json_get(e, "enabled"), true)));
            json_object_set(o, "text", json_new_string(t));
            json_object_set(o, "x", json_new_number(json_number(json_get(e, "x"), 0.02)));
            json_object_set(o, "y", json_new_number(
                json_number(json_get(e, "y"), (double)(OSD_CHAR_Y0 + OSD_CHAR_DY * (float)i))));
            json_object_set(o, "font_px", json_new_int(font));
            json_array_push(norm, o);
        }
        dump = json_dump(norm, false);
        json_free(norm);
        if (dump) { cfg_set_json("osd.text.regions", dump); free(dump); }
    }

    json_free(j);

    /* 立即按新模式重建区域：切模式/改字号都会改变区域条数与落点 */
    osd_apply_all();
    return fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"OSD 设置已生效\"}");
}

/* ---------------------------------------------------------------- 区域覆盖（隐私遮挡） */

/**
 * 「区域覆盖」（对齐实机同页文案；PRD LC-OSD-02「隐私区域覆盖（矩形遮挡，≤4）」）。
 *
 * 落点是 HAL 的 **HAL_OSD_COVER**（见 hal_osd.h v1.6）：Goke 侧本来就是同一套
 * RGN API 的另一个类型（COVER_RGN），数量上限与视频通路生命周期（open 才建、
 * close 拆）又和 OSD 完全一致，所以复用 osd 模块，不再单开一个 HAL 模块。
 *
 * 配置口径：`osd.cover.enable`（开关）+ `osd.cover.regions`（`[[x,y,w,h],…]`，
 * 归一化 0–1，最多 4 个）。坐标是**画面比例**，与实机同一语义（实机把同一组值
 * 按万分比存在 `cover.region_info` 里，3139 = 31.39%）；元素形状与
 * `alarm.motion.regions` 同族，平台侧将来可直接复用区域框选组件。
 *
 * 对外接口（本文件的 ep_cover_*）收发**万分比整数**（0–10000）：画面上拖一个框
 * 的精度按像素算就在千分位，整数百分比明显发涩；万分比又正好是实机口径，将来
 * 与平台互导不用换算系数。
 */
#define COVER_MAX        4       /* 对齐实机：graphLimit.rect / cover_reg_num 都是 4 */
#define COVER_COORD_MAX  10000   /* 万分比（实机同一口径） */
#define COVER_MIN_SPAN   100     /* 长或宽小于 1% 判为“过小”（实机同一条提示） */
#define COVER_AREA_DIV   4       /* 单块面积上限 = 画面面积的 1/4（**与 gk_osd.c 的
                                    GK_OSD_COVER_AREA_DIV 同值**：那边是硬件层的第二道防线）*/
/* 每个矩形占**两个** HAL 区域（主/子码流各一个）：遮挡是一块不透明位图，
   位图尺寸必须与 RGN 尺寸一致，而主/子分辨率不同（见 gk_osd.c 的 osd_attach_chns）。*/
#define COVER_CHNS       2

/** 遮挡色：不透明纯黑（2026-10-01 用户裁定「先按纯黑实现，真机看图后定」） */
#define COVER_COLOR_ARGB 0xFF000000u

static int s_cover_id[COVER_MAX][COVER_CHNS] = {
    { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }
};

/** 遮挡能力：HAL 上报 caps.cover 才算有（能力驱动；前端页签看同一份 features） */
static bool cover_ok(void)
{
    hal_osd_caps_t caps;
    if (!osd_ok() || !hal()->osd->get_caps) return false;
    if (hal()->osd->get_caps(&caps) != HAL_OK) return false;
    return caps.cover;
}

/** 读 osd.cover.regions，期望 `[[x,y,w,h], …]`（归一化 0–1）。
 *  与 osd_text_regions 同一取舍：只保证“是合法 JSON”，形状不合规的条目跳过，
 *  不因为一个坏条目把整页配置丢掉。返回的 DOM 由调用者 json_free。 */
static json_t *cover_regions(void)
{
    char buf[512];
    json_t *j;

    if (cfg_get_json("osd.cover.regions", buf, sizeof(buf)) != HAL_OK || buf[0] == '\0') return NULL;
    j = json_parse(buf, 0, NULL, 0);
    if (!j || !json_is(j, JSON_ARRAY)) {
        json_free(j);
        return NULL;
    }
    return j;
}

/** 画面尺寸（主码流）：用于在落盘前预检遮挡块面积。
 *
 *  ⚠ 不能用 cfg 的 video.0.main.w/h：**未写入时 cfg_get_int 读到的是规则下界
 *  （176×144）**，会把正常大小的遮挡块误判成“过大”（本轮踩过）。这里改成取
 *  HAL 上报的通道最大分辨率——与 gk 平台在 osd_rgn_create 里用的那套尺寸同源。 */
static void cover_frame_size(int *fw, int *fh)
{
    hal_video_caps_t caps;

    *fw = 1920;
    *fh = 1080;
    if (hal_has(HAL_MOD_VIDEO) && hal()->video && hal()->video->get_caps &&
        hal()->video->get_caps(&caps) == HAL_OK &&
        caps.max_size[0].w > 0 && caps.max_size[0].h > 0) {
        *fw = (int)caps.max_size[0].w;
        *fh = (int)caps.max_size[0].h;
    }
}

/** 清掉一个矩形占的所有通道槽位 */
static void cover_clear_slot(int i)
{
    int c;
    for (c = 0; c < COVER_CHNS; c++) {
        if (s_cover_id[i][c] >= 0) {
            hal()->osd->destroy_region(s_cover_id[i][c]);
            s_cover_id[i][c] = -1;
        }
    }
}

/** 按 cfg 重建全部遮挡区域（开机与每次保存各调一次）。
 *  条数会变（4 → 1），所以按“整体重建”而不是增量维护——与 osd_apply_all 同一理由。
 *
 *  @return 真正落实的**矩形条数**（小于请求条数 = 有矩形被 HAL 拒了，
 *          调用方必须如实报错：遮挡画不上去就是隐私泄露，不能当成功） */
static int cover_apply_all(void)
{
    bool enable = false;
    json_t *arr;
    int n, i, c, slot = 0, applied = 0;

    if (!cover_ok()) return 0;
    cfg_get_bool("osd.cover.enable", &enable);
    arr = cover_regions();
    n = arr ? (int)json_size(arr) : 0;
    if (n > COVER_MAX) n = COVER_MAX;

    for (i = 0; i < n; i++) {
        const json_t *e = json_at(arr, i);
        hal_osd_cfg_t cf;
        double x, y, w, h;
        int ok = 1;

        if (!json_is(e, JSON_ARRAY) || json_size(e) < 4) continue;
        x = json_number(json_at(e, 0), -1.0);
        y = json_number(json_at(e, 1), -1.0);
        w = json_number(json_at(e, 2), -1.0);
        h = json_number(json_at(e, 3), -1.0);
        if (x < 0.0 || y < 0.0 || w <= 0.0 || h <= 0.0) continue;
        if (x > 1.0) x = 1.0;
        if (y > 1.0) y = 1.0;
        if (w > 1.0 - x) w = 1.0 - x;
        if (h > 1.0 - y) h = 1.0 - y;
        if (w <= 0.0 || h <= 0.0) continue;

        memset(&cf, 0, sizeof(cf));
        cf.kind = HAL_OSD_COVER;
        cf.pos.x = (float)x;
        cf.pos.y = (float)y;
        cf.pos.w = (float)w;
        cf.pos.h = (float)h;
        cf.color_argb = COVER_COLOR_ARGB;

        for (c = 0; c < COVER_CHNS; c++) {
            int id = -1;
            /* 优先复用本槽位已建的句柄：连续保存时不必每次销毁重建（RGN 句柄有限，
               反复建/毁还会让画面闪一下）。改矩形走 set_pos（HAL 内部重建位图）。 */
            if (s_cover_id[slot][c] >= 0) {
                if (hal()->osd->set_pos(s_cover_id[slot][c], &cf.pos) == HAL_OK) {
                    hal()->osd->set_enable(s_cover_id[slot][c], enable);
                    continue;
                }
                hal()->osd->destroy_region(s_cover_id[slot][c]);
                s_cover_id[slot][c] = -1;
            }
            if (hal()->osd->create_region(c, &cf, &id) != HAL_OK) {
                ok = 0;
                break;
            }
            hal()->osd->set_enable(id, enable);   /* 关着也先建出来：开开关要立刻见效 */
            s_cover_id[slot][c] = id;
        }
        if (!ok) {
            cover_clear_slot(slot);   /* 只挂上一路半途而废：整条清掉，宁可少也不留半套 */
            continue;
        }
        applied++;
        slot++;
    }
    /* 多出来的槽位清掉（从 4 条减到 1 条时） */
    for (; slot < COVER_MAX; slot++) cover_clear_slot(slot);
    json_free(arr);
    return applied;
}

/** 回读：{"code":0,"enable":bool,"max":4,"regions":[{"x":..,"y":..,"w":..,"h":..},…]}
 *  坐标是**万分比整数**（0–10000），与 POST 对称。 */
static hal_err_t ep_cover_get(char *out, size_t out_cap)
{
    json_t *root, *regions, *arr;
    bool enable = false;
    char *txt;
    int i, n;
    hal_err_t rc;

    if (!cover_ok()) return HAL_ENOTSUP;
    cfg_get_bool("osd.cover.enable", &enable);
    arr = cover_regions();
    n = arr ? (int)json_size(arr) : 0;

    root = json_new_object();
    if (!root) {
        json_free(arr);
        return HAL_ENOMEM;
    }
    regions = json_new_array();
    json_object_set(root, "code", json_new_int(0));
    json_object_set(root, "enable", json_new_bool(enable));
    /* 上限跟着能力走：HAL 的区域上限就是平台能给的遮挡条数的上界 */
    json_object_set(root, "max", json_new_int(COVER_MAX));
    for (i = 0; i < n && i < COVER_MAX; i++) {
        const json_t *e = json_at(arr, i);
        json_t *o;
        double x, y, w, h;

        if (!json_is(e, JSON_ARRAY) || json_size(e) < 4) continue;
        x = json_number(json_at(e, 0), 0.0);
        y = json_number(json_at(e, 1), 0.0);
        w = json_number(json_at(e, 2), 0.0);
        h = json_number(json_at(e, 3), 0.0);
        o = json_new_object();
        if (!o) continue;
        json_object_set(o, "x", json_new_int((int)(x * COVER_COORD_MAX + 0.5)));
        json_object_set(o, "y", json_new_int((int)(y * COVER_COORD_MAX + 0.5)));
        json_object_set(o, "w", json_new_int((int)(w * COVER_COORD_MAX + 0.5)));
        json_object_set(o, "h", json_new_int((int)(h * COVER_COORD_MAX + 0.5)));
        json_array_push(regions, o);
    }
    json_free(arr);
    json_object_set(root, "regions", regions);

    txt = json_dump(root, false);
    json_free(root);
    if (!txt) return HAL_ENOMEM;
    rc = (strlen(txt) < out_cap) ? HAL_OK : HAL_ENOMEM;
    if (rc == HAL_OK) strcpy(out, txt);
    free(txt);
    return rc;
}

/** 保存：{"enable":bool,"regions":[{x,y,w,h},…]}（坐标万分比整数，可整段省略）。
 *  越界只夹取不报错（拖动越界是常事），但**长或宽太小**要报错——文案对齐实机
 *  `errStr.coverRectSizeErr`「区域长或宽的值过小，请重新绘画」。 */
static hal_err_t ep_cover_set(const http_req_t *req, char *out, size_t out_cap)
{
    json_t *j;
    const json_t *v;

    if (!cover_ok()) return HAL_ENOTSUP;
    if (!req->body || req->body_len <= 0)
        return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"请求体为空\"}", (int)HAL_EINVAL);

    j = json_parse(req->body, req->body_len, NULL, 0);
    if (!j || !json_is(j, JSON_OBJECT)) { json_free(j); return HAL_EINVAL; }

    if ((v = json_get(j, "regions")) != NULL) {
        char jbuf[320];
        size_t off = 0;
        int fw = 0, fh = 0, i, n;

        if (!json_is(v, JSON_ARRAY))
            return osd_reject(j, out, out_cap, "遮挡区域应为数组");
        n = (int)json_size(v);
        if (n > COVER_MAX)
            return osd_reject(j, out, out_cap, "最多只能设置 4 个遮挡区域");
        cover_frame_size(&fw, &fh);

        jbuf[off++] = '[';
        for (i = 0; i < n; i++) {
            const json_t *e = json_at(v, i);
            int x, y, w, h, k;

            if (!json_is(e, JSON_OBJECT))
                return osd_reject(j, out, out_cap, "遮挡区域应为对象");
            x = (int)json_int(json_get(e, "x"), 0);
            y = (int)json_int(json_get(e, "y"), 0);
            w = (int)json_int(json_get(e, "w"), 0);
            h = (int)json_int(json_get(e, "h"), 0);
            /* 先夹到画面内，再判大小（夹完可能就不够大了，两句提示就更准确） */
            if (x < 0) x = 0;
            if (x > COVER_COORD_MAX) x = COVER_COORD_MAX;
            if (y < 0) y = 0;
            if (y > COVER_COORD_MAX) y = COVER_COORD_MAX;
            if (w < 0) w = 0;
            if (w > COVER_COORD_MAX - x) w = COVER_COORD_MAX - x;
            if (h < 0) h = 0;
            if (h > COVER_COORD_MAX - y) h = COVER_COORD_MAX - y;
            if (w < COVER_MIN_SPAN || h < COVER_MIN_SPAN)
                return osd_reject(j, out, out_cap, "区域长或宽的值过小，请重新绘制");
            /* 设备端遮挡是“一块与矩形同尺寸的不透明位图”（见 gk_osd.c 里为什么
               不用 COVER_RGN），面积太大拿不到内存 → 在这里就拦住，
               不然就成了“保存成功但画面上没遮挡”——隐私功能上这属于不能接受的假成功。
               ⚠ 万分比要先换成像素再比（忘了换会把所有矩形都判成过大）。 */
            if ((long)(w * fw / COVER_COORD_MAX) * (long)(h * fh / COVER_COORD_MAX) >
                (long)(fw / COVER_AREA_DIV) * (long)fh)
                return osd_reject(j, out, out_cap,
                                  "遮挡区域过大：单个区域不能超过画面的 1/4");
            k = snprintf(jbuf + off, sizeof(jbuf) - off, "%s[%.4f,%.4f,%.4f,%.4f]",
                         i ? "," : "",
                         (double)x / COVER_COORD_MAX, (double)y / COVER_COORD_MAX,
                         (double)w / COVER_COORD_MAX, (double)h / COVER_COORD_MAX);
            if (k < 0 || (size_t)k >= sizeof(jbuf) - off)
                return osd_reject(j, out, out_cap, "遮挡区域过多");
            off += (size_t)k;
        }
        jbuf[off++] = ']';
        jbuf[off] = '\0';
        cfg_set_json("osd.cover.regions", jbuf);
    }
    if (json_get(j, "enable"))
        cfg_set_bool("osd.cover.enable", json_bool(json_get(j, "enable"), false));
    json_free(j);

    /* 立即落实，并**核对真的落实了几条**：HAL 侧可能因为内存/面积/通道限制建不出来，
       那种情况下必须报错（遮挡画不上去就是隐私泄露，不能当成功）。 */
    {
        json_t *chk = cover_regions();
        int want = chk ? (int)json_size(chk) : 0;
        int applied = cover_apply_all();

        json_free(chk);
        if (want > COVER_MAX) want = COVER_MAX;
        if (applied < want)
            return fmt_safe(out, out_cap, "{\"code\":%d,\"msg\":\"%s\"}", (int)HAL_EINVAL,
                            "遮挡区域过大或设备资源不足，未能全部生效（单个不能超过画面的 1/4）");
    }
    return fmt_safe(out, out_cap, "{\"code\":0,\"msg\":\"区域覆盖设置已生效\"}");
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
    /* 区域覆盖（隐私遮挡）：与 /api/v1/osd 同一模块的另一种区域，故路径平行 */
    if (strcmp(req->path, "/api/v1/cover") == 0) {
        if (strcmp(req->method, "GET") == 0) return ep_cover_get(out, out_cap);
        if (strcmp(req->method, "POST") == 0) return ep_cover_set(req, out, out_cap);
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
    /* 区域覆盖：同一个 osd 模块的遮挡区域，开机也要回放（不回放就重启即丢）。
       开机这条路不校验返回值：此刻视频通路往往还没起来，HAL 只登记不建区，
       真正的失败会在用户保存时如实反馈。 */
    (void)cover_apply_all();
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
