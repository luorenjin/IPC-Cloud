<script setup lang="ts">
// 设备详情（MGR-03）：概览 / 通道 / 配置 / 诊断 / 日志（PRD 五 Tab）
import ConfigFieldRow, { type CfgField } from '~/components/device/ConfigFieldRow.vue'
import NetworkSettings from '~/components/device/NetworkSettings.vue'
import MotionRegionEditor from '~/components/device/MotionRegionEditor.vue'
import { onBeforeRouteLeave } from 'vue-router'

const route = useRoute()
const api = useApi()
const toast = useToast()
// confirmBox 在顶部声明：配置面板的「放弃修改」与「离开确认」也要用它
const confirmBox = useConfirm()
const { t } = useI18n()
const devId = String(route.params.id)

const dev = ref<any>(null)
const loading = ref(false)
const tab = ref('overview')

const channels = computed(() => dev.value?.channels || [])
const metrics = computed(() => dev.value?.metrics || null)
const caps = computed<string[]>(() =>
  (dev.value?.capabilities || dev.value?.caps || [])
    .map((c: any) => (typeof c === 'string' ? c : c.name || c.key || ''))
    .filter(Boolean))
function hasCap(prefix: string) {
  return caps.value.some((c) => c.startsWith(prefix))
}

// 能力标签：按接入规范 §3.4 的顺序排列并中文化（未收录的标识原样显示，见 utils/enums.ts）。
// 原来把后端原始键直接铺在中文界面上（record.device.query 等），既看不懂，
// 也因为后端数组顺序不稳定而看不清「这台设备比那台少了什么」。
const capItems = computed(() => sortCapabilities(caps.value).map((raw) => ({
  raw,
  labelKey: capabilityKey(raw),
  known: !!CAPABILITY_MAP[raw]
})))

// 注意：后端 handleRebootDevice 目前对 idp 来源不做能力清单校验（有适配器就转发 cmd.reboot），
// 这里的 idp 分支判断是前端单独收紧的保守展示，不是与后端对称的双向契约；
// 真正的硬拒绝只有 rtsp（适配器 Reboot() 直接返回 EForbid）。后端若日后补上 idp 侧能力校验，
// 需要同步检查这里的判断是否还合理（AGENTS.md 双向契约一致性）。
const canReboot = computed(() => {
  const src = dev.value?.source
  if (!src) return false
  if (src === 'rtsp') return false
  if (src === 'idp' && caps.value.length > 0) return caps.value.includes('reboot')
  return true
})

// 来源展示名/颜色见 utils/enums.ts SOURCE_MAP（全站唯一来源，PRD §9.1）
// 设备/推流状态映射见 utils/enums.ts
const statusInfo = (s?: string) => deviceStatusInfo(s)
// 通道流状态字段名是 streamState（models.Channel），不是 streamStatus/status
const streamInfo = (ch: any) => streamStatusInfo(ch.streamState ?? ch.streamStatus ?? ch.status ?? '')
// 相对时间/百分比/温度格式化见 utils/format.ts
const pct = (v: any) => fmtPercent(v)
const temp = (v: any) => fmtTemp(v)

// 概览字段分两块：
// ① 基本信息：**恒定 6 项**（md 三列恰好两整行），空值占位为 —。字段数固定，
//    不同设备之间网格行数不跳动，也不会出现「一行只剩两个格子」的参差边框。
//    旧实现把空值整行删掉（审计 B7），字段数一变（IDP 无 IP/MAC、GB 无固件）就差分线。
// ② 接入与网络（PRD MGR-03 要求概览含「网络」）：四项**全空则整块不渲染**，
//    避免出现一整块全是 — 的空壳；块内空值仍占位，保持列对齐。
const coreRows = computed(() => {
  const d = dev.value || {}
  const last = d.lastSeenAt || d.lastOnline
  return [
    { key: 'id', label: t('device.detail.deviceId'), value: d.id },
    { key: 'model', label: t('device.detail.model'), value: d.model },
    { key: 'vendor', label: t('device.detail.vendor'), value: d.vendor },
    // 后端 deviceJSON 的键名是 fw（不是 firmware）
    { key: 'fw', label: t('device.detail.firmware'), value: d.fw || d.firmware },
    // hw 是设备 hello 自报值，芯片选型未冻结（《决策记录与待定事项》§2.3）→ 挂说明
    { key: 'hw', label: t('device.detail.hardware'), value: d.hw, hintKey: 'device.detail.hardwareHint' },
    { key: 'lastSeen', label: t('device.detail.lastOnline'), value: last ? ago(last, t) : '' }
  ]
})
const netRows = computed(() => {
  const d = dev.value || {}
  const rows = [
    { key: 'ip', label: 'IP', value: d.ip },
    { key: 'mac', label: 'MAC', value: d.mac },
    { key: 'location', label: t('device.detail.location'), value: d.location },
    { key: 'remark', label: t('common.remark'), value: d.remark }
  ]
  const hasAny = rows.some((r) => r.value !== undefined && r.value !== null && r.value !== '')
  if (!hasAny) return []
  // 补足到 3 的整数倍：栅格用 gap-px + 容器底色画分隔线（line-soft），
  // 缺格时底色会露出成一块灰块；补位格在单列（移动端）下隐藏，不然会多出空行。
  const need = (3 - (rows.length % 3)) % 3
  return [
    ...rows,
    ...Array.from({ length: need }, (_, i) => ({ key: 'filler' + i, label: '', value: '', filler: true }))
  ]
})

// ================= 诊断（MGR-07：POST /devices/:id/diag 返回 { results, at }；结果由后端保留 7 天） =================
const diag = reactive({ loading: false, ran: false, at: 0, items: [] as any[] })
async function runDiag() {
  diag.loading = true
  diag.items = []
  try {
    const res: any = await api.post(`/devices/${devId}/diag`)
    diag.items = res?.results || []
    diag.at = res?.at || Date.now()
    diag.ran = true
  } catch (e: any) {
    toastApiError(e, t('device.msg.diagFailed'))
  } finally {
    diag.loading = false
  }
}
// 诊断项状态灯颜色映射：成功=绿点/失败=红点/结果未知（理论上不会出现，容错兜底）=脉冲琥珀
function diagDotClass(it: any) {
  if (it.ok === true) return 'bg-success'
  if (it.ok === false) return 'bg-danger'
  return 'bg-warning animate-pulse'
}

// ================= 健康仪表条（概览 Tab 顶部三段式，纯展示计算属性） =================
const barDot: Record<string, string> = { success: 'bg-success', warning: 'bg-warning', danger: 'bg-danger', info: 'bg-info' }
const barText: Record<string, string> = { success: 'text-success', warning: 'text-warning', danger: 'text-danger', info: 'text-info' }
/** 健康条单段。onClick 存在时该段整体是一个按钮（目前只有「最近诊断结果」需要可点） */
interface HealthSeg {
  key: string
  icon: string
  titleKey: string
  color: string
  value: string
  sub: string
  /** 由 healthSegs 统一 t() 后的展示标题 */
  title?: string
  pulse?: boolean
  onClick?: () => void
  /** 可点段的 aria 文案键（运行 / 查看随当前是否已有结果而变化） */
  ariaKey?: string
}

/**
 * 在线时长段的标题随状态切换：在线且设备上报 metrics.uptime（秒，IDP 侧真实运行时长）时才叫「在线时长」，
 * 否则叫「最后在线」——旧实现是「标题恒为在线时长 + 离线时填先后心跳时间」，
 * 读出来是「在线时长 3 小时前」，语义不成立。非 IDP 协议不上报 uptime，就如实显示 —，不编造时长。
 */
const uptimeSeg = computed(() => {
  const d = dev.value || {}
  const st = statusInfo(d.status)
  const statusText = t('device.detail.currentStatus', { status: t(st.labelKey) })
  const up = metrics.value?.uptime
  if (d.status === 'online') {
    return up != null
      ? { color: st.color, titleKey: 'device.detail.uptime', value: fmtDuration(up), sub: statusText }
      : { color: st.color, titleKey: 'device.detail.uptime', value: EMPTY, sub: t('device.detail.uptimeUnknown') }
  }
  const last = d.lastSeenAt || d.lastOnline
  return { color: st.color, titleKey: 'device.detail.lastOnline', value: last ? ago(last, t) : EMPTY, sub: statusText }
})
const streamSeg = computed(() => {
  const chs = channels.value
  const total = chs.length
  const live = chs.filter((ch: any) => streamInfo(ch).color === 'success').length
  const color = !total ? 'info' : live === total ? 'success' : live > 0 ? 'warning' : 'info'
  return {
    color,
    value: total ? t('device.detail.streamCount', { live, total }) : t('device.detail.noChannel'),
    sub: total ? t('device.detail.streamingCount') : t('device.detail.noChannelReported')
  }
})
const diagSeg = computed(() => {
  // 结果可能是后端保留的旧记录（7 天窗口内），必须带出「什么时候跑的」：
  // 不带时间的话，6 天前的结果和刚刚跑完的结果长得一模一样。
  const ranAt = diag.at ? ago(diag.at, t) : EMPTY
  if (diag.loading) return { color: 'warning', value: t('device.diag.running'), sub: t('device.diag.probing'), pulse: true }
  if (!diag.ran) return { color: 'info', value: t('device.diag.notRun'), sub: t('device.diag.notRunSub') }
  if (!diag.items.length) return { color: 'info', value: t('device.diag.noResult'), sub: t('device.diag.noResultSub', { at: ranAt }) }
  const failCount = diag.items.filter((it: any) => it.ok === false).length
  return failCount
    ? { color: 'danger', value: t('device.diag.failCount', { n: failCount }), sub: t('device.diag.totalCount', { n: diag.items.length, at: ranAt }) }
    : { color: 'success', value: t('device.diag.allOk'), sub: t('device.diag.allOkSub', { n: diag.items.length, at: ranAt }) }
})
const healthSegs = computed<HealthSeg[]>(() => ([
  { key: 'uptime', icon: 'clock', ...uptimeSeg.value },
  { key: 'stream', icon: 'video', titleKey: 'device.detail.streamState', ...streamSeg.value },
  {
    key: 'diag', icon: 'activity', titleKey: 'device.detail.lastDiag', ...diagSeg.value, onClick: goDiag,
    // 已有结果时点击只跳转不再重跑（重跑是一次设备往返），aria 文案要跟着变
    ariaKey: diag.ran ? 'device.detail.viewDiagAria' : 'device.detail.runDiagAria'
  }
]).map((s) => ({ ...s, title: t(s.titleKey) })))

// 「最近诊断结果」段本身就是入口：点一下切到诊断 Tab 并直接开跑（已跑过/正在跑不重复触发）。
// 原来只写了「前往「诊断」页运行一键检测」却不可点，概览页最该提供的动作反而要用户自己找 Tab。
function goDiag() {
  tab.value = 'diag'
  if (!diag.loading && !diag.ran) runDiag()
}
function goLogs() {
  tab.value = 'logs'
}

// ================= 运行指标（status.metrics，PRD MGR-03） =================
// 只列有值的项，宽度用 flex 自适应；旧实现是固定 grid-cols-5 + v-show 隐藏空值，
// 设备只上报 3 项（IDP 模拟器：cpu/mem/temp）时右侧会留两个空槽，看上去像渲染坏了。
const metricItems = computed(() => {
  const m = metrics.value
  if (!m) return []
  const tf = m.tfHealth || (m.tfTotal ? `${m.tfUsed || 0}/${m.tfTotal} GB` : '')
  return [
    { key: 'cpu', label: 'CPU', value: pct(m.cpu) },
    { key: 'mem', label: t('device.detail.memory'), value: pct(m.memory ?? m.mem) },
    { key: 'temp', label: t('device.detail.temperature'), value: temp(m.temperature ?? m.temp) },
    { key: 'tf', label: t('device.detail.tfCard'), value: tf },
    { key: 'bitrate', label: t('device.detail.bitrate'), value: m.bitrate ? m.bitrate + ' kbps' : '' }
  ].filter((x) => x.value && x.value !== EMPTY)
})

// 指标刷新：IDP 每 30s 上报一次 status.report（simulator/idp/device.go 的 reportLoop），
// 因此概览停留期间自动跟进一次 + 提供手动刷新；两者都走 GET /devices/:id（metrics 同包返回），
// 不新增端点。不用 load()，是因为它会把整页塞进 UiLoading 转一圈。
const metricState = reactive({ busy: false, at: 0 })
async function refreshMetrics() {
  if (metricState.busy) return
  metricState.busy = true
  try {
    const res: any = await api.get(`/devices/${devId}`)
    if (!dev.value) return
    // 只合并概览相关的三个字段，不用整个响应覆盖 dev（避免把 channels 等一并替换掉）
    if (res?.metrics) dev.value.metrics = res.metrics
    if (res?.status) dev.value.status = res.status
    if (res?.lastSeenAt) dev.value.lastSeenAt = res.lastSeenAt
    metricState.at = Date.now()
  } catch {
    // 自动刷新失败不弹错：设备短暂离线是常态，WS 会推在线态，手动刷新按钮仍在
  } finally {
    metricState.busy = false
  }
}
let metricTimer: ReturnType<typeof setInterval> | null = null
function syncMetricTimer() {
  // 只在概览 Tab 停留且设备在线时轮询，离开/离线立即停，不在后台白打接口
  const shouldRun = tab.value === 'overview' && dev.value?.status === 'online'
  if (shouldRun && !metricTimer) metricTimer = setInterval(refreshMetrics, 30_000)
  if (!shouldRun && metricTimer) { clearInterval(metricTimer); metricTimer = null }
}
watch([tab, () => dev.value?.status], syncMetricTimer)
onUnmounted(() => {
  if (metricTimer) clearInterval(metricTimer)
  if (rereadTimer) clearTimeout(rereadTimer)
})

// ================= 通道操作 =================
async function toggleCh(ch: any, val: any) {
  try {
    await api.put(`/channels/${ch.id}`, { enabled: !!val })
    ch.enabled = !!val
    toast.success(t('device.msg.updatedOk'))
  } catch (e: any) {
    toastApiError(e, t('device.msg.setFailed'))
  }
}
async function refreshCover(ch: any) {
  try {
    await api.post(`/channels/${ch.id}/cover`)
    toast.success(t('device.msg.coverRefreshSubmitted'))
  } catch (e: any) {
    toastApiError(e, t('device.msg.opFailed'))
  }
}
const snapDlg = reactive({ show: false, src: '' })
async function takeSnap(ch: any) {
  try {
    const res: any = await api.post(`/channels/${ch.id}/snapshot`)
    snapDlg.src = res.url || res.snapshot || res.dataUrl || (typeof res === 'string' ? res : '')
    snapDlg.show = true
  } catch (e: any) {
    toastApiError(e, t('device.msg.snapFailed'))
  }
}

// ================= 远程配置（MGR-09） =================
// 字段声明对齐固件 `firmware/core/src/config.c` 的规则表：键名 / 类型 / 取值范围三处一致。
// CfgField 的类型定义与「一行字段怎么渲染」都在 components/device/ConfigFieldRow.vue 里，
// 本页只负责声明数据（cfgGroups）与持有状态（cfg）：控件类型由 type 决定，
// 不再像更早的实现那样把 image.mirror 这类 0/1 开关渲染成自由文本。
// 编码键带通道号：video.<ch>.<name>.<field>，0/main = 主码流。
interface CfgGroup { key: string; tab: CfgTabKey; titleKey: string; fields: CfgField[] }

// rebootRequired：固件规则表 reboot_required=true 键的静态镜像（服务端已按 supported 过滤），
// 不是「改了就必须重启才生效」的实时判定——纯粹用来在对应字段旁挂一个「需重启生效」提示。
//
// snapshot：上一次成功回读的配置快照，用来判定「有没有未保存的修改」。
// phase：保存→回读的完整过程，把原本静默的 5 秒回读变成可见状态。
// 初始 snapshot 取 '{}' 而不是空串：cfg.data 初值就是 {}，否则配置还没拉回来就显示「有未保存的修改」。
const cfg = reactive({
  loading: false,
  phase: 'idle' as 'idle' | 'sending' | 'rereading',
  snapshot: '{}',
  /** 与 snapshot 同源的基准值对象：分区块判断“本区块有没有未保存修改”时要按键取值 */
  snapshotData: {} as Record<string, any>,
  data: {} as any,
  denied: [] as string[],
  supported: [] as string[],
  rebootRequired: [] as string[]
})
/** 保存→回读期间页面上的写操作一律锁住，避免“下发中又改一笔”造成的价值混淆 */
const cfgSaving = computed(() => cfg.phase !== 'idle')
/**
 * 是否有未保存的修改。用 JSON 快照字符串比较，而不是逐字段 diff：
 * loadCfg 整体替换 cfg.data、编辑只改已有键的值，键序是稳定的；
 * 用户把值改回原样也会自然回到“不脏”，不需要额外处理。
 */
const isCfgDirty = computed(() => JSON.stringify(cfg.data) !== cfg.snapshot)
/**
 * 更新基准值。所有改动基准的地方都必须走这里，
 * 保证 snapshot（字符串，用于整体脏值比较）与 snapshotData（对象，用于分区块比较）永远一致。
 */
function setCfgBaseline(data: Record<string, any>) {
  cfg.snapshotData = { ...data }
  cfg.snapshot = JSON.stringify(cfg.snapshotData)
}
/**
 * 下发成功后刷新基准，但只写回这次真正参与下发的键：
 * 网络键走独立保存与强确认，若在这里一并写回基准，用户会以为网络修改也已经生效。
 */
function resetBaselineAfterPush(pushedKeys: string[]) {
  const snap: Record<string, any> = { ...cfg.snapshotData }
  for (const k of pushedKeys) snap[k] = cfg.data[k]
  setCfgBaseline(snap)
}
/** 自动回读定时器：离开页面要清掉，否则会在已卸载的组件上跑 loadCfg */
let rereadTimer: ReturnType<typeof setTimeout> | null = null

/** 画面调节四项（固件 0–100 整数），滑杆与数字框联动 */
const IMAGE_SLIDERS = [
  { key: 'image.brightness', labelKey: 'device.config.brightness', min: 0, max: 100 },
  { key: 'image.contrast', labelKey: 'device.config.contrast', min: 0, max: 100 },
  { key: 'image.saturation', labelKey: 'device.config.saturation', min: 0, max: 100 },
  { key: 'image.sharpness', labelKey: 'device.config.sharpness', min: 0, max: 100 }
]

// 子页签：22 个键按语义拆成 7 组，每个分组名直接对应它管的内容——
// OSD 叠加与移动侦测不再共用「事件侦测」这个筐，时间同步也不再挂在录像分组下面。
type CfgTabKey = 'image' | 'encode' | 'osd' | 'alarm' | 'record' | 'time' | 'maintain'
const CFG_TAB_KEYS: CfgTabKey[] = ['image', 'encode', 'osd', 'alarm', 'record', 'time', 'maintain']
function isCfgTabKey(v: unknown): v is CfgTabKey {
  return typeof v === 'string' && (CFG_TAB_KEYS as string[]).includes(v)
}
// 子页签挂进 URL query：把当前子页签的链接发给同事复核时，对方打开要落在同一分组，
// 不然每次都从「画面信息」开始翻，等于链接没带上关键信息。
const cfgTab = ref<CfgTabKey>(isCfgTabKey(route.query.tab) ? route.query.tab : 'image')
// 逐条写 t('…') 而不是拼字符串：i18n 检查脚本只能静态识别字面量，
// 拼出来的键会被当成「定义了但未引用」。
const cfgTabItems = computed(() => [
  { label: t('device.config.tab.image'), value: 'image' },
  { label: t('device.config.tab.encode'), value: 'encode' },
  { label: t('device.config.tab.osd'), value: 'osd' },
  { label: t('device.config.tab.alarm'), value: 'alarm' },
  { label: t('device.config.tab.record'), value: 'record' },
  { label: t('device.config.tab.time'), value: 'time' },
  { label: t('device.config.tab.maintain'), value: 'maintain' }
] as { label: string; value: CfgTabKey }[])

// 画面镜像在固件里是 image.mirror / image.flip 两个独立的 0/1 键。
// 面板按惯例合成一个四选一下拉（与厂商面板一致），保存时再拆回两个键。
const MIRROR_OPTIONS = [
  { value: 'none', labelKey: 'device.config.mirrorNone' },
  { value: 'mirror', labelKey: 'device.config.mirrorH' },
  { value: 'flip', labelKey: 'device.config.mirrorV' },
  { value: 'both', labelKey: 'device.config.mirrorHV' }
]
function cfgNum(k: string) {
  const n = Number(cfg.data?.[k])
  return Number.isFinite(n) ? n : 0
}
/**
 * 输入框回显值：原样返回，不做数值归一化。
 * 与 cfgNum() 的区别是「用户还没改完的值也要照原样显示」——归一化会把清空的数字框立刻变成 0，
 * 用户既清不掉、也看不出自己刚才填的是什么。
 */
function cfgDisplay(k: string) {
  const raw = cfg.data?.[k]
  return raw === undefined || raw === null ? '' : String(raw)
}
const mirrorMode = computed({
  get() {
    const m = cfgNum('image.mirror') === 1
    const f = cfgNum('image.flip') === 1
    return m && f ? 'both' : m ? 'mirror' : f ? 'flip' : 'none'
  },
  set(v) {
    cfg.data['image.mirror'] = v === 'mirror' || v === 'both' ? 1 : 0
    cfg.data['image.flip'] = v === 'flip' || v === 'both' ? 1 : 0
  }
})
const mirrorOptions = computed(() => MIRROR_OPTIONS.map((o) => ({ label: t(o.labelKey), value: o.value })))

/**
 * 亮度/对比度/饱和度 0–100 映射到 CSS filter 系数：中点 50 落在 1（视觉中性，即“不叠加任何效果”），
 * 两端线性伸展到 0.5/1.5——只为了让预览图跟着滑杆方向变化，不追求还原设备编码器的实际曲线。
 * 锐度没有对应的原生 CSS 效果，不在此列，页面上改用文字提示代替。
 */
function imageFilterValue(v: number) {
  return 0.5 + v / 100
}
/** 预览层实时叠加：滑杆/镜像下拉一改就生效，抓帧本身仍要靠「刷新预览」手动取新的设备帧 */
const previewStyle = computed(() => {
  const scaleX = mirrorMode.value === 'mirror' || mirrorMode.value === 'both' ? -1 : 1
  const scaleY = mirrorMode.value === 'flip' || mirrorMode.value === 'both' ? -1 : 1
  return {
    filter: `brightness(${imageFilterValue(cfgNum('image.brightness'))}) contrast(${imageFilterValue(cfgNum('image.contrast'))}) saturate(${imageFilterValue(cfgNum('image.saturation'))})`,
    transform: `scaleX(${scaleX}) scaleY(${scaleY})`
  }
})

/**
 * 数字框回写：只挡空值与非数字，**不静默钳制**。
 * 旧实现把越界值直接改成边界值，用户以为改成了 300、实际下发 100；
 * 现在越界由 invalidKeysByTab 就地报错并按页签拦住保存。
 */
function setCfgNum(key: string, v: string) {
  const n = Number(v)
  cfg.data[key] = v.trim() === '' || !Number.isFinite(n) ? v : n
}

/** 字段控件回写：数字字段走 setCfgNum（保留空值/非数字的半成品输入），其余按原值写入 */
function onCfgFieldInput(f: CfgField, v: any) {
  if (f.type === 'int') setCfgNum(f.key, String(v))
  else cfg.data[f.key] = v
}

/** 数值字段合法区间查找表（画面四项 + 各分组 int 字段）：就地提示与提交前拦截共用，本身是静态表，不需要按 tab 拆 */
const intBounds = computed(() => {
  const m: Record<string, { min: number; max: number }> = {}
  for (const f of IMAGE_SLIDERS) m[f.key] = { min: f.min, max: f.max }
  for (const g of cfgGroups) for (const f of g.fields) if (f.type === 'int') m[f.key] = { min: f.min, max: f.max }
  return m
})

/** 单个键是否越界：非空且不是有限数字、或超出区间 */
function isOutOfRange(k: string) {
  const b = intBounds.value[k]
  if (!b) return false
  const raw = cfg.data?.[k]
  if (raw === undefined || raw === null || raw === '') return false
  const n = Number(raw)
  return !Number.isFinite(n) || n < b.min || n > b.max
}

/**
 * 越界键按子页签分桶：保存按钮的禁用条件、就地报错与导航项红点都只认当前 cfgTab 这一份，
 * 不让 A 页签的越界值隔空拦住 B 页签的保存——这正是重分组前的问题④。
 */
const invalidKeysByTab = computed(() => {
  const m: Record<CfgTabKey, string[]> = { image: [], encode: [], osd: [], alarm: [], record: [], time: [], maintain: [] }
  m.image = IMAGE_SLIDERS.filter((f) => isOutOfRange(f.key)).map((f) => f.key)
  for (const g of cfgGroups) {
    // 被依赖禁用的字段不参与越界计算：禁用后用户看不到它的越界提示（控件已灰），
    // 若仍拦住保存，就成了「看不见的红点 + 保存变灰」——与重分组前那个跨页签隔空拦截同一性质。
    const bad = g.fields
      .filter((f) => f.type === 'int' && !cfgFieldDisabledReason(f) && isOutOfRange(f.key))
      .map((f) => f.key)
    if (bad.length) m[g.tab] = [...m[g.tab], ...bad]
  }
  return m
})

const boundText = (k: string) => {
  const b = intBounds.value[k]
  return b ? `${b.min}–${b.max}` : ''
}

// 三组「总开关 + 下游字段」的依赖声明。抽成常量是为了让同一组的下游字段共用一份定义，
// 避免三处各写一遍 hintKey 而出现文案不一致。
const MOTION_DEPENDS = { key: 'alarm.motion.enable', equals: true, hintKey: 'device.config.dependsMotion' }
const RECORD_DEPENDS = { key: 'record.enabled', equals: true, hintKey: 'device.config.dependsRecord' }
const NTP_DEPENDS = { key: 'time.ntp.enable', equals: true, hintKey: 'device.config.dependsNtp' }

// 各分组：键名与固件规则表对齐，控件类型与依赖关系由 CfgField 声明，渲染统一交给 ConfigFieldRow。
const cfgGroups: CfgGroup[] = [
  {
    key: 'video', tab: 'encode', titleKey: 'device.config.group.video',
    fields: [
      { key: 'video.0.main.codec', labelKey: 'device.config.encode', type: 'enum', options: [{ label: 'H.265', value: 'h265' }, { label: 'H.264', value: 'h264' }, { label: 'MJPEG', value: 'mjpeg' }] },
      { key: 'video.0.main.w', labelKey: 'device.config.width', type: 'int', min: 64, max: 1920 },
      { key: 'video.0.main.h', labelKey: 'device.config.height', type: 'int', min: 64, max: 1080 },
      { key: 'video.0.main.fps', labelKey: 'device.config.fps', type: 'int', min: 1, max: 30 },
      { key: 'video.0.main.kbps', labelKey: 'device.config.bitrate', type: 'int', min: 32, max: 16384 },
      { key: 'video.0.main.gop', labelKey: 'device.config.gop', type: 'int', min: 1, max: 300 },
      { key: 'video.0.main.rc', labelKey: 'device.config.rc', type: 'enum', options: [{ label: 'CBR', value: 'cbr' }, { label: 'VBR', value: 'vbr' }] }
    ]
  },
  {
    key: 'osd', tab: 'osd', titleKey: 'device.config.group.osd',
    fields: [
      { key: 'osd.channelName.enable', labelKey: 'device.config.osdName', type: 'bool' },
      { key: 'osd.time.enable', labelKey: 'device.config.osdTime', type: 'bool' }
    ]
  },
  {
    key: 'record', tab: 'record', titleKey: 'device.config.group.record',
    fields: [
      { key: 'record.enabled', labelKey: 'device.config.recordEnable', type: 'bool' },
      { key: 'record.mode', labelKey: 'device.config.recordMode', type: 'enum', dependsOn: RECORD_DEPENDS, options: [
        { label: t('device.config.rec_continuous'), value: 'continuous' },
        { label: t('device.config.rec_event'), value: 'event' },
        { label: t('device.config.rec_schedule'), value: 'schedule' }
      ] },
      { key: 'record.retention_days', labelKey: 'device.config.retention', type: 'int', min: 1, max: 365, dependsOn: RECORD_DEPENDS },
      { key: 'record.channel', labelKey: 'device.config.recordChannel', type: 'int', min: 0, max: 2, dependsOn: RECORD_DEPENDS }
    ]
  },
  {
    key: 'alarm', tab: 'alarm', titleKey: 'device.config.group.alarm',
    fields: [
      { key: 'alarm.motion.enable', labelKey: 'device.config.motionEnable', type: 'bool' },
      { key: 'alarm.motion.sensitivity', labelKey: 'device.config.motionSens', type: 'int', min: 0, max: 100, dependsOn: MOTION_DEPENDS }
    ]
  },
  {
    key: 'time', tab: 'time', titleKey: 'device.config.group.time',
    fields: [
      { key: 'time.ntp.enable', labelKey: 'device.config.ntpEnable', type: 'bool' },
      // 关掉 NTP 后服务器地址写了也不会被使用：禁用而非隐藏，让用户仍能看到当前配置
      { key: 'time.ntp.server', labelKey: 'device.config.ntp', type: 'str', dependsOn: NTP_DEPENDS },
      // 时区与 net.dhcp/net.ip 不同：改动不涉及断网风险，走通用可编辑渲染即可，
      // 不需要 net.* 那种只读 + 强确认处理（见本文件下方 time 页签的只读网络区块）。
      // 类型是 tz 而非 str：自由文本写错时区不会报错，只会静默生效成错的时区。
      { key: 'time.timezone', labelKey: 'device.config.timezone', type: 'tz' }
    ]
  },
  {
    // localUser.name / led.enable 说明书要求放"设备维护"页签，但该页签本身是手写模板
    // （重启入口 + 定时重启计划），不走 cfgGroups 通用渲染——这里新增两个独立分组，
    // 让这两个字段仍然走 CfgField 通用渲染机制，而不是在模板里手搓一遍 UI。
    key: 'localUser', tab: 'maintain', titleKey: 'device.config.group.localUser',
    fields: [
      // 行标签用「账户名」而不是「本地账户」：区块标题已经说了是哪个账户，
      // 两处同名只是把同一句话说两遍（分区后标题与行标签必然贴合，得有一方更具体）。
      { key: 'localUser.name', labelKey: 'device.config.localUser', type: 'str' }
    ]
  },
  {
    // 与账户分开：一个是身份凭据，一个是设备外观行为，放同一个区块等于没有分组。
    key: 'led', tab: 'maintain', titleKey: 'device.config.group.led',
    fields: [
      { key: 'led.enable', labelKey: 'device.config.led', type: 'bool' }
    ]
  }
]

/**
 * 字段当前是否因上游总开关而不可用；返回禁用原因文案（空串=可用）。
 * 设备侧对这些键照常保存，只是主开关关闭时不会生效，所以这里只禁用交互、不清空值、不隐藏字段。
 */
function cfgFieldDisabledReason(f: CfgField): string {
  const dep = f.dependsOn
  if (!dep) return ''
  return cfg.data?.[dep.key] === dep.equals ? '' : t(dep.hintKey)
}

/** gop / rc 改动频率低，收进「编码策略」页签的折叠区，降低主网格的字段密度 */
const ADVANCED_VIDEO_KEYS = ['video.0.main.gop', 'video.0.main.rc']
/** 画质档位代管的三个字段：与 supported 过滤后的 g.fields 对照，判断 chips 是否还有意义 */
const ENCODE_PRESET_GOVERNED_KEYS = ['video.0.main.fps', 'video.0.main.kbps', 'video.0.main.gop']

interface EncodePreset { key: string; label: string; fps: number; kbps: number; gop: number }
// 逐条写 t('…') 而不是拼字符串：与 cfgTabItems 同一顾虑，i18n 检查脚本只识别字面量。
// 四档数值是纯前端预设，不新增后端字段；gop 大致取 fps 的 2 倍（≈2 秒一个关键帧），
// 流畅档降帧率与码率以适配弱网，其余三档对齐常见摄像头面板的标清/高清/超清档位。
const encodePresets = computed<EncodePreset[]>(() => [
  { key: 'fluent', label: t('device.config.presetFluent'), fps: 15, kbps: 512, gop: 30 },
  { key: 'sd', label: t('device.config.presetSd'), fps: 25, kbps: 1024, gop: 50 },
  { key: 'hd', label: t('device.config.presetHd'), fps: 25, kbps: 2048, gop: 50 },
  { key: 'uhd', label: t('device.config.presetUhd'), fps: 30, kbps: 4096, gop: 60 }
])
/** 代管键 → 档位字段的映射：高亮判定与实际写入共用一份，避免两处各写一遍而跑偏 */
const GOVERNED_FIELD: Record<string, keyof EncodePreset> = {
  'video.0.main.fps': 'fps',
  'video.0.main.kbps': 'kbps',
  'video.0.main.gop': 'gop'
}
/**
 * 只拿设备确实支持的键来比对：设备没有 gop 时它的读回值是 0，
 * 把它算进去会让任何档位都匹配不上，芯片永远显示「自定义」。
 */
const governedKeys = computed(() => ENCODE_PRESET_GOVERNED_KEYS
  .filter((k) => !cfg.supported.length || cfg.supported.includes(k)))
/** 代管字段全部命中同一档才高亮该 chip；有一项被手动改动就不再匹配任何预设，回到「自定义」 */
const activeEncodePreset = computed(() => {
  const keys = governedKeys.value
  if (!keys.length) return 'custom'
  return encodePresets.value.find((p) => keys.every((k) => cfgNum(k) === p[GOVERNED_FIELD[k]]))?.key || 'custom'
})
function applyEncodePreset(p: EncodePreset) {
  // 不写设备不支持的键：否则点一下档位就会在下发时多出一条「被设备拒绝」，
  // 把一个纯前端的快捷操作变成误导用户报错
  for (const k of governedKeys.value) setCfgNum(k, String(p[GOVERNED_FIELD[k]]))
}
/** 只有 GOP 也在设备支持范围内时，提示里提 GOP 才是对的 */
const presetHasGop = computed(() => governedKeys.value.includes('video.0.main.gop'))

/** 「编码格式」是主网格里唯一的单值语义字段：让它独占一行，其余四项刚好两两成行，不留孤格 */
const VIDEO_FULL_ROW_KEYS = ['video.0.main.codec']

/** 与 firmware/profiles/*.json 的 ivs.max_regions 对齐；设备侧超限会整键拒绝，不截断 */
const REGION_MAX = 4
/** 设备是否支持区域框选（supported 现为平台白名单 ∩ 设备回包，旧固件/其它型号可能没有这个键） */
const regionsSupported = computed(() => !cfg.supported.length || cfg.supported.includes('alarm.motion.regions'))
/**
 * 区域值直接读写 cfg.data：它随「保存并下发」一起提交——这个键不是 reboot_required，
 * 也不涉及失联风险，不需要像网络字段那样配一套独立保存与强确认。
 */
const regionModel = computed<number[][]>({
  get: () => (Array.isArray(cfg.data['alarm.motion.regions']) ? cfg.data['alarm.motion.regions'] : []),
  set: (v) => { cfg.data['alarm.motion.regions'] = v }
})

// net.* 走独立的 NetworkSettings 区块而非通用字段渲染：这五个键改错会让设备从平台上失联，
// 且固件规则表里全是 reboot_required，「保存」与「生效」还隔着一次重启，
// 因此下发要独立成键、独立确认（按钮在下方保存栏，与通用保存并排但各管一段）。
//
// 这个清单只在这里定义一次，其它地方不要再各自过滤网络键（包括 saveCfg 的提交范围）。
const NET_KEYS = ['net.dhcp', 'net.ip', 'net.mask', 'net.gw', 'net.dns']
/** 设备实际支持的网络键（supported 是平台白名单 ∩ 设备回包，见 api/devices.go） */
const visibleNetKeys = computed(() => NET_KEYS
  .filter((k) => !cfg.supported.length || cfg.supported.includes(k)))
/**
 * 网络键是否有未保存修改。与 isNonNetDirty 一起构成“两块分别提交”的判断依据，
 * 保存栏据此同时给出两颗按钮各自的可用性——不靠开区块里再挂一颗按钮。
 */
const isNetDirty = computed(() => NET_KEYS.some(
  (k) => k in cfg.data && JSON.stringify(cfg.data[k]) !== JSON.stringify(cfg.snapshotData[k])))
/** 网络字段当前是否有校验错误（由 NetworkSettings 上报；非法值不该被下发） */
const netInvalid = ref(false)
/** 通用「保存并下发」实际提交的键值：排除网络键，它们只走 saveNetwork() */
function nonNetData(): Record<string, any> {
  const out: Record<string, any> = {}
  for (const [k, v] of Object.entries(cfg.data)) if (!NET_KEYS.includes(k)) out[k] = v
  return out
}
/**
 * 非网络键是否有未保存修改。
 * 按钮的可用性与提示只看「它自己能提交的那部分」，与“越界只拦当前页签”同一原则——
 * 否则只改了网络设置也会点亮「保存并下发」，用户点了却发现什么都没下发。
 * 离开拦截仍用 isCfgDirty（看全部），那里关心的是“走了会不会丢东西”。
 */
const isNonNetDirty = computed(() => Object.keys(cfg.data).some(
  (k) => !NET_KEYS.includes(k) && JSON.stringify(cfg.data[k]) !== JSON.stringify(cfg.snapshotData[k])))

// 只渲染设备确实拥有的键（supported 为空时不过滤，兼容旧后端）
const visibleCfgGroups = computed(() => cfgGroups
  .filter((g) => g.tab === cfgTab.value)
  .map((g) => ({ ...g, fields: cfg.supported.length ? g.fields.filter((f) => cfg.supported.includes(f.key)) : g.fields }))
  .filter((g) => g.fields.length))

// 画面预览：走 /channels/:id/snapshot（IDP 由设备上传一帧）。
// 调亮度/对比度时有个参照图才谈得上“调”，否则只能盲改数字。
const preview = reactive({ src: '', loading: false, at: 0 })
/**
 * 预览只是一次抓拍而不是实时流：必须把「什么时候抓的」和「设备是否在线」说出来，
 * 否则用户会以为自己看到的是当前画面。
 */
const previewStale = computed(() => dev.value?.status !== 'online')
const previewAtText = computed(() => (preview.at ? ago(preview.at, t) : EMPTY))
async function loadPreview() {
  const ch = channels.value[0]
  if (!ch?.id) return
  preview.loading = true
  const fallback = ch.coverUrl || ''
  try {
    const res: any = await api.post(`/channels/${ch.id}/snapshot`)
    preview.src = res?.url || fallback
    preview.at = Date.now()
  } catch {
    // 设备不在线/不支持抓图时退到已有封面，不弹错抢配置页的注意力
    preview.src = preview.src || fallback
  } finally {
    preview.loading = false
  }
}
async function loadCfg() {
  cfg.loading = true
  cfg.denied = []
  try {
    const res: any = await api.get(`/devices/${devId}/config`)
    cfg.data = res?.config || {}
    cfg.supported = res?.supported || []
    cfg.rebootRequired = res?.rebootRequired || []
  } catch (e: any) {
    toastApiError(e, t('device.msg.configLoadFailed'))
    cfg.data = {}
  } finally {
    // 无论成败都要落基准：失败时把它对齐到“当前实际显示的内容”，
    // 否则一个从未加载成功的面板会一直显示“有未保存的修改”
    setCfgBaseline(cfg.data)
    cfg.loading = false
  }
}
watch(tab, (v) => {
  if (v !== 'config') return
  // 设备不在线时 cfg.get 会等到超时，所以不在进详情页时就预拉，只在切到这个 Tab 时才发
  if (!Object.keys(cfg.data).length) loadCfg()
  if (!preview.src) loadPreview()
})
// 区域框选要有底图才谈得上“框”：进「移动侦测」页签时按需取一帧，
// 与画面信息页共用同一份抓帧状态（不重复抓、也不会因为切页签丢掉已抓到的图）
watch(cfgTab, (v) => {
  if (v === 'alarm' && !preview.src) loadPreview()
})
// 定时重启计划的响应式状态必须先于下面的 watch 声明：watch 带 immediate:true，
// 若 cfgTab 初始值直接来自 ?tab=maintain 深链，回调会在 <script setup> 顶层同步执行流里
// 立即跑一次并读 rebootPlan.loaded——此时若 rebootPlan 还没 const 初始化就是 TDZ 报错，
// 会打断整个 setup()（曾经在此处踩过一次：把 watch 放在 rebootPlan 声明之前导致过这个问题）。
const rebootPlan = reactive({
  loading: false, loaded: false, saving: false, enabled: false,
  days: [] as number[], time: '03:00', lastFiredKey: ''
})
// 定时重启只在切到「设备维护」子页签时才拉：它跟 cfg.get 是两次设备往返，没必要都预热；
// immediate:true 是必须的——cfgTab 初始值可能直接来自 URL（?tab=maintain 深链），
// 不加 immediate 的话「切换」这个触发条件从未发生，深链落地就会看到空白的定时重启区块。
watch(cfgTab, (v) => {
  if (v === 'maintain' && !rebootPlan.loaded) loadRebootPlan()
}, { immediate: true })
async function saveCfg() {
  // 越界项拦在本地，但只认当前页签：其它页签遗留的坏值不在这里堵门，交给设备端 rejected 兜底即可（问题④）
  const badInTab = invalidKeysByTab.value[cfgTab.value] || []
  if (badInTab.length) return toast.warning(t('device.msg.configOutOfRange', { n: badInTab.length }))
  // 记录“刚下发给设备的这份值”：它既用来在回读前判断用户是否又改了东西，
  // 也作为新的脏值基准（改动已交付）
  const values = nonNetData()
  cfg.phase = 'sending'
  try {
    const res: any = await api.put(`/devices/${devId}/config`, { values })
    cfg.denied = res?.rejected || []
    resetBaselineAfterPush(Object.keys(values))
    toast.success(cfg.denied.length ? t('device.msg.configPartlyRejected') : t('device.msg.configSent'))
    // 设备侧异步生效，回读一次才能显示设备上真实的值。这段等待必须可见：
    // 旧实现是静默的 setTimeout，表单会在几秒后自己变一遍，看上去像页面出了问题。
    cfg.phase = 'rereading'
    rereadTimer = setTimeout(async () => {
      rereadTimer = null
      // 等待期间用户又改了东西就跳过回读：用设备值覆盖面板比“没自动刷新”糟得多
      if (!isCfgDirty.value) await loadCfg()
      cfg.phase = 'idle'
    }, 5000)
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
    // 只有失败路径需要在这里复位：成功路径要停在 rereading 直到回读完成
    cfg.phase = 'idle'
  }
}

/**
 * 网络下发前的强确认：改错会让设备失联，且需重启才生效。
 * 按钮虽然和「保存并下发」并排（同一个提交区），但这一步不能省——两个按钮下发的东西不同，
 * 风险也不同（见 NetworkSettings 顶部注释）。
 */
async function askSaveNetwork() {
  const ok = await confirmBox.ask({
    title: t('device.config.netSaveConfirmTitle'),
    message: t('device.config.netSaveConfirmMsg'),
    detail: t('device.config.netSaveConfirmDetail'),
    danger: true,
    confirmText: t('device.config.netSaveConfirmOk')
  })
  if (ok) await saveNetwork()
}

/** 网络字段回写：只改页面状态，真正的下发走 saveNetwork() 的独立流程 */
function onNetChange(key: string, value: any) {
  cfg.data[key] = value
}

/**
 * 网络设置独立下发。不做 5 秒自动回读：这五个键在固件规则表里都是 reboot_required，
 * 设备重启前回读只会拿回旧值，反而让用户以为没生效。
 */
async function saveNetwork() {
  const values: Record<string, any> = {}
  for (const k of NET_KEYS) if (k in cfg.data) values[k] = cfg.data[k]
  cfg.phase = 'sending'
  try {
    const res: any = await api.put(`/devices/${devId}/config`, { values })
    cfg.denied = res?.rejected || []
    resetBaselineAfterPush(Object.keys(values))
    toast.success(cfg.denied.length ? t('device.msg.configPartlyRejected') : t('device.config.netSaved'))
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    cfg.phase = 'idle'
  }
}

/** 「重新回读」：会用设备上的值覆盖面板，脏值时必须先确认，不能静默丢弃用户的修改 */
async function reloadCfg() {
  if (isCfgDirty.value) {
    const ok = await confirmBox.ask({
      title: t('device.msg.discardEditsTitle'),
      message: t('device.msg.discardEditsMsg'),
      danger: true,
      confirmText: t('device.confirm.discardEdits')
    })
    if (!ok) return
  }
  await loadCfg()
}

// ================= 定时重启（MGR-08） =================
// 复用「配置」页的位置：与手动重启同属对设备的写操作，且都需要 config 权限。
// （rebootPlan 的声明已提前到上面 watch(cfgTab, ...) 之前，此处不重复声明）
async function loadRebootPlan() {
  rebootPlan.loading = true
  try {
    const res: any = await api.get(`/devices/${devId}/reboot-plan`)
    rebootPlan.enabled = !!res?.enabled
    rebootPlan.time = res?.schedule?.time || '03:00'
    // days 可能是 []any([]float64) 或 []number，统一成 number[] 再给星期选择器
    rebootPlan.days = Array.isArray(res?.schedule?.days) ? res.schedule.days.map((d: any) => Number(d)) : []
    rebootPlan.lastFiredKey = res?.lastFiredKey || ''
  } catch (e: any) {
    toastApiError(e, t('device.msg.rebootPlanLoadFailed'))
  } finally {
    rebootPlan.loading = false
    rebootPlan.loaded = true
  }
}
function toggleRebootDay(d: number) {
  rebootPlan.days = rebootPlan.days.includes(d) ? rebootPlan.days.filter((x) => x !== d) : [...rebootPlan.days, d].sort()
}
async function saveRebootPlan() {
  rebootPlan.saving = true
  try {
    await api.put(`/devices/${devId}/reboot-plan`, {
      enabled: rebootPlan.enabled,
      schedule: { days: rebootPlan.days, time: rebootPlan.time }
    })
    toast.success(t('device.msg.rebootPlanSaved'))
    await loadRebootPlan()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    rebootPlan.saving = false
  }
}

// ================= 编辑 =================
const editDlg = reactive({ show: false, name: '', location: '', remark: '', saving: false })
function openEdit() {
  editDlg.name = dev.value?.name || ''
  editDlg.location = dev.value?.location || ''
  editDlg.remark = dev.value?.remark || ''
  editDlg.show = true
}
async function saveEdit() {
  const name = editDlg.name.trim()
  if (!name) return toast.warning(t('device.msg.nameRequired'))
  editDlg.saving = true
  try {
    await api.put(`/devices/${devId}`, { name, location: editDlg.location.trim(), remark: editDlg.remark.trim() })
    toast.success(t('common.savedOk'))
    editDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    editDlg.saving = false
  }
}

// ================= 重启 / 转移 / 升级 / 安全删除（MGR-08/10/11/12） =================
const router = useRouter()

// 用 replace 不用 push：只是切了个子页签，不是导航到新页面，
// 否则点一次「返回」只退掉上一次切换的 tab，要连点多次才能真正离开设备详情页。
function selectCfgTab(v: CfgTabKey) {
  cfgTab.value = v
  router.replace({ query: { ...route.query, tab: v } })
}

/**
 * 手写 tablist 就得自己实现键盘行为：roving tabindex + ←/→/Home/End。
 * 只写 role="tab" 而不给键盘路径，对键盘与读屏用户而言这些页签是点不到的。
 */
function onCfgTabKeydown(e: KeyboardEvent, cur: CfgTabKey) {
  const keys = CFG_TAB_KEYS
  const i = keys.indexOf(cur)
  let next = -1
  if (e.key === 'ArrowRight') next = (i + 1) % keys.length
  else if (e.key === 'ArrowLeft') next = (i - 1 + keys.length) % keys.length
  else if (e.key === 'Home') next = 0
  else if (e.key === 'End') next = keys.length - 1
  if (next < 0) return
  e.preventDefault()
  const target = keys[next]
  selectCfgTab(target)
  // 焦点要跟着走：否则按方向键后焦点会掉在按钮外面，下一次按键就失效了
  nextTick(() => document.getElementById(`cfg-tab-${target}`)?.focus())
}

async function rebootDevice() {
  const ok = await confirmBox.ask({
    title: t('device.msg.rebootTitle'),
    message: t('device.msg.rebootMsg', { name: dev.value?.name || devId }),
    detail: t('device.msg.rebootDetail'),
    confirmText: t('device.confirm.rebootNow')
  })
  if (!ok) return
  try {
    await api.post(`/devices/${devId}/reboot`)
    toast.success(t('device.msg.rebootSent'))
  } catch (e: any) {
    toastApiError(e, t('device.msg.rebootFailed'))
  }
}

const moveDlg = reactive({ show: false, groupId: '', saving: false })
const groups = ref<any[]>([])
async function openMoveDlg() {
  try {
    const res: any = await api.get('/groups')
    groups.value = res?.items || []
  } catch (e: any) {
    toastApiError(e, t('device.msg.groupListFailed'))
  }
  moveDlg.groupId = dev.value?.groupId || ''
  moveDlg.show = true
}
async function saveMove() {
  if (!moveDlg.groupId) return toast.warning(t('device.msg.selectGroup'))
  moveDlg.saving = true
  try {
    await api.put(`/devices/${devId}`, { groupId: moveDlg.groupId })
    toast.success(t('device.msg.groupMoveOk'))
    moveDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, t('device.msg.moveFailed'))
  } finally {
    moveDlg.saving = false
  }
}

async function askDeleteDevice() {
  const devName = dev.value?.name || devId
  const ok = await confirmBox.ask({
    title: t('device.msg.deleteDetailTitle'),
    message: t('device.msg.deleteDetailMsg', { name: devName }),
    detail: t('device.msg.deleteDetailHint'),
    danger: true,
    confirmText: t('device.confirm.deleteOk'),
    inputConfirm: devName,
    inputPlaceholder: devName
  })
  if (!ok) return
  try {
    await api.request(`/devices/${devId}`, { method: 'DELETE', body: { confirmName: devName } })
    toast.success(t('device.msg.deleteOk'))
    router.push('/devices')
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
  }
}

// 恢复出厂设置隐含设备重启：确认交互镜像删除设备的 inputConfirm；
// 成功后重新拉取配置面板，直接展示设备侧已生效的出厂默认值，而不是本地臆测一份。
async function askFactoryReset() {
  const devName = dev.value?.name || devId
  const ok = await confirmBox.ask({
    title: t('device.msg.factoryResetTitle'),
    message: t('device.msg.factoryResetMsg', { name: devName }),
    detail: t('device.msg.factoryResetHint'),
    danger: true,
    confirmText: t('device.confirm.factoryResetOk'),
    inputConfirm: devName,
    inputPlaceholder: devName
  })
  if (!ok) return
  try {
    await api.post(`/devices/${devId}/config/reset`, { confirmName: devName })
    toast.success(t('device.msg.factoryResetOk'))
    loadCfg()
  } catch (e: any) {
    toastApiError(e, t('device.msg.factoryResetFailed'))
  }
}

// ================= 操作日志行（概览「最近事件」与「日志」Tab 共用） =================
// 后端返回的是 models.AuditLog：{ action（动词，如 config/diag/reboot）、target、result（success|fail）、
// username、ip、detail{method,path,status}、ts }（见 server/internal/api/audit.go）。
// 旧实现只看 action 与「成功/失败」：username（操作人）、ip、detail 全丢，
// 结果判定还写成 `o.result === '失败'` 这类字符串比较——后端从不发这些值，实际上始终靠 `o.ok` 兜底。
const opRows = computed(() => (dev.value?.recentOps || []).map((o: any) => {
  const detail = o?.detail && typeof o.detail === 'object' ? o.detail : {}
  const ok = o?.result ? o.result !== 'fail' : o?.ok !== false
  const method = detail.method || ''
  const path = detail.path || ''
  const status = detail.status
  const request = [method, path].filter(Boolean).join(' ')
  // 失败原因只说后端真记下的东西（HTTP 状态码 + 请求），不编造「网络超时」这类解释
  const detailText = !ok && request
    ? t('device.log.failReason', { method, path, status: status ?? EMPTY })
    : request
  return {
    ts: o.ts || o.time || o.createdAt || 0,
    action: o.action || o.type || '',
    actionKey: auditActionKey(o.action || o.type),
    operator: o.username || o.operator || o.user || EMPTY,
    ip: o.ip || '',
    ok,
    detailText
  }
}))

async function load() {
  loading.value = true
  try {
    dev.value = await api.get(`/devices/${devId}`)
    // 指标“更新于”的起点：首屏拿到的那份就是刚取回的，不必等 30s 轮询才有时间戳
    metricState.at = Date.now()
    // 诊断结果由后端保留 7 天（meta.lastDiag，见 api/devices.go 的 recentDiag）：
    // 有存量就回填，否则刷新一次页面概览就退回「尚未诊断」，而实际上刚跑过。
    const ld = dev.value?.lastDiag
    if (Array.isArray(ld?.results) && ld.results.length) {
      diag.items = ld.results
      diag.at = Number(ld.at) || 0
      diag.ran = true
    }
  } catch (e: any) {
    toastApiError(e, t('common.loadFailed'))
  } finally {
    loading.value = false
  }
}

/* 实时状态（E8）：只关心当前这台设备的事件 */
useWs((ev: any) => {
  if (!dev.value || ev.deviceId !== dev.value.id) return
  if (ev.type === 'device.online' || ev.type === 'device.offline') {
    dev.value.status = ev.type === 'device.online' ? 'online' : 'offline'
    dev.value.lastSeenAt = ev.ts || Date.now()
  }
})

// ================= 未保存修改的离开保护 =================
// 只拦「离开这个页面」：五 Tab 之间、配置子页签之间都不拦——cfg.data 是同一份状态，
// 切页签不会丢修改，此时弹确认只会变成噪音。
function onBeforeUnload(e: BeforeUnloadEvent) {
  if (!isCfgDirty.value) return
  e.preventDefault()
  // 部分浏览器仍要求设置 returnValue 才会弹出「离开此网站？」确认
  e.returnValue = ''
}
onMounted(() => window.addEventListener('beforeunload', onBeforeUnload))
onUnmounted(() => window.removeEventListener('beforeunload', onBeforeUnload))

onBeforeRouteLeave(async () => {
  if (!isCfgDirty.value) return true
  return await confirmBox.ask({
    title: t('device.msg.leaveUnsavedTitle'),
    message: t('device.msg.leaveUnsavedMsg'),
    danger: true,
    confirmText: t('device.confirm.leavePage')
  })
})

onMounted(load)
</script>

<template>
  <UiLoading :loading="loading">
    <UiCard flat>
      <!-- 头部 -->
      <div class="mb-1 flex items-center gap-2.5">
        <button class="flex items-center gap-1 rounded-chrome border border-line px-2 py-1 text-xs text-muted transition-colors hover:border-primary hover:text-primary" @click="navigateTo('/devices')">
          <Icon name="arrow-left" :size="12" />{{ t('device.detail.back') }}
        </button>
        <span class="text-base font-bold text-ink">{{ dev?.name || t('device.detail.title') }}</span>
        <UiTag v-if="dev" :color="statusInfo(dev.status).color as any" dot>{{ t(statusInfo(dev.status).labelKey) }}</UiTag>
        <UiTag :color="sourceInfo(dev?.source).color as any" plain>{{ t(sourceInfo(dev?.source).labelKey) }}</UiTag>
        <div class="ml-auto flex flex-wrap items-center gap-2">
          <UiButton variant="primary" @click="openEdit"><Icon name="edit" :size="13" />{{ t('common.edit') }}</UiButton>
          <UiTooltip v-if="!canReboot" :label="t('device.detail.rebootUnsupported')">
            <span><UiButton disabled><Icon name="refresh-cw" :size="13" />{{ t('device.detail.reboot') }}</UiButton></span>
          </UiTooltip>
          <UiButton v-else @click="rebootDevice"><Icon name="refresh-cw" :size="13" />{{ t('device.detail.reboot') }}</UiButton>
          <UiButton @click="openMoveDlg"><Icon name="folder" :size="13" />{{ t('device.detail.moveGroup') }}</UiButton>
          <UiTooltip :label="t('device.detail.otaDisabled')">
            <span><UiButton disabled><Icon name="upload" :size="13" />{{ t('device.detail.ota') }}</UiButton></span>
          </UiTooltip>
          <UiButton variant="dangerText" @click="askDeleteDevice"><Icon name="trash" :size="13" />{{ t('device.toolbar.delete') }}</UiButton>
        </div>
      </div>

      <UiTabs v-model="tab" :items="[
        { label: t('device.detail.tabOverview'), value: 'overview' },
        { label: t('device.detail.tabChannels'), value: 'channels' },
        { label: t('device.detail.tabConfig'), value: 'config' },
        { label: t('device.detail.tabDiag'), value: 'diag' },
        { label: t('device.detail.tabLogs'), value: 'logs' }
      ]">
        <!-- a) 概览 -->
        <div v-if="tab === 'overview'" class="space-y-4 pt-4">
          <!-- 健康仪表条：在线时长 / 码流状态 / 最近诊断结果，三段式横向排列 -->
          <div class="grid grid-cols-1 divide-y divide-line-soft rounded-signal border border-line bg-zone md:grid-cols-3 md:divide-x md:divide-y-0">
            <div
              v-for="seg in healthSegs" :key="seg.key"
              class="group relative flex items-center gap-3 px-4 py-3"
            >
              <span class="h-2 w-2 shrink-0 rounded-full" :class="[barDot[seg.color], seg.pulse ? 'animate-pulse' : '']" />
              <Icon :name="seg.icon" :size="15" class="shrink-0 text-placeholder" />
              <div class="min-w-0">
                <p class="text-xs text-placeholder">{{ seg.title }}</p>
                <p class="truncate text-sm font-semibold" :class="barText[seg.color]">{{ seg.value }}</p>
                <p class="truncate text-[11px]" :class="seg.onClick ? 'text-primary group-hover:underline' : 'text-muted'">{{ seg.sub }}</p>
              </div>
              <Icon v-if="seg.onClick" name="chevron-right" :size="14" class="ml-auto shrink-0 text-placeholder group-hover:text-primary" />
              <!-- 可点段用铺满整段的透明按钮接管点击与键盘焦点。
                   不要写 <component :is="'button'">：nuxt.config.ts 用 pathPrefix:false 把
                   components/ui/Button.vue 同时注册为全局 `Button`，动态 :is 传 'button' 会被解析成 UiButton
                   （类名合并 + h-8 把整段压成 32px，内容被裁）；铺在内容之上而非包住内容，
                   也避免把非交互文本塞进 button。 -->
              <button
                v-if="seg.onClick" type="button"
                class="ipc-focus-ring absolute inset-0 cursor-pointer outline-none"
                :aria-label="seg.ariaKey ? t(seg.ariaKey) : undefined"
                @click="seg.onClick()"
              />
            </div>
          </div>

          <!-- 基本信息：恒定六项，空值占位 —；单元格分隔线用 gap-px + 容器底色画，
               不再靠第一个/最后一个子元素的 border 修补（旧实现 last:border-0 在 3 列下只能去掉第 6 格） -->
          <div class="grid grid-cols-1 gap-px overflow-hidden rounded-signal border border-line bg-line-soft md:grid-cols-3">
            <div v-for="r in coreRows" :key="r.key" class="flex min-w-0 items-center gap-1 bg-surface px-3 py-2 text-sm">
              <span class="w-24 shrink-0 text-placeholder">{{ r.label }}</span>
              <span class="min-w-0 truncate" :class="r.value ? 'text-ink' : 'text-placeholder'" :title="r.value ? String(r.value) : ''">{{ r.value || EMPTY }}</span>
              <UiTooltip v-if="r.hintKey" :label="t(r.hintKey)">
                <span class="shrink-0 cursor-help text-placeholder"><Icon name="help-circle" :size="12" /></span>
              </UiTooltip>
            </div>
          </div>

          <!-- 接入与网络（PRD MGR-03：概览需含「网络」）：四项全空则整块不渲染 -->
          <div v-if="netRows.length">
            <p class="mb-2 text-xs text-placeholder">{{ t('device.detail.network') }}</p>
            <div class="grid grid-cols-1 gap-px overflow-hidden rounded-signal border border-line bg-line-soft md:grid-cols-3">
              <div
                v-for="r in netRows" :key="r.key" class="bg-surface px-3 py-2 text-sm"
                :class="r.filler ? 'hidden md:block' : 'flex min-w-0 items-center gap-1'"
              >
                <template v-if="!r.filler">
                  <span class="w-24 shrink-0 text-placeholder">{{ r.label }}</span>
                  <span class="min-w-0 truncate" :class="r.value ? 'text-ink' : 'text-placeholder'" :title="r.value ? String(r.value) : ''">{{ r.value || EMPTY }}</span>
                </template>
              </div>
            </div>
          </div>

          <!-- 能力集：中文名 + 展示顺序，保留接口原始值供对接核对；未收录标识原样显示 -->
          <div>
            <div class="mb-2 flex items-center gap-2">
              <p class="text-xs text-placeholder">{{ t('device.detail.caps') }}</p>
              <span v-if="capItems.length" class="text-[11px] text-placeholder">{{ t('device.detail.capsCount', { n: capItems.length }) }}</span>
            </div>
            <div class="flex flex-wrap gap-2">
              <UiTooltip v-for="c in capItems" :key="c.raw" :label="c.known ? t('device.detail.capRaw', { cap: c.raw }) : t('device.detail.capUnknown')">
                <UiTag :color="c.known ? 'primary' : 'default'" plain>{{ t(c.labelKey) }}</UiTag>
              </UiTooltip>
              <span v-if="!capItems.length" class="text-sm text-placeholder">{{ t('device.detail.capsNone') }}</span>
            </div>
          </div>

          <!-- status.metrics（IDP 运行指标，MGR-03）：只列有值的项，flex 自适应不留空槽 -->
          <div v-if="metricItems.length">
            <div class="mb-2 flex flex-wrap items-center gap-2">
              <p class="text-xs text-placeholder">{{ t('device.detail.metrics') }}</p>
              <span v-if="metricState.at" class="text-[11px] text-placeholder">{{ t('device.detail.metricsUpdated', { at: ago(metricState.at, t) }) }}</span>
              <span v-if="dev?.status !== 'online'" class="text-[11px] text-warning">{{ t('device.detail.metricsStale') }}</span>
              <UiButton class="ml-auto" variant="text" size="sm" :disabled="metricState.busy" @click="refreshMetrics">
                <Icon name="refresh" :size="12" :class="metricState.busy ? 'ipc-spin' : ''" />{{ t('common.refresh') }}
              </UiButton>
            </div>
            <div class="flex flex-wrap gap-3">
              <div v-for="m in metricItems" :key="m.key" class="min-w-[110px] flex-1 basis-28 rounded-signal border border-line py-3 text-center">
                <p class="text-xs text-placeholder">{{ m.label }}</p>
                <p class="mt-1 text-lg font-bold text-ink">{{ m.value }}</p>
              </div>
            </div>
          </div>

          <!-- 最近事件：动作中文化 + 操作人 + 失败原因，并给出进「日志」Tab 的出口 -->
          <div v-if="opRows.length">
            <div class="mb-2 flex items-center gap-2">
              <p class="text-xs text-placeholder">{{ t('device.detail.recentEvents') }}</p>
              <button type="button" class="ipc-focus-ring ml-auto flex items-center gap-0.5 rounded-chrome px-1 text-xs text-primary outline-none hover:underline" @click="goLogs">
                {{ t('device.detail.viewAllLogs') }}<Icon name="chevron-right" :size="12" />
              </button>
            </div>
            <div class="divide-y divide-line-soft rounded-signal border border-line">
              <div v-for="(o, i) in opRows.slice(0, 5)" :key="i" class="flex flex-col gap-0.5 px-3 py-2">
                <div class="flex min-w-0 items-center gap-2 text-[13px]">
                  <span class="shrink-0 text-placeholder">{{ ago(o.ts, t) }}</span>
                  <span class="shrink-0 text-body">{{ t(o.actionKey) }}</span>
                  <span class="min-w-0 flex-1 truncate text-placeholder" :title="o.ip ? t('device.log.operatorIp', { ip: o.ip }) : ''">{{ o.operator }}</span>
                  <UiTag :color="o.ok ? 'success' : 'danger'" plain>{{ o.ok ? t('common.success') : t('common.failed') }}</UiTag>
                </div>
                <p v-if="!o.ok && o.detailText" class="text-xs text-danger">{{ o.detailText }}</p>
              </div>
            </div>
          </div>
        </div>

        <!-- b) 通道 -->
        <div v-if="tab === 'channels'" class="pt-4">
          <UiTable
            :columns="[
              { key: 'name', label: t('common.name') },
              { key: 'no', label: t('device.channel.no'), width: '90px', align: 'center' },
              { key: 'enabled', label: t('device.channel.enabled'), width: '90px', align: 'center' },
              { key: 'stream', label: t('device.channel.streamStatus'), width: '100px' },
              { key: 'cover', label: t('device.channel.cover'), width: '120px' },
              { key: 'ops', label: t('common.action'), width: '180px', ellipsis: false }
            ]"
            :rows="channels" :empty="t('device.channel.empty')"
          >
            <template #no="{ row }">{{ row.channelNo ?? row.num ?? row.channel ?? '—' }}</template>
            <template #enabled="{ row }">
              <UiSwitch :model-value="!!row.enabled" size="sm" :aria-label="t('device.channel.enableAria', { name: row.name || row.id })" @update:model-value="toggleCh(row, $event)" />
            </template>
            <template #stream="{ row }"><UiTag :color="streamInfo(row).color as any">{{ t(streamInfo(row).labelKey) }}</UiTag></template>
            <template #cover="{ row }">
              <img v-if="row.coverUrl" :src="row.coverUrl" class="h-8 w-14 rounded-signal object-cover" alt="">
              <span v-else class="text-xs text-placeholder">—</span>
            </template>
            <template #ops="{ row }">
              <UiButton variant="text" size="sm" @click="refreshCover(row)">{{ t('device.channel.refreshCover') }}</UiButton>
              <UiButton variant="text" size="sm" @click="takeSnap(row)">{{ t('device.channel.snapshot') }}</UiButton>
            </template>
          </UiTable>
        </div>

        <!-- c) 配置（MGR-09，能力灰显） -->
        <div v-if="tab === 'config'" class="pt-4">
          <UiLoading :loading="cfg.loading">
            <div class="min-h-40 space-y-4">
              <div v-if="dev?.source !== 'idp'" class="flex items-center gap-2 rounded-signal border border-line bg-zone px-3 py-2.5 text-sm text-muted">
                <Icon name="info" :size="15" class="text-placeholder" />
                {{ t('device.config.unsupported', { source: t(sourceInfo(dev?.source).labelKey) }) }}
              </div>
              <template v-else>
                <!-- 手写而非 UiSegmented：越界红点要挂在每个页签项旁，组件本身没有角标 slot -->
                <div role="tablist" class="inline-flex max-w-xl flex-wrap items-center gap-0.5 rounded-chrome border border-line bg-zone p-0.5">
                  <button
                    v-for="item in cfgTabItems" :key="item.value" type="button" role="tab"
                    :id="`cfg-tab-${item.value}`"
                    class="relative rounded-chrome px-3 py-1 text-xs outline-none ipc-focus-ring transition-colors"
                    :class="cfgTab === item.value ? 'bg-primary text-white shadow-sm' : 'text-muted hover:text-ink'"
                    :aria-selected="cfgTab === item.value"
                    :aria-controls="`cfg-panel-${item.value}`"
                    :tabindex="cfgTab === item.value ? 0 : -1"
                    :aria-describedby="invalidKeysByTab[item.value]?.length ? `cfg-tab-err-${item.value}` : undefined"
                    @click="selectCfgTab(item.value)"
                    @keydown="onCfgTabKeydown($event, item.value)"
                  >
                    {{ item.label }}
                    <span v-if="invalidKeysByTab[item.value]?.length" class="absolute -right-0.5 -top-0.5 h-1.5 w-1.5 rounded-full bg-danger" />
                    <!-- 红点只是视觉加速：读屏用户靠这段文本得知“该分组有越界值”，不能只靠颜色 -->
                    <span
                      v-if="invalidKeysByTab[item.value]?.length"
                      :id="`cfg-tab-err-${item.value}`" class="sr-only"
                    >{{ t('device.config.tabHasError') }}</span>
                  </button>
                </div>

                <!-- 内容区是这块 tablist 的 tabpanel：与 role="tab" 配对后，读屏才能把“选中项”与“内容”关联起来 -->
                <div :id="`cfg-panel-${cfgTab}`" role="tabpanel" :aria-labelledby="`cfg-tab-${cfgTab}`" class="space-y-4">

                <!-- ① 画面信息：预览 + 镜像 + 四项滑杆 -->
                <section v-if="cfgTab === 'image'" class="rounded-signal border border-line">
                  <header class="flex items-center justify-between border-b border-line-soft px-3 py-2">
                    <span class="text-sm font-medium text-ink">{{ t('device.config.group.image') }}</span>
                    <UiButton size="sm" :disabled="preview.loading" @click="loadPreview">
                      <Icon name="refresh" :size="13" :class="preview.loading ? 'ipc-spin' : ''" />{{ t('device.config.refreshPreview') }}
                    </UiButton>
                  </header>
                  <div class="grid gap-4 p-3 md:grid-cols-[minmax(0,320px)_1fr]">
                    <div>
                      <!-- aspect-video 而不是固定高度：主码流多为 16:9，写死 h-44 会留黑边或裁掉画面 -->
                      <div class="flex aspect-video items-center justify-center overflow-hidden rounded-signal border border-line bg-zone">
                        <img v-if="preview.src" :src="preview.src" :style="previewStyle" class="block h-full w-full object-contain" :alt="t('device.config.preview')">
                        <span v-else class="text-xs text-placeholder">{{ t('device.config.previewEmpty') }}</span>
                      </div>
                      <!-- 抓拍不是实时流：把“什么时候抓的”与“设备此刻是否在线”如实说出来 -->
                      <p v-if="preview.src" class="mt-1.5 text-xs text-placeholder">
                        {{ previewStale ? t('device.config.previewOffline') : t('device.config.previewAt', { at: previewAtText }) }}
                      </p>
                    </div>
                    <div class="space-y-3">
                      <div class="flex items-center gap-3">
                        <label class="w-16 shrink-0 text-right text-sm text-muted">{{ t('device.config.mirror') }}</label>
                        <UiSelect v-model="mirrorMode" :options="mirrorOptions" size="sm" width="w-36" />
                      </div>
                      <!-- 滑杆与数字框联动是这一组特有的结构，用 #control 覆盖控件部分，提示区仍复用同一套优先级 -->
                      <ConfigFieldRow
                        v-for="f in IMAGE_SLIDERS" :key="f.key"
                        :label="t(f.labelKey)" label-width="w-16"
                        :rejected="cfg.denied.includes(f.key)"
                        :invalid="invalidKeysByTab[cfgTab]?.includes(f.key)"
                        :invalid-text="t('device.config.outOfRange', { range: boundText(f.key) })"
                      >
                        <UiSlider
                          class="max-w-52 flex-1" :model-value="cfgNum(f.key)" :min="f.min" :max="f.max"
                          @update:model-value="cfg.data[f.key] = $event"
                        />
                        <UiInput
                          type="number" size="sm" width="w-16" :invalid="invalidKeysByTab[cfgTab]?.includes(f.key)"
                          :model-value="cfgDisplay(f.key)"
                          @update:model-value="setCfgNum(f.key, $event)"
                        />
                      </ConfigFieldRow>
                      <!-- 锐度没有对应的原生 CSS 效果，如实告知而不是假装模拟了锐化：
                           单独成行，避免与越界/拒绝提示抢同一位置导致行宽参差 -->
                      <p
                        v-if="cfg.supported.length === 0 || cfg.supported.includes('image.sharpness')"
                        class="text-xs text-placeholder md:pl-[4.75rem]"
                      >{{ t('device.config.sharpnessHint') }}</p>
                    </div>
                  </div>
                </section>

                <!-- ② 其余配置分组：键名与固件对齐，控件按类型渲染 -->
                <section v-for="g in visibleCfgGroups" :key="g.key" class="rounded-signal border border-line">
                  <header class="border-b border-line-soft px-3 py-2 text-sm font-medium text-ink">{{ t(g.titleKey) }}</header>

                  <!-- 画质档位 chips：只代管 fps/kbps/gop 三项，w/h/codec/rc 不在预设范围内；
                       设备不支持任何一项代管字段时，chips 也没有意义，不渲染 -->
                  <div v-if="g.key === 'video' && governedKeys.length" class="border-b border-line-soft px-3 py-2.5">
                    <div class="flex flex-wrap items-center gap-2">
                      <button
                        v-for="p in encodePresets" :key="p.key" type="button"
                        class="rounded-chrome border px-2.5 py-1 text-xs transition-colors"
                        :class="activeEncodePreset === p.key ? 'border-primary bg-primary-soft text-primary' : 'border-line text-muted hover:border-primary'"
                        @click="applyEncodePreset(p)"
                      >{{ p.label }}</button>
                      <!-- 「自定义」是状态而不是可点操作：写成虚线框并且只在真的处于该状态时出现，
                           避免它和可选档位长得一样却点不动 -->
                      <span
                        v-if="activeEncodePreset === 'custom'"
                        class="rounded-chrome border border-dashed border-line px-2.5 py-1 text-xs text-placeholder"
                        :title="t('device.config.presetCustomHint')"
                      >{{ t('device.config.presetCustom') }}</span>
                    </div>
                    <!-- 档位会同时改帧率/码率/GOP，而 GOP 收在折叠的「高级参数」里：
                         不说清楚就变成“改了但看不到”的静默修改 -->
                    <p v-if="presetHasGop" class="mt-1.5 text-xs text-placeholder">{{ t('device.config.presetHint') }}</p>
                  </div>

                  <div class="grid grid-cols-1 gap-x-8 gap-y-3 p-3 md:grid-cols-2">
                    <ConfigFieldRow
                      v-for="f in (g.key === 'video' ? g.fields.filter((fld) => !ADVANCED_VIDEO_KEYS.includes(fld.key)) : g.fields)"
                      :key="f.key"
                      :class="VIDEO_FULL_ROW_KEYS.includes(f.key) ? 'md:col-span-2' : ''"
                      :label="t(f.labelKey)" :field="f" :model-value="cfg.data[f.key]"
                      :disabled="!!cfgFieldDisabledReason(f)" :disabled-hint="cfgFieldDisabledReason(f)"
                      :rejected="cfg.denied.includes(f.key)"
                      :invalid="invalidKeysByTab[cfgTab]?.includes(f.key)"
                      :reboot-required="cfg.rebootRequired.includes(f.key)"
                      :bound-text="f.type === 'int' ? boundText(f.key) : ''"
                      @update:model-value="onCfgFieldInput(f, $event)"
                    />
                  </div>

                  <!-- 高级参数：gop/rc 改动频率低，折叠掉以降低「编码策略」页签的单屏字段密度；
                       两项都不在 supported 里时不留一个空的可展开区块 -->
                  <details v-if="g.key === 'video' && g.fields.some((fld) => ADVANCED_VIDEO_KEYS.includes(fld.key))" class="border-t border-line-soft px-3 py-2">
                    <summary class="cursor-pointer text-xs text-muted">{{ t('device.config.advanced') }}</summary>
                    <div class="grid grid-cols-1 gap-x-8 gap-y-3 pt-3 md:grid-cols-2">
                      <ConfigFieldRow
                        v-for="f in g.fields.filter((fld) => ADVANCED_VIDEO_KEYS.includes(fld.key))"
                        :key="f.key"
                        :label="t(f.labelKey)" :field="f" :model-value="cfg.data[f.key]"
                        :disabled="!!cfgFieldDisabledReason(f)" :disabled-hint="cfgFieldDisabledReason(f)"
                        :rejected="cfg.denied.includes(f.key)"
                        :invalid="invalidKeysByTab[cfgTab]?.includes(f.key)"
                        :reboot-required="cfg.rebootRequired.includes(f.key)"
                        :bound-text="f.type === 'int' ? boundText(f.key) : ''"
                        @update:model-value="onCfgFieldInput(f, $event)"
                      />
                    </div>
                  </details>
                </section>

                <!-- 移动侦测区域：底图取自设备抓拍（与画面信息页共用同一次抓帧状态）；
                     该键不是 reboot_required，随下方通用「保存并下发」一并提交 -->
                <section v-if="cfgTab === 'alarm' && regionsSupported" class="rounded-signal border border-line">
                  <header class="flex items-center justify-between border-b border-line-soft px-3 py-2">
                    <span class="text-sm font-medium text-ink">{{ t('device.config.group.regions') }}</span>
                    <UiButton size="sm" :disabled="preview.loading" @click="loadPreview">
                      <Icon name="refresh" :size="13" :class="preview.loading ? 'ipc-spin' : ''" />{{ t('device.config.refreshPreview') }}
                    </UiButton>
                  </header>
                  <div class="p-3">
                    <p class="mb-3 text-xs text-placeholder">{{ t('device.config.regionsHint', { n: REGION_MAX }) }}</p>
                    <MotionRegionEditor
                      v-model="regionModel" :poster-src="preview.src"
                      :max-regions="REGION_MAX" :disabled="cfgSaving"
                    />
                  </div>
                </section>

                <!-- 网络：只负责渲染与就地校验，下发按钮在下方保存栏（理由见保存栏注释） -->
                <NetworkSettings
                  v-if="cfgTab === 'time' && visibleNetKeys.length"
                  :keys="visibleNetKeys"
                  :data="cfg.data"
                  :reboot-required="cfg.rebootRequired"
                  :saving="cfgSaving"
                  @change="onNetChange"
                  @invalid="netInvalid = $event"
                />

                <!-- ③ 设备维护：定时重启计划（MGR-08）。立即重启只保留页头工具栏那一个入口，这里不再重复放一个 -->
                <section v-if="cfgTab === 'maintain'" class="rounded-signal border border-line">
                  <header class="border-b border-line-soft px-3 py-2 text-sm font-medium text-ink">{{ t('device.config.group.maintain') }}</header>
                  <div class="space-y-3 p-3">
                    <div class="flex items-center gap-3">
                      <label class="w-24 shrink-0 text-right text-sm text-muted">{{ t('device.config.scheduledReboot') }}</label>
                      <!-- 禁用必须给出原因：与页头重启按钮同一个 Tooltip 文案，
                           否则用户只看到一个点不动的开关，不知道是自己没权限还是设备不支持 -->
                      <UiTooltip v-if="!canReboot" :label="t('device.detail.rebootUnsupported')">
                        <span>
                          <UiSwitch
                            v-model="rebootPlan.enabled" size="sm" :disabled="true"
                            :aria-label="t('device.config.scheduledReboot')"
                          />
                        </span>
                      </UiTooltip>
                      <UiSwitch
                        v-else v-model="rebootPlan.enabled" size="sm"
                        :aria-label="t('device.config.scheduledReboot')"
                      />
                      <span class="text-xs text-placeholder">
                        {{ rebootPlan.enabled ? t('device.config.planOn') : t('device.config.planOff') }}
                      </span>
                    </div>
                    <template v-if="rebootPlan.enabled">
                      <div class="flex items-center gap-3">
                        <label class="w-24 shrink-0 text-right text-sm text-muted">{{ t('device.config.rebootDays') }}</label>
                        <div class="flex flex-wrap items-center gap-1">
                          <button
                            v-for="d in [1, 2, 3, 4, 5, 6, 7]" :key="d" type="button"
                            class="h-7 rounded-chrome border px-2 text-xs transition-colors"
                            :class="rebootPlan.days.includes(d) ? 'border-primary bg-primary-soft text-primary' : 'border-line text-muted hover:border-primary'"
                            @click="toggleRebootDay(d)"
                          >{{ t('enum.day.' + d) }}</button>
                        </div>
                        <span v-if="!rebootPlan.days.length" class="text-xs text-placeholder">{{ t('device.config.rebootEveryDay') }}</span>
                      </div>
                      <div class="flex items-center gap-3">
                        <label class="w-24 shrink-0 text-right text-sm text-muted">{{ t('device.config.rebootTime') }}</label>
                        <UiInput v-model="rebootPlan.time" type="time" size="sm" width="w-32" />
                      </div>
                    </template>
                    <div class="flex flex-wrap items-center gap-3">
                      <span class="hidden w-24 shrink-0 md:block" />
                      <!-- 次级样式：这是另一个端点（PUT /reboot-plan）的动作，留在自己区块里，
                           但不应与页面级主按钮同级争视线 -->
                      <UiButton size="sm" :loading="rebootPlan.saving" @click="saveRebootPlan">{{ t('device.config.saveRebootPlan') }}</UiButton>
                      <span v-if="rebootPlan.lastFiredKey" class="text-xs text-placeholder">
                        {{ t('device.config.rebootLastFired', { at: rebootPlan.lastFiredKey }) }}
                      </span>
                    </div>
                  </div>
                </section>

                <!-- 「设备维护」页签本身没有通用保存按钮（重启相关是独立操作），
                     但本任务给它加了 localUser / led 这两个走通用 cfgGroups 渲染的分组，
                     所以要在有通用字段时放开这道口子，否则这两个新字段填了也存不下去 -->
                <div v-if="cfgTab !== 'maintain' || visibleCfgGroups.length > 0" class="flex flex-wrap items-center gap-2">
                  <UiButton
                    variant="primary" :loading="cfgSaving"
                    :disabled="cfgSaving || !isNonNetDirty || (invalidKeysByTab[cfgTab]?.length ?? 0) > 0"
                    @click="saveCfg"
                  >{{ t('device.config.submit') }}</UiButton>
                  <!-- 网络下发与「保存并下发」打同一个端点，但只带 net.* 且带危险确认。
                       按钮放在同一保存栏而不是区块里：区块尾再挂一颗同款主色按钮会与这颗上下相邻，
                       读起来像“重复的两次保存”；集中到一处后按钮区只有一个，各自的范围靠文案与配色区分。
                       只在网络有改动时出现，避免常态多一颗灰按钮。 -->
                  <UiButton
                    v-if="cfgTab === 'time' && isNetDirty"
                    variant="dangerOutline" :disabled="cfgSaving || netInvalid"
                    @click="askSaveNetwork"
                  >{{ t('device.config.netSave') }}</UiButton>
                  <UiButton :disabled="cfgSaving" @click="reloadCfg">{{ t('device.config.reload') }}</UiButton>
                  <span v-if="cfg.phase === 'rereading'" class="text-xs text-primary">{{ t('device.config.rereading') }}</span>
                  <span v-else-if="cfgTab === 'time' && netInvalid" class="text-xs text-danger">{{ t('device.config.netFixFirst') }}</span>
                  <span v-else-if="invalidKeysByTab[cfgTab]?.length" class="text-xs text-danger">{{ t('device.msg.configOutOfRange', { n: invalidKeysByTab[cfgTab].length }) }}</span>
                  <span v-else-if="cfg.denied.length" class="text-xs text-danger">{{ t('device.config.rejectedCount', { n: cfg.denied.length }) }}</span>
                  <!-- 没有任何反馈时按钮是灰的，用户会以为是权限/设备问题；明确告知“没改过” -->
                  <span v-else-if="isNonNetDirty" class="text-xs text-placeholder">{{ t('device.config.unsaved') }}</span>
                  <span v-else-if="cfgTab === 'time' && isNetDirty" class="text-xs text-placeholder">{{ t('device.config.netUnsaved') }}</span>
                </div>

                <!-- 恢复出厂设置排在保存栏之后并用危险配色：破坏性操作不该紧贴常规保存按钮，
                     否则手顺点两下就可能从“保存亮度”滑到“清空设备” -->
                <section v-if="cfgTab === 'maintain'" class="rounded-signal border border-danger/30">
                  <header class="border-b border-danger/20 px-3 py-2 text-sm font-medium text-danger">{{ t('device.config.factoryReset') }}</header>
                  <div class="flex flex-wrap items-center gap-3 p-3">
                    <UiButton variant="danger" size="sm" @click="askFactoryReset"><Icon name="alert-triangle" :size="13" />{{ t('device.config.factoryReset') }}</UiButton>
                    <span class="text-xs text-placeholder">{{ t('device.config.factoryResetDesc') }}</span>
                  </div>
                </section>
                </div>
              </template>
            </div>
          </UiLoading>
        </div>

        <!-- d) 诊断：示波器读数式列表——状态灯 + 探测项 + 耗时（等宽右对齐），扫读友好 -->
        <div v-if="tab === 'diag'" class="space-y-3 pt-4">
          <div class="flex items-center gap-3">
            <UiButton variant="primary" :disabled="diag.loading" @click="runDiag">
              <Icon name="activity" :size="14" :class="diag.loading ? 'ipc-spin' : ''" />{{ t('device.diag.run') }}
            </UiButton>
            <span v-if="diag.items.length && !diag.loading" class="text-xs text-placeholder">
              {{ t('device.diag.summary', { total: diag.items.length, pass: diag.items.filter((it) => it.ok).length, at: diag.at ? ago(diag.at, t) : EMPTY }) }}
            </span>
          </div>

          <div v-if="diag.items.length" class="divide-y divide-line-soft rounded-signal border border-line">
            <div v-for="(it, i) in diag.items" :key="i" class="flex items-center gap-3 px-3 py-2.5 text-sm">
              <span class="h-2 w-2 shrink-0 rounded-full" :class="diagDotClass(it)" />
              <span class="w-32 shrink-0 truncate text-ink">{{ it.name || it.item || t('device.diag.item') }}</span>
              <span class="min-w-0 flex-1 truncate text-[13px]" :class="it.ok === false ? 'text-danger' : 'text-muted'">
                {{ it.msg || (it.ok === false ? t('device.diag.itemFail') : it.ok === true ? t('device.diag.itemOk') : t('device.diag.itemWaiting')) }}
              </span>
              <span v-if="it.cost != null" class="ml-auto shrink-0 font-mono text-xs tabular-nums text-placeholder">{{ it.cost }}ms</span>
            </div>
          </div>
          <div v-else-if="diag.loading" class="rounded-signal border border-line-soft px-3 py-6 text-center text-sm text-placeholder">
            <Icon name="activity" :size="18" class="ipc-spin mx-auto mb-2 text-primary" />{{ t('device.diag.probing') }}
          </div>
          <p v-else-if="diag.ran" class="text-sm text-placeholder">{{ t('device.diag.noResult') }}</p>
          <p v-else class="text-sm text-placeholder">{{ t('device.diag.idle') }}</p>
        </div>

        <!-- e) 日志 -->
        <div v-if="tab === 'logs'" class="pt-4">
          <UiTable
            :columns="[
              { key: 'ts', label: t('common.time'), width: '120px' },
              { key: 'action', label: t('common.action'), width: '150px' },
              { key: 'operator', label: t('device.log.operator'), width: '130px' },
              { key: 'ok', label: t('device.log.result'), width: '90px' },
              { key: 'msg', label: t('common.detail') }
            ]"
            :rows="opRows" :empty="t('device.log.empty')"
          >
            <template #ts="{ row }">{{ ago(row.ts, t) }}</template>
            <template #action="{ row }">{{ t(row.actionKey) }}</template>
            <template #operator="{ row }"><span :title="row.ip ? t('device.log.operatorIp', { ip: row.ip }) : ''">{{ row.operator }}</span></template>
            <template #ok="{ row }"><UiTag :color="row.ok ? 'success' : 'danger'">{{ row.ok ? t('common.success') : t('common.failed') }}</UiTag></template>
            <template #msg="{ row }"><span class="font-mono text-xs text-placeholder">{{ row.detailText || EMPTY }}</span></template>
          </UiTable>
        </div>
      </UiTabs>
    </UiCard>

    <!-- 快照弹窗 -->
    <UiDialog v-model:open="snapDlg.show" :title="t('device.channel.snapTitle')" width="max-w-xl">
      <img v-if="snapDlg.src" :src="snapDlg.src" class="block w-full rounded-signal" :alt="t('device.channel.snapAlt')">
      <UiEmptyState v-else :text="t('device.channel.snapEmpty')" />
    </UiDialog>

    <!-- 编辑设备 -->
    <UiDialog v-model:open="editDlg.show" :title="t('device.edit.title')" width="max-w-md">
      <div class="grid grid-cols-[80px_1fr] items-center gap-x-3 gap-y-3">
        <label class="text-right text-sm text-muted">{{ t('device.detail.editName') }}</label>
        <UiInput v-model="editDlg.name" />
        <label class="text-right text-sm text-muted">{{ t('device.detail.location') }}</label>
        <UiInput v-model="editDlg.location" />
        <label class="text-right text-sm text-muted">{{ t('common.remark') }}</label>
        <UiInput v-model="editDlg.remark" />
      </div>
      <template #footer>
        <UiButton @click="editDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="editDlg.saving" @click="saveEdit">{{ t('common.save') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 转移分组弹窗 -->
    <UiDialog v-model:open="moveDlg.show" :title="t('device.detail.moveTitle')" width="max-w-sm">
      <div class="space-y-3">
        <label class="block text-xs text-muted">{{ t('device.move.targetGroup') }}</label>
        <UiSelect v-model="moveDlg.groupId" :options="groups.map(g => ({ label: g.name, value: g.id }))" class="w-full" />
      </div>
      <template #footer>
        <UiButton @click="moveDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :loading="moveDlg.saving" @click="saveMove">{{ t('device.transfer.submit') }}</UiButton>
      </template>
    </UiDialog>
  </UiLoading>
</template>
