# CLAUDE.md（platform/）

本文件为 Claude Code 在 `platform/` 目录（IpcCloud 云平台：服务端 + Web）下工作时提供指导。根目录总览见 `../CLAUDE.md`。

## 应答语言

必须使用中文应答用户（代码、标识符、命令行等技术内容保持原样）。

## 架构

```
web (Nuxt3 + Element Plus + h265web.js)
  └─ REST / WS ──▶ server (Go + Gin + GORM)
                     ├─ adapters: idp(MQTT) | gb28181(SIP) | onvif | rtsp
                     ├─ engine: 起播编排 / ZLM Hook / 告警 / 录像计划
                     ├─ media: ZLM 客户端 + 节点调度（粘性 + 最小负载）
                     ├─ PostgreSQL / Redis
                     └─ ZLMediaKit 节点 (WS-FLV / RTP / RTMP)
```

## 运行 / 构建

```bash
# 通过 Docker Compose 启动完整技术栈（Postgres、Redis、EMQX、ZLMediaKit、server、web、simulator）
cd platform && docker compose up -d --build
```
- Web：http://localhost:3000（默认账号 admin / Admin@12345）
- 服务端 API：http://localhost:8080/api/v1
- ZLM API：http://localhost:8081，EMQX Dashboard：http://localhost:18083
- 首次登录后按设置向导添加媒体节点（apiUrl 填 `http://zlm:80`，secret 见 ZLM 容器 `config.ini` 的 `api.secret`，publicHost 填宿主机 IP）。

```bash
# 不使用 Docker 的本地开发
cd platform/server && go run ./cmd/ipccloud
cd platform/web && npm install && npm run dev
```

`platform/` 目前没有自动化测试套件——需通过实际运行技术栈来验证行为，最好配合 `../simulator` 一起联调（见 `../simulator/CLAUDE.md`）。修改前端后应实际在浏览器里走一遍直播/回放等关键路径，不要只凭类型检查判断功能是否正常。

## 服务端（`server/`）

模块：`github.com/jetscam/ipccloud/server`（Go 1.25，Gin、GORM、paho.mqtt.golang、gorilla/websocket、go-redis）。入口：`cmd/ipccloud/main.go`。

### 目录结构（`server/internal/`）

- `api/` — Gin HTTP 处理器 + `router.go`（全部路由与权限的唯一权威来源）。中间件链为 `AuthMiddleware()` → `requireProjectID()` → `requirePerm("view"|"config"|"preview"|"ptz"|"playback"|"delete")`。新增接口时先在 `router.go` 里确认路由风格与权限档位，再写 handler。
- `adapter/` — 实现统一 `adapter.Adapter` 接口的协议适配器（`Source()`、`Start/Stop`、`StartStream/StopStream/Snapshot`、`QueryRecords/StartPlayback/PlaybackCtrl/StopPlayback`，以及设备管理 `Reboot/Diagnose/Transfer/Unbind`）。每种协议一个子包：`idp`（自研 MQTT+RTMP 私有协议）、`gb28181`（SIP/国标）、`onvif`、`rtsp`。适配器将所有差异归一化为统一的设备/事件模型——协议差异不得越过此接口向外泄漏。通过 `cmd/ipccloud/main.go` 中的 `adapter.Register()` 注册，用 `adapter.Get(dev.Source)` 查找。
- `engine/` — 编排核心：
  - `engine.go`：`StartPlay`/`StopPlay`（按需起播，选节点、发起设备侧推流或代理拉流、等待出流、签发播放 token）、事件订阅（把总线事件 `device.offline`、`alarm.*`、`node.status`、`ota.progress` 转化为平台告警记录）。
  - `playback.go`：录像回放会话。
  - `record.go`：录像计划执行器。
  - `hooks.go`：ZLM Hook 处理（register/unregister/stream-not-found 等），无鉴权，靠内网 + ZLM 侧 secret 信任。
- `media/` — `zlm.go`（ZLM HTTP API 客户端：openRtpServer、addStreamProxy、getSnap 等）、`scheduler.go`（节点选择：通道粘性 + 最小负载）、`token.go`（签名的播放/推流 token）。
- `bus/` — 内部发布订阅事件总线；`wshub/` 将其桥接到面向前端的 `/ws/v1/events` WebSocket 端点。
- `models/` — GORM 模型；设备携带 `Source` 判别字段（`idp|gb28181|onvif|rtsp`），驱动 `engine/` 与 `adapter/` 中的大量分支逻辑。
- `store/` — Postgres（GORM）+ Redis 连接、数据库播种（`store.Seed`，含默认管理员账号）。
- `devsvc/` — 设备/通道状态辅助函数（流状态迁移、节点查找），供 `api` 与 `engine` 共用。
- `auth/`、`crypto/`、`errs/`、`config/`、`task/`、`timeutil/` — 支撑基础设施。`errs` 定义应用的类型化错误值（`errs.ENotFound`、`errs.EForbid`、`errs.ENodeOffline`、`errs.EStreamTimeout` 等），由 engine/adapter 返回并在 `api/` 中转换为 HTTP 响应。`config.Load()` 从环境变量读取配置（见 `docker-compose.yml` 中 server 服务的环境变量，如 `DB_DSN`、`JWT_SECRET`、`ENCRYPTION_KEY`、`MQTT_BROKER`、`SIP_*`）。

### 流命名约定（关键约定）

`engine.streamNames`（对应 PRD §8.3）——协议行为正确与否依赖于此：
- `idp`：app=`live`，stream=`{deviceID}_{channelIdx}_{profile}`
- `gb28181`：app=`rtp`，stream 取自 `channel.Meta["gbStream"/"gbStreamSub"]`（已含 profile 后缀），缺失时回退为 `{gbChannelId}_{profile}`
- `onvif`/`rtsp`：app=`proxy`，stream=`{channelID}_{profile}`

新增设备来源类型时，必须同时扩展 `StartPlay` 与 `StopPlay` 中的 switch（以及 `streamNames`/`pullURL`）——这两处刻意保持并行而非合并抽象，因为各协议的起停流生命周期不同（推流 vs. 拉流 vs. RTP INVITE）。

## Web（`web/`）

Nuxt3 + Element Plus，SSR 关闭（`ssr: false`，纯 SPA）。脚本（`package.json`）：
```bash
npm run dev       # 开发
npm run build     # 生产构建
npm run generate  # 静态生成
npm run preview   # 预览生产构建
```

- `nuxt.config.ts`：`API_ORIGIN` 环境变量决定后端代理目标（本地默认 `http://localhost:8080`，Docker Compose 内为 `http://server:8080`）；`/api/v1/**`、`/ws/v1/**`（WS）、`/hooks/zlm/**` 均代理到后端；额外设置了 COOP/COEP 响应头（`same-origin`/`require-corp`），这是 h265web.js 多线程 WASM 解码的硬性要求（PRD §6.4），修改路由规则或响应头时不要破坏这两个头。
- 播放器依赖 `h265web.js` 部署到 `public/vendor/h265web.js`（该文件在仓库里不提供，需单独部署；缺失时播放器显示错误卡片，其余功能不受影响），并通过 `app.head.script` 在页面头部以 `async` 方式引入。
- `composables/useApi.ts`、`useAuth.ts`、`useWs.ts` 封装了 REST 调用、鉴权状态与 WebSocket 事件订阅，新增页面/组件优先复用这些 composable 而不是直接写 `fetch`。
- 直播页 `pages/live.vue`、回放页 `pages/playback.vue` 是与 `server/internal/engine` 交互最深的页面（起播/停播/回放控制），改动这两个页面前建议先读一遍对应的服务端 handler（`api/records.go`、`engine/playback.go`）。

## 跨领域注意事项

- 代码注释、错误信息、页面文案均为中文；新增内容保持一致。
- 协议/设备来源字符串（`idp`、`gb28181`、`onvif`、`rtsp`）在 `models`、`adapter`、`engine` 中作为字面判别值使用——追踪某个协议的端到端行为时，应搜索该来源字符串本身，而不仅仅是对应的 Go 类型。
