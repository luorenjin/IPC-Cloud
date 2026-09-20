# IpcCloud 平台 Web 端再审计与改进计划

## 背景（Context）

上一轮「值守台」重设计（`C:\Users\user\.claude\plans\web-refactored-floyd.md`）已把 `platform/web` 从"逐像素抄 TP-LINK 商云截图"的浅色消费云外观，迁移为深色专业监控台体系：设计令牌层、25 个 `components/ui/*` 组件、三套壳体、首页/登录/直播/回放四个旗舰页均已按新方案落地（当前 63 个文件、约 2100 行改动尚未提交）。

本轮任务是**再次审计**：用 `frontend-design` 技能的标准检查落地质量，用 PRD 附录 A 的 63 个需求编号逐条对照实现，找出未完成的功能点，输出可执行的改进计划。

审计方法：3 路并行排查（设计体系 / PRD 需求覆盖 / 逐页完成度）+ 对关键指控逐条到源码核实（含 `router.go` 全部路由）。以下结论均附文件位置。

**已确认的执行约束**（与用户对齐）：
- 范围：**前端为主 + 修前后端断链**。后端只修让前端功能断裂的小缺陷；较大的后端项单列为后续计划。
- i18n：**先隐藏语言切换入口**，新增/重做页面一律走 `t()`；全量词条接入作为独立最后阶段，可另行审批。
- 开始前**先把工作树提交为检查点**。
- **一次审批，按阶段顺序执行，每阶段末汇报验证结果**，用户可随时叫停。

---

## 一、审计结论

### 1.1 做得好、应保留的决策

- **令牌纪律接近满分**：全部色值集中在 `assets/css/main.css` 的 `@theme`，应用代码硬编码十六进制仅 1 处（且在注释里）。
- **双圆角制**（`--radius-chrome` 6px 给可点的外壳控件 / `--radius-signal` 2px 给可读的数据面）被实际遵守，是有信息含量的结构手段。
- **阴影退场**：深底用描边 + 内部高光表层级，只有浮层用扩散阴影；无装饰性渐变（4 处 gradient 全部功能性）。
- **模板痕迹基本干净**：无 eyebrow、无末尾「→」、全大写仅 2 处且为设备 ID 输入框的功能性大写、等宽字体 38 处均符合"只用于技术性数值"的自定约束。
- **动效克制成体系**：8 个 `ipc-*` keyframes，时长 0.12–0.22s。
- **登录页与首页有记忆点**：「开始值守」标题 + 信号网格背景；首页弧形在线率量规 + 左色条告警流。
- **`H265Player.vue` 实现扎实**，`live.vue` / `playback.vue` 是真实现（分屏/PTZ/预置位/三色时间轴/滚轮缩放/框选下载/9 档倍速）。
- **`ErrorCard` 的 code/msg/suggest 三段式**与 `toastApiError` 是好的共享先例。
- **Element Plus 已彻底清除**，`package.json` 干净；COOP/COEP 头与 `public/vendor/h265web.js` 完好。

### 1.2 问题总表（按严重度）

#### A. 假功能 / 假数据 / 断链（诚信与可用性，最高优先级）

| # | 问题 | 位置 |
|---|---|---|
| A1 | 设备列表「预览」弹窗整体是空壳：回放段硬编码数组、无流时显示实景照片 `/img/office-cam.jpg`、抓拍只弹"抓拍成功已保存至本地相册"、对讲（PRD 明确 MVP 不做）/倍速/暂停只改本地变量 | `components/DevicePreviewModal.vue:113,166-172,334` |
| A2 | **添加设备主链路断裂（ADD-01 P0）**：调用不存在的 `POST /devices`（`router.go` 无此路由，实际是 `/devices/idp/bind`）；验证码写死 `'123456'` 且无输入框；添加弹窗 6 个 Tab 只有 idp 分支有逻辑，ONVIF/RTSP/发现的后端路由都在但前端零调用 | `pages/devices/index.vue:295,316`；`scan.vue:151`（GET 调 POST 路由）、`:168`（同 404） |
| A3 | 设备表格空字段回退为竞品截图里的假值：型号 `TL-IPC455E-AI4`、IP `192.168.1.53`、MAC `4C-10-D5-85-3B-FB`；分组计数硬编码 `(0/1)`；「状态持续时长」恒 `---`、「置顶」恒「否」 | `pages/devices/index.vue:430,596-598` |
| A4 | 工具栏 3 个按钮无任何处理函数（网络设置配置 / 设置地理位置 / 设备升级），PRD 无前两项；「设备同步」只是重新 `load()` | `pages/devices/index.vue:503,507,509,515` |
| A5 | 登录页预填生产口令 `admin` / `Admin@12345`；「扫码登录」是纯图标占位（ACC-01 无此需求）；「忘记密码」用错误条冒充功能；占位符"手机号 / 用户名"（平台无手机号登录） | `pages/login.vue:8-9,54-62,82,105` |
| A6 | **固件升级假成功（MGR-10）**：调用不存在的 `/devices/:id/upgrade`，`catch` 外无条件 toast 成功 | `pages/devices/[id].vue:272-273` |
| A7 | **诊断结果恒空（MGR-07 P0）**：后端返回 `results`（`devices.go:250`），前端读 `res.items \|\| res.checks` | `pages/devices/[id].vue:83` |
| A8 | 媒体节点 sparkline 是假历史（打开时只有一个点补成直线）；「负载%」实为流数占比 | `pages/system/nodes.vue:77-115` |
| A9 | CRL 上传解析是伪实现（正则抓一段 base64 当设备 ID） | `pages/system/settings.vue:243` |
| A10 | 项目选择页把设备数标成「通道」数；每个项目串行发 2 次请求 | `pages/console.vue:16-29,106` |
| A11 | 分组拖拽排序 `.catch(() => {})` 吞错后仍 `toast.success('分组顺序已更新')` | `pages/system/projects.vue:280-283` |
| A12 | 任务中心抽屉 UI 完备，但前端不调 `/tasks`、后端 `engine.go:355 taskProgress` 是空函数，抽屉恒空 | `layouts/default.vue:265-282`、`composables/useTasks.ts` |

#### B. 可访问性近乎为零（政企私有化交付的验收风险）

| # | 问题 | 证据 |
|---|---|---|
| B1 | `aria-label` / `role` / `tabindex` / `focus-visible` / `prefers-reduced-motion` 全站 **0 命中**；`sr-only` 仅 1 处 | 全仓 grep |
| B2 | 侧栏菜单项、首页功能入口、项目列表行全是 `<div @click>`，键盘不可达；74 个 `<button>` 仅 4 个有 `title`，顶栏任务中心/消息是纯图标无可访问名称 | `layouts/default.vue:145-175`、`pages/index.vue:200`、`pages/console.vue:93` |
| B3 | 组件写了 `outline-none` 却未补 `focus-visible` 替代 | `Select.vue:33`、`Switch.vue:15`、`Table.vue:50` |

#### C. i18n 基础设施建好却未接入（ACC-09 P1 名不副实）

- `useI18n.ts` 仅 43 个词条；21 页中 20 页 `t()` 为 0，切 EN 后正文不变。硬编码最多：`devices/index.vue` 339 处、`system/roles.vue` 273、`devices/[id].vue` 271、`system/nodes.vue` 269、`system/settings.vue` 268。`layouts/default.vue` 自身也混用（`返回首页`、`所有企业及项目`、任务状态文案）。

#### D. 响应式覆盖不足

- 断点前缀全站仅 26 处集中在 4 个文件；`@media` 0 命中。侧栏 220px 固定不可折叠、无移动端抽屉；`live.vue:265` / `playback.vue:374` 通道树固定 232px。
- `w-55` / `w-58` / `h-13` 在 Tailwind v4 默认 spacing 中**不存在**，靠紧随其后的 inline `style="width: 220px"` 兜底——样式来源分裂（`layouts/default.vue:138-139`、`layouts/console.vue:27`、`live.vue:265`、`playback.vue:374`）。

#### E. 表单与数据流缺陷

| # | 问题 | 位置 |
|---|---|---|
| E1 | `UiButton` 的 `:loading` 是**幽灵 prop**：7 处传入，`Button.vue:3-7` 未声明 → 提交中无反馈、不禁用、可重复提交 | `console.vue:147`、`scan.vue:242`、`devices/[id].vue:546` 等 |
| E2 | 无 `middleware/`、无路由守卫：登录态只在两个 layout 的 `onMounted` 里查，未登录先渲染完整页面再跳转；ACC-01 要求"Token 过期跳登录并保留原路径" | `layouts/default.vue:17-19`、`layouts/console.vue:6-9` |
| E3 | 两处导出 `window.open` 直连，不带 Authorization / X-Project-Id | `pages/devices/index.vue:225`、`pages/system/audit.vue:115` |
| E4 | 编辑设备：名称可存空、无防重提交 | `pages/devices/[id].vue:206` |
| E5 | 全站校验只有 toast，无字段级内联错误 | 全站 |
| E6 | 吞错误：录像模板 `loadRefs` 失败→引用计数恒 0、级联提示与删除保护失效（`record/templates.vue:58`）；向导 `setupDone` 失败→下次再被拉回向导（`wizard.vue:79`）；设置页默认策略加载失败显示「未开启」（`settings.vue:115`） | — |
| E7 | 分页缺失：仅 `alarms/index` 与 `system/audit` 用 `UiPagination`；设备列表手写分页；角色/成员/节点/计划/模板全量渲染，`roles.vue:257` 硬编码 `pageSize: 200` | — |
| E8 | WS 只有 5 处订阅；设备列表、设备详情、直播通道树不订阅 → 在线状态不实时（值班员核心场景） | `pages/devices/index.vue`、`pages/devices/[id].vue`、`pages/live.vue` |
| E9 | `clampPort` 非法输入静默改写为默认值，且被误用于 maxStreams / weight | `pages/system/nodes.vue:171` |
| E10 | `record/templates.vue:83` 只校验名称可提交空计划；`roles.vue` 可创建零权限角色；`scan.vue:177` 绑定成功不重置表单 | — |
| E11 | Skeleton 近乎缺席（3 处）；`account.vue`、`wizard.vue` 三态全无；首页无 loading | — |

#### F. 技术债与残留

| # | 问题 | 位置 |
|---|---|---|
| F1 | 设备列表页与预览弹窗满篇「严格 100% 对齐图 4 / 商云原型图」注释，结构上仍是 TP-LINK 复刻（IPC Tab、可折叠统计卡、功能按钮阵列）；上一轮"逐屏定制"在这两处只换了皮 | `pages/devices/index.vue`（约 30 处）、`components/DevicePreviewModal.vue` |
| F2 | 8 个 ui 组件头注释与实际矛盾（仍写"白底 / 浅灰表头 / 品牌蓝"） | `Card.vue:2`、`Table.vue:2`、`Button.vue:2` 等 |
| F3 | 尺寸档位不齐：Button/Input 有 `lg`，Select/Switch 无 | `Select.vue:14`、`Switch.vue:5` |
| F4 | 重复实现：`fmtTime` 5 处、`fmtSchedule + DAY_NAMES` 3 处、`ago` 2 处、树构建 2 处、引用计数 2 处；`ALARM_KIND_MAP` 2 处靠注释"人工同步" | `nodes.vue:332`、`projects.vue:289`、`roles.vue:463`、`settings.vue:272`、`audit.vue:119`；`index.vue:49` vs `alarms/index.vue:7` |
| F5 | 协议来源（`idp/gb28181/onvif/rtsp`）展示名/颜色映射分散 3 处形态不一——根 CLAUDE.md 点名的四个字面判别值没有集中定义 | `devices/index.vue:26`、`devices/[id].vue:22`、`index.vue:9` |
| F6 | 状态色映射 4 套各自为政 | `nodes.vue:25-43`、`audit.vue:123`、`devices/[id].vue:29` |
| F7 | 首页「功能入口」7 面板与侧栏导航完全重复；顶栏「返回首页」按钮是抄图 4 的残留（PRD §4 顶栏无此项） | `pages/index.vue:101-110,196-213`、`layouts/default.vue:184-192` |
| F8 | `plugins/ui-alias.ts` 与自动扫描重复；`ScheduleGrid.vue:58` 死代码；`H265Player.vue:132` 调试 `console.log`；首页 `loadNames` 为取名字全量拉 `/devices`+`/channels` | — |

#### G. PRD 功能缺口（63 项：✅8 / ◐41 / ⬛仅后端 4 / ❌10）

决策记录**未延后任何平台功能编号**；PRD §2.2 自身声明的 MVP 不做项（9/16 分屏、对讲、轮巡、P2P、外部通知、对象存储、同步回放、地图、计费）不计入缺口，ALM-09 因此排除。

**P0 阻断级**（前端可修的在本计划内；纯后端的单列）

| 编号 | 缺口 | 缺什么 |
|---|---|---|
| ADD-01 | 添加设备 404（见 A2） | 前端改调 `/devices/idp/bind` + 验证码输入 |
| ADD-02 | `scan.vue:151` GET 调 POST（405）、`:168` 调 `/devices`（404） | 前端 |
| MGR-07 | 诊断结果恒空（见 A7） | 前端一行；后端诊断项缺 `suggest` 字段（小） |
| ALM-05 | 告警策略端点完备（`/alarm-policies`）、engine 已消费，但**无页面无入口** | 前端一页 |
| ACC-08 | 时间范围/对象类型筛选后端不读；`audit.vue:21-37 ACTION_MAP` 键与后端 action 值格式不匹配，中文映射恒不生效；导出忽略筛选 | 联调（小后端 + 前端） |
| DASH-01 | 六项概览仅渲染两项；播放路数/今日告警/节点健康后端已算前端不用；存储用量后端不返回 | 前端为主 |
| LIVE-01 | `H265Player.vue:79` 硬编码 `core:'mse_mp4'` 无 WebCodec 优先/WASM 降级；码率/延时字段从不赋值 | 前端 |
| LIVE-06 | 直播工具条无暂停、无音量（写死 muted） | 前端 |
| LIVE-02 | 前端关闭画面不调 `/channels/:id/stop` | 前端 |
| ALM-06 | `layouts/default.vue:26` 传 `unread=1` 而后端认 `read` 参数 → 未读角标错 | 前端一行 |
| SYS-01 | `nodes.go:52-66` 编辑忽略端口与 RTP 段；`scheduler.go:46` 索引 bug 漏最后一个节点 | 小后端 |
| MGR-13 | `devices.go:91` 导出忽略全部筛选 | 小后端 |
| ACC-02 | 节点自检失败原因是死代码：`nodes.go:217-220` 不返回 reason | 小后端 |
| **纯后端（单列后续）** | ACC-05 `Role.Scope` 只写不读→通道级权限隔离未实现；ALM-04 `stream_lost/record_fail/auth_fail` 无产生方；ADD-05 拒绝不写黑名单设备会重复注册；ACC-07 停用用户 WS 继续推送；ACC-01 锁定态在进程内存 + 密码复杂度不校验字母数字；`rtsp.go:153` nil err 求值 panic；LIVE-07 `channels.go:71-78` 无 Focus 字段 | 后端 |

**P1 缺口**（本计划纳入"后端已就绪、前端缺入口"的项）

| 编号 | 状态 | 缺什么 |
|---|---|---|
| ADD-06/07 | ⬛ ONVIF 发现与手动添加 ONVIF/RTSP 后端齐全，前端零调用 | 前端（并入添加设备弹窗重做） |
| ADD-03/08 | ◐ 预添加与批量导入仅 textarea、验证码写死、无 CSV 模板、无结果表 | 前端 |
| ADD-05 | ◐ 国标白名单三端点前端零调用 | 前端 |
| MGR-12 | ◐ 设备转移后端完整（含 `cmd.transfer`）前端零调用 | 前端 |
| ACC-04 | ◐ `POST /groups/move-devices` 前端零调用；分组树四处未复用同一组件 | 前端 |
| REC-07 | ⬛ `/storage/overview` 前端零调用 | 前端一卡片 |
| SYS-02 | ✅ 但 `nodes.vue:528` 字段名不匹配致来源通道列空 | 前端一行 |
| DASH-02 | ◐ 最近告警不可点击；最近操作未实现 | 前端 |
| ALM-10 | ❌ 首页 7 天告警统计；`alarmToday` 后端算了前端不渲染 | 前端 + 小后端（按天聚合） |
| REC-05 | ◐ `ScheduleGrid.vue:29` 粒度 30 分钟（PRD 1 分钟）；无复制到其他日/清空 | 前端 |
| REC-03 | ◐ 平台源截图只 toast | 前端 |
| P-18 | 任务中心恒空（见 A12） | 前端先拉 `/tasks`；后端 `taskProgress` 需实现（单列） |
| **需较大后端（单列后续）** | MGR-10 OTA 无 API；MGR-14 健康指标无时序；ADD-04 局域网发现；ADD-10 多路径合并；REC-06 事件录像（`hooks.go:240` 硬编码 timer）；ALM-08 联动快照；LIVE-08 对焦；ALM-03 ONVIF Events/国标多通道归属（`gb.go:358-365` 硬编码 idx=1）；MGR-06 国标同步不触发 Catalog；SET-01 `captchaRate` 无消费方；SET-02 `caStatus` 硬编码 | 后端 |

**路由 ↔ 页面差异**：前端调用但后端不存在——`POST /devices`、`POST /devices/:id/upgrade`。后端存在但前端零调用——`/devices/onvif/discover`、`/devices/onvif`、`/devices/rtsp`、`/devices/gb28181/whitelist*`、`/devices/:id/transfer`、`/groups/move-devices`、`/alarm-policies`、`/storage/overview`、`/tasks`、`/projects/:id/gb28181/params`。

---

## 二、改进计划（按阶段顺序执行，每阶段末汇报）

### 阶段 0 · 提交检查点（≈0.5 人日）

- 把工作树拆为两个提交：①「视觉体系迁移」（`platform/web/**`、`docker-compose.yml`、`web/Dockerfile`）；②「告警规则接入判定链路」（`server/**`、`store/migrate.go`、`engine/alarmrule*.go`、两份 Docs/superpowers 文档、PRD/决策记录改动）。`pages/system/settings.vue` 同时含两类改动，若无法干净拆分则归入①并在提交信息中说明。
- 提交前跑 `cd platform/server && go build ./... && go test ./...`、`cd platform/web && npm run build` 确认可构建。

### 阶段 1 · 止血：删假、修断链、补守卫（前端 ≈3–4 人日，后端 ≈1 人日，可前后端并行）

目标：让用户点得到的每个控件都是真的；P0 主链路可用。不改页面结构，不在假数据上做设计。

**前端**
- A2/ADD-01：`devices/index.vue` 添加设备改调 `POST /devices/idp/bind`，增加**验证码输入框**（后端 `devices.go:464` 标记 `binding:"required"`，这是阻塞项不是可选项；`lifecycle.go:188-206` 首次绑定即登记 HMAC、其后必须匹配，因此 simulator 联调首次任意输入即可）；`scan.vue:151/168` 改正 method/路由。
- A3：空字段一律显示 `—`（与 `Table.vue:84` 既有约定一致）；删 `(0/1)`、「状态持续时长」「置顶」两列。
- A4：删「网络设置配置」「设置地理位置」；「设备升级」入口移除（MGR-10 无 API，详情页入口改为禁用 + 说明"固件升级尚未开放"，遵循 PRD"能力灰显"）；「设备同步」改为对选中设备逐个调 `POST /devices/:id/sync`，无选中禁用。
- A5：删预填口令；删扫码模式与切换按钮；占位符改「用户名」；「忘记密码」改为静态说明文字。
- A6：删除 `[id].vue` 升级对话框假成功路径。
- A7/MGR-07：读 `res.results`；渲染探测项 + 状态灯 + 耗时（按上一轮"示波器读数"规格）。
- A1（临时止血，完整重做在阶段 4）：删 `/img/office-cam.jpg` 与假回放段、假抓拍、对讲按钮；回放 Tab 暂时改为跳转 `/playback?channelId=`；预览 Tab 保留真实拉流。
- A8：只有 1 个采样点时不画直线，显示"采集中"；「负载%」改名「流数占比」。
- A9：确认 `POST /idp/crl` 接受的格式后，前端改为**直接输入吊销设备 ID 列表**或**上传文件交后端解析**，删伪正则。
- A10：`console.vue` 文案改「N 台设备」，两请求 `Promise.all` 并行。
- A11：排序改 `await Promise.all(...)`，失败 `toastApiError` 并回滚本地顺序。
- A12：任务抽屉打开时拉 `GET /tasks` 渲染历史；无任务时空状态文案改为"暂无进行中的任务"。
- E1：`Button.vue` 实现 `loading` prop（禁用 + 旋转图标 + `aria-busy`）。
- E2：新增 `middleware/auth.global.ts`：无 token → `/login?redirect=<原路径>`；`useApi` 收到 401 → 清 token 跳登录并保留路径（ACC-01 验收项）。
- E3：`useApi` 新增 `download(path, params)`（带鉴权取 Blob → `a[download]`），替换两处 `window.open`；同时修 `devices.go:91` 让导出尊重筛选（MGR-13）。
- `useCookie('ipc_token')` 已散在 `useApi/useAuth/useWs` 三处，middleware 与 `download()` 会再加两处——改为 `useAuth` 暴露唯一 token getter。
- E4：编辑设备名称必填 + `loading` 防重。
- E6：三处吞错 catch 改为 `toastApiError`，`loadRefs` 失败时禁用级联删除按钮。
- E9：`clampPort` 非法输入报错而非静默改写；maxStreams/weight 用各自校验。
- ALM-06：`default.vue:26` 参数 `unread=1` → `read=0`（以后端 `handleListAlarms` 实际参数为准）。
- LIVE-02：`closeCell/closeAll` 调 `POST /channels/:id/stop`。
- SYS-02：`nodes.vue:528` 字段名对齐后端。

**小后端（断链修复，均不阻塞前端）**
- `nodes.go:52-66` 编辑写回端口与 RTP 段；`scheduler.go:46` 索引越界修正；`nodes.go:217-220` 自检返回 `reason`（ACC-02）；`audit-logs` 读时间范围与对象类型参数，并先给出 action 值清单供前端 `ACTION_MAP` 对齐（ACC-08）；`devices.go:91` 导出尊重筛选；`lifecycle.go:176-177` 绑定失败计数取值后被丢弃从不比阈值（ADD-01 限速）——补上阈值判断。

**验证**：走通 添加 IDP 设备（simulator）→ 列表出现 → 诊断有结果 → 导出下载成功；未登录直达 `/devices` 立即跳登录且登录后回到 `/devices`；每个按钮点下去都有真实响应或明确禁用。

### 阶段 2 · 共享原语与组件库补齐（≈5–6 人日，阶段 3–5 的硬前置；可拆 3 条并行线）

目标：后面所有页面重做都建立在同一套原语上，避免再制造重复。**四个硬前置**——`useWs` 单连接、`Table` 行展开、`usePlayback` 抽取、i18n 键约定——跳过任何一个，阶段 4 都会返工。

**纯函数与枚举（新建 `utils/`，Nuxt 自动导入，目录当前不存在）**
- `utils/format.ts`：`fmtTime / ago / fmtBytes / fmtSchedule / DAY_NAMES / dash()`，替换 5+2+3 处重复（F4）；`dash()` 是空值 `—` 的唯一出口。
- `utils/enums.ts`：`SOURCE_MAP`（四协议 label / colorToken / tagColor，替换 `devices/index.vue:26`、`devices/[id].vue:22`、`index.vue:9`，F5）、`DEVICE_STATUS_MAP` / `STREAM_STATUS_MAP` / `TASK_STATUS_MAP`（F6）、`ALARM_KIND_MAP`（F4）。这些枚举同时是 i18n 键的来源。后端 source 字符串仍作字面判别值。

**composables**
- `useWs` 改为**单连接多订阅**（当前 `useWs.ts:2-44` 每个调用方各开一条 WebSocket，layout 已占一条；不先改，阶段 4 补订阅会让每页 2–3 条连接、`wshub` 广播倍增）。保留旧签名做适配层，验证连接数 = 1 后删。
- `usePlayback(channelId)`：从 `playback.vue` 抽出 `records/days` 查询、`POST playback` 会话、`PUT` seek/pause/speed、download——供阶段 4 预览弹窗复用（否则复制约 150 行会话逻辑）。必须用 simulator 实测 seek/pause/speed/download 四路径；失败则 `playback.vue` 保留原逻辑，弹窗回放 Tab 暂做跳转。
- `useTasks`：挂载时补水 `GET /tasks`；后端 `models.Task` 无 `title` 而前端 `TaskItem.title` 必填——前端按 `type` 映射标题（不改后端）。
- `useForm()` + `UiField`：字段级内联校验（E5），阶段 4/5 全部表单使用。

**ui 组件**
- `Table.vue` 加 **行展开插槽 + 列显隐**（设备列表重做的硬前置；当前 13 处调用，无 expand 时须零 DOM 变化）。
- 焦点环：`main.css` 新增 `--ring` token；所有写了 `outline-none` 的组件补 `focus-visible:ring-2 ring-primary/60`（B3）。
- `prefers-reduced-motion: reduce` 下关闭全部 `ipc-*` 动画（B1）。
- `UiIconButton`：强制 `aria-label` prop（B2）。
- `UiSkeleton`：表格行 / 卡片两种形态（E11）。
- `H265Player.vue`：解码核心按 PRD 顺序选择（WebCodec → WASM 多线程 → 单线程），码率/延时真正写入标题条（LIVE-01）。放这里而非阶段 5，因为 `live.vue` 与阶段 4 的预览弹窗共用它，先修省一轮回归。
- `Select` / `Switch` 补 `lg` 档（F3）。
- `UiEmptyState` 补 `help` 链接插槽，对齐 PRD §9.1"插画 + 主按钮 + 帮助链接"。
- 修 `w-55/w-58/h-13` → `w-[220px]/w-[232px]/h-[52px]`，删 inline style（D）。
- 修正 8 个组件头注释（F2）；删 `plugins/ui-alias.ts`（与 `nuxt.config.ts:14-17` 三重注册）、`ScheduleGrid.vue:58` 死代码、`H265Player.vue:132` console.log（F8）。先清理，否则新组件又要在 `ui-alias.ts` 再注册一次。
- i18n **键约定**（最贵的排序错误在这里）：隐藏用户菜单里的语言切换项（保留 composable 与 cookie）；`useI18n.ts` 拆为 `locales/zh-CN/*.ts` 按页分文件；键名 = `页面.区块.用途`，枚举文案键由 `utils/enums.ts` 派生；此后新增/重做页面文案一律 `t()`。

**并行拆分**：①`utils` + 枚举 + 组件焦点/尺寸/注释；②`useWs` + `useTasks` + `useApi.download` 适配；③`usePlayback` + `useForm/UiField` + `Table` 扩展。①②可与阶段 1 并行，③等阶段 1 结束（`playback.vue` 改动大）。

**验证**：`npm run build` 通过；现有页面无视觉变化；`playback.vue` 改用 `usePlayback` 后 seek/倍速/下载正常；浏览器 Network 面板 WS 连接数 = 1；Tab 键可在登录页、首页、设备列表全部控件间移动且有可见焦点；系统开启"减少动态效果"后弹层无动画。

### 阶段 3 · 壳体、可访问性、响应式（≈2.5 人日）

- `layouts/default.vue`：侧栏改 `<nav>` + `<NuxtLink>`（键盘可达、可右键新开）；三档宽度——≥1280 展开 220px / 768–1279 收窄为 56px 图标栏（hover 展开）/ <768 抽屉；顶栏图标按钮全部 `UiIconButton`；全局搜索支持 ↑↓ Enter 与点击外部关闭；删「返回首页」（F7）；「所有企业及项目」→「切换项目」；壳体文案全部 `t()`。
- `layouts/console.vue`、`auth.vue`：同样修无效类与 aria。
- `live.vue` / `playback.vue`：通道树可收起（按钮 + <1024 默认收起）；PTZ 面板在窄屏改底部弹出。
- `pages/index.vue`：删「功能入口」7 面板（F7）；用 DASH-01 六卡片替代（设备在线/播放路数/今日告警/节点健康/存储用量/录像计划数——存储用量走 `/storage/overview`）；告警流每行可点击进消息中心详情（DASH-02）；首屏 Skeleton；`loadNames` 改用 `/dashboard` 返回名称（若后端不带则加字段，小后端）。
- `pages/console.vue`：项目行改 `<button>`；表头统计并行请求。

**验证**：1280 / 1024 / 768 / 390 四档宽度下首页、设备列表、直播、回放不出现横向滚动且主操作可达；键盘走查全部导航；屏幕阅读器（NVDA/VoiceOver 任一）能读出顶栏每个按钮名称。

### 阶段 4 · 旗舰页重做：设备列表 + 添加设备 + 预览弹窗（≈7–8 人日）

严格按上一轮方案"设备管理"一节的规格，彻底去 TP-LINK 化。**顺序即风险控制**：先抽 `components/device/AddDeviceDialog.vue` 并在旧列表页接入、用 simulator 验证四协议添加都通；再重写表格主体。这样重做期间"添加设备"始终可用，旧页可整文件 checkout 回滚。`AddDeviceDialog` 与 `DevicePreviewModal` 两条线可并行，`index.vue` 主体等前者完成。

- **`pages/devices/index.vue` 重写**：左侧分组树复用 `UiTree`（真实计数；拖拽设备到分组调 `/groups/move-devices`，ACC-04）；工具栏 = 添加设备 / 待确认(国标) / 批量（转移 MGR-12、重启、删除、导出）/ 搜索 / **来源筛选**（MGR-02，`useSourceMeta`）/ 状态筛选；`UiTable` + `UiPagination`；IP/MAC/序列号等宽右对齐；状态列信号灯；**订阅 WS `device.online/offline` 实时更新**（E8）；删 IPC Tab 与折叠统计卡，改为表格上方一行统计文字；删全部"对齐图 4"注释。
- **`components/device/AddDeviceDialog.vue`（从页面抽出，按 PRD ADD-\* 重做）**：6 个假 Tab 收敛为真实入口——Tab = 设备 ID（IDP：ID + 验证码 + 分组）/ 国标（展示 `/projects/:id/gb28181/params` 接入参数 + 白名单管理 ADD-05）/ ONVIF（`/devices/onvif/discover` 发现列表 + 手动 `/devices/onvif`）/ RTSP（`/devices/rtsp`）/ 批量（CSV 模板下载 + 粘贴/上传 → `/devices/idp/preadd` → 结果表成功/失败 Tab + 重试失败项，ADD-03/08，PRD §9.1 批量结果约定）。
- **`components/DevicePreviewModal.vue` 重写**：预览 Tab 复用 `live.vue` 的起流/停流与主/子码流两档；回放 Tab 用 `usePlayback` 真实录像段 + `/records/days` 日期圆点 + 会话控制；抓拍调 `POST /channels/:id/snapshot`；删对讲、假 OSD。
- **`pages/devices/[id].vue`**：编辑校验（`UiField`）、诊断结果面板、WS 实时状态、升级入口灰显说明。

**验证**：用 simulator 四种协议各接入一台：IDP 走验证码绑定、国标走待确认、ONVIF 走发现、RTSP 走手动；列表实时显示上下线；预览弹窗能看实时画面并回放当天录像段；批量导入 CSV 得到成功/失败结果表。

### 阶段 5 · PRD 功能补齐（前端侧，≈5–6 人日）

- ALM-05 告警策略：在 `alarms/rules.vue` 顶部增加 Tab「项目策略」（不改侧栏 6 组 IA），读写 `/alarm-policies`。
- LIVE-06：直播工具条加暂停 / 音量（取消写死 muted）。
- REC-03：平台源截图用 `<canvas>` 抓 `<video>` 帧下载。
- REC-05：`ScheduleGrid` 粒度可选 30/15/5 分钟 + 复制到其他日 + 清空；≤7 段上限提示。
- REC-07：系统设置或节点页加存储概览卡（`/storage/overview`）。
- DASH-03 / ALM-10：首页来源分布加离线数（需 `settings.go:194` bySource 补离线分组，小后端）；7 天告警柱状图（需后端按天聚合接口，若不做则前端先用 `/alarms` 分页聚合并标注性能限制）。
- E7：角色/成员/节点/计划/模板列表接 `UiPagination`，去掉 `pageSize: 200`。
- E10：模板至少一段布防校验；角色至少一项权限校验；`scan.vue` 成功后重置。
- E11：`account.vue`、`wizard.vue` 补三态。
- **后端单列清单**（本计划不做，输出为后续计划的输入）：ACC-05 Scope 过滤、ALM-04 事件产生方、ADD-05 黑名单、ACC-07 WS 状态校验、ACC-01 锁定持久化与复杂度、MGR-10 OTA API、MGR-14 时序指标、ADD-04、ADD-10、REC-06 事件录像、ALM-08、LIVE-07 Focus、ALM-03 国标多通道归属、P-18 `taskProgress`、`rtsp.go:153` panic。

**验证**：ALM-05 关闭 motion 后 simulator 的 motion 告警不再落库；直播暂停/音量可用；录像模板可复制星期；各列表翻页正常。

### 阶段 6 · 文案审校与 i18n 全量接入（≈5–6 人日，可另行审批；i18n / 响应式收尾 / a11y 收尾三线并行，同文件不双开）

- 文案：按钮动词统一（「确认」并入「确定」，保存/创建/删除贯穿 toast）；空状态一律给下一步动作；错误提示三段式；删除全部"对齐图 N / 商云"注释。
- i18n：21 页文案全部接入 `t()`，补 EN 词条，恢复语言切换入口，ACC-09 完整验收。
- 收尾：全站残余 `<div @click>` → `<button>/<a>`；表格容器 `overflow-x-auto`；<768 工具条换行。

### 工程量汇总

| 阶段 | 前端 | 后端（本计划内） | 可并行 |
|---|---|---|---|
| 0 检查点 | 0.5 | — | — |
| 1 止血 | 3–4 | 1 | 前后端并行；前端可拆"设备相关 / 其它"两线 |
| 2 原语 | 5–6 | — | 三线（见阶段内说明） |
| 3 壳体 | 2.5–3 | — | 壳体 / 首页两线 |
| 4 旗舰页 | 7–8 | — | AddDeviceDialog / PreviewModal 并行，index.vue 主体串行 |
| 5 PRD 补齐 | 5–6 | 0.5（bySource 离线数、按天聚合） | 按页拆 |
| 6 收尾 | 5–6 | — | 三线 |
| **合计** | **≈29–34 人日** | **≈1.5 人日** | 3 条并行线约 3–4 周 |

单列的后端清单（ACC-05 Scope 过滤、ALM-04 产生方、ADD-05 黑名单、MGR-10 OTA API、`taskProgress` 等）另估约 9–11 人日，不在本计划内。

### 风险与回滚点

1. **设备列表重做期间添加设备可用**：先抽 `AddDeviceDialog` 在旧页验证，再重写表格；每阶段独立提交，旧页整文件 checkout 回滚。
2. **空字段约定**：统一 `dash()` 输出 `—`；禁止示例值 / `'---'` / `'默认'` 兜底；分组缺失显示「未分组」。
3. **`usePlayback` 抽取失败**：`playback.vue` 保留原逻辑，弹窗回放 Tab 暂做跳转回放页。
4. **`useWs` 单连接改坏影响全站徽标**：保留旧签名做适配层，Network 面板验证连接数 = 1 后再删。
5. **`Table.vue` 扩展影响 13 处调用**：无 expand 时须零 DOM 变化，逐页目测。
6. **i18n 键约定未先定**：阶段 4 新页会在阶段 6 全量返工，故键约定放阶段 2 并作为阶段 4 代码评审项。

---

## 三、验证方式（贯穿）

`platform/` 无自动化测试，按 `platform/CLAUDE.md` 约定实际运行验证：

1. `cd platform && docker compose up -d --build`（或本地 `go run ./cmd/ipccloud` + `npm run dev`），配合 `simulator` 模拟四协议设备。
2. 每阶段末：`npm run build` 与 `go build ./... && go test ./...` 通过；浏览器逐页走查该阶段涉及页面并截图；键盘 Tab 走查；1280/1024/768 三档宽度检查。
3. 阶段 1 与阶段 4 额外做端到端：添加设备 → 列表 → 预览 → 回放 → 诊断 → 导出 → 删除（输名确认）。
4. 每阶段汇报：完成项 / 未完成项及原因 / 截图 / 发现的新问题，再进入下一阶段。

## 四、关键文件

- 令牌与全局样式：`platform/web/assets/css/main.css`
- 壳体：`platform/web/layouts/{default,console,auth}.vue`；新增 `platform/web/middleware/auth.global.ts`
- 组件库：`platform/web/components/ui/*`（重点 `Button.vue`、`Select.vue`、`Switch.vue`、`Table.vue`、`EmptyState.vue`；新增 `IconButton.vue`、`Field.vue`、`Skeleton.vue`）
- 新建 `platform/web/utils/format.ts`、`utils/enums.ts`（Nuxt 自动导入）；新增 composables `usePlayback.ts`、`useForm.ts`；重构 `useWs.ts`（单连接）、`useTasks.ts`（补水）、`useApi.ts`（`download()`）、`useAuth.ts`（token getter）；拆分 `useI18n.ts` → `locales/`
- 重写：`pages/devices/index.vue`、`components/DevicePreviewModal.vue`；新增 `components/device/AddDeviceDialog.vue`
- 规划代理的完整排序依据：`C:\Users\user\.claude\plans\using-superpowers-serialized-hare-agent-aroadmap-planner-a8b669dc46876cd9.md`
- 修改：`pages/login.vue`、`pages/index.vue`、`pages/console.vue`、`pages/devices/[id].vue`、`pages/live.vue`、`pages/playback.vue`、`pages/alarms/rules.vue`、`pages/system/{nodes,projects,settings,audit}.vue`、`pages/scan.vue`
- 小后端：`server/internal/api/{devices,nodes,audit}.go`、`server/internal/media/scheduler.go`
- 现有可复用：`composables/useToast.ts` 的 `toastApiError`、`components/ui/ErrorCard.vue`、`components/ui/Tree.vue`、`components/ui/Pagination.vue`、`components/ScheduleGrid.vue`
