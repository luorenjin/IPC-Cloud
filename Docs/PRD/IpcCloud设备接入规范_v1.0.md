# IpcCloud 设备接入规范 v1.0

| 项目 | 内容 |
|---|---|
| 文档编号 | IPC-SPEC-ACCESS-001 |
| 版本 | v1.0（待评审签字） |
| 日期 | 2026-09-04 |
| 适用范围 | IpcCloud 平台（Go 后端 / ZLMediaKit / Nuxt 前端）与所有接入设备：自研 IPC（IDP）、第三方 IPC/NVR（GB28181、ONVIF、RTSP） |
| 上游文档 | [私有协议优先接入方案 v1.1](./私有协议优先接入方案_IDP-v1.md)、[TP-LINK商云分析报告](./TP-LINK商云分析报告_IPC平台PRD参考.md)、[决策记录与待定事项](./决策记录与待定事项.md) |
| 不在范围 | 硬件选型、固件底层实现（HAL 以下）、云对云对接、计费 |

---

## 目录

1. [总则](#1-总则)
2. [术语与约定](#2-术语与约定)
3. [统一设备模型](#3-统一设备模型)
4. [接入方式与选择规则](#4-接入方式与选择规则)
5. [IDP v1 协议规范](#5-idp-v1-协议规范)
6. [GB28181 接入规范](#6-gb28181-接入规范)
7. [ONVIF / RTSP 接入规范](#7-onvif--rtsp-接入规范)
8. [媒体面规范（ZLMediaKit）](#8-媒体面规范zlmediakit)
9. [平台侧接入 API（摘要）](#9-平台侧接入-api摘要)
10. [错误码](#10-错误码)
11. [安全要求](#11-安全要求)
12. [一致性与验收](#12-一致性与验收)
- 附录 A：IDP 消息 JSON Schema
- 附录 B：时序图
- 附录 C：品牌 RTSP URL 模板
- 附录 D：变更记录

---

## 1. 总则

### 1.1 目标

1. 平台以**一套设备模型、一套媒体调度、一个播放器**承载多种接入协议。
2. 自研设备以 **IDP（IpcCloud Device Protocol）** 为主通道，获得消费级绑定体验与完整管理能力。
3. 第三方设备以 **GB28181** 为首选（跨网、设备主动注册），**ONVIF/RTSP** 为同网补充。
4. 所有协议差异被限制在**适配器（Adapter）**内，适配器对上只产出统一模型与统一事件。

### 1.2 规范用语

- **必须（MUST）**：不满足即不合规。
- **应当（SHOULD）**：推荐实现，偏离需说明理由。
- **可以（MAY）**：可选。

### 1.3 分层责任

| 层 | 责任 | 本规范章节 |
|---|---|---|
| 设备（固件） | 实现 IDP 客户端；或实现 GB28181 UA / ONVIF / RTSP 服务端 | §5（设备侧条款）、§6.4、§7.4 |
| 适配器（Go） | 协议收发、状态归一化、媒体起停、事件归一化 | §5、§6、§7 |
| 统一设备模型 / 调度 | 设备与通道生命周期、能力集、节点调度、播放地址 | §3、§8 |
| 前端 | 添加向导、列表、播放器、告警 | §9（接口） |

---

## 2. 术语与约定

| 术语 | 定义 |
|---|---|
| Device | 一台物理设备（IPC 或 NVR） |
| Channel | 设备下的视频通道；IPC 通常 1 个，NVR 多个。**所有视频业务挂在 Channel 上** |
| Source | 设备来源/接入协议：`idp` / `gb28181` / `onvif` / `rtsp` |
| Capability | 能力集，字符串数组，驱动 UI 显隐与功能路由 |
| Profile | 码流档位：`main`（主码流）/ `sub`（子码流） |
| Node | ZLMediaKit 媒体节点 |
| Stream Key | ZLM 中 `app/stream` 二元组 |
| DeviceID | IDP 设备唯一标识（17 位） |
| 国标 ID | GB28181 20 位编码 |
| 时间 | 所有时间字段为 **UTC 毫秒时间戳**（整数）；对外展示由前端按项目时区转换 |
| 字符串 | UTF-8；标识符区分大小写；DeviceID 全大写 |

---

## 3. 统一设备模型

### 3.1 实体

```
Project 1─* DeviceGroup(tree, depth≤4) 1─* Device 1─* Channel
Device: id, projectId, groupId, source, name, model, vendor, fw, hw,
        identity{deviceId | gbId | ip:port}, status, lastSeenAt,
        capabilities[], meta{...}, createdAt
Channel: id, deviceId, index, name, enabled, status, profiles[], coverUrl,
         capabilities[], meta{...}
```

### 3.2 设备状态机（所有 Source 通用）

```
 [pending]   待确认（仅 gb28181：未在白名单的注册设备）
    │ 管理员确认 / 白名单命中
    ▼
 [online] ◀────────────────────────┐
    │ 心跳/探测超时                  │ 恢复
    ▼                              │
 [offline] ─────────────────────────┘
    │ 平台删除 / 设备复位解绑
    ▼
 [removed]（软删除，保留 30 天可恢复）

 附加标志：error{code, msg}（如认证失败、流拉取失败），不改变主状态
```

离线判定阈值：

| Source | 判定 |
|---|---|
| idp | MQTT LWT 即时；或 3×Keepalive（默认 180s）无消息 |
| gb28181 | 3×Keepalive 周期（默认 180s）无 Keepalive |
| onvif | 30s 周期 `GetSystemDateAndTime` 探测，连续 3 次失败 |
| rtsp | 30s 周期 TCP 连接 + `OPTIONS` 探测，连续 3 次失败 |

### 3.3 通道状态

`enabled`（管理员开关）× `streamState ∈ {idle, starting, streaming, error}`。`streamState` 由 ZLM Hook 驱动（§8.5）。

### 3.4 能力集（Capability）

| 能力标识 | 含义 | idp | gb28181 | onvif | rtsp |
|---|---|---|---|---|---|
| `live.main` / `live.sub` | 主/子码流预览 | ✓ | ✓（StreamNumber） | ✓（Profiles） | 手填 |
| `live.h265` | 主码流 H.265 | 声明 | 探测 | 探测 | 探测 |
| `snapshot` | 抓图 | ✓ | 经 ZLM `getSnap` | ✓ | 经 ZLM |
| `record.device.query` | 设备端录像检索 | ✓ | 依型号 | ✗ | ✗ |
| `record.device.play` | 设备端回放 | ✓ | 依型号 | ✗ | ✗ |
| `record.device.speed` | 回放倍速 | ✓ | 依型号 | ✗ | ✗ |
| `record.device.seek` | 回放定位 | ✓ | 依型号 | ✗ | ✗ |
| `record.platform` | 平台侧录像 | ✓ | ✓ | ✓ | ✓ |
| `ptz` / `ptz.preset` | 云台/预置位 | 依 profile | ✓ | ✓ | ✗ |
| `focus` | 对焦 | 依 profile | ✗ | ✓（Imaging） | ✗ |
| `audio.talk` | 对讲 | 依 profile | ✗ | ✗ | ✗ |
| `event.motion` 等 | 智能事件 | ✓ | ✓（Alarm） | ✓（Events） | ✗ |
| `status.metrics` | CPU/温度/TF 等 | ✓ | ✗ | ✗ | ✗ |
| `config.remote` | 远程配置 | ✓ | 有限 | 有限 | ✗ |
| `ota` | 固件升级 | ✓ | ✗ | 有限 | ✗ |
| `reboot` | 远程重启 | ✓ | ✓ | ✓ | ✗ |

**规则**：
- 适配器**必须**在设备上线时产出能力集；不确定的能力**必须**通过探测确定或标为不支持，不得猜测为支持。
- UI **必须**按能力集灰显功能；后端 API 对不具备能力的操作返回 `E0403 CAPABILITY_NOT_SUPPORTED`。

### 3.5 统一事件

所有适配器向事件总线产出统一事件：

```json
{ "type": "device.online|device.offline|device.error|channel.stream|alarm.<kind>|record.done|ota.progress",
  "deviceId": "...", "channelId": "...", "source": "idp", "ts": 1788486083000,
  "data": { } }
```

`alarm.<kind>` 取值：`motion, intrusion, linecross, tamper, humanoid, vehicle, io, tf_error, disk_error, stream_lost`。

---

## 4. 接入方式与选择规则

| 设备类型 | 首选 | 备选 | 说明 |
|---|---|---|---|
| 自研 IPC | IDP | GB28181（固件同时具备） | IDP 不可用（无云连接）时可用 GB/ONVIF/RTSP 同网接入 |
| TP-LINK 商用 IPC/NVR | GB28181 | ONVIF/RTSP（同网） | 跨网必须 GB |
| 海康 IPC/NVR | GB28181 | ONVIF/RTSP | 海康国标含录像检索/回放 |
| 萤石 IPC | RTSP（`admin/验证码`） | ONVIF（部分型号） | 仅同网；无设备端回放 |
| 其他 ONVIF 设备 | ONVIF | RTSP | — |
| 仅有 RTSP 地址的源 | RTSP | — | 例如 NVR 通道、软件流 |

**同一设备同时通过多种方式接入**（如自研设备 IDP + GB 都上线）时，平台**必须**按 `identity` 去重合并为一个 Device，`source` 记录主通道（优先级 idp > gb28181 > onvif > rtsp），其余记录为 `altSources[]`。

---

## 5. IDP v1 协议规范

### 5.1 标识与凭证

| 项 | 规范 |
|---|---|
| DeviceID | 17 位：`MMM TTTT YYWW NNNNN C`（厂商 3 + 型号 4 + 生产年周 4 + 序列 5 + 校验 1）；字符集 `A-Z 2-9` 去除 `0 O 1 I`；校验位为前 16 位按 Luhn mod 32 |
| VerifyCode | 6 位，同字符集，随机；出厂写入设备安全分区；平台仅存 `HMAC-SHA256(salt, code)` |
| 设备证书 | X.509 v3，ECC P-256，`CN=<DeviceID>`，由 IpcCloud Device CA 签发，有效期 ≥10 年；私钥不出设备 |
| PSK 模式（可选） | 无证书能力设备：`PSK = HKDF(masterKey, DeviceID)`，TLS-PSK 密码套件；平台配置项 `idp.allowPsk` 默认 `false` |
| QR 内容 | `IPC1:<DeviceID>:<VerifyCode>:<Model>`；`MAY` 采用 `IPC1E:<base64(AES-GCM(...))>` 加密形式 |

### 5.2 传输

| 项 | 规范 |
|---|---|
| 协议 | MQTT 5.0（**必须**兼容 3.1.1 客户端）over TLS 1.2+；端口 8883，**应当**同时提供 443 |
| 认证 | 双向 TLS；Broker 以证书 CN 作为身份，ClientID **必须** = `dev-<DeviceID>`，否则拒绝 |
| ACL | 设备仅可发布 `idp/v1/<DeviceID>/up/#`、订阅 `idp/v1/<DeviceID>/down/#` |
| Keepalive | 60s；设备 LWT 主题 `idp/v1/<DeviceID>/up/status`，payload `{"type":"lwt"}` |
| QoS | `cmd/ack/event/ota/media/record/p2p` QoS 1；`status.report` QoS 0；`status.hello` QoS 1 |
| 消息体 | JSON，UTF-8，单条 ≤ 64KB；二进制（快照、日志）走 HTTPS 预签名 URL |
| 重连 | 指数退避 1s→60s，抖动 ±20%；重连后**必须**重发 `status.hello` |
| 时间 | 设备**必须**支持 NTP 与 `cfg.set{time}`；`ts` 为 UTC 毫秒 |

### 5.3 主题

```
idp/v1/<DeviceID>/up/status     设备 → 平台   hello, report, lwt
idp/v1/<DeviceID>/up/event      设备 → 平台   事件
idp/v1/<DeviceID>/up/ack        设备 → 平台   命令应答
idp/v1/<DeviceID>/up/ota        设备 → 平台   升级进度
idp/v1/<DeviceID>/up/p2p        设备 → 平台   P2P 信令（预留）
idp/v1/<DeviceID>/down/cmd      平台 → 设备   通用命令
idp/v1/<DeviceID>/down/cfg      平台 → 设备   配置
idp/v1/<DeviceID>/down/ota      平台 → 设备   升级
idp/v1/<DeviceID>/down/media    平台 → 设备   直播起停
idp/v1/<DeviceID>/down/record   平台 → 设备   录像检索与回放
idp/v1/<DeviceID>/down/p2p      平台 → 设备   P2P 信令（预留）
```

### 5.4 消息信封

所有消息**必须**包含：

```json
{ "v": 1, "type": "<domain>.<action>", "msgId": "<uuid>", "ts": 1788486083000, "data": { } }
```

应答（`up/ack`）：

```json
{ "v": 1, "type": "ack", "msgId": "<同请求>", "ts": ..., "code": 0, "msg": "ok", "data": { } }
```

- `code=0` 成功；非 0 见 §10。
- 设备对所有 `down/*` 命令**必须**在 5s 内回 `ack`（长任务先回 `code=0,data.state="accepted"`，后续用事件/进度上报）。
- 平台对未在 10s 内收到 `ack` 的命令重发 1 次，仍无应答记 `E7003 DEVICE_TIMEOUT`。
- 未知 `type` 设备**必须**回 `E0400 UNKNOWN_TYPE`，不得静默丢弃。

### 5.5 设备生命周期与绑定

#### 5.5.1 状态机

```
[factory] ──首次联网、TLS 成功──▶ [online_unbound]
[online_unbound] ──平台 cmd.bind──▶ [binding] ──设备 ack(0)──▶ [bound]
[bound] ──LWT/超时──▶ [bound_offline] ──hello──▶ [bound]
[bound] ──cmd.unbind / 本地解绑 / 复位──▶ [online_unbound]
```

设备在 `online_unbound` 状态**必须**仅接受 `cmd.bind`、`cmd.reboot`、`ota.*`、`cfg.get{time}`，其余命令回 `E7005 NOT_BOUND`。

#### 5.5.2 hello（设备上线首条）

```json
{ "v":1, "type":"status.hello", "msgId":"…", "ts":…,
  "data": {
    "model":"SP-R1-02", "vendor":"IpcCloud", "fw":"1.0.3", "hw":"A1",
    "bound": { "projectId":"p_123" },            // 未绑定时为 null
    "capabilities": ["live.main","live.sub","live.h265","snapshot",
                     "record.device.query","record.device.play","record.device.speed","record.device.seek",
                     "event.motion","status.metrics","config.remote","ota","reboot"],
    "channels": [ { "ch":1, "name":"IPC",
        "profiles":[ {"id":"main","codec":"h265","w":1920,"h":1080,"fps":25,"kbps":2048},
                     {"id":"sub","codec":"h264","w":640,"h":360,"fps":15,"kbps":512} ] } ],
    "storage": { "tf": { "present":true, "totalMB":61440, "freeMB":8120, "health":"ok" } },
    "net": { "ip":"192.168.1.23", "mac":"AA:BB:CC:DD:EE:FF", "type":"eth" },
    "localUserChanged": true                      // 默认密码是否已修改
  } }
```

#### 5.5.3 绑定流程（平台侧）

1. 用户提交 `DeviceID + VerifyCode + groupId`（或扫码、导入、预添加命中）。
2. 平台校验：设备存在且 `online_unbound`；`HMAC(code)` 匹配；同 DeviceID 每小时失败 ≤5 次。
3. 平台下发 `cmd.bind`：

```json
{ "type":"cmd.bind", "data": { "projectId":"p_123", "bindToken":"<jwt, 5min>", "deviceName":"门口", "time":1788486083000 } }
```

4. 设备校验 `bindToken` 签名（平台公钥预置），持久化 `projectId`，回 `ack(0)`；平台入库、置 `bound`，产出 `device.online`。
5. 若设备已 `bound` 到其他项目：平台返回 `E7001 ALREADY_BOUND`，UI 提供"转移 / 原项目删除 / 解绑申请"。

#### 5.5.4 预添加

- 平台导入 `DeviceID(+VerifyCode) → groupId, name, location`，有效期 7 天。
- 设备上线 `hello` 时命中预添加记录：若记录含 VerifyCode 则直接执行 5.5.3 步骤 3；否则置 `pending` 待管理员输入验证码。
- 过期未激活：记录保留并标 `expired`，可重新激活。

#### 5.5.5 解绑

| 途径 | 设备行为 |
|---|---|
| 平台 `cmd.unbind` | 清除 `projectId`、本地录像索引保留、回 `ack`、进入 `online_unbound` |
| 设备 Reset 键 5s | 同上，并上报 `event.bind_state{state:"unbound", by:"local"}` |
| 设备 Reset 键 10s | 恢复出厂（保留证书与 VerifyCode） |

### 5.6 状态与事件

`status.report`（周期 30s，QoS 0）：

```json
{ "type":"status.report", "data": { "cpu":37, "memUsedMB":51, "tempC":52, "uptimeS":86400,
   "streams":[{"ch":1,"profile":"main","kbps":2010,"fps":25,"consumers":2}],
   "storage":{"tf":{"freeMB":8000,"health":"ok"}}, "net":{"rssi":-61} } }
```

`event.*`（即时，QoS 1）：

```json
{ "type":"event.motion", "data": { "ch":1, "start":1788486083000, "end":null, "regions":[[0.1,0.2,0.5,0.6]],
   "snapshot": { "uploadUrl":"https://…/presigned", "done":false } } }
```

事件类型：`motion, humanoid, tamper, io, tf_error, tf_inserted, tf_removed, rebooted, bind_state, stream_limit, ota_state`。快照上传：设备 `PUT` 至预签名 URL 后发送 `event.snapshot_done{eventMsgId, url}`。

### 5.7 管理命令

| type | data | 设备行为 |
|---|---|---|
| `cmd.reboot` | `{at?: ts}` | 立即/定时重启；重启后 `event.rebooted` |
| `cmd.reset` | `{keepNetwork: bool}` | 恢复出厂 |
| `cmd.snapshot` | `{ch, profile, uploadUrl}` | 抓图上传，`ack.data.url` |
| `cmd.ptz` | `{ch, op: move\|stop\|preset_set\|preset_goto\|preset_del, pan, tilt, zoom, speed, preset}` | 无云台回 `E0403` |
| `cmd.focus` | `{ch, op: auto\|near\|far\|stop}` | 定焦回 `E0403` |
| `cmd.diag` | `{items:["ping","dns","stun","bandwidth"], target?}` | 回诊断结果 |
| `cmd.transfer` | `{projectId}` | 更新绑定项目（平台内转移） |
| `cmd.unbind` | `{}` | §5.5.5 |
| `cfg.get` | `{keys:[...]}` | 回配置 |
| `cfg.set` | `{values:{...}}` | 校验并应用；不可应用项在 `ack.data.rejected[]` 列出 |

配置键（`cfg`）最小集：`video.<ch>.<profile>.{codec,w,h,fps,kbps,rc,gop}`、`video.<ch>.osd.{name,time,pos}`、`image.{brightness,contrast,saturation,sharpness,flip,mirror,wdr,daynight}`、`record.{mode,schedule,eventPreS,eventPostS}`、`alarm.<kind>.{enabled,sensitivity,regions,schedule}`、`time.{ntp,tz}`、`net.{dhcp,ip,mask,gw,dns}`、`wifi.{ssid,psk}`、`localUser.{name,password}`、`protocols.{rtsp,onvif,gb28181}.enabled`、`gb28181.{serverId,domain,ip,port,deviceId,password,transport}`。

### 5.8 媒体：直播

```json
// 平台 → 设备
{ "type":"media.start", "data": { "ch":1, "profile":"main",
    "url":"rtmps://node1.example.com:1936/live/AB3K7…_1_main", "token":"<jwt>", "ttlS":60 } }
// 设备 → 平台
{ "type":"ack", "code":0, "data": { "state":"publishing" } }
// 平台 → 设备
{ "type":"media.stop", "data": { "ch":1, "profile":"main" } }
```

- 设备**必须**在收到 `media.start` 后 2s 内开始 RTMP publish，URL 追加 `?token=<jwt>`；`ttlS` 内未开始视为失败。
- 平台通过 ZLM `on_publish` 校验 token（含 `node, app, stream, exp`），一次性使用。
- RTMP 载荷：视频 H.264 或 **enhanced-RTMP H.265**；音频 G.711A/U 或 AAC；PTS 单调，音视频同源时钟。
- 同一通道两个 profile 可并存；设备**必须**支持至少 `main+sub` 同时推。
- 平台 `on_stream_none_reader` 触发 `media.stop`；设备在 `media.stop` 或 RTMP 断开后**必须**停止推流并释放编码器消费者。

### 5.9 媒体：录像检索与回放

```json
// 检索
{ "type":"record.query", "data": { "ch":1, "start":1788393600000, "end":1788480000000, "types":["timer","event"], "page":1, "pageSize":200 } }
{ "type":"ack", "code":0, "data": { "total":1440, "segments":[ {"s":1788393600000,"e":1788393660000,"type":"timer","sizeKB":7680}, … ] } }

// 回放
{ "type":"record.play", "data": { "ch":1, "sessionId":"s_9f…", "start":1788400000000, "end":1788403600000, "speed":1,
    "url":"rtmps://node1…/record/AB3K7…_1_s_9f…", "token":"<jwt>", "ttlS":60 } }
// 控制
{ "type":"record.ctrl", "data": { "sessionId":"s_9f…", "op":"pause" } }
{ "type":"record.ctrl", "data": { "sessionId":"s_9f…", "op":"resume" } }
{ "type":"record.ctrl", "data": { "sessionId":"s_9f…", "op":"seek", "ts":1788401200000 } }
{ "type":"record.ctrl", "data": { "sessionId":"s_9f…", "op":"speed", "speed":4 } }
{ "type":"record.stop", "data": { "sessionId":"s_9f…" } }
```

- `segments` 按 `s` 升序；单页 ≤ 500。
- 回放流**必须**保持原始 PTS（用于时间轴同步）；`speed` 支持 `0.25,0.5,1,2,4,8,16`（>1 时可丢非关键帧）；`seek` 精度 ≤1s；`pause` 停止发送但保持 RTMP 连接。
- 回放到 `end` 或文件末尾，设备发送 `event.record_eof{sessionId}` 并断开。
- 设备并发回放会话数在 `hello.limits.playbackSessions` 声明（≥1）。

### 5.10 OTA

```json
{ "type":"ota.start", "data": { "url":"https://…/fw.bin", "sha256":"…", "version":"1.1.0", "size":8388608, "at?":ts, "force":false } }
// 进度（设备 → 平台，up/ota）
{ "type":"ota.progress", "data": { "stage":"downloading|verifying|writing|rebooting|confirming|done|failed", "percent":42, "error?":"E8002" } }
```

- 设备**必须**校验 SHA256 与固件包内 `platform_id`/`model` 匹配，否则 `E8001 OTA_INCOMPATIBLE`。
- 双分区 A/B；新分区启动后 60s 内 IDP 上线成功即 `confirm`，否则 bootloader 回滚并上报 `ota.progress{stage:"failed", error:"E8003 ROLLBACK"}`。
- 非 `force` 且有活跃推流时，设备**应当**延后到无流时执行。

### 5.11 P2P（预留，v1 不实现）

主题 `down/p2p`、`up/p2p`，消息 `p2p.offer / answer / candidate / close`，字段 `sessionId, sdp, candidate`。

---

## 6. GB28181 接入规范

### 6.1 平台角色与参数

平台作为 **SIP 服务器（上级）**，每个项目分配一组接入参数并在 UI 展示：

| 参数 | 规范 |
|---|---|
| SIP 服务器编号 | 20 位，行业编码 `200`（中心服务器） |
| SIP 域 | 服务器编号前 10 位 |
| IP / 端口 | 节点公网/内网可达地址；UDP + TCP 5060 |
| 设备编号 | 平台预分配（行业编码 `132` IPC / `118` NVR）或接受设备自带编号 |
| 密码 | 每设备唯一，≥12 位随机 |
| 注册有效期 / 心跳 | 3600s / 60s（接受设备 30~180s） |

### 6.2 信令行为

| 流程 | 平台行为 |
|---|---|
| REGISTER | 401 Digest 挑战 → 校验密码；白名单命中则自动入组，否则进入 `pending`；应答带 `Date` 头 |
| Keepalive | 更新 `lastSeenAt`；3 周期缺失置 `offline` 并清理活跃会话 |
| Catalog | 注册成功后立即查询；分页聚合（`SumNum`/`SN`）；每 24h 或手动"同步"重查；通道增删产生事件 |
| DeviceInfo / DeviceStatus | 注册后查询，用于 `vendor/model/fw` 与能力判定 |
| INVITE（实时） | 选节点 → `openRtpServer(port=0, tcp_mode=1, stream_id)` → SDP `s=Play`，`m=video <port> TCP/RTP/AVP 96`，`a=setup:passive`，`y=<SSRC>`，`StreamNumber` 0/1 → 收 200 OK → ACK；UDP 兜底 |
| BYE | 无人观看 / 用户停止 / 设备离线时发送；随后 `closeRtpServer` |
| RecordInfo | `StartTime/EndTime/Type(all\|time\|alarm\|manual)`，分页聚合；结果归一化为 §5.9 的 `segments` |
| Playback | SDP `s=Playback`，`u=<channelId>:0`，`t=<start> <end>`；`INFO` 携带 RTSP 风格体：`PLAY … Scale: 4.0`、`PLAY … Range: npt=<sec>-`、`PAUSE`、`TEARDOWN` |
| PTZ | `DeviceControl PTZCmd`（A.3 编码）；预置位 `81/82/83` |
| Alarm | 接收 `Notify Alarm` → 统一事件；回 `Response`；可选 `AlarmSubscribe` |
| 校时 | 注册应答 `Date`；可选 `DeviceControl … <TimeSync>` |
| 级联（预留） | 平台作为下级向上级注册，v1 不实现 |

### 6.3 能力探测

注册后按 `Manufacturer/Model` 查内置**厂商能力表**（TP-LINK / 海康 / 大华 / 宇视 …）得到默认能力；`RecordInfo` 首次查询成功即置 `record.device.query`；`Playback` 首次成功置 `record.device.play`；`INFO Scale` 返回 200 置 `record.device.speed`。探测失败的能力标为不支持并记录原因。

### 6.4 设备侧要求（自研固件 GB UA）

与 §6.2 对应的 UA 侧实现，附加：TCP 被动优先；`RecordInfo` 读取本地索引；`Playback` 与 IDP `record.play` 共用文件读取与倍速逻辑；`Alarm` 与 IDP `event.*` 同源。

---

## 7. ONVIF / RTSP 接入规范

### 7.1 发现

| 方式 | 规范 |
|---|---|
| WS-Discovery | 平台后端在其所在网段发 Probe（UDP 3702 多播），收集 `XAddrs / Scopes(hardware, name)`；10s 超时 |
| IP 段扫描 | 可选；探测 80/8000/8080/554；对开放 80/8080 的地址尝试 ONVIF `GetSystemDateAndTime`（无需认证） |
| 结果去重 | 以 `MAC`（若可得）或 `IP+XAddr` 去重，标注"已添加" |

### 7.2 ONVIF 添加与探测

1. `GetDeviceInformation`（认证）→ `vendor/model/fw/serial`。
2. `GetCapabilities/GetServices` → Media/PTZ/Imaging/Events 端点。
3. `GetProfiles` → 取前两个 Profile 作为 `main/sub`（按分辨率排序）；`GetStreamUri(RTP-Unicast, RTSP)`；`GetSnapshotUri`。
4. 能力置位：`live.*`、`snapshot`、`ptz`（有 PTZ 节点）、`focus`（Imaging Move 支持）、`event.*`（PullPoint 订阅成功）。
5. 凭据加密存储；认证失败回 `E2001`。

### 7.3 RTSP 直连添加

- 输入完整 RTSP URL（含凭据）或选择**品牌模板**（附录 C）+ IP + 凭据自动拼接。
- 探测：`OPTIONS` → `DESCRIBE`（解析 SDP 编码）；成功后调用 ZLM `addStreamProxy` 验证 5s 内出流。
- 仅能得到 `live.main`（用户可再填子码流 URL 得 `live.sub`）、`snapshot`（经 ZLM）、`record.platform`。

### 7.4 状态探测

按 §3.2 表；探测使用最小权限请求，失败原因分类为 `E1001 UNREACHABLE / E2001 AUTH_FAILED / E3001 PROTOCOL_ERROR`。

### 7.5 设备侧要求（自研固件 ONVIF/RTSP 服务端）

- ONVIF：Profile S 核心接口（Device、Media、Imaging、Events PullPoint）、WS-Discovery 应答、WS-UsernameToken；能力按 profile 声明。
- RTSP：`/stream1`（main）、`/stream2`（sub）；Digest；RTP over TCP/UDP；`GET_PARAMETER` 保活。

---

## 8. 媒体面规范（ZLMediaKit）

### 8.1 节点模型

```
media_node: id, name, apiUrl, secret, publicHost, rtmpPort, rtmpsPort, httpPort, httpsPort,
            rtpPortRange, maxStreams, weight, status, lastKeepalive
```

节点健康由 `on_server_started` / `on_server_keepalive` 驱动；30s 无心跳置 `offline`。

### 8.2 调度

1. 查 `channel:{id}:{profile}:node` 粘性映射；存在且节点在线则复用。
2. 否则在 `status=online && streams<maxStreams` 中选 `streams/maxStreams/weight` 最小者。
3. 写映射（TTL 随流存在刷新）；流注销时删除。
4. 节点离线：删除其所有映射；活跃会话标记 `error`，下次请求重调度。

### 8.3 流命名

| 场景 | app | stream | 进入方式 |
|---|---|---|---|
| IDP 直播 | `live` | `<DeviceID>_<ch>_<profile>` | 设备 RTMP publish |
| IDP 回放 | `record` | `<DeviceID>_<ch>_<sessionId>` | 设备 RTMP publish |
| GB 直播 | `rtp` | `<gbChannelId>_<profile>`（`stream_id` 参数） | `openRtpServer` |
| GB 回放 | `rtp` | `<gbChannelId>_pb_<sessionId>` | `openRtpServer` |
| ONVIF/RTSP | `proxy` | `<deviceUuid>_<ch>_<profile>` | `addStreamProxy` |
| 平台录像点播 | `vod`（HTTP 文件） | 由录像索引给出路径 | ZLM 静态文件 |

### 8.4 播放地址与鉴权

- 前端播放地址：`wss://<publicHost>:<httpsPort>/<app>/<stream>.live.flv?token=<playToken>`（h265web.js WS-FLV）；`MAY` 提供 `.live.mp4`（fMP4）与 HLS。
- `playToken`：JWT，含 `uid, channelId, app, stream, exp(≤10min)`；ZLM `on_play` 回调后端校验 + 通道权限。
- 推流 token：JWT，含 `node, app, stream, exp(≤60s)`，`on_publish` 校验并标记已用。

### 8.5 Hook 映射

| Hook | 后端处理 |
|---|---|
| `on_publish` | 校验推流 token；拒绝未知流 |
| `on_play` | 校验播放 token 与通道权限；计数 |
| `on_stream_changed` regist=true | 通道 `streamState=streaming`；写粘性映射；推 WS |
| `on_stream_changed` regist=false | `streamState=idle`；清映射；GB 会话 → BYE |
| `on_stream_none_reader` | IDP → `media.stop`/`record.stop`；GB → BYE + `closeRtpServer`；proxy → `delStreamProxy` |
| `on_stream_not_found` | 按需拉流：查通道 → 执行对应起播；回 `close=false` 让播放器等待 |
| `on_flow_report` | 流量入库（通道、用户、字节、时长） |
| `on_record_mp4` | 平台录像索引 |
| `on_server_keepalive/started` | 节点健康 |
| `on_rtp_server_timeout` | GB 收流超时 → BYE + 标记通道 `error E4002` |

### 8.6 按需策略

- 无人观看默认 **30s** 后停流（可配 10~300s）。
- 首次点播允许最长 **10s** 出流等待；超时返回 `E4002 STREAM_TIMEOUT`。
- 平台录像（`record.platform`）常驻拉流不受"无人观看"影响。

---

## 9. 平台侧接入 API（摘要）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/v1/devices/idp/bind` | `{deviceId, verifyCode, groupId, name}` → 触发 §5.5.3 |
| POST | `/api/v1/devices/idp/preadd` | 批量预添加（文件或数组） |
| POST | `/api/v1/devices/idp/discover` | 局域网 UDP 发现（私有化部署） |
| GET | `/api/v1/projects/{id}/gb28181/params` | 返回 §6.1 参数 |
| GET/POST | `/api/v1/devices/gb28181/pending` | 待确认列表 / 确认入组 |
| POST | `/api/v1/devices/gb28181/whitelist` | 预填国标 ID + 密码 + 分组 |
| POST | `/api/v1/devices/onvif/discover` | WS-Discovery / IP 扫描任务 |
| POST | `/api/v1/devices/onvif` | `{ip, port, user, pass, groupId}` → §7.2 |
| POST | `/api/v1/devices/rtsp` | `{url \| brand+ip+user+pass, groupId}` → §7.3 |
| POST | `/api/v1/devices/import` | 通用批量导入（CSV/Excel），行内 `source` 列 |
| DELETE | `/api/v1/devices/{id}` | 解绑/删除（软删除） |
| POST | `/api/v1/devices/{id}/transfer` | 跨分组/项目 |
| POST | `/api/v1/channels/{id}/play` | `{profile}` → `{url, token, node}` |
| GET | `/api/v1/channels/{id}/records` | `{start, end, types, source: device\|platform}` → segments |
| POST | `/api/v1/channels/{id}/playback` | `{start, end, speed, source}` → `{sessionId, url, token}` |
| PUT | `/api/v1/playback/{sessionId}` | `{op: pause\|resume\|seek\|speed, …}` |
| DELETE | `/api/v1/playback/{sessionId}` | 结束 |
| WS | `/ws/v1/events` | 设备状态、告警、任务进度 |

所有长任务（发现、导入、升级）返回 `taskId`，进度经 WS 推送。

---

## 10. 错误码

| 码 | 名称 | 场景 | 用户提示（建议） |
|---|---|---|---|
| E0400 | UNKNOWN_TYPE / BAD_REQUEST | 参数或消息类型错误 | — |
| E0403 | CAPABILITY_NOT_SUPPORTED | 设备不支持该操作 | 该设备不支持此功能 |
| E1001 | UNREACHABLE | 设备/地址不可达 | 检查网线、IP、防火墙 |
| E1002 | DEVICE_NOT_ONLINE | IDP 设备未上线 | 确认设备已通电联网；可将设备 DNS 改为 114.114.114.114 |
| E2001 | AUTH_FAILED | 用户名/密码错误 | 萤石设备请用 6 位验证码作为密码 |
| E2002 | ONVIF_DISABLED | ONVIF 未开启或不兼容 | 在设备 Web 开启 ONVIF，或改用 RTSP |
| E3001 | PROTOCOL_ERROR | RTSP/ONVIF/SIP 协议错误 | 核对地址与协议 |
| E3002 | CODEC_UNSUPPORTED | 编码不支持（如 MJPEG/SVAC） | 将设备编码改为 H.264/H.265 |
| E4001 | NODE_OFFLINE | 无可用媒体节点 | 联系管理员检查流媒体服务 |
| E4002 | STREAM_TIMEOUT | 起流超时 | 检查设备带宽，尝试 TCP |
| E4003 | STREAM_LIMIT | 超出并发/带宽上限 | 稍后重试 |
| E5001 | DISK_FULL | 平台录像磁盘不足 | 清理或扩容 |
| E6001 | GB_AUTH_FAILED | 国标注册密码错误 | 核对平台密码 |
| E6002 | GB_CATALOG_EMPTY | 目录为空 | 检查设备通道配置 |
| E6003 | GB_RECORD_UNSUPPORTED | 不支持录像检索/回放 | 该型号不支持设备端回放 |
| E7001 | ALREADY_BOUND | 设备已绑定其他项目 | 请原项目转移/删除，或提交解绑申请 |
| E7002 | VERIFY_CODE_INVALID | 验证码错误 | 核对标贴验证码 |
| E7003 | DEVICE_TIMEOUT | 设备未应答 | 稍后重试 |
| E7004 | PREADD_EXPIRED | 预添加过期 | 重新激活 |
| E7005 | NOT_BOUND | 设备未绑定 | 先完成绑定 |
| E7006 | RATE_LIMITED | 绑定尝试过多 | 1 小时后重试 |
| E8001 | OTA_INCOMPATIBLE | 固件与设备不匹配 | 选择正确固件 |
| E8002 | OTA_DOWNLOAD_FAILED | 下载/校验失败 | 检查网络后重试 |
| E8003 | OTA_ROLLBACK | 新固件启动失败已回滚 | 联系支持 |

---

## 11. 安全要求

| 领域 | 要求 |
|---|---|
| 传输 | IDP：TLS 1.2+ 双向认证；ONVIF：WS-UsernameToken，凭据传输走 HTTPS 时优先；RTSP：Digest；SIP：Digest；平台 API：HTTPS + JWT |
| 凭据存储 | 第三方设备密码 AES-256-GCM 加密落库，密钥由 KMS/环境注入；日志与导出**必须**脱敏 |
| 验证码 | 平台不存明文；同 DeviceID 绑定失败 ≤5 次/小时（`E7006`） |
| 设备默认密码 | 自研固件首次绑定或本地登录**必须**强制修改；`hello.localUserChanged=false` 的设备 UI 标黄提示 |
| 端口暴露 | 自研固件默认关闭 telnet/ssh；ONVIF/RTSP 可在 IDP `cfg.set` 关闭 |
| Token | 推流 token ≤60s 一次性；播放 token ≤10min，绑定用户与通道 |
| 审计 | 所有绑定/解绑/转移/配置/升级操作记录操作者、对象、结果 |
| 证书吊销 | 平台维护设备证书吊销列表，Broker 拒绝吊销证书 |

---

## 12. 一致性与验收

### 12.1 IDP 设备一致性测试（固件必须通过）

| 编号 | 用例 | 通过标准 |
|---|---|---|
| C-01 | 首次上线 `hello` 字段完整、能力集与 profile 一致 | schema 校验通过 |
| C-02 | 绑定：正确验证码 ≤10s 完成；错误验证码 `E7002`；已绑定 `E7001` | 全部符合 |
| C-03 | LWT：断电 ≤5s 平台置离线 | 符合 |
| C-04 | `media.start` 后 ≤2s 出流；`media.stop` 后 ≤2s 停流；main+sub 并存 | 符合 |
| C-05 | 无人观看 30s 平台下发 `media.stop`，设备停止推流 | 符合 |
| C-06 | `record.query` 分页正确；`record.play` PTS 连续；`pause/resume/seek/speed` 各项 | seek ≤1s，speed 8x 不断流 |
| C-07 | `cfg.set` 非法值进入 `rejected[]`，合法值 5s 内生效并可 `cfg.get` 回读 | 符合 |
| C-08 | OTA：正确包升级并 confirm；错包 `E8001`；启动失败回滚 `E8003` | 符合 |
| C-09 | 未知 `type` 回 `E0400`；所有命令 5s 内 `ack` | 符合 |
| C-10 | ACL：设备尝试发布他人主题被拒 | 符合 |

### 12.2 GB28181 适配器验收

- 已购 TP-LINK 每型号：注册、目录、主/子码流点播、TCP 被动与 UDP、RecordInfo/Playback（能力探测正确置位或标不支持）。
- 海康 IPC/NVR：同上，含 PTZ 与 Alarm。
- 对接 wvp-GB28181-pro 作为对照：同一设备在两平台行为一致。

### 12.3 ONVIF/RTSP 适配器验收

- ONVIF Device Test Tool 生成的模拟设备与至少 2 个品牌真机：发现、添加、双码流、PTZ（如有）、事件。
- 萤石至少 1 型号：RTSP 模板拉流成功；错误密码 `E2001` 文案正确。

### 12.4 媒体面验收

- 2 节点 100 通道均衡度 ≤1.3；节点宕机后通道自动重调度。
- 播放/推流 token 过期与越权均被拒绝。
- 按需拉流：首次点播首帧 ≤1.5s（局域网），无人 30s 释放。

---

## 附录 A：IDP 消息 JSON Schema（节选）

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://ipccloud.example.com/schemas/idp/v1/envelope.json",
  "type": "object",
  "required": ["v", "type", "msgId", "ts"],
  "properties": {
    "v":     { "const": 1 },
    "type":  { "type": "string", "pattern": "^(status|event|ack|cmd|cfg|ota|media|record|p2p)(\\.[a-z_]+)?$" },
    "msgId": { "type": "string", "minLength": 8, "maxLength": 64 },
    "ts":    { "type": "integer", "minimum": 0 },
    "code":  { "type": "integer" },
    "msg":   { "type": "string" },
    "data":  { "type": "object" }
  }
}
```

```json
{
  "$id": "https://ipccloud.example.com/schemas/idp/v1/status.hello.json",
  "type": "object",
  "required": ["model", "vendor", "fw", "capabilities", "channels", "net"],
  "properties": {
    "model": { "type": "string" }, "vendor": { "type": "string" }, "fw": { "type": "string" }, "hw": { "type": "string" },
    "bound": { "type": ["object", "null"], "properties": { "projectId": { "type": "string" } } },
    "capabilities": { "type": "array", "items": { "type": "string" }, "uniqueItems": true },
    "channels": { "type": "array", "minItems": 1, "items": {
      "type": "object", "required": ["ch", "profiles"],
      "properties": {
        "ch": { "type": "integer", "minimum": 1 }, "name": { "type": "string" },
        "profiles": { "type": "array", "minItems": 1, "items": {
          "type": "object", "required": ["id", "codec", "w", "h", "fps"],
          "properties": {
            "id": { "enum": ["main", "sub", "third"] }, "codec": { "enum": ["h264", "h265"] },
            "w": { "type": "integer" }, "h": { "type": "integer" }, "fps": { "type": "integer" }, "kbps": { "type": "integer" }
          } } } } } },
    "storage": { "type": "object" },
    "net": { "type": "object", "required": ["ip", "mac"], "properties": {
      "ip": { "type": "string" }, "mac": { "type": "string" }, "type": { "enum": ["eth", "wifi", "4g"] } } },
    "limits": { "type": "object", "properties": { "playbackSessions": { "type": "integer", "minimum": 1 }, "consumers": { "type": "integer" } } },
    "localUserChanged": { "type": "boolean" }
  }
}
```

（`media.start`、`record.play`、`record.ctrl`、`ota.start`、`cfg.set` 等 schema 随规范源码仓库 `schemas/idp/v1/` 维护，文档只列关键字段。）

## 附录 B：时序图

### B.1 IDP 绑定

```
用户/前端        Go 后端            Broker           设备
  │ 输入ID+码 ──▶│                   │                │
  │              │ 校验在线未绑定/HMAC│                │
  │              │──cmd.bind────────▶│──────────────▶│ 校验 bindToken
  │              │◀─ack(0)───────────│◀──────────────│ 持久化 projectId
  │◀─成功 ───────│ 入库 bound, WS 推送│                │
```

### B.2 IDP 直播（按需）

```
前端       后端            ZLM 节点         Broker      设备
 │ play ─▶│ 选节点          │               │           │
 │        │──media.start────────────────────▶│─────────▶│ RTMP publish ─▶ ZLM
 │        │◀─on_publish(校验 token)──────────│           │
 │        │◀─on_stream_changed(regist)───────│           │
 │◀─url───│                 │               │           │
 │ 播放 ─────────────────▶ │               │           │
 │ 关闭   │◀─on_stream_none_reader(30s)─────│           │
 │        │──media.stop─────────────────────▶│─────────▶│ 停推
```

### B.3 GB28181 实时点播

```
前端     后端(SIP 服务器)        ZLM 节点        国标设备
 │ play ▶│ 选节点 → openRtpServer ─▶│               │
 │       │──INVITE(SDP: TCP passive, port, SSRC, StreamNumber)──▶│
 │       │◀─200 OK(SDP)──────────────────────────────│
 │       │──ACK──────────────────────────────────────▶│ RTP/PS ─▶ ZLM
 │       │◀─on_stream_changed────────│               │
 │◀─url──│                           │               │
 │ 关闭  │◀─on_stream_none_reader────│               │
 │       │──BYE──────────────────────────────────────▶│
 │       │──closeRtpServer──────────▶│               │
```

### B.4 GB28181 回放与控制

```
前端      后端                          ZLM         设备
 │ 选日期 ▶│──MESSAGE RecordInfo(start,end)─────────────▶│
 │◀─时间轴─│◀─RecordList(分页聚合)──────────────────────│
 │ 点击 t ▶│ openRtpServer ──────────▶│                │
 │         │──INVITE(s=Playback, t=start end)──────────▶│ 推 PS
 │ 4x     ▶│──INFO PLAY Scale:4.0──────────────────────▶│
 │ seek   ▶│──INFO PLAY Range:npt=<sec>-───────────────▶│
 │ 暂停   ▶│──INFO PAUSE───────────────────────────────▶│
 │ 关闭   ▶│──BYE + closeRtpServer──▶│                │
```

## 附录 C：品牌 RTSP URL 模板

| 品牌 | 主码流 | 子码流 | 备注 |
|---|---|---|---|
| 萤石 | `rtsp://admin:{验证码}@{ip}:554/h264/ch1/main/av_stream` | `…/h264/ch1/sub/av_stream` | 需在萤石 APP 开启局域网 RTSP；部分型号 H.265 路径为 `/h265/…` |
| 海康 | `rtsp://{user}:{pwd}@{ip}:554/Streaming/Channels/101` | `…/Channels/102` | NVR 通道 N：`{N}01/{N}02` |
| TP-LINK | `rtsp://{user}:{pwd}@{ip}:554/stream1` | `…/stream2` | 商用 IPC |
| 大华 | `rtsp://{user}:{pwd}@{ip}:554/cam/realmonitor?channel=1&subtype=0` | `…subtype=1` | — |
| 宇视 | `rtsp://{user}:{pwd}@{ip}:554/video1` | `…/video2` | — |
| 自研（IDP 设备本地） | `rtsp://{user}:{pwd}@{ip}:554/stream1` | `…/stream2` | 与 TP-LINK 一致 |

模板为默认值，允许用户编辑；凭据字段在 UI 中单独输入，不在模板内明文展示。

## 附录 D：变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v1.0 | 2026-09-04 | 首版：统一设备模型、IDP v1、GB28181/ONVIF/RTSP 适配器、媒体面、错误码、验收 |
