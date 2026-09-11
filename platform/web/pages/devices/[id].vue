<script setup lang="ts">
// 设备详情（MGR-03）：概览 / 通道 / 配置 / 诊断 / 日志（PRD 五 Tab）
const route = useRoute()
const api = useApi()
const toast = useToast()
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

// 重启能力判定必须与后端 supportsReboot 同构，否则会出现「按钮可点但接口 403」：
// rtsp 无重启通道；idp 在声明了能力清单时必须显式声明 reboot；其余来源默认允许。
// 后端改动时此处需同步（AGENTS.md 双向契约一致性）。
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

// 概览字段（空字段隐藏，修复审计 B7）
// 键名必须与后端 deviceJSON 一致：响应里是 fw / hw / lastSeenAt，
// 之前读的是 firmware / firmwareVersion / lastOnline / lastSeen，
// 四个键全部取不到值，行又被下面的 filter 静默丢掉——固件与最后在线因此从未显示过。
const infoRows = computed(() => {
  const d = dev.value || {}
  return [
    { label: t('device.detail.deviceId'), value: d.id },
    { label: t('device.detail.model'), value: d.model },
    { label: t('device.detail.vendor'), value: d.vendor },
    { label: t('device.detail.firmware'), value: d.fw || d.firmware },
    { label: t('device.detail.hardware'), value: d.hw },
    { label: 'IP', value: d.ip },
    { label: 'MAC', value: d.mac },
    { label: t('device.detail.location'), value: d.location },
    { label: t('common.remark'), value: d.remark },
    { label: t('device.detail.lastOnline'), value: d.lastSeenAt || d.lastOnline ? ago(d.lastSeenAt || d.lastOnline, t) : '' }
  ].filter((r) => r.value !== undefined && r.value !== null && r.value !== '')
})

// ================= 诊断（MGR-07：读取 /devices/:id/diag 的 { results, at }） =================
const diag = reactive({ loading: false, ran: false, items: [] as any[] })
async function runDiag() {
  diag.loading = true
  diag.items = []
  try {
    const res: any = await api.post(`/devices/${devId}/diag`)
    diag.items = res?.results || []
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
const uptimeSeg = computed(() => {
  const d = dev.value || {}
  const st = statusInfo(d.status)
  // IDP 会上报 metrics.uptime（秒），这是设备侧真实运行时长，优先用它；
  // 其它协议没有 uptime，退回「最后心跳距今」。两者都比原来恒为 — 有意义。
  const up = metrics.value?.uptime
  const value = d.status === 'online' && up != null
    ? fmtDuration(up)
    : d.lastSeenAt || d.lastOnline ? ago(d.lastSeenAt || d.lastOnline, t) : EMPTY
  return { color: st.color, value, sub: t('device.detail.currentStatus', { status: t(st.labelKey) }) }
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
  if (diag.loading) return { color: 'warning', value: t('device.diag.running'), sub: t('device.diag.probing'), pulse: true }
  if (!diag.ran) return { color: 'info', value: t('device.diag.notRun'), sub: t('device.diag.notRunSub') }
  if (!diag.items.length) return { color: 'info', value: t('device.diag.noResult'), sub: t('device.diag.noResultSub') }
  const failCount = diag.items.filter((it: any) => it.ok === false).length
  return failCount
    ? { color: 'danger', value: t('device.diag.failCount', { n: failCount }), sub: t('device.diag.totalCount', { n: diag.items.length }) }
    : { color: 'success', value: t('device.diag.allOk'), sub: t('device.diag.allOkSub', { n: diag.items.length }) }
})
const healthSegs = computed(() => [
  { key: 'uptime', icon: 'clock', title: t('device.detail.uptime'), ...uptimeSeg.value },
  { key: 'stream', icon: 'video', title: t('device.detail.streamState'), ...streamSeg.value },
  { key: 'diag', icon: 'activity', title: t('device.detail.lastDiag'), ...diagSeg.value }
])

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
// 控件由 type 决定（开关 / 下拉 / 数字），不再像旧实现那样把 image.mirror 这种 0/1 开关
// 渲染成自由文本——那样既容易写出越界值，也看不出合法取值。
// 编码键带通道号：video.<ch>.<name>.<field>，0/main = 主码流。
type CfgField =
  | { key: string; labelKey: string; type: 'int'; min: number; max: number }
  | { key: string; labelKey: string; type: 'bool' }
  | { key: string; labelKey: string; type: 'enum'; options: { label: string; value: string }[] }
  | { key: string; labelKey: string; type: 'str' }
interface CfgGroup { key: string; tab: CfgTabKey; titleKey: string; fields: CfgField[] }

// rebootRequired：固件规则表 reboot_required=true 键的静态镜像（服务端已按 supported 过滤），
// 不是「改了就必须重启才生效」的实时判定——纯粹用来在对应字段旁挂一个「需重启生效」提示。
const cfg = reactive({ loading: false, saving: false, data: {} as any, denied: [] as string[], supported: [] as string[], rebootRequired: [] as string[] })

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
    const bad = g.fields.filter((f) => f.type === 'int' && isOutOfRange(f.key)).map((f) => f.key)
    if (bad.length) m[g.tab] = [...m[g.tab], ...bad]
  }
  return m
})

const boundText = (k: string) => {
  const b = intBounds.value[k]
  return b ? `${b.min}–${b.max}` : ''
}

// 其余分组：键名已与固件对齐、按类型渲染，精细 UI（子页签/联动）待后续补充。
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
      { key: 'record.mode', labelKey: 'device.config.recordMode', type: 'enum', options: [
        { label: t('device.config.rec_continuous'), value: 'continuous' },
        { label: t('device.config.rec_event'), value: 'event' },
        { label: t('device.config.rec_schedule'), value: 'schedule' }
      ] },
      { key: 'record.retention_days', labelKey: 'device.config.retention', type: 'int', min: 1, max: 365 },
      { key: 'record.channel', labelKey: 'device.config.recordChannel', type: 'int', min: 0, max: 2 }
    ]
  },
  {
    key: 'alarm', tab: 'alarm', titleKey: 'device.config.group.alarm',
    fields: [
      { key: 'alarm.motion.enable', labelKey: 'device.config.motionEnable', type: 'bool' },
      { key: 'alarm.motion.sensitivity', labelKey: 'device.config.motionSens', type: 'int', min: 0, max: 100 }
    ]
  },
  {
    key: 'time', tab: 'time', titleKey: 'device.config.group.time',
    fields: [
      { key: 'time.ntp.enable', labelKey: 'device.config.ntpEnable', type: 'bool' },
      { key: 'time.ntp.server', labelKey: 'device.config.ntp', type: 'str' },
      // 时区与 net.dhcp/net.ip 不同：改动不涉及断网风险，走通用可编辑渲染即可，
      // 不需要 net.* 那种只读 + 强确认处理（见本文件下方 time 页签的只读网络区块）。
      { key: 'time.timezone', labelKey: 'device.config.timezone', type: 'str' }
    ]
  },
  {
    // localUser.name / led.enable 说明书要求放"设备维护"页签，但该页签本身是手写模板
    // （重启入口 + 定时重启计划），不走 cfgGroups 通用渲染——这里新增一个独立分组，
    // 让这两个字段仍然走 CfgField 通用渲染机制，而不是在模板里手搓一遍 UI。
    key: 'localSettings', tab: 'maintain', titleKey: 'device.config.group.localSettings',
    fields: [
      { key: 'localUser.name', labelKey: 'device.config.localUser', type: 'str' },
      { key: 'led.enable', labelKey: 'device.config.led', type: 'bool' }
    ]
  }
]

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
/** 三项代管字段（fps/kbps/gop）都命中同一档才高亮该 chip；只要其中一项被手动改动就不再匹配
 * 任何预设——不去猜用户改动后的组合是否“恰好”等于某个预设之外的合理值，避免给出误导性的选中态。 */
const activeEncodePreset = computed(() => {
  const fps = cfgNum('video.0.main.fps')
  const kbps = cfgNum('video.0.main.kbps')
  const gop = cfgNum('video.0.main.gop')
  return encodePresets.value.find((p) => p.fps === fps && p.kbps === kbps && p.gop === gop)?.key || 'custom'
})
function applyEncodePreset(p: EncodePreset) {
  setCfgNum('video.0.main.fps', String(p.fps))
  setCfgNum('video.0.main.kbps', String(p.kbps))
  setCfgNum('video.0.main.gop', String(p.gop))
}

// net.dhcp/net.ip：已进白名单（cfgKeys 可读可写），但本任务只做只读展示——
// 现有 CfgField 类型没有只读变体，且这两项固件规则表标记为 reboot_required，改动
// 有让设备/云端失联的风险，专属的强确认交互（DHCP 开关/IP 输入框）留给后续任务，
// 这里不经过 CfgField/cfgGroups 通用渲染，单独走 time 页签内的手写只读区块。
const NETWORK_READONLY_FIELDS = [
  { key: 'net.dhcp', labelKey: 'device.config.dhcp' },
  { key: 'net.ip', labelKey: 'device.config.ip' }
]
const visibleNetworkFields = computed(() => NETWORK_READONLY_FIELDS
  .filter((f) => !cfg.supported.length || cfg.supported.includes(f.key)))

// 只渲染设备确实拥有的键（supported 为空时不过滤，兼容旧后端）
const visibleCfgGroups = computed(() => cfgGroups
  .filter((g) => g.tab === cfgTab.value)
  .map((g) => ({ ...g, fields: cfg.supported.length ? g.fields.filter((f) => cfg.supported.includes(f.key)) : g.fields }))
  .filter((g) => g.fields.length))

// 画面预览：走 /channels/:id/snapshot（IDP 由设备上传一帧）。
// 调亮度/对比度时有个参照图才谈得上“调”，否则只能盲改数字。
const preview = reactive({ src: '', loading: false })
async function loadPreview() {
  const ch = channels.value[0]
  if (!ch?.id) return
  preview.loading = true
  const fallback = ch.coverUrl || ''
  try {
    const res: any = await api.post(`/channels/${ch.id}/snapshot`)
    preview.src = res?.url || fallback
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
    cfg.loading = false
  }
}
watch(tab, (v) => {
  if (v !== 'config') return
  // 设备不在线时 cfg.get 会等到超时，所以不在进详情页时就预拉，只在切到这个 Tab 时才发
  if (!Object.keys(cfg.data).length) loadCfg()
  if (!preview.src) loadPreview()
})
// 定时重启只在切到「设备维护」子页签时才拉：它跟 cfg.get 是两次设备往返，没必要都预热
watch(cfgTab, (v) => {
  if (v === 'maintain' && !rebootPlan.loaded) loadRebootPlan()
})
async function saveCfg() {
  // 越界项拦在本地，但只认当前页签：其它页签遗留的坏值不在这里堵门，交给设备端 rejected 兜底即可（问题④）
  const badInTab = invalidKeysByTab.value[cfgTab.value] || []
  if (badInTab.length) return toast.warning(t('device.msg.configOutOfRange', { n: badInTab.length }))
  cfg.saving = true
  try {
    const res: any = await api.put(`/devices/${devId}/config`, { values: cfg.data })
    cfg.denied = res?.rejected || []
    toast.success(cfg.denied.length ? t('device.msg.configPartlyRejected') : t('device.msg.configSent'))
    setTimeout(loadCfg, 5000)
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    cfg.saving = false
  }
}

// ================= 定时重启（MGR-08） =================
// 复用「配置」页的位置：与手动重启同属对设备的写操作，且都需要 config 权限。
const rebootPlan = reactive({
  loading: false, loaded: false, saving: false, enabled: false,
  days: [] as number[], time: '03:00', lastFiredKey: ''
})
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
const confirmBox = useConfirm()
const router = useRouter()

// 用 replace 不用 push：只是切了个子页签，不是导航到新页面，
// 否则点一次「返回」只退掉上一次切换的 tab，要连点多次才能真正离开设备详情页。
function selectCfgTab(v: CfgTabKey) {
  cfgTab.value = v
  router.replace({ query: { ...route.query, tab: v } })
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

// ================= 操作日志行 =================
const opRows = computed(() => (dev.value?.recentOps || []).map((o: any) => ({
  ts: o.ts || o.time || o.createdAt || 0,
  action: o.action || o.type || '—',
  operator: o.operator || o.user || o.username || '—',
  ok: o.ok ?? !(o.result === '失败' || o.result === 'fail' || o.result === 'error'),
  msg: o.msg || o.detail || ''
})))

async function load() {
  loading.value = true
  try {
    dev.value = await api.get(`/devices/${devId}`)
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
            <div v-for="seg in healthSegs" :key="seg.key" class="flex items-center gap-3 px-4 py-3">
              <span class="h-2 w-2 shrink-0 rounded-full" :class="[barDot[seg.color], seg.pulse ? 'animate-pulse' : '']" />
              <Icon :name="seg.icon" :size="15" class="shrink-0 text-placeholder" />
              <div class="min-w-0">
                <p class="text-xs text-placeholder">{{ seg.title }}</p>
                <p class="truncate text-sm font-semibold" :class="barText[seg.color]">{{ seg.value }}</p>
                <p class="truncate text-[11px] text-muted">{{ seg.sub }}</p>
              </div>
            </div>
          </div>

          <div class="grid grid-cols-1 gap-x-8 gap-y-2 rounded-signal border border-line md:grid-cols-3">
            <div v-for="r in infoRows" :key="r.label" class="flex border-b border-line-soft px-3 py-2 text-sm last:border-0">
              <span class="w-24 shrink-0 text-placeholder">{{ r.label }}</span>
              <span class="min-w-0 truncate text-ink" :title="String(r.value)">{{ r.value }}</span>
            </div>
          </div>

          <div>
            <p class="mb-2 text-xs text-placeholder">{{ t('device.detail.caps') }}</p>
            <div class="flex flex-wrap gap-2">
              <UiTag v-for="(c, i) in caps" :key="i" color="primary" plain>{{ c }}</UiTag>
              <span v-if="!caps.length" class="text-sm text-placeholder">{{ t('device.detail.capsNone') }}</span>
            </div>
          </div>

          <!-- status.metrics（IDP 运行指标，MGR-03） -->
          <div v-if="metrics">
            <p class="mb-2 text-xs text-placeholder">{{ t('device.detail.metrics') }}</p>
            <div class="grid grid-cols-2 gap-3 md:grid-cols-5">
              <div v-for="m in [
                { label: 'CPU', value: pct(metrics.cpu) },
                { label: t('device.detail.memory'), value: pct(metrics.memory ?? metrics.mem) },
                { label: t('device.detail.temperature'), value: temp(metrics.temperature ?? metrics.temp) },
                { label: t('device.detail.tfCard'), value: metrics.tfHealth || (metrics.tfTotal ? (metrics.tfUsed || 0) + '/' + metrics.tfTotal + 'GB' : '') },
                { label: t('device.detail.bitrate'), value: metrics.bitrate ? metrics.bitrate + 'kbps' : '' }
              ]" :key="m.label" v-show="m.value && m.value !== '—'"
                class="rounded-signal border border-line py-3 text-center">
                <p class="text-xs text-placeholder">{{ m.label }}</p>
                <p class="mt-1 text-lg font-bold text-ink">{{ m.value }}</p>
              </div>
            </div>
          </div>

          <!-- 最近事件 -->
          <div v-if="opRows.length">
            <p class="mb-2 text-xs text-placeholder">{{ t('device.detail.recentEvents') }}</p>
            <div class="space-y-1">
              <div v-for="(o, i) in opRows.slice(0, 5)" :key="i" class="flex items-center gap-2 text-[13px]">
                <span class="text-placeholder">{{ ago(o.ts, t) }}</span>
                <span class="text-body">{{ o.action }}</span>
                <UiTag :color="o.ok ? 'success' : 'danger'" plain>{{ o.ok ? t('common.success') : t('common.failed') }}</UiTag>
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
                    class="relative rounded-chrome px-3 py-1 text-xs outline-none ipc-focus-ring transition-colors"
                    :class="cfgTab === item.value ? 'bg-primary text-white shadow-sm' : 'text-muted hover:text-ink'"
                    :aria-selected="cfgTab === item.value"
                    @click="selectCfgTab(item.value)"
                  >
                    {{ item.label }}
                    <span v-if="invalidKeysByTab[item.value]?.length" class="absolute -right-0.5 -top-0.5 h-1.5 w-1.5 rounded-full bg-danger" />
                  </button>
                </div>

                <!-- ① 画面信息：预览 + 镜像 + 四项滑杆 -->
                <section v-if="cfgTab === 'image'" class="rounded-signal border border-line">
                  <header class="flex items-center justify-between border-b border-line-soft px-3 py-2">
                    <span class="text-sm font-medium text-ink">{{ t('device.config.group.image') }}</span>
                    <UiButton size="sm" :disabled="preview.loading" @click="loadPreview">
                      <Icon name="refresh" :size="13" :class="preview.loading ? 'ipc-spin' : ''" />{{ t('device.config.refreshPreview') }}
                    </UiButton>
                  </header>
                  <div class="grid gap-4 p-3 md:grid-cols-[minmax(0,320px)_1fr]">
                    <div class="flex h-44 items-center justify-center overflow-hidden rounded-signal border border-line bg-zone">
                      <img v-if="preview.src" :src="preview.src" :style="previewStyle" class="block h-full w-full object-contain" :alt="t('device.config.preview')">
                      <span v-else class="text-xs text-placeholder">{{ t('device.config.previewEmpty') }}</span>
                    </div>
                    <div class="space-y-3">
                      <div class="flex items-center gap-3">
                        <label class="w-16 shrink-0 text-right text-sm text-muted">{{ t('device.config.mirror') }}</label>
                        <UiSelect v-model="mirrorMode" :options="mirrorOptions" size="sm" width="w-36" />
                      </div>
                      <div v-for="f in IMAGE_SLIDERS" :key="f.key" class="flex items-center gap-3">
                        <label class="w-16 shrink-0 text-right text-sm text-muted">{{ t(f.labelKey) }}</label>
                        <UiSlider
                          class="max-w-52 flex-1" :model-value="cfgNum(f.key)" :min="f.min" :max="f.max"
                          @update:model-value="cfg.data[f.key] = $event"
                        />
                        <UiInput
                          type="number" size="sm" width="w-16" :invalid="invalidKeysByTab[cfgTab]?.includes(f.key)"
                          :model-value="String(cfgNum(f.key))"
                          @update:model-value="setCfgNum(f.key, $event)"
                        />
                        <span v-if="cfg.denied.includes(f.key)" class="text-xs text-danger">{{ t('device.config.rejected') }}</span>
                        <span v-else-if="invalidKeysByTab[cfgTab]?.includes(f.key)" class="text-xs text-danger">{{ t('device.config.outOfRange', { range: boundText(f.key) }) }}</span>
                        <!-- 锐度没有对应的原生 CSS 效果，如实告知而不是假装模拟了锐化 -->
                        <span v-else-if="f.key === 'image.sharpness'" class="text-xs text-placeholder">{{ t('device.config.sharpnessHint') }}</span>
                      </div>
                    </div>
                  </div>
                </section>

                <!-- ② 其余配置分组：键名与固件对齐，控件按类型渲染 -->
                <section v-for="g in visibleCfgGroups" :key="g.key" class="rounded-signal border border-line">
                  <header class="border-b border-line-soft px-3 py-2 text-sm font-medium text-ink">{{ t(g.titleKey) }}</header>

                  <!-- 画质档位 chips：只代管 fps/kbps/gop 三项，w/h/codec/rc 不在预设范围内；
                       设备若不支持这三项（supported 已把它们过滤出 g.fields），chips 也没有意义，不渲染 -->
                  <div v-if="g.key === 'video' && g.fields.some((fld) => ENCODE_PRESET_GOVERNED_KEYS.includes(fld.key))" class="flex flex-wrap items-center gap-2 border-b border-line-soft px-3 py-2.5">
                    <button
                      v-for="p in encodePresets" :key="p.key" type="button"
                      class="rounded-chrome border px-2.5 py-1 text-xs transition-colors"
                      :class="activeEncodePreset === p.key ? 'border-primary bg-primary-soft text-primary' : 'border-line text-muted hover:border-primary'"
                      @click="applyEncodePreset(p)"
                    >{{ p.label }}</button>
                    <span
                      class="rounded-chrome border px-2.5 py-1 text-xs"
                      :class="activeEncodePreset === 'custom' ? 'border-primary bg-primary-soft text-primary' : 'border-line text-placeholder'"
                    >{{ t('device.config.presetCustom') }}</span>
                  </div>

                  <div class="grid grid-cols-1 gap-x-8 gap-y-3 p-3 md:grid-cols-2">
                    <div v-for="f in (g.key === 'video' ? g.fields.filter((fld) => !ADVANCED_VIDEO_KEYS.includes(fld.key)) : g.fields)" :key="f.key" class="flex items-center gap-3">
                      <label class="w-24 shrink-0 text-right text-sm text-muted">{{ t(f.labelKey) }}</label>
                      <UiSwitch
                        v-if="f.type === 'bool'" size="sm"
                        :model-value="Boolean(cfg.data[f.key])" :aria-label="t(f.labelKey)"
                        @update:model-value="cfg.data[f.key] = $event"
                      />
                      <UiSelect
                        v-else-if="f.type === 'enum'"
                        :model-value="String(cfg.data[f.key] ?? '')" :options="f.options" size="sm" width="w-32"
                        @update:model-value="cfg.data[f.key] = $event"
                      />
                      <UiInput
                        v-else-if="f.type === 'int'" type="number" size="sm" width="w-24"
                        :invalid="invalidKeysByTab[cfgTab]?.includes(f.key)"
                        :model-value="String(cfgNum(f.key))"
                        @update:model-value="setCfgNum(f.key, $event)"
                      />
                      <UiInput
                        v-else size="sm" width="w-48"
                        :model-value="String(cfg.data[f.key] ?? '')"
                        @update:model-value="cfg.data[f.key] = $event"
                      />
                      <!-- 分辨率（video.0.main.w/h）在固件规则表里 reboot_required=true：改了不会立即生效，
                           得提前说清楚，否则用户会以为保存下发就已经生效 -->
                      <UiTag v-if="cfg.rebootRequired.includes(f.key)" color="warning" plain>⚡ {{ t('device.config.rebootRequiredBadge') }}</UiTag>
                      <span v-if="cfg.denied.includes(f.key)" class="text-xs text-danger">{{ t('device.config.rejected') }}</span>
                      <span v-else-if="invalidKeysByTab[cfgTab]?.includes(f.key)" class="text-xs text-danger">{{ t('device.config.outOfRange', { range: boundText(f.key) }) }}</span>
                      <!-- 合法区间就地显示：否则用户只能靠"试一次被拒"来猜范围 -->
                      <span v-else-if="f.type === 'int'" class="text-xs text-placeholder">{{ boundText(f.key) }}</span>
                    </div>
                  </div>

                  <!-- 高级参数：gop/rc 改动频率低，折叠掉以降低「编码策略」页签的单屏字段密度；
                       两项都不在 supported 里时不留一个空的可展开区块 -->
                  <details v-if="g.key === 'video' && g.fields.some((fld) => ADVANCED_VIDEO_KEYS.includes(fld.key))" class="border-t border-line-soft px-3 py-2">
                    <summary class="cursor-pointer text-xs text-muted">{{ t('device.config.advanced') }}</summary>
                    <div class="grid grid-cols-1 gap-x-8 gap-y-3 pt-3 md:grid-cols-2">
                      <div v-for="f in g.fields.filter((fld) => ADVANCED_VIDEO_KEYS.includes(fld.key))" :key="f.key" class="flex items-center gap-3">
                        <label class="w-24 shrink-0 text-right text-sm text-muted">{{ t(f.labelKey) }}</label>
                        <UiSwitch
                          v-if="f.type === 'bool'" size="sm"
                          :model-value="Boolean(cfg.data[f.key])" :aria-label="t(f.labelKey)"
                          @update:model-value="cfg.data[f.key] = $event"
                        />
                        <UiSelect
                          v-else-if="f.type === 'enum'"
                          :model-value="String(cfg.data[f.key] ?? '')" :options="f.options" size="sm" width="w-32"
                          @update:model-value="cfg.data[f.key] = $event"
                        />
                        <UiInput
                          v-else-if="f.type === 'int'" type="number" size="sm" width="w-24"
                          :invalid="invalidKeysByTab[cfgTab]?.includes(f.key)"
                          :model-value="String(cfgNum(f.key))"
                          @update:model-value="setCfgNum(f.key, $event)"
                        />
                        <UiInput
                          v-else size="sm" width="w-48"
                          :model-value="String(cfg.data[f.key] ?? '')"
                          @update:model-value="cfg.data[f.key] = $event"
                        />
                        <span v-if="cfg.denied.includes(f.key)" class="text-xs text-danger">{{ t('device.config.rejected') }}</span>
                        <span v-else-if="invalidKeysByTab[cfgTab]?.includes(f.key)" class="text-xs text-danger">{{ t('device.config.outOfRange', { range: boundText(f.key) }) }}</span>
                        <span v-else-if="f.type === 'int'" class="text-xs text-placeholder">{{ boundText(f.key) }}</span>
                      </div>
                    </div>
                  </details>
                </section>

                <!-- 网络（只读）：net.dhcp/net.ip 已进白名单可读可写，但本任务不开放编辑交互，
                     见上方 visibleNetworkFields 的注释——改网络参数有让设备/云端失联的风险，
                     强确认交互留给后续任务，这里只展示当前值 + 需重启徽标 + 说明文案 -->
                <section v-if="cfgTab === 'time' && visibleNetworkFields.length" class="rounded-signal border border-line">
                  <header class="border-b border-line-soft px-3 py-2 text-sm font-medium text-ink">{{ t('device.config.group.network') }}</header>
                  <div class="space-y-3 p-3">
                    <p class="text-xs text-placeholder">{{ t('device.config.networkHint') }}</p>
                    <div v-for="f in visibleNetworkFields" :key="f.key" class="flex items-center gap-3">
                      <label class="w-24 shrink-0 text-right text-sm text-muted">{{ t(f.labelKey) }}</label>
                      <span class="text-sm text-ink">{{ f.key === 'net.dhcp' ? (cfg.data[f.key] ? t('common.enabled') : t('common.disabled')) : String(cfg.data[f.key] ?? '—') }}</span>
                      <UiTag v-if="cfg.rebootRequired.includes(f.key)" color="warning" plain>⚡ {{ t('device.config.rebootRequiredBadge') }}</UiTag>
                    </div>
                  </div>
                </section>

                <!-- ③ 设备维护：定时重启计划（MGR-08）。立即重启只保留页头工具栏那一个入口，这里不再重复放一个 -->
                <section v-if="cfgTab === 'maintain'" class="rounded-signal border border-line">
                  <header class="border-b border-line-soft px-3 py-2 text-sm font-medium text-ink">{{ t('device.config.group.maintain') }}</header>
                  <div class="space-y-3 p-3">
                    <div class="flex items-center gap-3">
                      <label class="w-24 shrink-0 text-right text-sm text-muted">{{ t('device.config.scheduledReboot') }}</label>
                      <UiSwitch
                        v-model="rebootPlan.enabled" size="sm" :disabled="!canReboot"
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
                      <UiButton variant="primary" size="sm" :loading="rebootPlan.saving" @click="saveRebootPlan">{{ t('common.save') }}</UiButton>
                      <span v-if="rebootPlan.lastFiredKey" class="text-xs text-placeholder">
                        {{ t('device.config.rebootLastFired', { at: rebootPlan.lastFiredKey }) }}
                      </span>
                    </div>
                  </div>
                </section>

                <!-- 恢复出厂设置单独成块并用危险配色：与常规维护操作物理隔离，降低误触概率 -->
                <section v-if="cfgTab === 'maintain'" class="rounded-signal border border-danger/30">
                  <header class="border-b border-danger/20 px-3 py-2 text-sm font-medium text-danger">{{ t('device.config.factoryReset') }}</header>
                  <div class="flex flex-wrap items-center gap-3 p-3">
                    <UiButton variant="danger" size="sm" @click="askFactoryReset"><Icon name="alert-triangle" :size="13" />{{ t('device.config.factoryReset') }}</UiButton>
                    <span class="text-xs text-placeholder">{{ t('device.config.factoryResetDesc') }}</span>
                  </div>
                </section>

                <!-- 「设备维护」页签本身没有通用保存按钮（重启相关是独立操作），
                     但本任务给它加了 localSettings 这个走通用 cfgGroups 渲染的分组，
                     所以要在有通用字段时放开这道口子，否则这两个新字段填了也存不下去 -->
                <div v-if="cfgTab !== 'maintain' || visibleCfgGroups.length > 0" class="flex flex-wrap items-center gap-2">
                  <UiButton variant="primary" :disabled="cfg.saving || (invalidKeysByTab[cfgTab]?.length ?? 0) > 0" @click="saveCfg">{{ t('device.config.submit') }}</UiButton>
                  <UiButton @click="loadCfg">{{ t('device.config.reload') }}</UiButton>
                  <span v-if="invalidKeysByTab[cfgTab]?.length" class="text-xs text-danger">{{ t('device.msg.configOutOfRange', { n: invalidKeysByTab[cfgTab].length }) }}</span>
                  <span v-else-if="cfg.denied.length" class="text-xs text-danger">{{ t('device.config.rejectedCount', { n: cfg.denied.length }) }}</span>
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
              {{ t('device.diag.summary', { total: diag.items.length, pass: diag.items.filter((it) => it.ok).length }) }}
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
            <template #ok="{ row }"><UiTag :color="row.ok ? 'success' : 'danger'">{{ row.ok ? t('common.success') : t('common.failed') }}</UiTag></template>
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
