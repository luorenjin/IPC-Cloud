# IpcCloud 工程智能体规范 (AGENTS.md)

面向在本仓库工作的编码智能体（Claude Code / OpenCode 等）。本文件是仓库根级唯一规范正文；根目录 `CLAUDE.md` 仅作指针。深入子系统前先读：[`platform/CLAUDE.md`](platform/CLAUDE.md) / [`firmware/CLAUDE.md`](firmware/CLAUDE.md) / [`firmware/web/AGENTS.md`](firmware/web/AGENTS.md) / [`simulator/CLAUDE.md`](simulator/CLAUDE.md)。

## 语言与文档溯源

- **全中文**：回答、代码注释、提交信息、错误信息、前端 UI 文案均用简体中文；技术专有名词（Qt6, C++, libcamera, DMA, YUV, JSON, ISP, Sobel, Debian, ZLMediaKit 等）保持原样。
- **有效规格只在 `Docs/PRD/` 的本项目文档**：`IpcCloud平台PRD_v1.0.md`（LIVE-02 / MGR-05 / ALM-03 等编号）、`IpcCloud设备接入规范_v1.0.md`（§8.3 流命名）、`IPC固件平台化架构_HAL适配方案.md`、`决策记录与待定事项.md`。
- **竞品存档陷阱**：`Docs/PRD/README.md`、`Docs/PRD/用户手册/`、`Docs/PRD/问题指南/`、`Docs/PRD/版本更新/` 是 TP-LINK 爬取存档（详见 `TP-LINK商云分析报告_IPC平台PRD参考.md`），**不是**本项目规格，严禁当需求执行。
- **硬件基线已冻结（2026-09-21）**：GK7205V200 + GC2053 + 16MB SPI NOR（单槽 OTA）+ 本 SKU 无 WiFi，见《决策记录与待定事项.md》§2.2 与 `硬件基线冻结与文档排查_GK7205V200_GC2053.md`；§2.3 仍待定项（镜头焦距、DDR 预算、加密器件型号等）只做接口预留。芯片差异仍只进 `firmware/platform/<soc>/`，`core/`/`modules/` 保持芯片无关。

## 仓库边界与验证方式

三个独立产品共享同一 PRD（`Docs/PRD/`），**无 monorepo 工作区**（无 `go.work`、无根 package.json）。典型协作：改 `platform/server/internal/adapter/*` 后用 `simulator` 对应协议闭环验证；`firmware` 独立演进，只在协议/接入规范层与 `platform` 共享设计文档。

| 目录 | 技术栈 | 入口 / 关键路径 |
|---|---|---|
| `platform/server` | Go 1.25 + Gin + GORM | `cmd/ipccloud/main.go`；路由权威：`internal/api/router.go` |
| `platform/web` | Nuxt3 SPA（`ssr: false`）+ Tailwind + Reka UI | `nuxt.config.ts`；直播/回放：`pages/live.vue`、`pages/playback.vue` |
| `firmware` | C11 + CMake | 根 `CMakeLists.txt`；`-DIPC_PLATFORM=` / `-DIPC_PROFILE=`；本机控制台源：`firmware/web/`（见 [`firmware/web/AGENTS.md`](firmware/web/AGENTS.md)） |
| `simulator` | Go 1.25 | `main.go`（`-mode=all\|idp\|gb\|onvif\|rtsp`） |

**本仓库没有统一 lint / typecheck / formatter，也没有 monorepo 级 test runner。** 验证按子系统：

```bash
# 平台全栈（推荐；含 simulator 跟随启动）
cd platform && docker compose up -d --build

# 本地分启
cd platform/server && go run ./cmd/ipccloud
cd platform/web && npm install && npm run dev   # 仅 dev/build/generate/preview

# 服务端：少量单测（media/engine/rtsp/timeutil 等），非全量套件
cd platform/server && go test ./...
cd platform/server && go build ./... && go vet ./internal/... && go test ./...   # 改后端后的常规门禁
# 单包：cd platform/server && go test ./internal/media/

# 前端改文案后必须跑（须在 platform/web 下执行；结果写 i18n-check.txt）
cd platform/web && python scripts/i18n-check.py

# 固件（Windows/MSVC）
cmake -S firmware -B firmware/build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build firmware/build-msvc --config Debug
ctest --test-dir firmware/build-msvc -C Debug
# 单套件直接跑 build*/tests/<套件>/ 可执行文件 + profile JSON，不必用 ctest 过滤
# 门禁：mock 平台 hal_conformance fail 必须为 0；gk7205v200 以「fail 数不高于基线」为准（看门狗/OTA/视频为已知未实现）

# 固件真机（GK7205V200，串口 COM6；交叉编译 cd firmware/docker && make -f Makefile fw-all）
python firmware/tools/board.py deploy-app       # 只换 ipc_app，约 5s，保留配置与激活状态
python firmware/tools/board.py reflash-rootfs   # 改了 rootfs（开机脚本/busybox）时用，保留 MAC/IP
python firmware/tools/console_e2e.py            # 控制台真机浏览器验收（13 用例，系统 Chrome）

# 模拟器（对接本地平台）
cd simulator && go run . -mode=all -broker=tcp://127.0.0.1:1883 -platform=http://127.0.0.1:8080 -assets=./assets
```

- `platform/` **没有**端到端自动化测试：改协议/起停流后应用 simulator 闭环（接入-鉴权-心跳-起播-告警），不要只靠静态走读或 `go test` 下结论。
- 改前端音视频路径后必须在真实浏览器验证直播/回放，构建通过≠功能正确。
- 默认账号 `admin` / `Admin@12345`；媒体节点 apiUrl=`http://zlm:80`，secret 见 `platform/zlm/config.ini` 的 `api.secret`。
- 仓库根 `.gitignore` 含全局 `*.txt`（仅有 `!CMakeLists.txt` 例外）：**新增 `.txt` 会被静默忽略**；i18n 报告 `platform/web/scripts/i18n-check.txt` 属预期产物。

## 服务端协议与流（易踩坑）

- **来源判别字面值**：`idp`、`gb28181`、`onvif`、`rtsp` 贯穿 `models` / `adapter` / `engine` 与 simulator——追协议行为要搜字符串本身，不是只搜类型名。
- **适配器隔离**：协议差异只留在 `internal/adapter/*`，实现 `adapter.Adapter`，在 `cmd/ipccloud/main.go` 里 `adapter.Register(...)`；查找用 `adapter.Get(dev.Source)`。协议私有逻辑不得溢出到 adapter 外。
- **流命名（PRD §8.3，改一处必须全链路对齐）**。命名逻辑**内联在** `StartPlay` / `StopPlay` 的 `switch dev.Source`（`engine/engine.go`；`streamNames`/`pullURL` 为辅助，`record.go` 等处还有平行构造）：
  - `idp`：app=`live`，stream=`{deviceID}_{channelIdx}_{profile}`
  - `gb28181`：app=`rtp`，stream 取 `channel.Meta["gbStream"/"gbStreamSub"]`（meta **已含** profile 后缀），缺省 `{gbChannelId}_{profile}`
  - `onvif` / `rtsp`：app=`proxy`，stream=`{channelID}_{profile}`
- **`StartPlay` / `StopPlay` 的 switch 刻意并行、不合并抽象**：各协议生命周期不同（推流 vs 拉流 vs RTP INVITE）。新增 `Source` 必须同时改这两处、`streamNames` / `pullURL`，以及录像/抓图等所有按 source 分支的地方。
- **远程配置键三处逐字对齐**（否则「保存成功但没生效」）：平台 `api/devices.go` 的 `cfgKeys` ↔ 模拟器 `simulator/idp/config.go` 的 `cfgRules`（且 `cfgDefaults` 必须覆盖全部 `cfgRules`）↔ 固件 `firmware/core/src/config.c` 的 `register_common_rules` + video pattern。键名写错只进 `cfg.set` 的 `rejected[]`，不报错。类型、长度与字符集（固件 `cfg_rule_t.charset`，如 `time.ntp.server`、`time.timezone`）同样三处一致。
- 路由与权限档位以 `internal/api/router.go` 为唯一权威：`AuthMiddleware` → `requireProjectID` → `requirePerm("view"|"config"|"preview"|"ptz"|"playback"|"delete")`。

## 前端（`platform/web`）

- 纯 SPA；REST/WS 一律走 `composables/useApi.ts`、`useAuth.ts`、`useWs.ts`，**禁止**绕过拦截器直接 `fetch`。
- UI：**Tailwind CSS 4 + 自维护 `components/ui/*`（Reka UI 原语）**，**不是** Element Plus（历史文档里若仍写 Element Plus，以 `package.json` 为准）。
- 新增/改动文案：`locales/zh-CN/*.ts` 与 `locales/en/*.ts` **同步成对**，然后 `python scripts/i18n-check.py`（键一一对应、无重复、无死键）。
- `nuxt.config.ts` 的 **COOP `same-origin` + COEP `require-corp` 不可删改**：h265web.js 多线程 WASM / SharedArrayBuffer 硬依赖；COEP 还会影响跨源资源（回放走同源 `/media` 代理）。
- `API_ORIGIN` 决定后端代理；Docker 内另有运行时 `WS_UPSTREAM`（`server/plugins/ws-proxy.ts`），缺失会导致 WS 握手失败（回落到容器自身 `localhost:8080`）。
- 播放器运行时资源在 `public/vendor/h265web.js`（及 `h265web_wasm.*` / `ext*.js`），**已入库**；缺失时仅播放器报错，其余功能可用。
- `pages/live.vue`、`pages/playback.vue` 与 `server/internal/engine` 耦合最深，改动前先读 `api/records.go`、`engine/playback.go`。

## 固件（`firmware/`）

- **CMake 硬门禁（configure 即 `FATAL_ERROR`）**：`core/`、`modules/` 禁止 `#include "platform/..."` 或芯片宏（`PLATFORM_|GK7205|RV1106|HI35...`）。平台逻辑只进 `platform/<soc>/`，导出 `const hal_ops_t *hal_platform_get(void)`。
- **零拷贝**：帧引用计数下传，用完必须 `release_frame`；禁止把帧数据拷进业务层。
- 测试门禁：改固件后 `tests/hal_conformance`（及 ctest）**fail=0**；不得为过测改 `core/`/`modules/` 绕门禁。
- `core_test` / `console_test` 只在 `IPC_PLATFORM=mock` 注册；交叉编译默认 `BUILD_TESTING=OFF`（非 mock）。
- 控制台嵌入前端：改 `firmware/web/` 后必须 `python firmware/scripts/gen_assets.py firmware/web firmware/modules/console/console_assets.c`（不进 CMake）；勿手改生成物。细节见 [`firmware/web/AGENTS.md`](firmware/web/AGENTS.md)。
- 平台层用 `system()` 拉起的常驻进程（udhcpc / ntpd）会继承 fd：新建 socket / epoll 必须设 CLOEXEC（见 `http_server.c` 的 `set_cloexec`），否则 `ipc_app` 重启后 8080 仍被占用。拼进 shell 命令或写进 `/etc/ipc/net.conf` 的值，console 与平台两层都要做字符白名单。
- 板端脚本（`firmware/docker/rootfs-overlay/**`）必须 LF（`firmware/.gitattributes` 已强制），串口输出用英文——串口终端按 GBK 显示，中文会乱码。

## 模拟器契约（`simulator/`）

改 `simulator` 或 `platform/server/internal/adapter/<同名协议>/` 任一侧时，**必须同步另一侧假设**（IDP 17 位设备号、GB28181 20 位国标编码与密码、鉴权参数、流命名、cfg 键/类型/范围等）。单侧改完会造成本地联调“看起来正常、生产失败”。

## 路径怪癖（历史问题，当前磁盘已干净）

- **2026 复核**：`D:\JetsCam\IpcCloud` 目录名字节为纯 ASCII `IpcCloud`，**不再含 U+200C**；仓库内子路径扫描亦为 0。父目录有修复脚本 `D:\JetsCam\fix-repo-path.ps1`（把尾部 ZWNJ 目录名改回干净名）。
- `firmware/docker/Makefile`、`firmware/docker/README.md`、`hal_conformance/CMakeLists.txt` 等注释**仍按“路径曾含 ZWNJ”描述**——那是历史实测结论，对应防御写法仍建议保留：**不要**把仓库绝对路径写进 Make 变量或在 make 变量替换阶段展开 `$(CURDIR)`/`$(shell pwd)`；recipe 内用子 shell 的 `$$(pwd)` 现算。GnuWin32 make 对特殊字符路径的脆弱性与 ZWNJ 是否仍在无关，硬编码绝对路径仍不安全。
- 若将来路径再次被改脏，用 `fix-repo-path.ps1` 校验/重命名，勿只改文档。
