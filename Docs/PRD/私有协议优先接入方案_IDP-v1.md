# 私有协议优先 + 第三方设备直连接入方案 —— IDP v1 / GB28181 / ONVIF-RTSP

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.1（替代 v1.0，删除云对云路线） |
| 编写日期 | 2026-09-04 |
| 变更性质 | 替代 [MVP执行方案](./MVP执行方案_待决策事项分析.md) §3"决策项 B"：**自研设备走私有协议 IDP；第三方设备（TP-LINK / 海康 / 萤石）走设备直连协议，GB28181 为首选，ONVIF/RTSP 为补充** |
| 现状约束 | 项目已采购部分 **TP-LINK IPC**，需在 MVP 内接入 |
| 技术栈 | ZLMediaKit / Nuxt.js / Go / GK7205V200·V300 固件 |

---

## 目录

1. [决策结论](#1-决策结论)
2. [第三方设备直连能力矩阵（TP-LINK / 海康 / 萤石）](#2-第三方设备直连能力矩阵tp-link--海康--萤石)
3. [接入适配层架构](#3-接入适配层架构)
4. [IDP v1 协议设计（自研 GK 板）](#4-idp-v1-协议设计自研-gk-板)
5. [GB28181 适配器（TP-LINK / 海康 首选）](#5-gb28181-适配器tp-link--海康-首选)
6. [ONVIF / RTSP 适配器（萤石 / 补充）](#6-onvif--rtsp-适配器萤石--补充)
7. [添加设备交互（统一入口）](#7-添加设备交互统一入口)
8. [媒体面：直播与 TF 卡回放](#8-媒体面直播与-tf-卡回放)
9. [对 MVP 里程碑的调整](#9-对-mvp-里程碑的调整)
10. [兼容性验证计划](#10-兼容性验证计划)
11. [风险与待确认](#11-风险与待确认)
- 附录 A：IDP 消息集速查
- 附录 B：三类协议差异对照

---

## 1. 决策结论

| 设备来源 | 接入协议 | 优先级 | 理由 |
|---|---|---|---|
| **自研 GK7205 板** | **IDP v1**（云端主通道：MQTT/TLS 控制面 + RTMP(S) 按需推流）；固件**同时必须提供 ONVIF Profile S 服务端、RTSP 服务端、GB28181 设备端 UA**（见 §4.1） | MVP-P0 | 云端走 IDP 获得消费级绑定与完整能力；ONVIF/RTSP 保证局域网/NVR/第三方软件即插即用；GB28181 UA 保证能注册到第三方国标平台或作为我方接入兜底 |
| **已购 TP-LINK IPC** | **GB28181-2016**（设备 Web「平台接入」填 SIP 参数） | MVP-P0 | TP-LINK 商用 IPC 原生支持国标；已购设备不可改固件；无需公网 IP（设备主动注册） |
| **海康 IPC/NVR** | **GB28181** 首选；ONVIF/RTSP 备选；ISUP 可选 | MVP-P0（GB）/ P1 | 海康国标实现最完整（含录像检索与回放） |
| **萤石 IPC** | **RTSP 直连**（验证码为密码）→ **ONVIF**（部分型号）；国标一般不支持 | P1 | 萤石消费级设备普遍不开放国标；RTSP 需在萤石 APP 内开启"局域网 RTSP/萤石工作室" |
| 云对云 | 不做 | — | 用户明确排除 |

**统一原则**：一个设备模型、一套媒体调度（ZLM）、一个播放器（h265web.js）；各协议只是适配器，UI 按**能力集**灰显差异。

---

## 2. 第三方设备直连能力矛阵（TP-LINK / 海康 / 萤石）

| 能力 | TP-LINK 商用 IPC（GB28181） | TP-LINK（ONVIF/RTSP） | 海康 IPC/NVR（GB28181） | 海康（ONVIF/RTSP） | 萤石 IPC（RTSP） | 萤石 IPC（ONVIF） |
|---|---|---|---|---|---|---|
| 接入方式 | 设备 Web 填 SIP 服务器 ID/域/IP/端口/设备 ID/密码 → 主动注册 | 平台填 IP/凭据 | 配置 › 网络 › 高级 › 平台接入 › 28181 | 平台填 IP/凭据 | 平台填 `rtsp://admin:{验证码}@ip:554/...` | 部分型号，凭据 admin/验证码 |
| 跨网（NAT） | 是（设备出连） | 否（需同网/VPN） | 是 | 否 | 否 | 否 |
| 实时预览 主/子码流 | 是（StreamNumber） | 是（Profile） | 是 | 是 | 是（`/h264/ch1/main|sub/av_stream`） | 是 |
| H.265 | 是（PS 封装） | 是 | 是 | 是 | 依型号 | 依型号 |
| TF/硬盘录像检索 | **待验证**（RecordInfo 支持度因型号而异） | 否（ONVIF Profile G 罕见） | 是（RecordInfo） | 否 | 否 | 否 |
| 录像回放（倍速/seek） | **待验证** | 否 | 是（Playback + scale/Range） | 否 | 否 | 否 |
| 云台 | 是（PTZCmd） | 是（ONVIF PTZ） | 是 | 是 | 否 | 部分 |
| 告警事件 | 是（Alarm 上报，依型号） | ONVIF Events（依型号） | 是 | ONVIF Events | 否 | 部分 |
| 设备状态 | 在线/离线（Keepalive） | 在线/离线（探测） | 同左 | 同左 | 探测 | 探测 |
| 固件升级/配置 | 否（走设备 Web） | 有限 | 否 | 有限 | 否 | 否 |

> "待验证"项在 M0 用已购 TP-LINK 型号逐一实测（§10）。若 TP-LINK 国标不支持回放，则 TP-LINK 的 TF 回放退化为"仅平台侧录像回放"（ZLM 服务端录像）。

### 2.1 海康 ISUP/EHome（可选，P2）

海康项目型设备支持 ISUP 5.0 主动注册（4G/跨网场景），海康提供 Linux C SDK（HCISUPCMS/HCISUPStream，闭源 .so，Go 经 cgo 调用），ZLM 支持 ehome RTP 收流。仅在客户海康设备无法走国标时启用。

### 2.2 萤石私有协议（可选，P2）

萤石设备开放 8000 端口的海康私有协议，可用 HCNetSDK 以 `admin/{验证码}` 登录，实现**本地 TF 录像检索与回放**（这是萤石设备唯一可行的本地回放路径）。闭源 SDK，Linux .so，需评估授权条款；P2 评估。

---

## 3. 接入适配层架构

```
                        ┌──────────────────── Go 后端 ────────────────────┐
                        │ 统一设备模型 Device{source, capabilities, channels} │
                        │ 统一媒体调度 Scheduler → ZLM 节点（RTMP/RTP/Proxy） │
                        └────┬─────────────┬─────────────┬─────────────────┘
                             │             │             │
                   ┌─────────▼────┐ ┌──────▼──────┐ ┌────▼──────────┐   ┌──────────────┐
                   │ IDP Adapter  │ │ GB28181     │ │ ONVIF/RTSP    │   │ ISUP / HCNet │
                   │ MQTT/TLS     │ │ Adapter     │ │ Adapter       │   │ (可选, P2)   │
                   │ RTMP 推流    │ │ SIP + RTP/PS│ │ SOAP + RTSP拉 │   │ cgo .so      │
                   └──────┬───────┘ └──────┬──────┘ └────┬──────────┘   └──────┬───────┘
                          │                │             │                     │
                   GK7205 自研固件   TP-LINK IPC / 海康   萤石 IPC / 海康 /      海康项目机 /
                                                        TP-LINK(同网)        萤石(本地回放)
```

媒体到 ZLM 的三种进法：
- IDP：设备 **RTMP publish** 到指定节点（`on_publish` 鉴权）。
- GB28181：节点 `openRtpServer` 收 **RTP/PS**（INVITE SDP 指定）。
- ONVIF/RTSP：节点 `addStreamProxy(rtsp://…)` **主动拉流**。

三者出口一致：`WS-FLV(H.265/H.264)` → h265web.js。

---

## 4. IDP v1 协议设计（自研 GK 板）

### 4.1 固件多协议栈（必须项：IDP + ONVIF + RTSP + GB28181）

> 固件采用**平台化分层架构**：本节四个协议栈与录像/OTA/控制台均位于通用层（一套代码），芯片与外围 IC 差异由 HAL 适配层隔离。分层、HAL 接口、能力清单与新芯片适配流程见 [IPC固件平台化架构_HAL适配方案.md](./IPC固件平台化架构_HAL适配方案.md)。量产基线已确认为 GK7205 系列。

#### 4.1.1 架构：一个媒体总线，多个协议消费者

```
 Sensor → ISP → VENC(main H.265 / sub H.264) ─┐
 Audio  → AENC(G.711A / AAC)                  ─┤
                                              ▼
                              ┌──── 帧总线（共享内存环形队列，零拷贝）────┐
                              │ 每路码流一个 ring；消费者按需订阅；总消费者上限可配 │
                              └───┬─────────┬──────────┬──────────┬──────────┘
                                  │         │          │          │
                       ┌──────────▼──┐ ┌────▼─────┐ ┌──▼───────┐ ┌▼────────────┐
                       │ RTSP Server │ │ ONVIF    │ │ GB28181  │ │ IDP Client  │
                       │ 554 常开    │ │ Server   │ │ UA       │ │ MQTT + RTMP │
                       │ TCP/UDP     │ │ 80/8080  │ │ SIP 5060 │ │ 推流        │
                       │ Digest 鉴权 │ │ Profile S│ │ PS/RTP   │ │ 云端主通道  │
                       └─────────────┘ └──────────┘ └──────────┘ └─────────────┘
                                  │                        │
                       ┌──────────▼────────────────────────▼──────────┐
                       │ TF 录像器：PS/MP4 分段 + SQLite 索引（供 GB RecordInfo / IDP record.* 共用） │
                       └───────────────────────────────────────────────┘
```

#### 4.1.2 各协议栈要求

| 协议 | 状态 | 必须实现 | 实现来源 | 备注 |
|---|---|---|---|---|
| **RTSP 服务端** | 常开 | `DESCRIBE/SETUP/PLAY/TEARDOWN`，RTP over TCP/UDP，Digest 鉴权，主/子码流 URL（`/stream1`、`/stream2`），H.265/H.264 + G.711A/AAC，`GET_PARAMETER` 保活，并发 ≥4（V300）/ ≥2（V200） | OpenIPC majestic 自带；或自研（live555 过重不推荐） | 局域网调试、NVR 接入、第三方软件（VLC/ZLM 拉流） |
| **ONVIF 服务端** | 常开 | Profile S 核心：WS-Discovery 应答、`GetCapabilities/GetServices/GetDeviceInformation/GetSystemDateAndTime/SetSystemDateAndTime/GetNetworkInterfaces/SystemReboot/GetUsers/SetUser`、Media `GetProfiles/GetStreamUri/GetSnapshotUri/GetVideoEncoderConfiguration(s)/SetVideoEncoderConfiguration/GetVideoSources`、Imaging `GetImagingSettings/SetImagingSettings`、Events `CreatePullPointSubscription/PullMessages`（移动侦测）、PTZ 接口返回 `NotSupported`（无云台机型）；WS-UsernameToken 鉴权 | majestic 提供基础 ONVIF；缺失接口自研（轻量 SOAP：mxml/ezxml，不引入 gSOAP） | 目标：通过 ONVIF Device Test Tool Profile S 核心用例；能被海康/大华 NVR 与我方 ONVIF Adapter 添加 |
| **GB28181 设备端 UA** | 可配置启用 | REGISTER（Digest）、Keepalive、Catalog、DeviceInfo、DeviceStatus、INVITE 实时（PS/RTP，TCP 被动 + UDP）、BYE、`RecordInfo`、`Playback` INVITE + `INFO PLAY/PAUSE/TEARDOWN/scale/Range`、`Alarm` 上报、PTZCmd 解析（返回不支持）、NTP/注册应答校时 | 自研精简 SIP UA（消息类型有限）；避免 GPL 的 eXosip2，或评估 LGPL/MIT 栈 | 可同时注册到 **我方 GB Adapter**（IDP 不可用时兜底）或 **第三方国标平台**（wvp/海康/政务） |
| **IDP 客户端** | 默认启用 | §4.2 全部 | 自研（MQTT：paho.mqtt.c 或 mosquitto lib；RTMP：librtmp/自研） | 云端主通道 |

#### 4.1.3 接入模式与共存规则

| 模式（设备 Web「平台接入」可选，可多选） | 默认 | 说明 |
|---|---|---|
| IDP 云接入 | 开 | 绑定后自动启用；未绑定时仅心跳与绑定命令 |
| GB28181 注册 | 关 | 填写 SIP 服务器参数后启用；可与 IDP 同时开（如同时上报政务平台） |
| ONVIF | 开 | 局域网常开；可在 Web 关闭 |
| RTSP | 开 | 局域网常开；可在 Web 关闭 |

- **消费者上限**：帧总线订阅者总数 **V200 ≤ 4（量产）**、V300 ≤ 6（含 TF 录像 1 路）；超限时新请求返回 `503/453 Not Enough Bandwidth`，IDP 上报 `event.stream_limit`。
- **凭据统一**：ONVIF/RTSP/设备 Web 共用一套本地账号；首次通过 IDP 绑定或本地 Web 登录时**强制修改默认密码**；IDP `cfg.set{localUser}` 可远程重置。
- **时间统一**：任一通道（IDP/GB/ONVIF `SetSystemDateAndTime`/NTP）校时后全局生效，录像索引以 UTC 存储。
- **录像索引共用**：GB `RecordInfo` 与 IDP `record.query` 读同一 SQLite；GB `Playback` 与 IDP `record.play` 共用同一"文件读取 → PS/RTP 或 RTMP"发送器，仅封装层不同。

#### 4.1.4 内存预算（常驻 RSS 估算，需 M2 实测）

> **量产型号为 V200（64MB）**，以下以 V200 为主预算，V300 为高配参考。

| 组件 | **V200（64MB，量产）** | V300（128MB，高配） |
|---|---|---|
| 内核 + MPP/ISP/VENC 缓冲 | ~28MB（VB 池按 1080p 主 + 640×360 子精算，关闭多余 VPSS 通道） | ~40MB |
| WiFi 内核驱动 + 固件 blob + wpa_supplicant | ~3MB | ~3MB |
| majestic（RTSP + 基础 ONVIF） | ~8MB | ~10MB |
| ONVIF 补充服务（无 gSOAP） | ~1.5MB | ~2MB |
| GB28181 UA | ~2.5MB | ~3MB |
| IDP 客户端（MQTT + mbedTLS + RTMP） | ~3.5MB | ~4MB |
| TF 录像器 + SQLite | ~2MB | ~3MB |
| 本地 Web 控制台（静态资源走 Flash，进程复用 ONVIF HTTP） | ~0.5MB | ~1MB |
| 预留（OTA 下载缓冲 ≥4MB、页缓存、突发） | ≥8MB | ≥30MB |
| **合计** | **~57MB / 64MB（余量 ~7MB，紧张）** | ~96MB / 128MB |

**V200 默认启用集与裁剪顺序**（M2 实测超预算时依次执行）：
1. 默认启用：IDP、RTSP、ONVIF 核心（Device/Media/Imaging）、GB28181 实时、TF 录像、WiFi。
2. 裁剪 ①：ONVIF Events（PullPoint）改为按需启动进程，空闲退出。
3. 裁剪 ②：GB28181 回放（Playback）关闭，保留实时；TF 回放仅走 IDP。
4. 裁剪 ③：子码流降至 480×270，VB 池再降 ~2MB。
5. 裁剪 ④：协议模块编译为可加载 `.so`，未启用的协议不驻留。
6. **P2P 不在 V200 上提供**（三期仅 V300）。

> V100 不承诺四协议并存：仅 RTSP + GB28181（或 RTSP + IDP 精简），M0 实测后定。

### 4.2 IDP 协议要点

（与 v1.0 一致，摘要如下；完整字段见附录 A）

| 项 | 规格 |
|---|---|
| 标识 | DeviceID 17 位（厂商 3 + 型号 4 + 年周 4 + 序列 5 + 校验 1）；VerifyCode 6 位；QR `IPC1:{id}:{code}:{model}` |
| 凭证 | 每台唯一 X.509（ECC P-256）；V100/V200 可降级 TLS-PSK |
| 传输 | MQTT 5.0 over TLS，8883/443；Keepalive 60s，3 周期判离线，LWT 即时离线 |
| Topic | `idp/v1/{DeviceID}/up|down/{status\|event\|ack\|cmd\|cfg\|ota\|media\|record\|p2p}` |
| 状态机 | 出厂未激活 → 在线未绑定 → 绑定中 → 已绑定 →（离线/解绑） |
| 绑定 | ID+验证码 / 扫码 / 批量导入 / 预添加（7 天）/ 局域网 UDP 发现（私有化部署） |
| 冲突 | `E7001 已被绑定` → 转移/分享、原项目删除、解绑申请工单 |
| 解绑 | 平台 `cmd.unbind`；设备 Reset 5s 仅解绑；Reset 10s 复位 |
| 管理 | reboot(定时)、reset、cfg.get/set、ota(A/B+回滚)、status.report(30s)、event.*、snapshot、diag.run、ptz(预留) |
| 媒体 | `media.start{ch,profile,url,token,ttl}` → RTMP(S) 推流；`media.stop`；无人观看由 ZLM `on_stream_none_reader` 触发 |
| 回放 | `record.query/play/ctrl(pause\|resume\|seek\|speed)/stop`；二期 `record.download` |
| P2P | `p2p.offer/answer/candidate`（三期） |

---

## 5. GB28181 适配器（TP-LINK / 海康 首选）

### 5.1 平台侧（Go）

| 组件 | 实现要点 |
|---|---|
| SIP 服务端 | UDP+TCP 5060；REGISTER Digest（每设备独立密码）；401 挑战；过期 3600s |
| 目录同步 | 注册成功后 `Catalog` 查询，分页聚合；`DeviceInfo` 取厂商/型号/固件用于能力判定 |
| 心跳 | `Keepalive` 60s，3 次超时离线；离线后清理活跃流与端口 |
| 实时点播 | Scheduler 选节点 → `openRtpServer(tcp_mode=1 被动优先, 兼容 UDP)` → INVITE（SDP 含 SSRC）→ ACK；`on_stream_changed` 写映射；无人观看 → BYE + `closeRtpServer` |
| 清晰度 | INVITE SDP 中 `StreamNumber`（0 主 1 子），TP-LINK/海康均支持 |
| 录像 | `RecordInfo`（分页 SN 聚合）→ 时间轴；`Playback` INVITE（`t=` 起止）→ `INFO PLAY scale / Range / PAUSE / TEARDOWN` |
| 云台 | `DeviceControl PTZCmd`（8 方向/变倍/预置位） |
| 告警 | `Alarm` MESSAGE → 统一事件；`AlarmSubscribe`（依型号） |
| 校时 | 注册应答带 `Date` 头 |
| 待确认列表 | 未在白名单的注册设备进入"待确认"，管理员确认后入组；白名单（预填国标 ID+密码+目标分组）自动入组 |

### 5.2 TP-LINK 设备侧配置（写入用户引导页）

```
设备 Web › 设置 › 网络 › 平台接入 › GB28181
  SIP 服务器编号：3402000000200000000x（平台展示）
  SIP 服务器域  ：3402000000
  SIP 服务器 IP ：<平台公网/内网 IP>
  SIP 服务器端口：5060
  设备编号      ：34020000001320000xxx（平台按项目分配，或用设备默认）
  设备密码      ：<平台生成，每台唯一>
  注册有效期/心跳：3600 / 60
  传输协议      ：TCP（优先）
```
平台"国标接入"Tab 直接展示上述参数并提供 `[复制]`；设备注册成功后页面实时出现在"待确认/已自动入组"列表。

### 5.3 海康设备侧

`配置 › 网络 › 高级设置 › 平台接入 › 28181`，字段同上；海康默认支持录像检索/回放/云台/告警，能力集最全。

---

## 6. ONVIF / RTSP 适配器（萤石 / 补充）

| 项 | 实现 |
|---|---|
| 发现 | WS-Discovery（3702 多播）+ IP 段端口探测（80/8000/554）——私有化部署后端与设备同网时可用 |
| ONVIF | `GetDeviceInformation / GetProfiles / GetStreamUri / PTZ / Events(PullPoint)`；库：`gowvp/onvif` 或 `use-go/onvif` |
| RTSP 直连 | 手填 URL；ZLM `addStreamProxy(url, rtp_type=0 TCP)`；失败回显 RTSP 状态码 |
| 萤石预设模板 | 平台内置 URL 模板：`rtsp://admin:{验证码}@{ip}:554/h264/ch1/main/av_stream`（子码流 `sub`）；提示用户先在萤石 APP 开启局域网 RTSP |
| 海康 RTSP 模板 | `rtsp://{user}:{pwd}@{ip}:554/Streaming/Channels/101`（102 子码流） |
| TP-LINK RTSP 模板 | `rtsp://{user}:{pwd}@{ip}:554/stream1`（`stream2` 子码流）——用于同网调试或国标不可用时兜底 |
| 状态 | 30s TCP/ONVIF 探测，3 次失败离线；流级状态独立 |
| 局限 | 无跨网能力、无设备端录像回放（Profile G 罕见）→ 这类设备的回放依赖**平台侧录像**（ZLM `startRecord`） |

---

## 7. 添加设备交互（统一入口）

| Tab | 面向 | 输入 | 结果 |
|---|---|---|---|
| 自有设备（ID 添加） | GK 板 | 分组、DeviceID、验证码、名称 | 绑定 ≤10s |
| 扫码 / 批量导入 / 预添加 | GK 板 | QR / Excel | 同上；预添加 7 天有效 |
| **国标接入** | TP-LINK / 海康 | 只读展示 SIP 参数 `[复制]`；可选预填白名单（国标 ID + 密码 + 分组） | 设备注册后出现在"待确认"或自动入组 |
| 自动发现 | 同网第三方 | `[开始发现]` → 结果表 → 勾选 + 凭据 | 批量添加，逐行结果 |
| 手动添加（ONVIF/RTSP） | 萤石 / 其他 | 协议、IP、端口、凭据；或选品牌模板自动拼 URL；`[测试连接]` | 通道拉取、封面抓图 |

统一元素：顶部"确认设备已联网"提示；来源标签（自有 / 国标 / ONVIF / RTSP）；错误码 + 建议（`E1002 未上线：检查网线/DNS，可将设备 DNS 改为 114.114.114.114`；`E6001 国标注册密码错误`；`E2001 RTSP 认证失败：萤石设备请使用 6 位验证码作为密码`）。

---

## 8. 媒体面：直播与 TF 卡回放

### 8.1 直播（三条进路、同一出口）

| 来源 | 起播动作 | 停播 |
|---|---|---|
| IDP | `media.start` → 设备 RTMP publish `live/{dev}_{ch}_{pf}` | `on_stream_none_reader` → `media.stop` |
| GB28181 | `openRtpServer` → INVITE → RTP/PS `rtp/{ssrc}` | `on_stream_none_reader` → BYE + `closeRtpServer` |
| ONVIF/RTSP | `addStreamProxy` `proxy/{dev}_{ch}_{pf}` | `on_stream_none_reader` → `delStreamProxy` |

前端 h265web.js 统一消费 `ws://node/{app}/{stream}.live.flv`，`webcodec_hevc` 优先，`wasm_hevc` 兜底。

### 8.2 TF 卡 / 本地录像回放

| 来源 | 检索 | 回放 | 控制 |
|---|---|---|---|
| IDP（GK 板） | `record.query` → SQLite 索引 | `record.play` → RTMP `record/{dev}_{ch}_{sid}` | `record.ctrl` pause/resume/seek/speed 0.25~16 |
| GB28181（海康；TP-LINK 待验证） | `RecordInfo` | `Playback` INVITE | `INFO PLAY scale/Range/PAUSE` |
| ONVIF/RTSP（萤石等） | 无 | **平台侧录像**（ZLM `startRecord` MP4 + `on_record_mp4` 索引） | 播放器本地 seek/倍速（MP4 点播） |
| 萤石本地 TF（P2） | HCNetSDK 录像查询 | HCNetSDK 回放 → 转推 ZLM | SDK 控制 |

UI 统一：存储位置下拉（设备存储 / 平台存储，按能力显示）、日期、24h 时间轴（定时/事件着色）、倍速 9 档、快进 30s、截图、电子放大。

---

## 9. 对 MVP 里程碑的调整

| 里程碑 | 内容 | 退出条件 |
|---|---|---|
| **M0** | 冻结 IDP v1；**已购 TP-LINK 型号国标能力实测**（§10）；ZLM×2 + Go + Nuxt 骨架；h265web.js Spike | TP-LINK 兼容矛阵签字；WS-FLV H.265 三浏览器可播 |
| **M1** | **GB28181 Adapter**（注册/目录/点播/BYE/待确认/白名单）+ 调度器 + 前端"国标接入"Tab + 直播 | **已购 TP-LINK IPC 全部上线并可预览**；无人观看 30s 停流 |
| **M1'（并行）** | **IDP Adapter**（Broker、bind、status、event、media.start/stop、`on_publish` 鉴权）+ 前端自有设备 Tab | GK 板扫码/输 ID 绑定 ≤10s，首帧 ≤1.5s |
| **M2** | GK 板固件：OpenIPC 基座 + 帧总线 + **RTSP 服务端 + ONVIF Profile S 服务端 + GB28181 设备端 UA + IDP 客户端（RTMP 推流）** + TF 录像索引 + OTA A/B | 24h 稽定；RTSP 被 VLC/ZLM 拉流；**ONVIF Device Test Tool Profile S 核心用例通过，海康/大华 NVR 可添加**；**GB UA 可注册到我方 GB Adapter 与 wvp-GB28181-pro 并点播/回放**；四协议并存时 V300 RSS ≤80MB、V200 ≤56MB；OTA 回滚可用 |
| **M3** | 回放：GB `RecordInfo/Playback`（海康；TP-LINK 视 M0 结果）+ IDP `record.*` + 时间轴 UI | 定位 ≤1s；倍速不断流 |
| **M3b** | **ONVIF/RTSP Adapter** + 发现 + 品牌 URL 模板（萤石/海康/TP-LINK）+ 平台侧录像 | 萤石 IPC 可预览；无设备端回放时平台录像可回放 |
| **M4** | 加固：SIP 密码策略、MQTT ACL/吊销、验证码限速、压测 200 通道 | 渗透基线通过 |
| 后续 | 9/16 分屏、`record.download`、ISUP/HCNetSDK 适配器（P2）、P2P（三期）、国标级联上级平台 | — |

> 变化点：GB28181 由"M3b 兼容"**提前为 M1 核心**（服务已购 TP-LINK）；云对云 M5 **删除**；新增 ONVIF/RTSP M3b 服务萤石。

---

## 10. 兼容性验证计划（M0）

| 测试项 | TP-LINK（每型号） | 海康 | 萤石 |
|---|---|---|---|
| 国标 REGISTER（Digest, TCP/UDP） | 必测 | 必测 | 预期不支持 |
| Catalog / DeviceInfo 字段 | 必测（型号、固件用于能力判定） | 必测 | — |
| INVITE 实时 主/子码流（StreamNumber） | 必测；记录 PS 内编码（H.264/H.265）、音频（G.711） | 必测 | — |
| TCP 被动 / UDP 收流稳定性 | 必测（跨 NAT） | 必测 | — |
| RecordInfo 查询 | **必测（关键待验证）** | 必测 | — |
| Playback + scale/Range/PAUSE | **必测（关键待验证）** | 必测 | — |
| PTZ / Alarm | 有云台型号测 | 必测 | — |
| ONVIF GetProfiles/GetStreamUri | 备测 | 备测 | 必测（部分型号） |
| RTSP 直连 URL 模板 | 备测 | 备测 | 必测（需 APP 开启局域网 RTSP） |
| h265web.js 播放（WS-FLV） | 必测 | 必测 | 必测 |

输出：《第三方设备兼容性矩阵 v1》，直接驱动 UI 能力灰显规则与 M3 范围。

---

## 11. 风险与待确认

| # | 风险 / 待确认 | 影响 | 缓解 |
|---|---|---|---|
| R1 | TP-LINK 国标不支持录像检索/回放 | TP-LINK 无法回放 TF 卡 | 平台侧录像（ZLM）兜底；或 TP-LINK NVR 走国标（NVR 国标回放通常完整） |
| R2 | TP-LINK 国标仅 UDP 或 PS 内为私有音频 | 弱网花屏 / 无声 | 强制 TCP 被动；音频关闭或转码（P2） |
| R3 | 萤石设备 RTSP 需用户在 APP 开启且仅同网 | 萤石跨网不可用 | 明示限制；跨网需私有化部署在同网或 VPN；P2 评估 HCNetSDK |
| R4 | 海康/萤石私有 SDK 闭源与授权 | 法务/维护 | 仅作 P2 可选适配器，cgo 隔离进程 |
| R5 | 三条媒体进路在多节点下的调度差异 | 复杂度 | 调度器输出统一 `{node, app, stream}`，进路细节封装在各 Adapter |
| R6 | 同一 UI 下不同来源能力差异 | 用户困惑 | 来源标签 + 能力集灰显 + 悬浮说明 |
| R7 | 固件四协议并存导致 V200 内存/CPU 超限 | V200 功能缩水 | 帧总线零拷贝、消费者上限、子码流降分辨率；M2 实测不达标则 V200 关闭 ONVIF Events 与 GB 回放 |
| R8 | majestic 自带 ONVIF 覆盖不足，且为闭源 | 无法通过 Device Test Tool | 自研 ONVIF 补充服务与 majestic 并存（不同端口/路径转发）；必要时全自研轻量 ONVIF |
| R9 | GB28181 设备端 SIP 栈许可（eXosip2 GPL） | 固件开源义务 | 自研精简 UA 或选用 LGPL/MIT 栈；M0 完成许可评审 |
| R10 | 多协议同时暴露扩大攻击面（ONVIF/RTSP 默认开） | 安全 | 强制改默认密码、失败锁定、可在 Web/IDP 关闭；出厂关闭 telnet/ssh |
| 待确认 1 | 已购 TP-LINK 的具体型号与固件版本清单 | M0 测试范围 | 请提供 |
| 待确认 2 | 是否存在已购 TP-LINK NVR（可作为国标回放载体） | R1 缓解路径 | 请提供 |
| 待确认 3 | 萤石设备是否必须支持跨网预览 | 是否需 P2 私有 SDK | 请确认 |
| 待确认 4 | V100/V200 是否接受 PSK 降级 | 证书方案 | 请确认 |

---

## 附录 A：IDP 消息集速查

| Topic 后缀 | 方向 | 消息 | 关键字段 |
|---|---|---|---|
| up/status | ↑ | hello, report | model, fw, capabilities, channels, storage, cpu, mem, temp, kbps |
| up/event | ↑ | motion, tamper, io, tf_error, rebooted, bind_state | ch, ts, snapshotUrl? |
| up/ack | ↑ | 通用应答 | msgId, code, msg, data |
| down/cmd | ↓ | bind, unbind, transfer, reboot, reset, snapshot, ptz, diag.run | msgId, params |
| down/cfg | ↓ | get, set | keys / values |
| down/ota · up/ota | ↓↑ | start, cancel · progress | url, sha256, ver, at · percent, stage, error |
| down/media | ↓ | start, stop | ch, profile, url, token, ttl |
| down/record | ↓ | query, play, ctrl, stop, download | ch, start, end, speed, sessionId, url, token |
| down/p2p · up/p2p | ↓↑ | offer, answer, candidate, close | sdp, candidate, sessionId |

错误码：`E7xxx` 绑定类（7001 已被绑定、7002 验证码错误、7003 设备离线、7004 预添加过期）；`E6xxx` 国标类（6001 注册密码错误、6002 目录为空、6003 不支持录像检索）；其余沿用上游 `E1~E5`。

## 附录 B：三类协议差异对照

> 注：自研 GK 板固件**四种方式同时具备**（IDP 云端主通道 + ONVIF/RTSP 局域网常开 + GB28181 UA 可选），下表按"平台侧如何接入"划分。

| 维度 | IDP（自研板→我方云） | GB28181（TP-LINK/海康/自研板） | ONVIF/RTSP（萤石/海康/TP-LINK/自研板，同网） |
|---|---|---|---|
| 接入方向 | 设备出连 | 设备出连 | 平台主动拉 |
| 跨网 | 是 | 是 | 否 |
| 绑定体验 | 消费级 | 工程师式（填 SIP 参数） | 工程师式（填 IP/凭据） |
| 媒体进 ZLM | RTMP publish | RTP/PS（openRtpServer） | addStreamProxy |
| 设备端回放 | 是 | 依型号 | 否 |
| 状态可观测 | 强 | 弱 | 弱 |
| OTA/配置 | 是 | 否 | 有限 |
| P2P | 三期 | 否 | 否 |
