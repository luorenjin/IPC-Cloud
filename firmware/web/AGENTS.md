# firmware/web — 本机控制台前端 (AGENTS.md)

根级规范见 [`../../AGENTS.md`](../../AGENTS.md)；固件分层/构建见 [`../CLAUDE.md`](../CLAUDE.md)。本文件只写 `firmware/web/` 及其嵌入链路。

## 这是什么

设备本机 Web 管理端的**源文件**（原生 JS 单页，无框架、无构建、无 CDN、无 npm）。与 `platform/web`（云平台 Nuxt）**完全无关**，不要混用依赖或组件。

- 入口：`index.html` → `js/core.js` → `ui` / `router` / `views/*` → `app.js`
- 运行：浏览器直开 `index.html`（`file://` 可用），或经 `modules/console` 内嵌静态资源访问
- 设计依据：`Docs/superpowers/specs/2026-09-07-ipc-local-web-console-design.md`、`2026-09-21-控制台真机落地-design.md`

## 改完必须重新生成内嵌资源

**`console_assets.c` 不是手写文件，也不会被 CMake 自动再生成。**

```bash
# 仓库根目录执行
python firmware/scripts/gen_assets.py firmware/web firmware/modules/console/console_assets.c
```

- 改 `firmware/web/**` 任意文件后**必须**跑上面命令，并把 `console_assets.c` 一并提交。
- 跑完后确认生成物条目数 ≈ `firmware/web` 下文件数（当前源侧已有 `js/views/*`、`assets/*` 等；若 `s_assets` 仍只有 `/index.html` `/app.js` `/style.css`，说明**未再生成**，真机会 404/兜底回 `index.html`）。
- gzip 用 `mtime=0`：相同输入 → 相同字节，避免无意义 diff。
- 生成物头注释：**请勿手工编辑** `console_assets.c`。

## 分层门禁（生成物也会被扫）

`firmware/CMakeLists.txt` 在 configure 阶段扫描 `core/**` 与 `modules/**`（含 `console_assets.c`）：出现 `#include "platform/` 或芯片宏 `PLATFORM_|GK7205|RV1106|HI35…` → **FATAL_ERROR**。

- 前端资源内容里**不得**出现 `GK7205` 等字样（文案、注释、示例 IP 命名都要避开）。
- 不要为绕过门禁把平台相关字符串塞进 `web/`。

## 本地怎么验证

| 方式 | 用途 |
|---|---|
| 直接打开 `firmware/web/index.html` | 改 UI 时最快；部分 API 会失败，属预期 |
| mock 构建 + `console_test` / 起 `ipc_app --profile profiles/mock-x86.json` | 验静态资源与 `/api/v1/*` 契约 |
| 真机（gk7205v200 rootfs） | 登录 → 配置读写 → 网络/系统页；**嵌入后**再冒烟一次 |

无独立单测 runner、无 lint。改完至少：重新 `gen_assets.py` → `cmake --build`（mock）→ ctest 或直接开浏览器走关键路径。

## 前端结构约定

- 全局命名空间 `window.IPC`：`S` 状态、`page(id, fn)` 注册页、`render()` 由 `router.js` 提供。
- 页面插件在 `js/views/*.js`，通过 `IPC.page('pImage', …)` 注册；`router.js` 的 `TAB_PAGE` 把页签映到 id——**加页要同时改 views 与 TAB_PAGE**。
- 文案当前硬编码中文（无 i18n 目录）；与 `platform/web` 的 `locales/` **无关**。
- 预览图：`assets/preview-still.jpg` 占位；实时 WS-FLV / h265web **本期未接**（真机 milestone 明确非目标）。
- **能力驱动 UI（R0）**：`GET /api/v1/system/info` 返回 `caps`（硬件）+ `features`（功能合成）+ `modules`（模块态）。
  - 前端用 `IPC.feat(id)` / `IPC.S.features` 决定导航与页签显隐，**禁止**在 JS 写死型号/芯片能力。
  - `router.js` 的 `NAV[].feat`、`SUB_FEAT`、`TOP_FEAT` 是功能 ID → 入口映射表；**新增入口必须挂 feature ID**。
  - `file://` 本地预览时 features 为 null，走开发态全量树；一旦 API 返回，false 必须隐藏（无假开关）。
  - 功能 ID 与 `console_api.c` 的 `build_features_object` 一一对应，改一侧必须同步另一侧。
- **能力与模块管理（R1）**：页签在「系统设置 → 能力与模块」（`js/views/modules.js`）。
  - 数据源 `GET /api/v1/system/capabilities`（features 明细 + modules 目录）。
  - 模块开关写 `module.<name>.enabled`（`console_api_register_rules` 登记，**需重启生效**）。
  - 目录在 `console_api.c` 的 `MODULE_CATALOG`：加模块必须同步目录、profile 门控与前端。

## 与后端契约

- REST：`/api/v1/config`、`/api/v1/system/*`、鉴权 `/api/v1/auth/*`（`console_api.c` / `console_auth.c`）。
- 配置键走 `core/config`（`register_common_rules` + profile seed）；**不要**在前端另造一套校验/键名。远程配置三处对齐见根 `AGENTS.md`（平台 / 模拟器 / 固件规则表）——本地控制台读写的是**同一套** `cfg_*` 键。
- 静态路由：`console_static.c` 注册 `/` 兜底，未命中 → `index.html`；响应恒带 `Content-Encoding: gzip` + ETag。

## 易踩坑清单

1. 只改 `web/` 不跑 `gen_assets.py` → 设备/测试二进制仍是旧资源。
2. 手改 `console_assets.c` → 下次生成被覆盖，且易引入门禁宏。
3. 在 `web/` 引 npm / 外链 CDN → 设备无外网、无 node 构建链。
4. 与 `platform/web` 混淆 → 那是云平台 SPA，依赖、COOP/COEP、h265web 都不同。
5. 生成物含 `GK7205` 等 → CMake configure 直接失败。
