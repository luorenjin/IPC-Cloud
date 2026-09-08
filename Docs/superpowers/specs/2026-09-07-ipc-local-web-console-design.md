# IPC 设备本机 Web 管理端 设计文档

- 日期：2026-09-07
- 范围：
  - **A 部分 · 本地 Web 管理端**：`firmware/modules/console` + `firmware/modules/common/http_server`
  - **B 部分 · 录像子系统**：`firmware/modules/recorder` + `modules/common/{record_index, record_reader, ps_mux, mp4_mux}`
- 状态：设计已评审通过，待编写实施计划

两部分经回放链路耦合（B 提供帧源，A 提供界面与传输），接口契约见 §6.2，可并行实现。

## 1. 背景与目标

设备需提供通过 IP 直接访问的本机 Web 管理端（对标 TP-LINK 摄像头的本地管理界面），使设备可完全脱离云平台独立使用。

本地控制台在 `Docs/PRD/决策记录与待定事项.md` 中已冻结为固件必备能力，`firmware/docs/模块划分与依赖规则.md` 已规划 `console` 模块与共享 `http_server` 部件，`core/module.h` 已声明 `mod_console`，`core/config` 已预留 `localUser.*` 键前缀。本设计填充这一空白。

### 1.1 已确定的产品决策

| 决策项 | 结论 |
|---|---|
| 产品定位 | 完整本地管理端，设备可脱离云平台独立使用 |
| 前端形态 | 原生 JS 单页，无框架，资源内嵌固件 |
| 本地预览 | WS-FLV，主/子码流可切换（互斥），内嵌 flv.js + h265web.js |
| 编码参数 | 分辨率/帧率/码率/GOP 可配置，影响全局，走 `/api/v1/config` |
| 预览呈现 | 码流切换、显示尺寸、服务端抽帧，仅影响本地预览 |
| 本地回放 | **纳入本期**，连续播放（跨段无缝），复用 WS-FLV 与同一播放器 |
| 录像存储 | MPEG-PS 分段，与 GB28181 共用 `ps_mux` |
| 录像下载 | MP4，需实现 `mp4_mux`（由 P1 提升为必需） |
| 录像空洞 | 自动跳过，无缝接下一段，时间轴标注跳跃位置 |
| 配置冲突 | 本地与云端同级，后写覆盖前写 |
| 登录凭据 | 出厂验证码（机身标签），首次强制改密 |
| 初始配网 | 设备开 AP 热点，经本 Web 配网 |
| 传输安全 | HTTP + 挑战-响应抑制密码明文传输，不上 TLS |
| 体积控制 | 通用层实现完整功能，打包期按需裁剪 |

### 1.2 非目标

- HTTPS / TLS（需 mbedTLS 进 Flash 预算，本期不做，`security` 配置项预留开关）
- mDNS / UPnP 设备发现（后续可选增强）
- WebRTC 预览（决策记录已明确 P2P/WebRTC 不纳入 MVP，三期评估）
- 云端录像与本地录像的同步/合并（两者独立，本设计只管本地 TF 卡录像）
- 多通道录像（本硬件为单 sensor，`video.channels` 的 ch0/ch1 是同一画面的主/子码流，非多路摄像头）

## 2. 模块划分与依赖

### 2.1 `modules/common/http_server`

通用轻量 HTTP/1.1 服务件，**是被链接的库，不是 L3 模块**（无 `module_desc_t`）。职责单一：监听 socket、解析请求、按注册的路由前缀分发、写响应。不包含任何业务语义。

```c
typedef struct {
    const char *method;        /* "GET"/"POST"/NULL 表示任意 */
    const char *path;
    const char *query;         /* ? 后部分，未解析 */
    const char *body;          /* 已完整读入，有大小上限 */
    size_t      body_len;
    const char *(*header)(void *req, const char *name);
    void       *conn;          /* 连接句柄，用于升级 WS / 分块写 */
} http_req_t;

typedef int (*http_handler_fn)(http_req_t *req, void *user);

hal_err_t http_route(const char *prefix, http_handler_fn fn, void *user);
hal_err_t http_server_start(uint16_t port);
```

三个使用者按前缀瓜分路径空间，按最长前缀匹配，互不重叠：

| 使用者 | 前缀 | 说明 |
|---|---|---|
| `console` | `/`、`/api/*`、`/ws/*` | 静态资源 + REST + WS-FLV；`/` 为兜底 |
| `onvif` | `/onvif/*` | SOAP 服务端 |
| `snapshot` | `/snapshot*` | 抓图，ONVIF 与 console 共用 |

共用 80 端口。`profiles/SP-R1-02.json` 中 `protocols.onvif.port` 已为 80，共享设计化解了端口冲突。

### 2.2 `modules/console`

L3 模块，导出 `core/module.h` 中已声明的 `mod_console`。

```c
const module_desc_t mod_console = {
    .name = "console",
    .deps = (const char*[]){ "snapshot", NULL },
    .footprint = { .rss_kb_estimate = CONSOLE_RSS_TOTAL, .threads = 0 },
    .enabled  = console_enabled,
    .init     = console_init,     /* 注册 cfg 规则、注册路由、订阅 CONFIG 事件 */
    .start    = console_start,
    .stop     = console_stop,
    .health   = console_health,
};
```

依赖 `snapshot`（抓图复用）与 `recorder`（回放帧源），**不依赖 `onvif`**——两者只是恰好共用 http_server，无逻辑依赖。

`IPC_CONSOLE_PLAYBACK=OFF` 构建时 `deps` 中去掉 `recorder`，回放端点返回 `HAL_ENOTSUP`。

### 2.3 录像子系统模块（B 部分）

| 组件 | 形态 | 职责 |
|---|---|---|
| `modules/recorder` | L3 模块 | 录像计划执行、分段落盘、空间回收、`record` 事件发布 |
| `modules/common/ps_mux` | 库 | H.264/H.265 + G.711 → MPEG-PS 打包（与 gb28181 共用） |
| `modules/common/record_index` | 库 | SQLite 索引 `segments(start,end,type,path,size)` |
| `modules/common/record_reader` | 库 | **跨段连续帧流**：按时间定位、按 speed 输出、空洞跳过 |
| `modules/common/mp4_mux` | 库 | PS → MP4 转封装（下载用） |

```c
const module_desc_t mod_recorder = {
    .name = "recorder",
    .deps = NULL,
    .footprint = { .rss_kb_estimate = RECORDER_RSS_TOTAL, .threads = 1 },
    .enabled  = recorder_enabled,   /* storage.tf && record.enabled */
    /* ... */
};
```

`recorder` 常驻订阅主码流 frame_bus（占 1 个消费者名额），`record_reader` **不订阅 frame_bus**（它从磁盘读，不是实时流），因此回放不消耗消费者名额。

### 2.4 线程归属

`http_server` 持有唯一的 epoll 线程，console 声明 `threads = 0`。避免 console/onvif/snapshot 三家各起一个线程，保住接入方案"V200 ≤ 14 线程"的预算。

**代价是一条贯穿全设计的硬约束：console 的所有 handler 必须非阻塞。** 见 §5.3、§6.4、§10.4。

`recorder` 自持 1 个写盘线程（磁盘 I/O 必然阻塞，不能放在 epoll 线程上）。

### 2.5 硬件访问边界

console 不直接调用 `hal_*`。配置走 `core/config`，抓图走 `snapshot`，网络状态走 `core/netmgr`，录像检索走 `record_index`。

**唯一例外**：WS-FLV 预览需直连 `frame_bus` 订阅码流。这是数据面，绕不开。

## 3. 路由与 REST API

### 3.1 路径空间

```
/                      console：SPA 入口 index.html
/assets/*              console：内嵌静态资源（gzip 预压缩）
/api/v1/*              console：REST，需鉴权（登录接口除外）
/ws/v1/live            console：WS-FLV 实时预览流
/ws/v1/playback        console：WS-FLV 回放流
/onvif/*               onvif：SOAP
/snapshot              snapshot：JPEG 抓图
```

### 3.2 端点清单

全部返回 JSON，错误用统一信封 `{"code":<hal_err>, "msg":"中文说明"}`。

| 方法 路径 | 用途 | 鉴权 |
|---|---|---|
| `POST /api/v1/auth/challenge` | 取盐值与 nonce | 否 |
| `POST /api/v1/auth/login` | 提交摘要，换取会话 token | 否 |
| `POST /api/v1/auth/logout` | 注销 | 是 |
| `POST /api/v1/auth/password` | 改密（首次强制） | 是 |
| `GET  /api/v1/system/info` | 型号/序列号/固件版本/运行时长/能力清单 | 是 |
| `GET  /api/v1/system/status` | CPU/内存/温度/各模块 health | 是 |
| `POST /api/v1/system/reboot` | 重启 | 是 |
| `POST /api/v1/system/reset` | 恢复出厂（可选保留网络配置） | 是 |
| `GET  /api/v1/config?prefix=` | 按前缀聚合读配置 | 是 |
| `PUT  /api/v1/config` | 批量写配置，返回 rejected 列表 | 是 |
| `GET  /api/v1/net/status` | 网口/WiFi 链路状态、IP、MAC、`mode` | 是 |
| `GET  /api/v1/net/wifi/scan` | WiFi 扫描结果 | 是 |
| `POST /api/v1/net/wifi/connect` | 连接 WiFi（配网） | 是 |
| `GET  /api/v1/video/params` | 编码参数、图像参数运行时实况 | 是 |
| `GET  /api/v1/storage/info` | TF 卡容量/健康/挂载状态 | 是 |
| `POST /api/v1/storage/format` | 格式化 TF 卡 | 是 |
| `GET  /api/v1/records/timeline?from=&to=` | 时间轴：录像分布区间（非逐段列表） | 是 |
| `GET  /api/v1/records/segments?from=&to=` | 分段明细（下载选择用） | 是 |
| `GET  /api/v1/records/download?from=&to=` | 导出 MP4，流式输出 | 是 |
| `POST /api/v1/ota/upload` | 上传固件包（分块） | 是 |
| `GET  /api/v1/ota/progress` | 升级进度 | 是 |
| `GET  /api/v1/diag/logs` | 导出日志环形缓冲 | 是 |

### 3.3 配置统一走 `/api/v1/config`

视频参数、图像参数、告警区域、录像计划、时间设置、协议开关全部是 `core/config` 的键，已有成熟的规则校验、批量 apply 带拒绝列表、原子持久化与 event_bus 广播。为它们各建 REST 端点等于把校验逻辑抄第二遍。

前端按 `prefix` 拉取（如 `?prefix=image.`），按批次提交。`cfg_apply_json` 的 `rejects` 数组直接映射为表单字段级错误：

```json
{ "code": 0, "applied": 5,
  "rejected": [ {"key":"video.0.main.kbps", "reason":"超出范围 128-4096"} ] }
```

写配置**部分成功**而非整体失败，与 IDP 协议 `ack.data.rejected[]` 语义一致，云端与本地两条链路行为统一。

`/video/params`、`/storage/info` 等**只读**端点保留，因其返回运行时实况（HAL 查询结果）而非配置值，语义不同，不可混用。

### 3.4 能力探测与降级

未实现或不存在的能力返回 `HAL_ENOTSUP`（HTTP 501），**不是 404**。前端启动时拉取 `/api/v1/system/info` 中由 profile 导出的能力清单，据此渲染菜单树，不硬编码功能开关。

这使"回放分期""TF 卡选配""型号无 WiFi"三种情况共用同一条降级代码路径。

## 4. 鉴权与会话

约束：出厂验证码作初始密码 + HTTP 明文传输。后者要求密码绝不明文过线，且不可重放。

### 4.1 凭据存储

出厂验证码（8 位随机大写字母数字）在产线烧录进 `hal_crypto` 安全存储。首次启动时派生：

```
salt       = 随机 16 字节（hal_crypto 随机数）
stored_key = PBKDF2-SHA256(password, salt, iter=4096, dklen=32)
```

存于 `localUser.salt` / `localUser.key` / `localUser.iter`。**明文密码不落盘。**

迭代次数 4096 而非 OWASP 建议的十万级：GK7205V200 类低端 SoC 上需压在 200ms 内完成。这是嵌入式场景的务实取舍，以挑战-响应机制与登录锁定作为补偿。

### 4.2 登录流程（SCRAM 式挑战-响应）

浏览器端用 Web Crypto API（`crypto.subtle` 原生支持 PBKDF2 + HMAC，无需引入库）：

```
1. POST /auth/challenge {user}
   ← {salt, iter, nonce}          nonce 单次有效、60s 过期

2. 前端本地计算（密码不出浏览器）：
     client_key = PBKDF2(password, salt, iter)
     proof      = HMAC-SHA256(client_key, nonce)
   POST /auth/login {user, nonce, proof}

3. 设备端用 stored_key 同样计算，恒定时间比对
   ← {token, must_change_password}
```

密码本身从不上线，重放由 nonce 阻断。

### 4.3 会话

- token 32 字节随机值，服务端内存表保存，**不持久化**，重启即失效
- `Set-Cookie: HttpOnly; SameSite=Strict`
- 并发上限 4 个会话，超出踢最旧
- 空闲 30 分钟过期

### 4.4 首次强制改密

`profiles/*.json` 中 `security.force_password_change: true` 已要求。登录响应带 `must_change_password`，前端锁定改密页。

**服务端必须独立拦截**：未改密状态下，除 `/auth/*` 与 `/system/info` 外所有端点返回 `HAL_EPERM`。前端限制不作数。

### 4.5 暴力破解防护

- 按客户端 IP 计数，连续 5 次失败锁定 60 秒，指数退避至上限 15 分钟
- IP 表最多 8 项，满则淘汰最旧
- `/auth/challenge` 对不存在的用户返回**伪造但稳定**的 salt/iter（由用户名 HMAC 派生），避免用户名枚举

### 4.6 凭据恢复

`cfg_reset` 清除 `localUser.*` 后回落到安全存储中的出厂验证码，重走首次改密流程。物理复位键（`gpio_map.reset_key`）长按触发同一路径，是用户忘记密码的唯一救济手段。

### 4.7 已知残留风险

HTTP 明文下**会话 token 在局域网内可被嗅探劫持**。挑战-响应保护的是密码（价值更高、常跨设备复用），不是会话。

缓解：token 短生命周期、重启失效、HttpOnly。若后续需更高强度，`security` 配置项可扩展 HTTPS 开关，但需 mbedTLS 进 Flash 预算。**本期接受此风险。**

## 5. 本地预览链路（WS-FLV）

### 5.1 数据通路与码流选择

```
GET /ws/v1/live?stream=main|sub&fps_div=1|2|3        默认 sub、fps_div=1
```

```
hal_video（ch0 主码流 H.265/H.264 或 ch1 子码流 H.264）
  → frame_bus 订阅（占 1 个消费者名额）
  → FLV 封装
  → WebSocket 二进制帧
  → 浏览器 flv.js（H.264）/ h265web.js（H.265）→ <video>
```

**主/子码流互斥，console 始终只占 1 个 frame_bus 消费者名额。** 依据：`limits.frame_bus_consumers: 4` 为全局预算，recorder 常驻占 1，RTSP 占 2（`rtsp_sessions: 2`），余量为 1。

切换码流时先 `frame_bus_unsubscribe(旧通道)` 再 `subscribe(新通道)`，随即 `request_idr()`。切换瞬间有约 1 个 GOP 的黑屏（主码流 gop=50@25fps 即约 2 秒）。

所有 WS 连接**共享同一订阅、看同一路码流**。第二个连接请求不同码流时返回 `HAL_EBUSY` 并附当前码流，前端提示"另一客户端正在预览主码流"。

### 5.2 FLV 封装

`modules/common/flv_mux`：H.264/H.265 帧 → FLV tag。与 `rtmp_push` 共享 tag 封装逻辑，**不共享 RTMP 握手/chunk 层**。

首帧前必须发送 FLV header 与 sequence header，否则播放器无法起播：
- H.264：AVC sequence header（由 SPS/PPS 构造 AVCDecoderConfigurationRecord）
- H.265：HEVC sequence header（由 VPS/SPS/PPS 构造 HEVCDecoderConfigurationRecord，enhanced-RTMP 扩展）

同一套封装代码同时服务实时预览（§5）与回放（§6），帧源不同而封装逻辑一致。

### 5.3 非阻塞写与背压

每个 WS 会话持有有界发送队列（子码流 128KB ≈ 2s@512kbps，主码流 512KB ≈ 1s@4096kbps）：

```c
/* frame_bus 回调内 */
if (queue_free(s) < frame_len) {
    s->dropping = true;          /* 队列积压：丢弃至下一个 IDR，不阻塞 */
    s->stats.dropped++;
} else if (s->dropping && !is_idr(frame)) {
    continue;                    /* 丢弃期间跳过非关键帧 */
} else {
    s->dropping = false;
    queue_push(s, frame);        /* 仅入队，不写 socket */
}
epoll_mod(s->fd, EPOLLOUT);      /* 唤醒 epoll 线程执行发送 */
```

**核心纪律：frame_bus 回调内绝不调用 `write()`**，只入队并置 EPOLLOUT，实际发送在 epoll 线程的可写事件中完成。慢客户端只会撑满自身队列而丢帧，不会拖住 epoll 线程，也不会影响 ONVIF。

丢帧恢复**必须对齐 IDR**，从非关键帧续传会导致解码器花屏。恢复时调用 `hal_video.request_idr()` 主动请求关键帧，缩短黑屏时间。

### 5.4 并发连接限制

并发 WS 连接限 2 个（每连接独立队列，受内存约束），超出返回 `HAL_EBUSY`。

### 5.5 按需启停

无 WS 连接时不订阅 frame_bus。`frame_bus` 自带"按需 start / idle stop"，最后一个预览连接断开后该路编码器可停止，节省算力与功耗。

要求断连检测可靠：WS ping/pong 心跳 15 秒，两次无响应判定断开。

### 5.6 服务端抽帧

`?fps_div=N` 在推流时按 N 丢帧（N=2 即输出一半），降低弱网与低性能客户端压力，**同时节省带宽**——放在服务端而非浏览器端的理由。

**只丢非 IDR 帧**，IDR 必须全部保留，否则解码器花屏。

### 5.7 编码器参数配置（影响全局）

走 `/api/v1/config` 的 `video.*` 键，无独立端点（§3.3）。可配项与范围：

| 配置键 | 主码流（ch0） | 子码流（ch1） |
|---|---|---|
| `video.0.main.w` / `.h` | ≤1920×1080 | — |
| `video.0.main.fps` | ≤30 | — |
| `video.0.main.kbps` | 128–4096 | — |
| `video.0.main.codec` | h265 / h264 | — |
| `video.0.main.gop` | 1–150 | — |
| `video.1.sub.*` | — | ≤704×396，仅 h264 |

校验规则在 `console_init()` 中经 `cfg_register_rules()` 登记，**上下界由 profile 的 `channels[].max` 动态生成**而非硬编码——换传感器或型号时规则自动跟随。

变更经 event_bus 广播 CONFIG 事件，`hal_video.set_encoder` 热应用。

**必须二次确认**：改编码参数影响录像、云端推流、RTSP 全部下游消费者，UI 上须明示。

### 5.8 预览呈现参数（仅本地）

预览页独立控制，**不写 config、不影响编码器**：

| 控制 | 实现 |
|---|---|
| 码流切换 | `?stream=main\|sub`（§5.1） |
| 显示尺寸 | 适应窗口 / 原始尺寸 / 50%，纯 CSS |
| 流畅度 | `?fps_div=N`（§5.6） |

### 5.9 前端播放器与 COOP/COEP

| 编码 | 播放器 | gzip 体积 |
|---|---|---|
| H.264 | flv.js（纯 JS + MSE） | ~90KB |
| H.265 | h265web.js（WASM） | ~400KB–1MB |

h265web.js 的 WASM 多线程需 `SharedArrayBuffer`，**要求 http_server 对 console 响应下发**：

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

COEP 会拦截无 CORP 头的跨源资源，但本地 Web 全部资源同源内嵌、无外部依赖，实际无影响。此处与云平台 `nuxt.config.ts` 的处理一致。

`IPC_CONSOLE_H265=OFF` 构建时**不下发**这两个头（无必要且徒增限制），主码流为 H.265 时预览选项禁用并说明原因。

### 5.10 降级路径

MSE 不可用时（个别老旧浏览器、部分 iOS Safari 版本）自动退为 `/snapshot` 定时轮询（1fps）。该端点本就为 ONVIF 存在，零额外成本。图像参数调节场景用轮询已足够。

## 6. 本地回放链路

回放复用 §5 的 FLV 封装、WebSocket 传输与播放器实例，**差异仅在帧源**：实时预览取自 `frame_bus`，回放取自 `record_reader`。

### 6.1 连续播放（核心要求）

录像在磁盘上按 `storage.segment_s: 60` 分段。用户拖到任意时间点后应能持续播放下去，跨越段边界时**不卡顿、不重新起播、不切换播放器实例**。

这要求 `record_reader` 是**流式拼接器**而非文件读取器，需解决三个问题：

**① 段边界预读衔接**

读到当前段末尾前预先打开下一段，帧流对上层连续无断点。播放器全程单一实例、单一 MSE SourceBuffer。

**② 跨段时间戳重写**

每段 PS 的时间戳各自从 0 开始，直接拼接会导致播放器时间轴跳变、MSE 报错。`record_reader` 输出的每帧 **PTS/DTS 必须重写**为相对回放起点的连续毫秒值：

```
out_pts = (segment.start_utc - playback_origin_utc) * 1000 + frame_pts_in_segment
```

**③ 空洞跳过**

无录像时段（未开录、断电、事件录像间隙）**自动跳过，无缝接下一段**。

代价是**播放时间不再线性对应真实时间**，因此：
- WS 每帧附带该帧的真实 UTC 时间（FLV `onMetaData` 之外另开一条 WS 文本消息通道，或用 FLV script tag）
- UI 上始终显示当前帧的真实时间，而非播放器的 `currentTime`
- 时间轴标注跳跃位置，让用户理解画面为何"突然变化"

### 6.2 `record_reader` 接口契约（A/B 两部分的边界）

```c
typedef struct record_reader record_reader_t;

/* 打开一个从 start_utc_ms 起的连续回放流；跨段、跳空洞由实现内部处理 */
record_reader_t *rr_open(uint64_t start_utc_ms, int speed_x);

/* 取下一帧；返回的帧 PTS 已按 §6.1② 重写为连续值
   out_real_utc_ms 回填该帧的真实时间，供 UI 显示
   无更多录像时返回 HAL_ENOENT */
hal_err_t rr_next(record_reader_t *r, hal_frame_t *f, uint64_t *out_real_utc_ms);

hal_err_t rr_seek(record_reader_t *r, uint64_t utc_ms);   /* 定位到最近的 IDR */
hal_err_t rr_set_speed(record_reader_t *r, int speed_x);  /* 1/2/4/8 */
void      rr_close(record_reader_t *r);
```

**`rr_next` 是阻塞的磁盘读**，因此不能在 epoll 线程调用——见 §6.4。

### 6.3 回放控制

```
GET /ws/v1/playback?from=<utc_ms>&speed=1
```

控制指令经同一 WS 上行文本消息发送（避免额外 REST 往返）：

| 指令 | 语义 |
|---|---|
| `{"op":"seek","utc":<ms>}` | 定位，服务端对齐到最近 IDR |
| `{"op":"speed","x":1\|2\|4\|8}` | 变速 |
| `{"op":"pause"}` / `{"op":"resume"}` | 暂停 / 继续 |
| `{"op":"step"}` | 暂停态下单帧步进 |

**变速实现**：2x 起只输出 I 帧与 P 帧、8x 只输出 I 帧，避免带宽随倍速线性增长。倍速下播放器按正常速率消费，实际时间推进由服务端控制送帧节奏。

**暂停**：服务端停止送帧但保持 WS 连接与 reader 打开；不依赖播放器端暂停（那样会积压缓冲）。

### 6.4 回放线程模型（关键约束）

`rr_next` 是阻塞磁盘读，**绝不能在 epoll 线程执行**，否则回放会卡住 ONVIF 与所有 HTTP 请求。

方案：回放会话由 `recorder` 的写盘线程之外**另起 1 个读线程**（仅在有回放会话时存在，会话结束即退出）：

```
读线程：rr_next() → flv_mux → queue_push(会话队列) → epoll_mod(EPOLLOUT)
epoll 线程：可写事件 → 从队列取数据 write()
```

与 §5.3 的实时预览背压模型完全一致，唯一区别是生产者从 frame_bus 回调换成了读线程。

**回放会话限 1 路**（读线程数与内存所限），第二个请求返回 `HAL_EBUSY`。

### 6.5 时间轴数据

```
GET /api/v1/records/timeline?from=&to=
```

返回**区间**而非逐段列表——60 秒一段时一天有 1440 段，逐段返回过大：

```json
{ "code": 0,
  "ranges": [
    {"start": 1757000000000, "end": 1757003600000, "type": "timed"},
    {"start": 1757005000000, "end": 1757005180000, "type": "event"}
  ] }
```

`record_index` 的 SQLite 查询中把连续分段合并为区间（相邻段间隔 ≤2×`segment_s` 视为连续）。定时录像与事件录像分别成区间，UI 用不同颜色区分。

### 6.6 录像下载（MP4）

```
GET /api/v1/records/download?from=&to=
```

`mp4_mux` 把选定时段的 PS 分段**流式转封装**为 MP4 输出，不落临时文件（Flash 无空间）。

**技术约束**：MP4 的 `moov` 索引需在写完全部数据后才能确定大小。流式输出采用 **fragmented MP4（fMP4）**，`moov` 在前、数据分片跟随，无需回填。通用播放器与浏览器均支持 fMP4。

下载同样是阻塞磁盘读，走与 §6.4 相同的读线程模型。**下载与回放互斥**（共用读线程配额），并发时返回 `HAL_EBUSY`。

导出时段跨空洞时，MP4 内容为跳过空洞后的连续画面，与回放行为一致。

## 7. 录像子系统（B 部分）

### 7.1 录像触发与计划

| 模式 | 配置键 | 说明 |
|---|---|---|
| 关闭 | `record.mode = "off"` | 不录像 |
| 定时 | `record.mode = "timed"` | 按周计划表录制，`record.schedule` 存 7×24 位图 |
| 事件 | `record.mode = "event"` | IVS/GPIO 事件触发，含前录 `record.pre_s`（默认 5s）与后录 `record.post_s`（默认 30s） |
| 全天 | `record.mode = "always"` | 持续录制 |

事件录像的**前录**需要环形缓冲：recorder 常驻订阅主码流，在内存中保留最近 `pre_s` 秒的帧（约 5s×4096kbps ≈ 2.5MB），事件触发时把缓冲内容一并落盘。这是 recorder 内存占用的主要来源。

事件来源经 event_bus 订阅：IVS（移动/人形/遮挡）、GPIO（告警输入）、STORAGE（插拔）。

### 7.2 分段与存储格式

- 格式：**MPEG-PS**，与 GB28181 回放共用 `ps_mux`（`firmware/docs/模块划分与依赖规则.md` §4.2 已规划此复用）
- 分段长度：`storage.segment_s`（默认 60 秒）
- **每段必须以 IDR 开始**——保证 seek 可对齐到段首，且单段可独立解码
- 路径：`/mnt/sd/record/{YYYYMMDD}/{HHMMSS}_{type}.ps`

选择 PS 而非 MP4 的理由：MP4 需写 `moov` 索引，断电时末尾段会损坏；PS 是流式格式，截断只损失末尾几帧。**嵌入式设备断电是常态，这是决定性因素。**

### 7.3 索引（`record_index`）

SQLite 表：

```sql
CREATE TABLE segments (
  id      INTEGER PRIMARY KEY,
  start   INTEGER NOT NULL,   -- UTC ms
  end     INTEGER NOT NULL,
  type    TEXT NOT NULL,      -- 'timed' | 'event' | 'always'
  path    TEXT NOT NULL,
  size    INTEGER NOT NULL
);
CREATE INDEX idx_start ON segments(start);
```

三个消费者共用：本地回放（§6）、GB28181 `RecordInfo`、IDP `record.query`。

**索引与文件的一致性**：断电可能导致索引与实际文件不符。启动时做一次轻量校对——扫描目录与索引比对，删除索引中不存在的文件记录，为孤立文件补录（按文件名解析时间）。全量扫描在 256GB 卡上可能耗时，放在后台线程，不阻塞录像启动。

### 7.4 空间回收

`storage.reserve_pct: 5`——剩余空间低于 5% 时按**最旧优先**删除分段，同时删索引记录。

事件录像可配置为**优先保留**（`record.keep_event_first`，默认 true）：先删定时录像，事件录像保留更久。

回收在写盘线程中做，每分钟检查一次。

### 7.5 recorder 内存构成

| 项 | 大小 | 说明 |
|---|---|---|
| 前录环形缓冲 | ~2.5MB | `pre_s`×主码流码率，事件模式才分配 |
| PS 打包缓冲 | 256KB | |
| 写盘缓冲 | 512KB | 减少小块写，延长 TF 卡寿命 |
| SQLite 页缓存 | 256KB | |
| **合计（事件模式）** | **~3.5MB** | |
| **合计（定时/全天模式）** | **~1MB** | 无前录缓冲 |

`RECORDER_RSS_TOTAL` 按 `record.mode` 与 `record.pre_s` 编译期取上界声明。

## 8. 配网流程与网络配置

### 8.1 AP 配网启动条件

```
未配网 = (以太网 link down) AND (WiFi 未配置 OR WiFi 连接失败超时 60s)
```

以太网插着绝不开 AP——有线场景直接用 IP 访问。WiFi 已配但连不上（改密码、换路由器）**允许**回落 AP，作为重新配网的救济路径。

### 8.2 HAL 扩展（需升 API 版本）

`hal_net.h` 当前只有 `wifi_scan` / `wifi_connect` / `wifi_disconnect`，**无 AP 模式接口**。需扩展：

```c
/* 可选能力，不支持返回 HAL_ENOTSUP */
hal_err_t (*wifi_ap_start)(const char *ssid, const char *psk, uint8_t channel);
hal_err_t (*wifi_ap_stop)(void);
```

`hal_net_caps_t` 增加 `bool wifi_ap;`。

这是本设计**唯一的 HAL 改动**。影响：`HAL_API_VERSION` 升版，`tests/hal_conformance` 新增用例，mock 平台需提供假实现以支持 x86 验证。

### 8.3 AP 参数与访问

- SSID：`IPC-{序列号后6位}`
- **带 WPA2 密码**，取出厂验证码（与 Web 登录同值，印于标签）。开放热点会让邻近用户直接进入配网页，不可接受。
- 设备固定 `192.168.169.1/24`，运行极简 DHCP 服务

用户连上热点后访问 `http://192.168.169.1` 进入**同一套 Web**。前端按 `/api/v1/net/status` 返回的 `mode: "ap"` 只渲染配网向导，隐藏其余菜单。

**Captive Portal 探测响应**：响应 `/generate_204`（Android）与 `/hotspot-detect.html`（iOS），返回重定向使手机自动弹出配网页，免去手动输 IP。成本为 http_server 两条路由，体验提升显著。

### 8.4 配网提交与切换

```
POST /api/v1/net/wifi/connect {ssid, psk, sec}
  → 立即返回 202（不等待结果）
  → 停 AP → wifi_connect → 等待 DHCP
  → 成功：写入 config，status_led 指示
  → 失败：60s 后重开 AP，保留失败原因供查询
```

**不等待连接结果即返回**：切换网络必然断开当前 HTTP 连接，同步等待无意义。用户体验依靠 LED 指示与重连后访问新 IP。前端提交后提示"正在连接，请将手机连回家庭 WiFi 后访问 xxx"。

**设备发现问题**：配网后 IP 由路由器 DHCP 分配，用户不知其值。缓解：前端展示设备 hostname（`ipc-{序列号后6位}.local`）；用户亦可经路由器管理页或云平台查询。mDNS 为后续可选增强，不在本期。

### 8.5 有线网络配置

经 `/api/v1/config` 的 `net.*` 键（DHCP/静态 IP/DNS）。改静态 IP 同样断连接，提交后返回 202 并提示新地址。

**必须防呆**：静态 IP 与当前网段不符时前端二次确认——配错只能物理复位。

### 8.6 netmgr 依赖

IP 层配置按 `hal_net.h` 注释所述"由 core 网络管理器完成，不在 HAL 内"，即 `core/netmgr`（`firmware/docs/模块划分与依赖规则.md` 标注"待补"，尚未实现）。

console **不自行调用系统网络接口**，而是写 `net.*` 配置项，由 netmgr 订阅 CONFIG 事件后执行。

**配网功能需 netmgr 先落地**，见 §11.2 实施顺序。

## 9. 可裁剪性与资源声明

**原则：通用层实现完整功能，体积在打包期裁剪，而非在设计期削减功能。**

此原则与 `firmware/CLAUDE.md` 中"不要向 core/modules 添加芯片相关假设"一致——不以某块开发板的 Flash 容量约束通用层设计。

### 9.1 裁剪机制

复用既有 CMake 模块开关约定（`-DIPC_MOD_<NAME>=ON/OFF`），在 console 内再分特性开关：

| 开关 | 默认 | 关闭后 | 省下（gzip） |
|---|---|---|---|
| `IPC_CONSOLE_PREVIEW` | ON | 无 WS-FLV 预览，退为抓图轮询 | ~90KB（flv.js） |
| `IPC_CONSOLE_H265` | ON | 主码流 H.265 时不可预览；不下发 COOP/COEP | ~400KB–1MB（h265web.js） |
| `IPC_CONSOLE_PLAYBACK` | ON | 无回放页面，`deps` 去掉 recorder | ~20KB |
| `IPC_CONSOLE_DOWNLOAD` | ON | 无录像下载，不编入 `mp4_mux` | ~5KB + mp4_mux 代码 |
| `IPC_CONSOLE_WIFI` | AUTO | 无配网向导 | ~10KB |
| `IPC_CONSOLE_OTA` | ON | 无本地升级页 | ~8KB |

`AUTO` 表示**跟随 profile 能力**（`network.wifi` 存在则编入），同一份代码在无 WiFi 型号上自动不带配网 UI。前端资源构建期按开关树摇，未启用页面不进产物。

`IPC_CONSOLE_H265` 是体积杠杆最大的开关——关掉它可省下超过全部其余资源之和。

### 9.2 Web 资源体积

| 资源 | 原始 | gzip 后 |
|---|---|---|
| h265web.js（WASM + glue） | ~1.5MB | ~400KB–1MB |
| flv.js | ~250KB | ~90KB |
| 页面 HTML/CSS/JS（原生，无框架） | ~220KB | ~55KB |
| 图标（内联 SVG，无图片文件） | ~20KB | ~6KB |
| **全功能合计** | ~2MB | **~550KB–1.15MB** |
| 关闭 H265 | ~490KB | **~151KB** |
| 仅配置管理 + 系统信息 | ~200KB | **~50KB** |

资源以 **gzip 预压缩形式**存储，HTTP 直接带 `Content-Encoding: gzip` 下发，运行时不解压——同时节省 Flash、CPU 与内存。

h265web.js 体积区间较宽，取决于所选构建变体（是否含多线程 worker、是否裁剪不用的 profile），实施时按实际产物核定。

### 9.3 资源声明随配置计算

`rss_kb_estimate` 按启用特性编译期累加，而非硬编码：

```c
#define CONSOLE_RSS_BASE      64
#if IPC_CONSOLE_PREVIEW
   /* 主/子码流互斥，按主码流队列取上界 */
#  define CONSOLE_RSS_PREVIEW (CONSOLE_WS_QUEUE_MAIN_KB * CONSOLE_WS_MAX_CONN)
#else
#  define CONSOLE_RSS_PREVIEW 0
#endif
#if IPC_CONSOLE_PLAYBACK
#  define CONSOLE_RSS_PLAYBACK (CONSOLE_WS_QUEUE_MAIN_KB * 1)   /* 回放限 1 路 */
#else
#  define CONSOLE_RSS_PLAYBACK 0
#endif
#define CONSOLE_RSS_TOTAL (CONSOLE_RSS_BASE + CONSOLE_RSS_PREVIEW + CONSOLE_RSS_PLAYBACK)
```

默认值：`CONSOLE_WS_QUEUE_SUB_KB = 128`（512kbps×2s）、`CONSOLE_WS_QUEUE_MAIN_KB = 512`（4096kbps×1s）、`CONSOLE_WS_MAX_CONN = 2`（与 §5.4 一致）。

全功能默认配置：`CONSOLE_RSS_TOTAL = 64 + 512×2 + 512 = 1600KB ≈ 1.56MB`。

这超出 `firmware/docs/模块划分与依赖规则.md` 中 console 的 0.5MB 规划值，原因是该规划未预见主码流预览与回放。按"声明值随构建配置如实计算"的原则，**应更新文档中的规划值而非削减实现**——`module_loader` 的预算检查依赖声明的准确性。

加上 recorder 的 ~3.5MB（§7.5），录像+本地 Web 合计约 5MB，在 `limits.mem_budget_mb: 56` 内占比约 9%，可接受。

### 9.4 profile 修正

本设计涉及的 `profiles/SP-R1-02.json` 改动：

```diff
- "sensors": ["sc230ai"],
+ "sensors": ["gc2053"],
```

纯事实修正（模组实际采用格科微 GC2053）。

**待打包期核定**：`ota.slot_size_mb` 当前为 40，该值基于 NAND Flash 假设。若目标硬件为 16MB SPI Nor Flash，双槽各 40MB 不成立，需在固件打包方案中按实际分区表重新核定。**本设计不修改此值**，仅记录该问题。

`network.wifi.module`（当前 `"TBD"`）与 `storage.tf` 属型号配置，由出货形态决定，本设计不预设。console 经 §3.4 能力探测机制适配任意组合。

## 10. 错误处理与健康检查

### 10.1 错误码映射

console 不发明新错误码，沿用 `hal_err_t`，HTTP 层映射：

| hal_err_t | HTTP | 场景 |
|---|---|---|
| `HAL_OK` | 200 | |
| `HAL_EINVAL` | 400 | 参数非法、配置校验失败 |
| `HAL_EPERM` | 401/403 | 未登录 / 未改密 |
| `HAL_ENOENT` | 404 | 资源不存在；回放时该时段无录像 |
| `HAL_EBUSY` | 409 | 预览码流冲突、回放/下载已占用、OTA 进行中 |
| `HAL_ENOTSUP` | 501 | 能力不存在（无 TF 卡、无 WiFi、H265 未编入） |
| 其他 | 500 | |

响应体始终带原始 `code`，前端据此精确判断而非推测 HTTP 状态。

**`ENOTSUP → 501` 是能力探测的基石**（§3.4），不可退化为 404，否则前端无法区分"无此功能"与"路径错误"。

### 10.2 `console_health()` 判定

启动器每 10 秒调用，连续 3 次失败触发模块重启，故判定**只反映 console 自身**：

```c
static hal_err_t console_health(char *detail, size_t cap) {
    if (!http_route_registered)  return HAL_EIO;   /* 路由丢失 */
    if (session_table_corrupt)   return HAL_EIO;
    return HAL_OK;
}
```

**刻意排除**：监听端口可达性（属 http_server 健康）、有无活跃预览（零连接是正常态）、WiFi 连接状态（非 console 职责）、TF 卡状态（属 recorder 职责）。纳入这些会导致"拔网线即重启 console"之类的错误行为。

### 10.3 `recorder_health()` 判定

```c
static hal_err_t recorder_health(char *detail, size_t cap) {
    if (write_thread_stalled)     return HAL_EIO;   /* 写盘线程卡死 */
    if (index_db_error)           return HAL_EIO;
    if (consecutive_write_fails > 3) return HAL_EIO;
    return HAL_OK;
}
```

**刻意排除**：TF 卡未插（合法状态，`enabled()` 已判定）、空间不足（由回收机制处理，非故障）。

TF 卡拔出时 recorder 不应被判为失败——经 STORAGE 事件转入空闲，插回后恢复。

### 10.4 OTA 上传

固件包数 MB，而 http_server 单线程 epoll、请求体有大小上限，**不可整包读入内存**。采用分块上传：

```
POST /api/v1/ota/upload   Content-Range: bytes 0-65535/4194304
  → 每块 64KB，收到即调 hal_sys OTA write 落盘，不缓存
  → 末块触发 ota_end + 签名校验
```

每块为独立短请求，天然不阻塞 epoll 线程。进度经 `/api/v1/ota/progress` 轮询。

升级完成后 `ota_switch` + 重启，新固件启动后**必须** `ota_confirm`（`ota.confirm_timeout_s: 60`），否则回滚。确认动作归 ota 模块，console 只负责上传。

## 11. 测试策略与实施顺序

### 11.1 测试

遵循固件既有形态（x86 + mock 平台，无需硬件），纳入 `ctest`，保持 `fail = 0` 标准。

| 测试 | 覆盖 |
|---|---|
| `tests/http_server_test` | 请求解析（含畸形/超长/分块）、路由分发、最长前缀匹配、并发连接、EPOLLOUT 背压 |
| `tests/console_test` | 鉴权全流程（challenge/proof/重放拒绝/锁定退避）、配置读写与 rejected 映射、能力探测降级、错误码映射、主子码流互斥 |
| `tests/recorder_test` | 分段落盘、IDR 对齐段首、索引一致性校对、空间回收、前录缓冲 |
| `tests/record_reader_test` | **跨段连续性**（见下）、seek 对齐 IDR、变速抽帧、空洞跳过 |
| `tests/hal_conformance`（扩充） | `wifi_ap_start` / `wifi_ap_stop` 用例 |

**必须覆盖的六个易错点**：

1. **背压丢帧对齐 IDR** —— 构造慢消费者，断言恢复后首帧为 IDR
2. **frame_bus 回调不阻塞** —— 断言回调内无 write 系统调用（打桩计数）
3. **未改密时服务端拦截** —— 逐端点验证返回 EPERM，不可仅靠前端
4. **跨段时间戳连续单调** —— 构造 3 个连续分段，断言输出帧 PTS 严格递增且无跳变（§6.1②）
5. **空洞跳过后真实时间正确** —— 构造带空洞的录像，断言 `rr_next` 回填的 `real_utc_ms` 与分段实际时间一致（§6.1③）
6. **回放读线程不占 epoll 线程** —— 断言 `rr_next` 调用发生在非 epoll 线程（§6.4）

第 4、5 两项是"连续播放"要求的直接验证，是回放链路最易出错处。

### 11.2 实施顺序

两部分可并行。**B 部分是 A 部分回放功能的前置**，但不阻塞 A 的其余功能。

**A 部分（本地 Web 管理端）**
```
A1. hal_net AP 接口 + mock 实现 + 一致性测试     ← 改 HAL，先做
A2. modules/common/http_server + 测试            ← 基础件
A3. modules/console 骨架：路由、鉴权、config 读写 ← 核心
A4. 前端 SPA：登录/系统/配置/网络页
A5. core/netmgr（若未实现）→ 配网向导 + Captive Portal
A6. flv_mux + WS-FLV 实时预览 + flv.js/h265web.js 集成
A7. OTA 上传页
```

**B 部分（录像子系统）**
```
B1. modules/common/ps_mux                        ← gb28181 亦依赖
B2. modules/common/record_index（SQLite）
B3. modules/recorder：分段落盘、计划、回收
B4. modules/common/record_reader：跨段连续流      ← 回放核心难点
B5. modules/common/mp4_mux（fMP4 流式输出）
```

**汇合**
```
C1. 回放页 + /ws/v1/playback（需 A6 + B4）
C2. 录像下载（需 B5）
```

A3 完成即为可用的最小管理端（登录、查看状态、修改配置）。A6 完成即有实时预览。回放需两条线都推进到位。

## 12. 风险登记

| 风险 | 影响 | 缓解 |
|---|---|---|
| 单线程 epoll 上 WS 推流阻塞 ONVIF | 高 | §5.3 纪律 + 测试断言；建议纳入 code review 清单 |
| 回放阻塞磁盘读误入 epoll 线程 | 高 | §6.4 独立读线程；测试第 6 项断言 |
| 跨段时间戳重写出错致播放器报错/卡死 | 高 | §6.1② 明确公式；测试第 4 项断言 |
| h265web.js 体积区间不确定（400KB–1MB） | 中 | §9.2 实施时按实际产物核定；`IPC_CONSOLE_H265` 可整体关闭 |
| console 实际内存 1.56MB 远超文档规划的 0.5MB | 中 | §9.3 更新规划文档；预算内占比可接受 |
| HTTP 明文下会话 token 可被劫持 | 中 | §4.7，本期接受；预留 HTTPS 开关 |
| 配网依赖 `core/netmgr`，该模块尚未实现 | 中 | §11.2 A5 前置 |
| 索引与录像文件因断电不一致 | 中 | §7.3 启动后台校对 |
| HAL 改动需升 `HAL_API_VERSION` | 低 | 同步补一致性测试与 mock 实现 |
| `ota.slot_size_mb: 40` 在 16MB Nor 上不成立 | 低 | §9.4 记录，打包期核定 |
| TF 卡写入寿命受高码率录像影响 | 低 | §7.2 分段 + §7.5 写盘缓冲减少小块写 |
