# IPC 固件平台化架构 —— 通用功能层 + 芯片/IC 适配层（HAL）

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 编写日期 | 2026-09-04 |
| 决策记录 | **当前只冻结架构分层、HAL 接口与能力清单 schema**；芯片（GK7205 系列为讨论倾向）、传感器 IC、是否外接 ISP、Flash、WiFi 模块均**待硬件选型评审**。本文中 `platform/gk7205v200` 等目录与 profile 数值为示例。WiFi 走 Linux 通用无线框架（HAL 通用）；镜头类型由设备型号在 profile 声明。见 [决策记录与待定事项](./决策记录与待定事项.md) |
| 设计目标 | IPC 固件（含设备本地控制台）**通用功能一套代码**；不同厂商 SoC 与外围 IC（传感器、WiFi、音频、Flash 等）通过**适配层**接入；新增芯片只实现 HAL，不改业务层 |
| 关联文档 | [私有协议优先接入方案 §4.1](./私有协议优先接入方案_IDP-v1.md)、[MVP 执行方案 §2](./MVP执行方案_待决策事项分析.md)、[SP-R1-02 规格核对报告](./SP-R1-02规格参数核对报告.md) |

---

## 目录

1. [设计原则](#1-设计原则)
2. [分层架构](#2-分层架构)
3. [HAL 接口定义](#3-hal-接口定义)
4. [芯片/IC 适配矩阵](#4-芯片ic-适配矩阵)
5. [产品能力清单（Capability Manifest）](#5-产品能力清单capability-manifest)
6. [构建系统与代码组织](#6-构建系统与代码组织)
7. [新芯片适配流程与完成标准](#7-新芯片适配流程与完成标准)
8. [测试策略](#8-测试策略)
9. [对里程碑的影响](#9-对里程碑的影响)
10. [风险](#10-风险)
- 附录 A：HAL 头文件草案
- 附录 B：SP-R1-02 能力清单示例

---

## 1. 设计原则

| 原则 | 落地方式 |
|---|---|
| 业务层零平台代码 | 协议栈、录像、OTA、控制台、IDP 等仅调用 HAL 接口与核心服务，禁止 `#ifdef GK7205` |
| 适配层薄且可枚举 | HAL 以 C 接口 + 函数表（vtable）形式定义；每个平台一个目录；每个外围 IC 一个驱动插件 |
| 能力由数据驱动 | 各产品型号一份能力清单 JSON，驱动 ONVIF `GetCapabilities`、IDP `hello.capabilities`、GB `DeviceInfo`、控制台菜单显隐 |
| 零拷贝不因抽象牺牲 | HAL 交付帧为共享内存句柄（fd + offset），业务层帧总线只传引用 |
| 可在 x86 上测 | 提供 `hal-mock`（文件源/合成帧），业务层与协议栈可在 PC 上单元测试与 CI |
| 调优数据视为资产 | ISP/传感器调优参数按 `platform+sensor` 存为二进制/JSON 资产，不写入代码 |
| 一套控制台 | 设备本地 Web 控制台（HTML/JS）与 IDP 配置共用同一配置模型与校验规则 |

---

## 2. 分层架构

```
┌──────────────────────────── L4 产品层 ────────────────────────────┐
│ profiles/SP-R1-02.json（能力清单、默认配置、OSD/LED 策略、外设映射）│
├──────────────────────────── L3 功能模块层（通用，一套代码）─────────┤
│ IDP 客户端 │ RTSP 服务端 │ ONVIF 服务端 │ GB28181 UA │ TF 录像器 │
│ OTA(A/B)  │ 本地 Web 控制台 │ 告警/IVS 事件 │ 抓图 │ PTZ(预留) │ P2P(三期) │
├──────────────────────────── L2 核心服务层（通用）─────────────────┤
│ 帧总线(零拷贝) │ 配置中心(JSON schema) │ 事件总线 │ 日志 │ 定时器 │
│ 安全存储(证书/密钥) │ 时间同步 │ 网络管理(eth/wifi 状态机) │ 看门狗 │
├──────────────────────────── L1 HAL 硬件抽象层 ────────────────────┤
│ hal_video │ hal_audio │ hal_osd │ hal_ivs │ hal_gpio │ hal_net │
│ hal_storage │ hal_sys(flash/ota/wdt/chipid) │ hal_crypto(可选硬加密)│
├──────────────────────────── L0 平台实现层（按芯片/IC）────────────┤
│ platform/gk7205v300 │ platform/gk7205v200 │ platform/rv1106(未来) │ platform/mock(x86) │
│   ├ soc/  (MPP/ISP/VENC 封装)   ├ sensor/ (sc230ai, imx335, sc530ai…)          │
│   ├ wifi/ (aic8800, rtl8188, ssv6x5x…)  ├ codec/ (internal, es8388…)          │
│   └ tuning/ (isp_sc230ai_gk7205v300.bin …)                                      │
├──────────────────────────── BSP ─────────────────────────────────┤
│ 厂商 Linux 内核 + 驱动（Goke SDK / OpenIPC openhisilicon / Rockchip SDK）        │
└──────────────────────────────────────────────────────────────────┘
```

**依赖方向**：L4 → L3 → L2 → L1 → L0，严格单向；L0 不得被 L2 以上直接引用。

---

## 3. HAL 接口定义

### 3.1 模块与职责

| 模块 | 职责 | 关键接口（摘要） | 平台差异点 |
|---|---|---|---|
| `hal_video` | 传感器初始化、ISP 模式、编码通道（多流）、参数动态调整、抓图、**镜头控制（可选）** | `open/close`, `set_encoder(ch, codec, w, h, fps, rc, bitrate, gop)`, `get_frame(ch, &frame)`, `release_frame`, `request_idr`, `snapshot(ch, jpeg_q)`, `set_isp(mode: linear/wdr/hdr)`, `set_image(brightness…, flip, mirror)`, `set_daynight(mode)`, `lens_focus(dir/abs)`, `lens_zoom(dir/abs)`, `lens_af_trigger` | MPP vs rkmpi API；HDR 帧率约束；最大分辨率/帧率；**镜头类型由 profile `video.lens` 声明，定焦型号 `lens_*` 返回 `HAL_ENOTSUP`，ONVIF Imaging Focus/PTZ Zoom 能力随之关闭** |
| `hal_audio` | 采集/播放、编码格式、增益、回声消除开关 | `capture_open(rate, fmt)`, `read`, `play_open`, `write`, `set_gain`, `set_aec` | 内置 Codec vs 外置 I2S Codec |
| `hal_osd` | 区域叠加（文本/位图） | `create_region`, `update_text`, `set_pos`, `destroy` | 区域数量、字体渲染位置（SoC 叠加 vs 软件叠加） |
| `hal_ivs` | 移动侦测、人形检测、区域入侵（能力可选） | `md_enable(sensitivity, regions)`, `hd_enable`, `poll_event(&evt)` | GK7205 用 IVE/MD 模块；RV1106 用 NPU/RKNN；mock 用帧差 |
| `hal_gpio` | IR-CUT、红外灯/白光灯、状态 LED、复位键、报警 I/O、光敏 | `set(pin_id, level)`, `get`, `pwm(pin_id, duty)`, `on_key(cb)` | 引脚映射来自产品 profile |
| `hal_net` | 以太网/WiFi（扫描、连接、状态）、MAC | `eth_status`, `wifi_scan`, `wifi_connect(ssid, psk, sec)`, `wifi_status`, `get_mac` | **WiFi 在 HAL 层通用**：统一基于 Linux nl80211 + wpa_supplicant 控制接口实现，不区分模块；模块差异（SDIO/USB 内核驱动、固件 blob、上电/复位 GPIO）下沉到 BSP 与 profile `network.wifi.module`；HAL 只需一份实现 |
| `hal_storage` | TF 卡挂载/卸载、健康、格式化 | `mount`, `umount`, `stat(total/free/health)`, `format(exfat)` | SDIO 控制器、热插拔检测方式 |
| `hal_sys` | 芯片 ID、复位、看门狗、Flash 分区与 OTA 写入、温度、内存 | `chip_id`, `reboot`, `wdt_feed`, `ota_write(slot, data)`, `ota_switch_slot`, `ota_confirm`, `get_temp`, `get_mem` | 分区布局、bootloader 双分区切换机制 |
| `hal_crypto` | 设备证书/私钥安全存储与签名（有硬件则用硬件） | `secure_read(key)`, `secure_write`, `sign(data)`, `random` | 有无 OTP/eFuse/加密引擎 |

### 3.2 帧结构（零拷贝）

```c
typedef struct {
    int      ch;            // 编码通道（0 主 1 子 …）
    uint32_t codec;         // HAL_CODEC_H264 / H265 / MJPEG / G711A / AAC
    uint64_t pts_us;        // 单调时钟微秒
    int      is_key;        // IDR
    int      fd;            // 共享内存 fd（或 -1）
    uint32_t offset, size;  // 数据在共享区中的位置
    void    *priv;          // 平台私有句柄，release 时回传
} hal_frame_t;
```

帧总线只复制该结构体；消费者用完调用 `hal_video.release_frame(&frame)`。

### 3.3 注册与选择

- 每个平台实现导出 `const hal_ops_t *hal_platform_get(void)`；`hal_ops_t` 为各模块函数表的聚合。
- 编译期通过 `PLATFORM=` 选择目录；运行期通过 `hal_video.probe_sensor()` 读取 I2C ID 匹配 `sensor/` 插件并加载对应 `tuning/` 资产，允许同一固件支持多传感器 SKU。

---

## 4. 芯片/IC 适配矩阵

### 4.1 SoC 层差异（需实现 `platform/<soc>/soc/`）

| 差异点 | GK7205V300 | GK7205V200 | RV1106G2（未来，用于验证可移植性） |
|---|---|---|---|
| 媒体 SDK | Goke MPP（Hi3516EV300 兼容）/ OpenIPC | 同 | Rockchip rkmpi |
| 最大编码 | 5MP@20 / 1080p@60（待确认） | 3MP@30 | 5MP@30 |
| WDR | 2F | 2F | 2F HDR |
| 智能 | IVE/MD | IVE/MD | NPU（RKNN） |
| 内存 | 128MB | 64MB | 128MB |
| Flash 启动 | SPI NOR/NAND | 同 | SPI NAND/eMMC |
| 音频 | 内置 Codec | 内置 | 内置 |
| PHY | 内置 FE | 内置 FE | 内置 FE |
| 硬加密 | 无（软存储 + 文件系统权限） | 无 | 有 OTP |

### 4.2 外围 IC 差异（需实现 `platform/<soc>/{sensor,wifi,codec}/`）

| IC 类别 | SP-R1-02 首发 | 备选/后续 | 适配内容 | 适配落点 |
|---|---|---|---|---|
| 图像传感器 | SC230AI（2MP，MIPI 2-lane） | SC4336/SC3335（3MP，V200 上限）；SC530AI/IMX335（5MP，需 V300） | 驱动寄存器序列、模式表（线性/HDR）、ISP 调优资产 | `platform/<soc>/sensor/`、`tuning/` |
| 镜头 | **由设备型号决定**（SP-R1-02 待定：定焦 / 电动） | 电动变焦 + AF 型号 | 马达驱动（步进/音圈）、行程标定；定焦无需适配 | profile `video.lens`；有马达时 `platform/<soc>/lens/` |
| WiFi 模块 | 待定（SDIO 或 USB） | AIC8800 / RTL8188FU / SSV6X5X | **仅内核驱动 + 固件 blob + 上电 GPIO**；用户空间统一 nl80211/wpa_supplicant，HAL 无需改动 | BSP + profile `network.wifi.module` |
| 音频 Codec | SoC 内置 | ES8388 等外置 | I2S/I2C 初始化、增益表 | `platform/<soc>/codec/` |
| IR-CUT / 补光 | GPIO 直驱 | PWM 调光 | 引脚映射 | profile `gpio_map` |
| Flash | 128MB SPI NAND | 16MB NOR（低配） | 分区表、OTA 双分区 | BSP + `hal_sys` |
| 光敏/红外传感 | ADC 或 GPIO | — | 阈值 | profile |

---

## 5. 产品能力清单（Capability Manifest）

每个产品型号一份 `profiles/<model>.json`，固件启动时加载，驱动：
- ONVIF `GetCapabilities/GetServices/GetProfiles` 返回内容；
- IDP `status.hello.capabilities/channels`；
- GB28181 `DeviceInfo/Catalog`；
- 本地控制台菜单显隐与参数范围校验；
- HAL 引脚映射与传感器白名单。

字段分组：`identity`（型号/厂商/硬件版本）、`video`（通道、编码、分辩率/帧率上限、HDR 支持）、`audio`、`ivs`、`storage`、`network`、`gpio_map`、`protocols`（各协议开关与默认端口）、`limits`（帧总线消费者上限、RTSP 并发）。示例见附录 B。

---

## 6. 构建系统与代码组织

```
ipc-firmware/
├── bsp/                      # 各厂商内核/驱动引用（子模块）：openipc-gk7205, rockchip-rv1106
├── platform/                 # L0
│   ├── gk7205v300/ {soc, sensor, wifi, codec, tuning}
│   ├── gk7205v200/
│   ├── rv1106/               # 未来
│   └── mock/                 # x86 测试
├── hal/                      # L1 接口头文件 + 注册表
├── core/                     # L2 帧总线/配置/事件/日志/网络/安全存储
├── modules/                  # L3 idp, rtsp, onvif, gb28181, recorder, ota, console, ivs, snapshot
├── profiles/                 # L4 SP-R1-02.json …
├── tools/                    # 生产烧录、证书注入、DeviceID 生成
├── tests/                    # HAL 一致性测试、协议测试、x86 单测
└── build/                    # Buildroot 外部树；make PLATFORM=gk7205v300 SENSOR=sc230ai WIFI=aic8800 PROFILE=SP-R1-02
```

- 构建基座：**Buildroot（以 OpenIPC 外部树为参考）**；业务层为独立 C 程序（可选 C++17 子模块），通过 CMake 编译为单一守护进程 + 若干可选插件 `.so`（协议模块可按 profile 裁剪以省内存）。
- CI：矩阵构建全部 `PLATFORM×SENSOR×PROFILE`；x86 `mock` 目标运行单测与协议一致性测试；产出带版本/哈希的 OTA 包。
- 版本策略：`固件版本 = 业务层版本 + 平台层版本`，OTA 包内含 `platform_id` 校验，防止刷错芯片。

---

## 7. 新芯片适配流程与完成标准

| 步骤 | 内容 | 完成标准（DoD） |
|---|---|---|
| S1 BSP 引入 | 内核、驱动、根文件系统可启动，网络可用 | 串口登录、`ping` 通 |
| S2 `hal_sys` | 芯片 ID、Flash 分区、OTA 双分区、看门狗 | HAL 一致性测试 `sys` 组通过；A/B 切换与回滚成功 |
| S3 `hal_video` + 首个传感器 | 双码流编码、抓图、IDR、动态改码率、日夜 | 一致性测试 `video` 组通过；RTSP 模块无改动即可拉流 |
| S4 `hal_audio` / `hal_osd` / `hal_gpio` / `hal_storage` / `hal_net` | 按模块 | 对应测试组通过 |
| S5 `hal_ivs` | 移动侦测（必选）、人形（可选） | 事件触发录像；误报率达阈值 |
| S6 调优资产 | ISP/传感器参数 | 图像质量基线用例（T5~T8）通过 |
| S7 产品 profile | 引脚映射、能力清单 | ONVIF DTT、GB 注册、IDP 绑定**零业务代码修改**通过 |
| S8 稼定性 | 四协议并存 24h | RSS/CPU 在预算内，无泄漏 |

**关键验证**：S7 是"通用功能一套代码"的证明点——若某芯片适配需要改动 `modules/` 或 `core/`，视为 HAL 设计缺陷，需回补接口而非打补丁。

---

## 8. 测试策略

| 层 | 测试 | 环境 |
|---|---|---|
| HAL 一致性 | 每个 HAL 模块一组黑盒用例（参数边界、并发、错误码），所有平台必须通过同一套 | 目标板 |
| 业务层单测 | 帧总线、配置中心、录像索引、OTA 状态机、IDP 状态机 | x86 mock |
| 协议一致性 | ONVIF Device Test Tool Profile S；GB28181 对接 wvp-GB28181-pro 与我方 GB Adapter；RTSP 对接 VLC/ZLM/海康 NVR；IDP 对接平台 | x86 mock + 目标板 |
| 图像质量 | 分辰率/帧率/HDR/低照/日夜（核对报告 §6 T2~T8） | 目标板 + 光学环境 |
| 稽定性/资源 | 24~72h 四协议并存 + 录像 + 断网重连 + 掉卡插卡 | 目标板 |
| 安全 | 默认密码强制修改、失败锁定、TLS 证书校验、OTA 签名校验、端口最小暴露 | 目标板 |

---

## 9. 对里程碑的影响

| 里程碑 | 调整 |
|---|---|
| M0 | 增加：冻结 HAL v1 接口（附录 A）、SP-R1-02 profile 初稿（含 `video.lens`）、代码仓库骨架与 CI 矩阵；确认 WiFi 模块型号（仅影响 BSP 驱动与内存核算） |
| M2 | 表述改为：**实现 `platform/gk7205v200`（量产，含 sc230ai、选定 WiFi 模块 BSP）+ 通用 L2/L3 + SP-R1-02 profile**；退出条件增加"HAL 一致性测试全过"、"S7 零业务改动"、**"64MB 下 RSS ≤56MB"** |
| M2b（新增，可与 M3 并行） | `platform/gk7205v300` 适配 + 高配 profile（128MB，放开 ONVIF Events 常驻、GB 回放、更多并发），**用第二个平台验证可移植性**；仅允许改 `platform/` 与 `profiles/` |
| 后续 | `platform/rv1106`（若有第二代硬件）作为跨厂商可移植性验证；`platform/mock` 从 M0 起持续维护 |

---

## 10. 风险

| # | 风险 | 缓解 |
|---|---|---|
| R1 | 厂商 SDK 闭源 blob（Goke MPP、majestic）限制 HAL 的控制粒度 | 优先直接调用 MPP 而非依赖 majestic；majestic 仅作 M0 快速验证 |
| R2 | ISP 调优不可移植，跨 SoC 图像风格差异 | 调优资产按 `soc+sensor` 管理，产品层定义图像质量验收基线 |
| R3 | 抽象层引入性能开销 | 帧零拷贝、HAL 调用无锁热路径；基准：抽象层 CPU 开销 <3% |
| R4 | HAL 接口过早固化，后续芯片特性（如 NPU）难以表达 | `hal_ivs` 以能力位声明可选特性；接口带版本号，允许向后兼容扩展 |
| R5 | 64MB 平台裁剪与 128MB 平台功能不一致导致测试矩阵膨胀 | profile 明确"低配集"，CI 只测声明的能力 |
| R6 | 团队在 M2 期间为赶进度在业务层写平台分支 | 代码评审门禁：`modules/`、`core/` 禁止包含平台头文件与 `#ifdef PLATFORM_*` |

---

## 附录 A：HAL 头文件草案

```c
// hal/hal_video.h
typedef struct {
    int  (*open)(const hal_video_cfg_t *cfg);
    int  (*close)(void);
    int  (*probe_sensor)(char *name, size_t n);            // 返回传感器型号
    int  (*set_encoder)(int ch, const hal_enc_cfg_t *cfg); // codec/w/h/fps/rc/bitrate/gop
    int  (*get_frame)(int ch, hal_frame_t *f, int timeout_ms);
    int  (*release_frame)(hal_frame_t *f);
    int  (*request_idr)(int ch);
    int  (*snapshot)(int ch, int quality, hal_frame_t *jpeg);
    int  (*set_isp_mode)(hal_isp_mode_t mode);             // LINEAR / WDR / HDR
    int  (*set_image)(const hal_image_t *img);             // brightness/contrast/saturation/sharpness/flip/mirror
    int  (*set_daynight)(hal_daynight_t mode);             // AUTO / DAY / NIGHT
    int  (*get_caps)(hal_video_caps_t *caps);              // 最大分辨率/帧率/通道数/HDR 支持
} hal_video_ops_t;

// hal/hal_sys.h
typedef struct {
    int  (*chip_id)(char *buf, size_t n);
    int  (*reboot)(void);
    int  (*wdt_feed)(void);
    int  (*ota_begin)(int slot, size_t total);
    int  (*ota_write)(const void *data, size_t n);
    int  (*ota_end)(const uint8_t sha256[32]);
    int  (*ota_switch_slot)(int slot);
    int  (*ota_confirm)(void);                              // 新固件运行正常后确认，否则 bootloader 回滚
    int  (*get_temp)(int *milli_c);
    int  (*get_mem)(uint32_t *total_kb, uint32_t *free_kb);
} hal_sys_ops_t;

// hal/hal.h
typedef struct {
    uint32_t version;                 // HAL 接口版本
    const char *platform_id;          // "gk7205v300"
    const hal_video_ops_t   *video;
    const hal_audio_ops_t   *audio;
    const hal_osd_ops_t     *osd;
    const hal_ivs_ops_t     *ivs;     // 可为 NULL：能力清单需声明无 IVS
    const hal_gpio_ops_t    *gpio;
    const hal_net_ops_t     *net;
    const hal_storage_ops_t *storage;
    const hal_sys_ops_t     *sys;
    const hal_crypto_ops_t  *crypto;  // 可为 NULL：退化为软存储
} hal_ops_t;

const hal_ops_t *hal_platform_get(void);
```

## 附录 B：SP-R1-02 能力清单示例

```json
{
  "identity": { "model": "SP-R1-02", "vendor": "IpcCloud", "hw": "A1", "platform": "gk7205v200" },
  "video": {
    "sensors": ["sc230ai"],
    "lens": { "type": "fixed", "af": false, "zoom": false },
    "channels": [
      { "ch": 0, "name": "main", "codecs": ["h265", "h264"], "max": { "w": 1920, "h": 1080, "fps": 30 }, "default": { "codec": "h265", "w": 1920, "h": 1080, "fps": 25, "kbps": 2048, "rc": "vbr" } },
      { "ch": 1, "name": "sub",  "codecs": ["h264"],         "max": { "w": 704,  "h": 396,  "fps": 30 }, "default": { "codec": "h264", "w": 640,  "h": 360,  "fps": 15, "kbps": 512,  "rc": "cbr" } }
    ],
    "isp": { "wdr": true, "hdr": true, "3dnr": true, "daynight": "icr_auto" },
    "snapshot": { "codec": "mjpeg", "max": { "w": 1920, "h": 1080 } }
  },
  "audio": { "in": ["mic", "line"], "out": [], "codecs": ["g711a", "g711u", "aac"] },
  "ivs": { "motion": true, "humanoid": true, "engine": "ive" },
  "storage": { "tf": true, "max_gb": 256, "fs": ["exfat", "fat32"] },
  "network": {
    "eth": true,
    "wifi": { "module": "TBD", "bus": "sdio", "power_gpio": 20, "bands": ["2.4g"], "security": ["wpa2", "wpa3"], "stack": "nl80211+wpa_supplicant" }
  },
  "gpio_map": { "ircut_a": 12, "ircut_b": 13, "ir_led": 14, "status_led": 15, "reset_key": 16, "light_sensor_adc": 0 },
  "protocols": {
    "idp":    { "enabled": true,  "broker": "mqtts://idp.example.com:8883" },
    "rtsp":   { "enabled": true,  "port": 554 },
    "onvif":  { "enabled": true,  "port": 80, "profiles": ["S"], "events": "on_demand" },
    "gb28181":{ "enabled": false, "transport": "tcp", "playback": false }
  },
  "limits": { "frame_bus_consumers": 4, "rtsp_sessions": 2, "playback_sessions": 1, "mem_budget_mb": 56 }
}
```

> 电动镜头型号只需改 `video.lens` 为 `{ "type": "motorized", "af": true, "zoom": true, "driver": "ms41929" }`，ONVIF Imaging Focus / PTZ Zoom 能力与控制台菜单自动开启，业务层无改动。
