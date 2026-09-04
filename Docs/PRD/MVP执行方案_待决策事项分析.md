# IpcCloud MVP 执行方案 —— 待决策事项技术分析

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 编写日期 | 2026-09-04 |
| 上游文档 | [TP-LINK商云分析报告_IPC平台PRD参考.md](./TP-LINK商云分析报告_IPC平台PRD参考.md) |
| 决策范围 | ① GK7205 系列固件定制 ② 设备兼容性与私有协议定义 ③ 播放器（h265web.js）④ 流媒体多节点调度 ⑤ TF 卡录像播放 ⑥ P2P 播放 |
| 技术栈约束 | ZLMediaKit / Nuxt.js / Go / GK7205V100·V200·V300 |

> **变更通告（2026-09-04，v1.1）**：决策项 B 的结论已由 [私有协议优先接入方案_IDP-v1.md](./私有协议优先接入方案_IDP-v1.md) **替代**——自研 GK 板走私有协议 IDP（MQTT/TLS 控制面 + RTMP 按需推流）；**已购 TP-LINK IPC 与海康设备走 GB28181（M1 核心）**；萤石设备走 ONVIF/RTSP 直连（M3b）；不做云对云。本文 §5.2（国标 SDP 调度）、§6.2（国标回放时序）对 TP-LINK/海康仍有效；涉及 GK 板的部分以新文为准。

---

## 目录

1. [决策总览与 MVP 边界](#1-决策总览与-mvp-边界)
2. [决策项 A：GK7205 系列固件定制](#2-决策项-agk7205-系列固件定制)
3. [决策项 B：设备兼容性与 IpcCloud 设备规范](#3-决策项-b设备兼容性与-ipccloud-设备规范)
4. [决策项 C：播放器 h265web.js](#4-决策项-c播放器-h265webjs)
5. [决策项 D：流媒体多节点调度](#5-决策项-d流媒体多节点调度)
6. [决策项 E：IPC 本地 TF 卡录像播放](#6-决策项-eipc-本地-tf-卡录像播放)
7. [决策项 F：P2P 播放](#7-决策项-fp2p-播放)
8. [MVP 目标架构](#8-mvp-目标架构)
9. [实施步骤（里程碑制）](#9-实施步骤里程碑制)
10. [资源需求](#10-资源需求)
11. [风险登记册](#11-风险登记册)
- 附录 A：评估打分明细
- 附录 B：与上游需求清单的映射

---

## 1. 决策总览与 MVP 边界

### 1.1 一页结论

| 决策项 | MVP 结论 | 关键理由 |
|---|---|---|
| A. 固件平台 | **讨论中，未冻结。** 倾向 GK7205 系列（V200 量产倾向）；具体芯片、传感器 IC、是否外接 ISP、Flash 待硬件选型评审。固件当前只做通用层与 HAL 接口（见 [平台化架构](./IPC固件平台化架构_HAL适配方案.md)），L0 实现待硬件定后启动 | 平台与接入不依赖硬件结论，可先行；见 [决策记录与待定事项](./决策记录与待定事项.md) |
| B. 兼容性 | **GB28181-2016 作为唯一接入基线**；GK 板在国标之上叠加"IpcCloud 扩展通道"（MQTT over TLS）承载配置/OTA/P2P 信令 | 已购 GB 设备零改动即可接入；避免为两类设备维护两套接入逻辑；扩展能力不污染国标 |
| C. 播放器 | **采用 h265web.js（PRO 免费版）为唯一播放器**：直播走 WS-FLV(H.265)，回放走 WS-FLV/MP4，后期 P2P 走其 WebRTC/WHEP 核心 | 2026/08 版已原生支持 WebRTC WHEP 与 ZLM 私有 WebRTC API；WASM/WebCodec 双解码路径绕开浏览器 H.265 支持不一致问题；官方推荐配套 ZLM |
| D. 多节点 | **MVP 做"控制面调度、数据面无状态"的最简集群**：后端维护节点表 + 最少流数调度 + 通道粘性；不做 ZLM 溯源级联 | 国标 INVITE 的 SDP 必须在信令阶段确定收流节点，调度天然落在 Go 后端；溯源级联属于 P2 优化 |
| E. TF 卡回放 | **走 GB28181 录像检索（RecordInfo）+ 回放 INVITE（设备推 PS 流 → ZLM → WS-FLV）**；GK 板二期增加 HTTP 分段直读实现秒级 seek 与下载 | 已有国标设备与 GK 板走同一链路；ZLM 原生支持 PS 收流；倍速/暂停/定位由国标 MANSCDP 覆盖 |
| F. P2P | **MVP 不纳入**；三期在 GK 板（V300/V200）实现设备端 WebRTC（libdatachannel/libpeer + coturn），H.264 子码流先行，局域网直连为首要收益，跨网失败降级 ZLM 中继 | 行业数据显示纯 P2P 穿透成功率 20%~92% 不等，中继兜底不可省；V100 内存不足；浏览器 H.265 WebRTC 支持仍不统一 |

### 1.2 MVP 边界（In / Out）

| In（MVP 必须） | Out（明确不做，留给后续里程碑） |
|---|---|
| GB28181 注册/心跳/目录/实时点播/BYE | 固件在线升级源（仅本地包 OTA） |
| 设备/通道管理、分组、在线状态 | 第三方 ONVIF 设备接入适配器（M3b） |
| h265web.js 直播（WS-FLV），1/4 分屏 | 9/16 分屏（依赖 WebCodec 硬解，二期） |
| TF 卡录像检索 + 国标回放（播放/暂停/倍速/拖动） | 录像下载、HTTP 直读、平台侧录像 |
| 多 ZLM 节点静态注册 + 调度 + 健康检查 | ZLM 溯源级联、跨节点迁移 |
| GK 板固件：双码流、RTSP、国标设备端、TF 录像、MQTT 扩展通道、OTA | P2P（三期）、语音对讲、PTZ（GK 板无云台，仅协议预留） |
| 基础告警（离线、流断）+ 消息中心 | 智能事件（移动侦测上报三期）、外部通知 |
| RBAC（通道级）、操作日志 | OpenAPI、分享链接 |

> 固件接入方式已升级为**四协议必备**：IDP 云端主通道 + ONVIF Profile S 服务端 + RTSP 服务端 + GB28181 设备端 UA，详见 [私有协议优先接入方案 §4.1](./私有协议优先接入方案_IDP-v1.md)。下文 §2.3 的"取流/国标设备端/扩展通道"三行以该节为准。

---

## 2. 决策项 A：GK7205 系列固件定制

### 2.1 芯片能力对照

| 参数 | GK7205V100 | GK7205V200 | GK7205V300 |
|---|---|---|---|
| CPU | Cortex-A7（以 datasheet 为准） | Cortex-A7 900MHz | Cortex-A7 900MHz |
| 内置内存 | 64MB 级（需确认） | 64MB DDR2 级（需确认） | 128MB DDR3L（1Gb） |
| 最大编码 | 1080p 级 | 3MP 级 | 5MP（2592×1944）/ 2880×1620 |
| 编码格式 | H.264/H.265 | H.264/H.265 | H.264/H.265/MJPEG |
| 海思对位 | Hi3516EV100 级 | Hi3516EV200 | Hi3516EV300（引脚兼容） |
| OpenIPC 支持 | 未在 openhisilicon 列表 | 是（gk7205v200 族） | 是（gk7205v200 族） |
| 结论 | 仅兼容验证 | 兼容目标 | **主开发目标** |

> 注：V100/V200 内存规格以贵方持有的官方 datasheet 为准；本文按行业公开资料给出量级。

### 2.2 固件方案对比

| 方案 | 说明 | 开发复杂度 | 资源占用 | 兼容性 | 风险 |
|---|---|---|---|---|---|
| A1 厂商 SDK + 自研应用层 | Goke 官方 SDK（MPP/ISP/VENC）+ 自研 RTSP/国标/录像/MQTT | 高（全部自写） | 可控（自定裁剪） | 官方支持稳妥 | 周期长、人力大 |
| A2 **OpenIPC 基座 + 自研业务层**（推荐） | OpenIPC（Buildroot + openhisilicon 内核模块 + majestic 流引擎）提供 RTSP/双码流/ISP 调优/OTA 框架；自研国标设备端、TF 录像索引、MQTT 扩展、P2P | 中 | majestic ~10MB 级 RSS；自研部分可控 | V200/V300 官方在列；V100 需自行适配或走 A1 | majestic 为闭源二进制（免费使用），深度定制受限 |
| A3 ZLMediaKit C API 直接跑在设备端 | 用 ZLM 做设备端 RTSP/RTMP 推流/录像 | 中 | ARM32 下 RSS 偏大（估 15~30MB），64MB 平台吃紧 | 与云端同栈、协议一致 | V100/V200 内存风险；国标设备端仍需自研 |

**结论**：采用 **A2**。V300/V200 走 OpenIPC 基座；V100 若必须支持，采用 A1 精简版（仅国标 + 单码流 + 无 P2P）。

### 2.3 固件功能清单（MVP）

| 模块 | 功能 | 备注 |
|---|---|---|
| 编码 | 主码流 H.265 ≤2560×1440@20fps；子码流 H.264 640×360@15fps | 子码流强制 H.264：为 P2P（WebRTC 浏览器兼容）与 16 分屏预留 |
| 取流 | RTSP（局域网调试用）、GB28181 实时点播（PS over RTP，UDP/TCP 被动） | TCP 被动优先，穿越 NAT 更稳 |
| 国标设备端 | REGISTER（Digest）、Keepalive、Catalog、DeviceInfo、INVITE/ACK/BYE、RecordInfo、Playback（PLAY/PAUSE/TEARDOWN/scale）、PTZ 指令解析（预留） | 基于 eXosip2/osip2（GPL 注意）或自研精简 SIP UA |
| TF 录像 | 定长分段（建议 60s）MP4 或 PS 文件；SQLite 索引（起止时间、类型、文件、大小）；循环覆盖；事件标记 | RecordInfo 查询直接查索引表 |
| 扩展通道 | MQTT over TLS 长连：上报心跳/状态/存储/告警；下发配置/OTA/P2P 信令 | 详见 §3 |
| OTA | 从平台下发 URL → 校验 SHA256 → 双分区 A/B 切换 → 回滚 | 商云无回退机制，此处补齐 |
| 安全 | 出厂强制改密、国标密码与 MQTT 证书按设备唯一、关闭 telnet | — |

---

## 3. 决策项 B：设备兼容性与 IpcCloud 设备规范

### 3.1 问题定义

平台需同时承载两类设备：
1. **已购的 GB28181 设备**（第三方，不可改固件）——只能用国标能力。
2. **GK 开发板自研固件**——可任意扩展。

### 3.2 方案对比

| 方案 | 说明 | 复杂度 | 兼容性 | 结论 |
|---|---|---|---|---|
| B1 全私有协议 | GK 板走自研协议，GB 设备走国标，平台两套接入 | 高 | 差（两套状态机） | 否 |
| B2 **国标基线 + 扩展通道**（推荐） | 所有设备走 GB28181 做注册/点播/回放；GK 板额外建立 MQTT 通道承载国标覆盖不到的能力 | 中 | 好（单一接入模型） | **采用** |
| B3 国标 + 私有 SIP MESSAGE 扩展 | 在 MANSCDP XML 里加私有字段 | 低 | 一般（与第三方平台互通时易冲突；SIP 不适合大 payload 与推送） | 否 |

### 3.3 IpcCloud 设备规范 v1（概要）

```
IpcCloud Device Spec v1
├── 必选：GB28181-2016 子集
│   ├── REGISTER / Keepalive（60s，3 次超时判离线）
│   ├── Catalog / DeviceInfo / DeviceStatus
│   ├── INVITE 实时（PS/RTP，TCP 被动 + UDP）
│   ├── RecordInfo（按时间范围/类型查询，分页）
│   └── Playback INVITE + INFO(PLAY/PAUSE/TEARDOWN, scale=0.25~8)
└── 可选：IpcCloud Ext（MQTT over TLS 8883，Topic 前缀 ipc/{deviceId}/）
    ├── ↑ status      设备状态：CPU/内存/温度/TF 容量与健康/码率/在线时长（30s）
    ├── ↑ event       告警事件：move/tamper/io/tf_error（即时）
    ├── ↓ config      编码参数/OSD/录像计划/时间同步
    ├── ↓ ota         固件包 URL + SHA256 + 版本 → ↑ ota/progress
    ├── ↓ p2p/offer   WebRTC 信令（三期）
    └── ↓ record/http 录像文件直读授权（二期）
```

平台侧行为：设备注册时按 `Manufacturer/Model` 或 MQTT 首次上线判定 **能力集（Capability Set）**，UI 按能力灰显功能（无扩展通道的设备不显示 CPU/TF 健康等）。

---

## 4. 决策项 C：播放器 h265web.js

### 4.1 候选对比

| 播放器 | H.265 直播 | 解码路径 | 回放/Seek | WebRTC | 许可 | 结论 |
|---|---|---|---|---|---|---|
| **h265web.js PRO** | WS-FLV/HTTP-FLV/WS-TS/WS-HEVC/HLS | WebCodec 硬解 → WASM(SIMD) 软解 → MSE | MP4/FLV/TS 点播，倍速、精准 seek、截图、MediaInfo | 2026/08 新增 WHEP + ZLM 私有 API | PRO 现为免费；需审阅仓库内 LICENSE-Free 条款 | **采用** |
| jessibuca (v3) | 同类 | WASM/WebCodec | 支持 | 需 Pro 版 | 免费版有限制，Pro 商业授权 | 备选 |
| mpegts.js | H.265 需浏览器 MSE 支持 | MSE 硬解 | 支持 | 无 | Apache-2.0 | H.265 覆盖不稳，否 |
| ZLMRTCClient.js | WebRTC only | 浏览器原生 | 无 | 是 | MIT | 仅作 P2P 备选 |

### 4.2 h265web.js 在各链路中的用法

| 链路 | ZLM 输出 | h265web.js 入口 | 说明 |
|---|---|---|---|
| 实时预览（H.265 主码流） | `ws://node/live/{stream}.live.flv` | `core: 'webcodec_hevc'` 优先，探测失败自动 `wasm_hevc` | WS-FLV 延时 ≤1s |
| 实时预览（H.264 子码流，分屏） | 同上 | 同上（H.264 同样支持） | 4 分屏 WASM 可承受；9/16 分屏需 WebCodec |
| 国标回放 | `ws://node/rtp/{ssrc}.live.flv`（回放流以直播形式输出） | 同直播；倍速/拖动通过后端 → SIP INFO 控制，播放器仅显示 | 关键：回放不是文件点播，seek 由设备完成 |
| GK 板录像直读（二期） | 设备 HTTP MP4（经平台反代） | `load_media(url)` MP4 点播，本地 seek/倍速 | 秒级拖动、可下载 |
| P2P（三期） | 设备端 WebRTC | `core: 'webrtc'`（WHEP 或自定义信令） | 浏览器原生解码，H.264 子码流先行 |

### 4.3 性能约束与对策

| 约束 | 数据 | 对策 |
|---|---|---|
| WASM 软解 CPU | 官方建议 ≤30fps、码率 ≤1.5Mbps，最佳 300~600kbps | 主码流单画面；分屏一律子码流 |
| 多线程 WASM | 需 HTTPS + COOP/COEP 响应头 | Nuxt 服务端/Nginx 统一下发 `Cross-Origin-Opener-Policy: same-origin`、`Cross-Origin-Embedder-Policy: require-corp` |
| WebCodec 硬解可用性 | Chrome/Edge 桌面版对 HEVC 硬解依赖 OS 解码器 | 启动时探测 `VideoDecoder.isConfigSupported({codec:'hvc1.…'})`，选择核心 |
| 首屏 | WASM 加载 ~2MB | 站点预加载、Service Worker 缓存 |

### 4.4 需验证事项（Spike）

1. 仓库 `LICENSE-Free_*.MD` 与 PRO 免费声明的商用条款一致性（法务确认）。
2. WS-FLV H.265 + G.711A 音频在 Chrome/Edge/Safari 最新版的实际首帧与 CPU 占用。
3. 回放流（ZLM `/rtp/` app）在 PAUSE 后恢复时播放器缓冲行为。
4. `core: 'webrtc'` 是否支持自定义信令（非 WHEP），决定三期 P2P 信令设计。

---

## 5. 决策项 D：流媒体多节点调度

### 5.1 方案对比

| 方案 | 机制 | 复杂度 | 性能/扩展性 | 适用 |
|---|---|---|---|---|
| D1 单节点 | 一台 ZLM | 低 | 单机上限（参考：200 路拉流/1000 路播放级） | PoC |
| D2 **控制面调度 + 无状态节点**（推荐） | Go 后端维护节点表；开流时选节点并写"通道→节点"粘性映射；播放地址直接指向该节点 | 中 | 线性扩容；节点故障时通道重调度 | **MVP** |
| D3 ZLM 溯源级联 | 边缘节点 `on_stream_not_found` 时按 `origin_url` 从源节点拉 | 中高 | 观看端就近、源节点仅出一路 | 多地域/大量观看者（P2） |
| D4 统一接入网关（SLB/DNS） | 前置 LB 做四层分发 | 中 | 对国标 SDP 端口无法透明 | 否 |

### 5.2 D2 设计要点

```
节点表 media_node
  id | name | api_url | secret | sip_recv_ip(公网) | rtp_port_range | max_streams | weight | status | last_keepalive

调度流程（实时点播）
  1. 查 Redis: channel:{id}:node → 已有活跃流则直接复用（粘性）
  2. 否则筛选 status=online 且 streams<max 的节点，按 (streams/max*weight) 最小者
  3. 调 ZLM openRtpServer(节点) → 得端口 → 组 SDP → SIP INVITE
  4. on_stream_changed(regist=true) → 写映射；on_stream_none_reader → BYE + closeRtpServer + 删映射
健康
  on_server_keepalive / on_server_started 更新 status；30s 无心跳→offline→该节点通道标记需重建
```

**为什么调度必须在 Go 后端**：GB28181 的收流 IP/端口在 INVITE 的 SDP 中确定，无法事后由流媒体层透明迁移；RTSP 拉流类设备虽可在任意节点 `addStreamProxy`，但保持同一模型可减少分叉。

### 5.3 MVP 验收

- 2 个 ZLM 节点，100 路国标通道，均衡度（最大/最小流数比）≤1.3。
- 关闭一个节点后，其通道在下一次点播时自动落到健康节点，无需人工干预。
- 节点管理页可见：状态、流数、带宽、最近心跳。

---

## 6. 决策项 E：IPC 本地 TF 卡录像播放

### 6.1 方案对比

| 方案 | 链路 | 复杂度 | 兼容性 | 体验 | 结论 |
|---|---|---|---|---|---|
| E1 **国标回放**（推荐 MVP） | RecordInfo 查询 → Playback INVITE → 设备读 TF 推 PS 流 → ZLM → WS-FLV → h265web.js；INFO 控制 PLAY/PAUSE/scale/Range | 中 | **已购 GB 设备 + GK 板通用** | 拖动需设备重定位，延时 1~2s；倍速由设备端跳帧 | **采用** |
| E2 HTTP 分段直读 | 平台经扩展通道授权 → 反代设备 HTTP → h265web.js MP4 点播 | 中 | 仅 GK 板 | 秒级 seek、可下载、可缩略 | 二期（GK 板增强） |
| E3 先上传再播 | 设备把 TF 录像上传平台 | 低 | 通用 | 占用上行与平台存储 | 否（违背"减少带宽"目标） |
| E4 ONVIF Profile G | ONVIF Replay | 高 | 第三方支持度低 | — | 否 |

### 6.2 E1 详细流程

```
浙 前端                 Go 后端                         ZLM 节点                GK 板 / GB 设备
  │ 选日期/通道          │                               │                       │
  │──GET /records──────▶│──SIP MESSAGE RecordInfo──────────────────────────────▶│ 查 SQLite 索引
  │◀─时间轴段落─────────│◀─RecordList(分页, 类型)───────────────────────────────│
  │ 点击时间点           │                               │                       │
  │──POST /playback────▶│ 选节点 → openRtpServer ───────▶│                       │
  │                     │──INVITE(s=Playback, t=start end, SDP)────────────────▶│ 读文件推 PS/RTP
  │◀─ws flv url─────────│◀──on_stream_changed───────────│◀──RTP───────────────│
  │ h265web.js 播放      │                               │                       │
  │──PUT speed=4───────▶│──INFO PLAY scale=4.0─────────────────────────────────▶│ 跳帧推送
  │──PUT seek=t────────▶│──INFO PLAY Range npt=t-─────────────────────────────▶│ 重定位
  │──PUT pause─────────▶│──INFO PAUSE──────────────────────────────────────────▶│
  │ 关闭/30s 无人看      │──BYE + closeRtpServer─────────▶│                       │
```

### 6.3 GK 板固件侧要求

| 项 | 要求 |
|---|---|
| 录像分段 | 60s 定长；文件名含起始时间；MP4（便于二期 HTTP 直读）或 PS（便于国标直推，无需转封装）——建议 **PS 分段 + 索引**，二期直读时按需转 MP4 |
| 索引 | SQLite：`segments(start_ts, end_ts, type[timer|event], path, size, has_audio)`；启动扫描修复 |
| RecordInfo | 支持 StartTime/EndTime/Type 过滤，分页 SN，返回 Item 列表（≤ 100/页） |
| Playback | 单设备并发回放 ≥1 路（V300 ≥2 路）；支持 scale 0.25/0.5/1/2/4/8；PAUSE 停止推送保持会话；Range 重定位精度 ≤1s |
| 时间 | NTP 同步（平台下发），录像时间戳以 UTC 存储 |
| 循环覆盖 | 剩余 <5% 时删除最旧非事件段；事件段保护期可配 |

### 6.4 已购 GB 设备

按 GB28181-2016 应能支持 RecordInfo 与 Playback；需在 Phase 1 对每型号做**兼容性矩阵测试**（是否支持 scale、Range、TCP 被动）。不支持项在 UI 灰显。

---

## 7. 决策项 F：P2P 播放

### 7.1 目标澄清

"P2P 减少流媒体带宽"在以下场景收益最大：
1. **浏览器与设备同局域网**（现场巡检、门店内看店）：直连后平台**零转发带宽**，且延时最低。
2. **单观看者跨网**：直连成功即节省一路中继带宽。
3. **多观看者**：P2P 反而增加设备上行；应由 ZLM 中继/级联承担。

### 7.2 方案对比

| 方案 | 机制 | 设备端资源 | 复杂度 | 穿透成功率（行业公开数据） | 浏览器兼容 | 结论 |
|---|---|---|---|---|---|---|
| F1 **设备端标准 WebRTC**（推荐三期） | libdatachannel(C++)/libpeer(C) 在设备端做 PeerConnection；平台做信令 + coturn(STUN/TURN)；浏览器用 h265web.js `core:'webrtc'` 或原生 RTCPeerConnection | libdatachannel + mbedtls：数 MB 级；libpeer 更轻（ESP32 可跑） | 中高 | 直连约 70~80%，Symmetric NAT 约 20% 需 TURN；EZVIZ 公开数据 P2P 仅 ~20%，TUTK ~92%（含 relay） | 标准 WebRTC，全平台 | **采用**，H.264 子码流先行 |
| F2 私有 UDP 打洞 + 私有传输 + WASM 解码 | TUTK/Kalay 类自研 | 中 | 高（打洞、拥塞控制、加密全部自研） | 同上 | 需自写 JS 接收端 | 否（安全教训多：iLnkP2P/Reolink CVE） |
| F3 ZLM WebRTC 中继（非 P2P） | 设备 → ZLM → 浏览器 WebRTC | 无 | 低 | 100% | 全平台 | 作为 **F1 的兜底** |
| F4 边缘 ZLM 节点（门店/园区本地） | 局域网内部署小型 ZLM，浏览器就近取流 | 需边缘硬件 | 中 | 100%（局域网） | 全平台 | 大门店场景备选 |

### 7.3 F1 落地要点

| 项 | 设计 |
|---|---|
| 信令 | 复用 MQTT 扩展通道：平台转发 SDP offer/answer 与 ICE candidates（trickle） |
| ICE | coturn 自建：STUN 免费探测；TURN 仅兜底，计入带宽预算 |
| 编码 | 三期首版 **H.264 子码流**（浏览器 WebRTC 全兼容）；H.265 待 Chrome 硬解 HEVC-in-WebRTC 普及后开放，或用 DataChannel 承载 HEVC ES 交给 h265web.js WASM 解码（需验证其是否暴露裸流喂入 API） |
| 会话上限 | 单设备 P2P 并发 ≤2（V300）/ ≤1（V200）；超出走 ZLM |
| 降级策略 | ICE 10s 未连通 → 前端自动切 ZLM WS-FLV；UI 显示"直连/中继"标识 |
| 芯片适用 | V300 支持；V200 需实测内存；V100 不支持 |
| 安全 | DTLS-SRTP 默认；信令走 TLS + 设备证书；不硬编码任何密钥 |

### 7.4 为什么 MVP 不做

- 与 A~E 相比它对"能否用起来"没有贡献，而对固件资源、信令、TURN 运维引入最多不确定性。
- 带宽收益在多观看者场景为负，需要先通过 MVP 拿到真实观看模式数据再决策 TURN 预算。

---

## 8. MVP 目标架构

```
┌──────────────── Nuxt.js（SPA 为主）────────────────┐
│ 设备/通道树 │ 1·4 分屏预览 │ 回放时间轴 │ 告警 │ 节点管理 │
│ <H265Player>：h265web.js（webcodec_hevc→wasm_hevc）│
│ WebSocket：状态/告警/回放控制回执                  │
└─────────────────────┬──────────────────────────────┘
                      │ REST / WS
┌─────────────────────▼──────────────── Go 后端 ─────────────────────────────┐
│ auth/rbac │ device/channel │ gb28181-sip (REGISTER/Catalog/INVITE/INFO/BYE) │
│ scheduler(media_node) │ zlm-client + hooks │ record(RecordInfo 代理)       │
│ alarm/notify(ws) │ mqtt-ext(GK 板扩展通道) │ ota │ audit                   │
│ PostgreSQL · Redis · coturn(三期)                                           │
└───────┬──────────────────────┬────────────────────────┬────────────────────┘
        │ REST(secret)/Hook    │ SIP 5060 UDP/TCP        │ MQTT/TLS 8883
┌───────▼──────────┐    ┌──────▼───────────┐     ┌───────▼───────────────────┐
│ ZLM 节点 ×N      │◀───│ 已购 GB28181 设备 │     │ GK7205 V300/V200 自研固件 │
│ openRtpServer    │RTP │ (仅国标能力)      │     │ OpenIPC 基座 + 国标 UA    │
│ → WS-FLV/HLS     │◀───┼──────────────────┼─────│ + TF 录像索引 + MQTT + OTA│
└──────────────────┘    └──────────────────┘     └───────────────────────────┘
```

---

## 9. 实施步骤（里程碑制）

> 不给出时间估算；每个里程碑以"退出条件"作为完成判定。

### M0 决策冻结与环境

| 任务 | 交付物 | 退出条件 |
|---|---|---|
| 确认 V100/V200 datasheet 内存与编码规格 | 芯片能力表（替换 §2.1 待确认项） | 三款芯片参数表签字 |
| h265web.js 许可与 4 项 Spike（§4.4） | Spike 报告 | 法务通过；WS-FLV H.265 在 3 浏览器可播 |
| 已购 GB 设备型号清单 + 国标能力探测 | 兼容性矿阵初稿 | 每型号 REGISTER/INVITE/RecordInfo/Playback 打勾 |
| 搭建 ZLM ×2 + Go 骨架 + Nuxt 骨架 + PostgreSQL/Redis | 可运行的空平台 | CI 通过、`on_server_keepalive` 入库 |
| 冻结《IpcCloud 设备规范 v1》 | 规范文档 | 固件与后端团队共同签字 |

### M1 国标接入与直播（平台）

| 任务 | 退出条件 |
|---|---|
| SIP 服务端：REGISTER Digest、Keepalive、Catalog、DeviceInfo | 已购设备注册上线，通道自动入库，3 次心跳超时判离线 |
| 调度器 + INVITE 实时点播 + Hook 闭环（`on_stream_changed`/`none_reader`） | 100 路通道在 2 节点均衡；无人观看 30s 自动 BYE |
| 前端：通道树、单/4 分屏、h265web.js 封装、清晰度切换（国标 StreamNumber） | 局域网首帧 ≤1.5s；4 分屏 CPU 可接受 |
| RBAC（通道级）、操作日志、设备离线/流断告警、消息中心 | 未授权通道不可见；告警 ≤3s 推送 |

### M2 GK 板固件 MVP

| 任务 | 退出条件 |
|---|---|
| OpenIPC 基座移植到 V300 板：内核、ISP、majestic 双码流、RTSP | RTSP 主/子码流稼定 24h |
| 国标设备端 UA：REGISTER/Keepalive/Catalog/INVITE/BYE（TCP 被动 + UDP） | 平台点播成功率 ≥99%（局域网 1000 次） |
| TF 录像：PS 分段 + SQLite 索引 + 循环覆盖 | 断电重启后索引自修复；剩余 <5% 自动覆盖 |
| MQTT 扩展通道：status/event/config/ota | 平台可见 CPU/TF 健康；OTA A/B 切换 + 回滚 |
| V200 移植验证 | 同上功能集；内存余量 ≥15MB |

### M3 TF 卡回放

| 任务 | 退出条件 |
|---|---|
| 后端 RecordInfo 代理（分页聚合）、Playback INVITE、INFO 控制（PLAY/PAUSE/scale/Range） | 已购设备与 GK 板均可回放 |
| 固件 RecordInfo/Playback 实现（含 scale 0.25~8、Range 定位） | 定位精度 ≤1s；倍速 8x 不断流 |
| 前端时间轴（日期、类型色块、拖动、倍速 9 档、暂停、截图） | 拖动后 ≤2s 出画 |
| 多节点下回放调度 + 会话回收 | 关闭页面 30s 内 BYE、端口释放 |

### M4 加固与验收

| 任务 | 退出条件 |
|---|---|
| 压测：200 通道 / 50 并发观看 / 20 并发回放 | 节点 CPU <70%；无内存泄漏 |
| 安全：SIP 密码策略、MQTT 证书、API 鉴权、日志脱敏 | 渗透基线通过 |
| 兼容性矩阵终版、错误码文案（E1xxx~E6xxx） | 每个失败路径有可读提示 |

### 后续（非 MVP）

- **M5** GK 板 HTTP 录像直读与下载、9/16 分屏（WebCodec）、ONVIF 第三方接入。
- **M6** P2P（§7.3）、ZLM 溯源级联、智能事件、OpenAPI。

---

## 10. 资源需求

| 角色 | 主要工作 | MVP 阶段 |
|---|---|---|
| 嵌入式固件（Linux/媒体） | OpenIPC 移植、国标 UA、TF 录像、MQTT、OTA | M2、M3 |
| Go 后端 | SIP 服务端、调度器、ZLM 集成、REST/WS | M1、M3 |
| 前端（Nuxt） | 播放器封装、分屏、时间轴、管理页 | M1、M3 |
| 测试/兼容性 | 国标设备矩阵、压测、浏览器矩阵 | M0~M4 |
| 硬件环境 | V300 开发板 ≥3、V200 ≥2、V100 ≥1、TF 卡（高耐久 ≥64GB）、2 台 ZLM 服务器（4C8G 起）、已购 GB 设备各型号 ≥1 | M0 |
| 基础软件 | ZLMediaKit、PostgreSQL、Redis、Nginx（HTTPS + COOP/COEP）、coturn（三期） | M0 |

---

## 11. 风险登记册

| # | 风险 | 影响 | 概率 | 缓解 |
|---|---|---|---|---|
| R1 | V100/V200 内存不足以承载国标 UA + 录像 + MQTT | 兼容目标缩水 | 中 | M0 实测 RSS；V100 降为"仅国标单码流"；必要时去 MQTT 用 SIP 心跳扩展 |
| R2 | 已购 GB 设备不支持 Playback scale/Range 或仅 UDP | 回放体验分化 | 中 | 兼容性矩阵 + UI 灰显；不支持 Range 的用"重新 INVITE 指定起止时间"模拟定位 |
| R3 | h265web.js 许可条款变化 | 法务/替换成本 | 低 | 播放器封装层隔离 API，备选 jessibuca；锁定版本并归档 |
| R4 | WASM 软解在低端 PC 上 4 分屏卡顿 | 体验 | 中 | 分屏强制子码流 ≤600kbps；探测 WebCodec 优先 |
| R5 | OpenIPC majestic 闭源导致编码参数/录像行为不可控 | 固件定制受限 | 中 | 录像与国标 UA 不依赖 majestic，自研进程从 RTSP/共享内存取帧；必要时切 A1 |
| R6 | 国标 TCP 被动模式下 NAT/防火墙阻断 | 跨网接入失败 | 中 | 同时支持 UDP；节点公网端口段固定并文档化 |
| R7 | 多节点粘性映射在节点宕机后残留 | 点播失败 | 低 | 心跳超时清理映射；点播前二次校验 `getMediaList` |
| R8 | P2P 三期 TURN 带宽成本超预期 | 收益为负 | 中 | MVP 阶段采集观看模式（局域网占比、单/多观看者比例）再定 |
| R9 | eXosip2/osip2 为 GPL | 固件开源义务 | 中 | 评估 LGPL/商业 SIP 栈或自研精简 UA（国标设备端所需消息类型有限） |

---

## 附录 A：评估打分明细

评分 1~5（5 最优）；权重：开发复杂度 30%、资源需求 20%、兼容性 30%、性能 20%。

| 决策 | 方案 | 复杂度 | 资源 | 兼容 | 性能 | 加权 | 选择 |
|---|---|---|---|---|---|---|---|
| A | A1 厂商 SDK 全自研 | 2 | 4 | 4 | 4 | 3.4 | |
| A | A2 OpenIPC + 自研业务 | 4 | 4 | 4 | 4 | **4.0** | ✓ |
| A | A3 ZLM C API 设备端 | 3 | 2 | 3 | 4 | 3.0 | |
| B | B1 全私有 | 2 | 3 | 2 | 4 | 2.6 | |
| B | B2 国标 + MQTT 扩展 | 4 | 4 | 5 | 4 | **4.3** | ✓ |
| B | B3 国标 + SIP 扩展 | 4 | 4 | 3 | 3 | 3.5 | |
| C | h265web.js | 4 | 3 | 5 | 4 | **4.1** | ✓ |
| C | jessibuca | 4 | 3 | 4 | 4 | 3.8 | |
| C | mpegts.js | 5 | 5 | 2 | 5 | 3.9 | |
| D | D1 单节点 | 5 | 5 | 3 | 2 | 3.8 | |
| D | D2 控制面调度 | 4 | 4 | 5 | 4 | **4.3** | ✓ |
| D | D3 溯源级联 | 3 | 3 | 4 | 5 | 3.7 | |
| E | E1 国标回放 | 4 | 4 | 5 | 3 | **4.1** | ✓ |
| E | E2 HTTP 直读 | 4 | 4 | 2 | 5 | 3.6 | 二期 |
| E | E3 先上传 | 5 | 1 | 5 | 3 | 3.8 | |
| F | F1 设备端 WebRTC | 2 | 3 | 4 | 5 | 3.4 | 三期 |
| F | F2 私有 P2P | 1 | 3 | 3 | 5 | 2.8 | |
| F | F3 ZLM 中继 | 5 | 4 | 5 | 3 | 4.4 | 兜底 |

## 附录 B：与上游需求清单的映射

| 本文决策 | 上游需求编号（TP-LINK 商云分析报告 §7） |
|---|---|
| A 固件 | MGR-11、MGR-12、MGR-18（设备端提供数据源） |
| B 设备规范 | ADD-06、ADD-07、MGR-04、MGR-15 |
| C 播放器 | LIVE-01、LIVE-04、LIVE-05、LIVE-06、REC-07 |
| D 多节点 | SYS-01、LIVE-02、LIVE-11 |
| E TF 回放 | REC-05、REC-06、REC-07；REC-08（二期） |
| F P2P | 新增（超出商云能力范围），影响 LIVE-01 降级链 |
