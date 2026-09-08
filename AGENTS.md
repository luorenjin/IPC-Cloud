# IpcCloud 工程全局智能体规范 (AGENTS.md)

本文件是 Antigravity 智能体在 IpcCloud 项目中执行编码、调试、架构设计与重构时的统一行为准则。

---

## 1. 语言与协作规范
- **全中文沟通与文档**：所有回答、代码注释、提交记录、错误信息及前端 UI 文案必须使用简体中文。技术专有名词（如 Qt6, C++, libcamera, DMA, YUV, JSON, ISP, Sobel, Debian, ZLMediaKit 等）保持原样。
- **PRD 与设计溯源**：
  - 核心设计依据统一引用 Docs/PRD/ 下的有效文档（如 IpcCloud平台PRD_v1.0.md 中的 LIVE-02、MGR-05、ALM-03 等编号）。
  - **重要警示**：Docs/PRD/用户手册/、问题指南/ 及 README.md 为竞品（TP-LINK）分析存档，**严禁**误当作本项目真实规格。
- **决策边界**：芯片/传感器/Flash 选型明确尚未冻结（参见《决策记录与待定事项.md》），严禁在代码中硬编码任何未经冻结的硬件假设。

---

## 2. 协议接入与服务端设计守则 (platform/server/)
- **来源判别字面值**：idp、gb28181、onvif、tsp 作为字面判别值贯穿全系统。追踪协议行为时，必须搜索该字符串本身。
- **协议适配器隔离**：
  - 所有协议差异严格封装在 server/internal/adapter/*，严禁协议私有逻辑溢出到外部。
  - 新增协议必须实现 dapter.Adapter 接口，并在 cmd/ipccloud/main.go 注册。
- **流命名铁律（PRD §8.3）**：
  - idp: app=live，stream={deviceID}_{channelIdx}_{profile}
  - gb28181: app=tp，stream 取自 channel.Meta["gbStream"/"gbStreamSub"]，缺省回退 {gbChannelId}_{profile}
  - onvif / tsp: app=proxy，stream={channelID}_{profile}
- **起停流分支设计**：engine.StartPlay 和 StopPlay 中的 switch 分支故意保持并行，严禁强行统一抽象而丢失各协议独特的生命周期（推流 vs. 拉流 vs. RTP INVITE）。

---

## 3. 前端与 WebAssembly 开发守则 (platform/web/)
- **纯 SPA 与网络栈**：Nuxt3 运行在 ssr: false 模式。API 与 WS 调用统一使用 useApi.ts、useAuth.ts、useWs.ts，严禁直接使用原生 etch 绕过拦截器。
- **WASM 多线程隔离头不可破坏**：
  - 
uxt.config.ts 中的 COOP (same-origin) 与 COEP (equire-corp) 是 h265web.js 启用多线程 Worker 与 SharedArrayBuffer 的必要条件，**绝对不可随意移除或修改**。
- **验证闭环**：前端涉及音视频播放（pages/live.vue）、回放（pages/playback.vue）的改动，必须在真实浏览器环境联调验证。

---

## 4. 固件架构分层与零拷贝守则 (irmware/)
- **CMake 架构硬门禁**：
  - core/ 与 modules/ 严禁直接 #include "platform/..." 或使用芯片特异性宏（如 PLATFORM_|GK7205|RV1106|HI35...）。构建系统会在配置阶段直接触发 FATAL_ERROR。
  - 所有平台相关逻辑必须下沉到 platform/<soc>/ 并通过 const hal_ops_t *hal_platform_get(void) 导出。
- **零拷贝与内存安全**：
  - 视频帧数据严禁拷贝至业务层，必须采用引用计数零拷贝，使用完毕后必须显式调用 elease_frame。
- **测试门禁**：修改固件代码后，必须保证 	ests/hal_conformance 完全通过（ail 必须恒为 0）。

---

## 5. 模拟器与联调守则 (simulator/)
- **端到端验证优先**：修改 platform 或协议适配器后，优先使用 simulator 对应协议拉起模拟设备完成闭环验证（“接入-鉴权-心跳-起播-告警”），杜绝仅凭静态走读推测行为。
- **双向契约一致性**：修改模拟器行为时，必须严格保证与 platform/server/internal/adapter/ 的协议假设（如 IDP 17 位设备号、GB28181 20位国标编码、鉴权参数）完全同步。