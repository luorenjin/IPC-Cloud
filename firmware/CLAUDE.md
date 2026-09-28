# CLAUDE.md（firmware/）

本文件为 Claude Code 在 `firmware/` 目录（IPC 固件通用层：HAL v1 / core / profiles / mock / console）下工作时提供指导。根级规范（应答语言、PRD 溯源、硬件未冻结等）见 [`../AGENTS.md`](../AGENTS.md)。

## 状态与依据

**HAL v1 + L2 core 已实现并通过 x86 测试**；`modules/console` + `common/http_server` + `app/` 已落地；**`platform/gk7205v200/` 为倾向性骨架**（交叉编译见 `docker/`），**芯片/传感器/ISP/Flash 选型仍未冻结**——不要向 `core/` 或 `modules/` 添加芯片假设，待定项只做接口预留。

本机控制台前端源在 **`web/`**（改后必须 `scripts/gen_assets.py` 重生成 `console_assets.c`）：见 [`web/AGENTS.md`](web/AGENTS.md)。

设计依据：
- `../Docs/PRD/IPC固件平台化架构_HAL适配方案.md`
- `../Docs/PRD/IpcCloud设备接入规范_v1.0.md`
- `../Docs/PRD/决策记录与待定事项.md` —— 明确列出哪些硬件项"待定"（芯片型号、传感器 IC、是否外接 ISP、Flash 容量类型、WiFi 模块型号、音频输出），改动前先确认相关字段是否仍处于待定状态。

## 目录结构

```
firmware/
├── hal/                      L1 HAL v1 接口（10 个头文件）+ hal.c 聚合层
├── core/                     L2 核心服务（已实现）
│   ├── include/core/         os / json / log / profile / event_bus / config / frame_bus / module
│   └── src/                  对应 .c 实现（Windows + POSIX 同源）
├── modules/                  L3：console + common/http_server 已实现；idp/rtsp/… 待后续
├── web/                      本机控制台前端源（见 web/AGENTS.md）
├── app/                      主程序入口 main.c
├── profiles/                 L4 能力清单：schema/profile.v1.schema.json、SP-R1-02.json、mock-x86.json
├── platform/mock/            L0 x86 模拟平台；platform/gk7205v200/ 倾向性骨架（选型未冻结）
├── scripts/gen_assets.py     web/ → console_assets.c（手动、生成物入库）
├── tests/hal_conformance/    HAL 一致性测试（任何平台必须全过）
├── tests/core_test/          core 层单元测试
├── tests/console_test/       控制台/API 单测（仅 IPC_PLATFORM=mock）
├── docker/                   GOKE SDK 交叉编译与 rootfs 打包（见 docker/README.md）
├── docs/模块划分与依赖规则.md
└── CMakeLists.txt            -DIPC_PLATFORM=mock|gk7205v200|…  -DIPC_PROFILE=<name>
```

## 构建与测试

```powershell
# Windows / MSVC
cmake -S firmware -B firmware/build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build firmware/build-msvc --config Debug
firmware\build-msvc\tests\hal_conformance\Debug\hal_conformance.exe firmware\profiles\mock-x86.json
ctest --test-dir firmware/build-msvc -C Debug   # 一次跑全部
```

```bash
# Linux / GCC
cmake -S firmware -B firmware/build && cmake --build firmware/build
(cd firmware && ./build/tests/hal_conformance/hal_conformance profiles/mock-x86.json)
```

期望输出：`RESULT: platform=mock pass=… fail=0` 与 `RESULT: core pass=… fail=0`（用例数随测试演进变化，**`fail` 必须为 0**）。

单独运行某个测试套件：直接执行 `firmware/build*/tests/<套件名>/`（MSVC 下为 `.../Debug/<套件名>.exe`）下生成的可执行文件，传入对应 profile JSON 路径作为参数，不必通过 `ctest` 过滤器。

交叉编译 / rootfs：`docker/`（GnuWin32 make 建议 `make -f Makefile …`；宿主路径勿在 make 变量阶段用 `$(CURDIR)`，recipe 内 `$$(pwd)` 现算——见 `docker/README.md`）。

## HAL v1 一览

| 头文件 | 模块 | 必选 | 要点 |
|---|---|---|---|
| `hal_types.h` | 公共 | — | 错误码 `HAL_E*`、`hal_frame_t`（零拷贝，`release_frame` 归还）、编解码枚举、版本 `HAL_API_VERSION` |
| `hal_video.h` | 视频 | 是 | 多通道编码、`get_frame/release_frame`、`request_idr`、抓图、ISP 模式、图像参数、日夜、镜头（可选，定焦返回 `HAL_ENOTSUP`） |
| `hal_audio.h` | 音频 | 否 | 采集（PCM/G.711/AAC）、播放、增益/AEC/AGC/NS |
| `hal_osd.h` | OSD | 否 | 文本/时间/位图区域，HAL 内渲染 |
| `hal_ivs.h` | 智能 | 否 | 移动/人形/入侵/越界/遮挡，轮询事件 |
| `hal_gpio.h` | GPIO | 是 | 逻辑引脚（映射来自 profile），PWM，ADC，按键事件 |
| `hal_net.h` | 网络 | 是 | 以太网/WiFi 状态、扫描、连接（Linux 上统一 nl80211/wpa_supplicant 实现） |
| `hal_storage.h` | TF | 是 | 挂载/健康/格式化/插拔事件 |
| `hal_sys.h` | 系统 | 是 | 芯片信息、单调时钟、看门狗、OTA A/B（begin/write/end/switch/confirm） |
| `hal_crypto.h` | 安全 | 否 | 安全存储（私钥不可读）、设备证书、签名、随机数 |
| `hal.h` | 聚合 | — | `hal_ops_t` 函数表、`hal_platform_get()`（平台导出）、`hal_init/hal/hal_has/hal_strerror` |

## 分层规则（CMake 强制，非靠约定）

`CMakeLists.txt` 会扫描 `core/*.c core/*.h modules/*.c modules/*.h`（含生成的 `console_assets.c`），一旦发现 `#include "platform/` 或芯片宏（`PLATFORM_|GK7205|RV1106|HI35...`）即在 configure 阶段 `FATAL_ERROR` 失败。**不要为了绕过这个门禁而把平台相关代码塞进 `core/`/`modules/`**——应在 `platform/<soc>/` 下新增 HAL 操作或平台实现，或在 `hal/` 增加新的通用接口。前端资源文案里也不要出现 `GK7205` 等字样。

## 平台实现约定（新增芯片平台时遵守）

1. 新建 `platform/<soc>/`，提供 `CMakeLists.txt` 生成静态库 `ipc_platform`，导出 `const hal_ops_t *hal_platform_get(void)`。
2. `hal_ops_t.version` 填 `HAL_API_VERSION`；`platform_id` 与 profile `identity.platform` 一致。
3. 可选模块不支持时置 `NULL`，并在 profile 中同步声明（如 `ivs.engine="none"`）。
4. 帧数据不得拷贝到业务层；`get_frame` 返回引用，`release_frame` 归还。
5. 必须通过 `tests/hal_conformance`；不得为通过测试修改 `core/`、`modules/`（CMake 门禁会拒绝平台头/平台宏进入通用层）。

## 能力清单（profile）

- Schema：`profiles/schema/profile.v1.schema.json`（JSON Schema 2020-12）。
- 关键字段：`identity.platform`、`video.lens`（由型号决定定焦/电动）、`video.channels[].max/default`、`ivs.engine`、`network.wifi.module`（模块型号仅影响 BSP 驱动）、`gpio_map`、`protocols.*.enabled`、`limits.mem_budget_mb`。
- 校验：CI 中用任意 JSON Schema 校验器（如 `ajv`）校验 `profiles/*.json`；固件启动时由 `core/profile` 做关键字段校验。

## L2 core 已实现

| 服务 | 文件 | 说明 |
|---|---|---|
| OS 抽象 | `core/src/os.c` | 线程/互斥/条件变量/单调时钟/原子文件替换，Win32 与 POSIX 同源 |
| JSON | `core/src/json.c` | DOM 解析/查询(点分路径)/构建/序列化，含错误偏移 |
| 日志 | `core/src/log.c` | 分级+模块级、环形缓冲（供诊断上传）、`log_mask` 脱敏 |
| 能力清单 | `core/src/profile.c` | 校验后**原子提交**（失败不破坏现有配置）、能力字符串导出 |
| 事件总线 | `core/src/event_bus.c` | 环形队列 + 单投递线程 + 域掩码订阅，回调锁外执行 |
| 配置中心 | `core/src/config.c` | 规则校验（`*` 通配一层）、profile 播种、原子持久化、批量 apply 带拒绝列表、前缀聚合读取 |
| 帧总线 | `core/src/frame_bus.c` | 每通道单生产者、引用计数零拷贝、慢消费者丢最旧、按需 start/idle stop；按 HAL `max_held_frames` 收紧环深 |
| 模块启动器 | `core/src/module_loader.c` | 拓扑排序、enabled 过滤、init/start/stop/deinit、内存预算检查、health 失败自动重启 |

## 下一步

见 `docs/模块划分与依赖规则.md` §8 与 `Docs/superpowers/specs/2026-09-07-ipc-local-web-console-design.md`（A 线 Task 9–11：flv_mux / WS-FLV / OTA 仍待 video HAL）。已落地：console + 静态前端 + `app/` + `platform/gk7205v200` 交叉编译骨架。硬件选型冻结前勿扩 `core/`/`modules/` 芯片假设。
