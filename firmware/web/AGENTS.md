# firmware/web — 本机控制台前端 (AGENTS.md)

根级规范见 [`../../AGENTS.md`](../../AGENTS.md)；固件分层/构建见 [`../CLAUDE.md`](../CLAUDE.md)。本文件只写 `firmware/web/` 及其嵌入链路。

## 这是什么

设备本机 Web 管理端的**源文件**（原生 JS 单页，无框架、无构建、无 CDN、无 npm）。与 `platform/web`（云平台 Nuxt）**完全无关**，不要混用依赖或组件。

- 入口：`index.html` → `js/crypto.js` → `js/core.js` → `ui` / `router` / `views/*` → `app.js`
- 运行：经设备（或本地 mock `ipc_app`）内嵌静态资源访问。`file://` 只能看壳层——激活、登录、保存都要设备接口
- 设计依据：`Docs/superpowers/specs/2026-09-07-ipc-local-web-console-design.md`、`2026-09-21-控制台真机落地-design.md`

## 功能取舍：对标 TP-LINK 实机（强制，详见根 `AGENTS.md`）

- 对标基准 = TP-LINK 摄像头实机控制台（`http://172.16.1.180/`，登录凭据向用户索取，勿写入仓库文件）；设置/功能模块按**通用能力**呈现，当前设备虽是 IDP 自研摄像头，也**先照实机对齐，不要隐藏功能模块**。
- **认为对标功能不合理时不得自行删除/隐藏/改语义**：先向用户说明「实机怎样、我们怎样、我为何觉得不合理、建议怎么办」，拿到确认后再改；未确认前保持原样。
- 可自行处理的只有纯样式微调、既有 `IPC.feat(id)` 能力门控（设备未上报即隐藏）、以及用户已授权的重构。
- **先懂功能再对齐**：逐项对齐前先弄清①业务场景（这选项解决什么问题）②参数语义与原理（范围/单位/默认值/联动，及背后的机制，如码率控制、侦测灵敏度、录像触发、NTP/时区）③本项目落点（`cfg_*` 键 + feature/module ID + `console_api.c` 端点，还是**暂无后端能力**）。查 PRD 或问用户，别照抄控件却不接后端、别猜默认值；多项对齐先做「参数 → 语义 → 原理 → 键/ID → 现状」清单。

## 改完必须重新生成内嵌资源

**`console_assets.c` 不是手写文件，也不会被 CMake 自动再生成。**

```bash
# 仓库根目录执行
python firmware/scripts/gen_assets.py firmware/web firmware/modules/console/console_assets.c
```

- 改 `firmware/web/**` 任意文件后**必须**跑上面命令，并把 `console_assets.c` 一并提交。
- **gzip 总量预算 400KB（PRD §6，2026-09-29 由 200KB 上调）**：资源 gzip 后会**内嵌进 `ipc_app`**，直接吃 rootfs（16MB NOR，rootfs 分区 10M，实测可用 ~2.8M）。`gen_assets.py` 已带门禁——超 400KB 打印错误并返回非 0，用到 90% 打印警告；当前实测约 190KB（47%）。删资源/压资源时别顺手改预算，要改先量 flash。
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
| 直接打开 `firmware/web/index.html` | 只看样式/布局；登录与所有 API 都会失败 |
| mock 构建后起 `ipc_app --profile profiles/mock-x86.json --port 28080` | 本地走完整流程（激活/登录/保存）。**别用 18080**：`http_server_test` 占用该端口 |
| `python firmware/tools/board.py deploy-app` + `python firmware/tools/console_e2e.py` | 真机部署 + 13 用例浏览器验收（系统 Chrome，playwright `channel="chrome"`） |

无独立单测 runner、无 lint。改完至少：重新 `gen_assets.py` → `cmake --build`（mock）→ ctest → 浏览器走关键路径（确认没有页面脚本错误）。改了 `js/crypto.js` 还要跑其 `selfTest()`（与 `console_test` 的 `test_proof_cross_vector` 共用向量）。

## 鉴权与请求（一律走 core.js）

- 请求用 `IPC.api(method, path, body)`：401 / -101 自动回登录页（先查 `auth/state`，未激活则进激活页）；-3 为登录锁定。**不要**裸 `fetch`（`auth/state` 探测除外）。
- 写配置 `IPC.saveCfg(obj)`（`rejected_total>0` 即失败并列出键）；读配置 `IPC.getCfg(prefix)`。
- 激活 / 登录 / 改密走 `IPC.auth.*`：挑战-应答，口令不出浏览器。局域网 http 不是安全上下文，**没有 WebCrypto**，PBKDF2/HMAC 由 `js/crypto.js` 纯 JS 实现。
- 口令长度按 UTF-8 **字节** 8–63：用 `IPC.pwdError()`，不要用 `str.length` 或 `maxlength`（22 个汉字 length=22 但有 66 字节）。
- `IPC.ui.saveRow(onSave)`：`onSave` 返回 Promise，resolve 后才提示（resolve 字符串则以它为提示，用于转述设备回复）；**不传 `onSave` 就不渲染保存按钮**——禁止只弹 toast 的假保存。

## 前端结构约定

- 全局命名空间 `window.IPC`：`S` 状态、`page(id, fn)` 注册页、`render()` 由 `router.js` 提供。
- **预览只有两种接法，别自己开 WebSocket**（接流/断流由 `core.js` 的 `attachPreviews` / `detachPreviews` 统一管，`router.js` 在换页前后各调一次）：
  - 子码流（MJPEG）→ `<img data-preview="sub">`；
  - **主码流（H.264 裸流）→ `<video data-preview-h264="/ws/v1/preview?stream=main" muted playsinline>`**，由 `js/preview-player.js` transmux 到 MSE。**写成 `img data-preview="main"` 一定黑屏**——主码流帧是 `'K'/'P' + Annex-B`，`<img>` 解不了（实测 `naturalWidth` 恒 0）。
  - 板端预览是**单消费者**：同一时刻只能有一条，所以换页/换码流必须先 `detachPreviews()` 再接新的。
- 页面上有**暂无设备端接口**的控件时（如图像页的曝光/白平衡/补光设置），按根 `AGENTS.md` 保持可见，但**必须在页面上如实标注"不会下发"**，不得做成假开关；真接上的项要能走 `IPC.saveCfg` / `IPC.api` 并有 cfg 键。

- 页面插件在 `js/views/*.js`，通过 `IPC.page('pImage', …)` 注册；`router.js` 的 `TAB_PAGE` 把页签映到 id——**加页要同时改 views 与 TAB_PAGE**。
- 文案当前硬编码中文（无 i18n 目录）；与 `platform/web` 的 `locales/` **无关**。
- 预览图：`assets/preview-still.jpg` 占位；实时 WS-FLV / h265web **本期未接**（真机 milestone 明确非目标）。
- **能力驱动 UI（R0）**：`GET /api/v1/system/info` 返回 `caps`（硬件）+ `features`（功能合成）+ `modules`（模块态）。
  - 前端用 `IPC.feat(id)` / `IPC.S.features` 决定导航与页签显隐，**禁止**在 JS 写死型号/芯片能力。
  - `router.js` 的 `NAV[].feat`、`SUB_FEAT`、`TOP_FEAT` 是功能 ID → 入口映射表；**新增入口必须挂 feature ID**。
  - `IPC.feat(id)` 只在设备上报 `true` 时可见，**未上报的 ID 同样隐藏**。没有后端实现的入口挂一个设备不会上报的 ID（如 `network.ports`、`network.ftp`、`system.diag`），就会一直隐藏；后端实现后再上报。
  - 功能 ID 与 `console_api.c` 的 `build_features_object` 一一对应，改一侧必须同步另一侧。
- **能力与模块管理（R1）**：页签在「系统设置 → 能力与模块」（`js/views/modules.js`）。
  - 数据源 `GET /api/v1/system/capabilities`（features 明细 + modules 目录）。
  - 模块开关写 `module.<name>.enabled`（`console_api_register_rules` 登记，**需重启生效**）；只有固件里已注册的模块 `toggleable=true`，其余开关禁用。
  - 目录在 `console_api.c` 的 `MODULE_CATALOG`：加模块必须同步目录、profile 门控与前端。

## 与后端契约

- REST：`/api/v1/config`、`/api/v1/system/{info,status,capabilities,log,time,net/apply,reboot,reset}`、`/api/v1/storage/info`、鉴权 `/api/v1/auth/*`（`console_api.c` / `console_auth.c`）。
- 会改地址或断连的操作（重启、恢复出厂、网络应用）设备先回响应再执行：前端要显示等待遮罩并轮询 `auth/state`，改静态 IP 后按 `new_ip` 跳转；配置未变化时设备返回 `unchanged:true`。
- 配置键走 `core/config`（`register_common_rules` + profile seed）；**不要**在前端另造一套校验/键名。远程配置三处对齐见根 `AGENTS.md`（平台 / 模拟器 / 固件规则表）——本地控制台读写的是**同一套** `cfg_*` 键。
- 静态路由：`console_static.c` 注册 `/` 兜底，未命中 → `index.html`；响应恒带 `Content-Encoding: gzip` + ETag。

## 易踩坑清单

1. 只改 `web/` 不跑 `gen_assets.py` → 设备/测试二进制仍是旧资源。
2. 手改 `console_assets.c` → 下次生成被覆盖，且易引入门禁宏。
3. 在 `web/` 引 npm / 外链 CDN → 设备无外网、无 node 构建链。
4. 与 `platform/web` 混淆 → 那是云平台 SPA，依赖、COOP/COEP、h265web 都不同。
5. 生成物含 `GK7205` 等 → CMake configure 直接失败。
6. 用 Python 脚本批量改 `js/*.js` 时注意换行转义：`'\n'` 被写成真换行会让整页脚本报错（改完 `node --check` 每个文件）。
