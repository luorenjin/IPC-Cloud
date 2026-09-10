<script setup lang="ts">
// 通道树收起状态：窄屏下画面优先，树可整体收起为一条竖边（响应式，PRD 无固定宽度要求）
const treeCollapsed = ref(false)

// 录像回放（REC-01~04/08）：通道树 + 日期(录像高亮) + 存储位置 + 24h 时间轴(三色/缩放/框选下载) + 双源控制
const api = useApi()
const route = useRoute()
const toast = useToast()
const confirmBox = useConfirm()
const { upsert: upsertTask, open: taskOpen } = useTasks()
const { t } = useI18n()

const DAY = 86400000
const fmt = (ts: any) => new Date(Number(ts)).toLocaleString('zh-CN', { hour12: false })

// ---------- 通道树 ----------
const channels = ref<any[]>([])
const devices = ref<any[]>([])
const treeSearch = ref('')
const treeData = computed(() =>
  devices.value.map((d) => ({
    label: d.name,
    value: 'dev-' + d.id,
    children: channels.value.filter((c) => c.deviceId === d.id).map((c) => ({ label: c.name, value: c.id, id: c.id }))
  })).filter((n) => n.children.length)
)
const channelId = ref('')
const deviceId = computed(() => channels.value.find((c) => c.id === channelId.value)?.deviceId || '')
const curChannelName = computed(() => channels.value.find((c) => c.id === channelId.value)?.name || t('live.playback.noChannel'))
function onNodeClick(data: any) { if (data.id) channelId.value = data.id }

// ---------- 日期 / 存储位置（无能力项不显示 REC-01） ----------
const dateVal = ref<Date>(new Date())
const dayStart = computed(() => new Date(dateVal.value).setHours(0, 0, 0, 0))
const source = ref<'device' | 'platform'>('platform')
const canDevice = ref(false)
async function loadCapability() {
  canDevice.value = false
  if (!deviceId.value) return
  try {
    const d: any = await api.get(`/devices/${deviceId.value}`)
    canDevice.value = (d.capabilities || []).includes('record.device.query')
  } catch (e: any) {
    // 能力探测失败会让"设备录像"入口静默消失，需说明原因
    toastApiError(e, t('live.msg.capabilityFailed'))
  }
  if (!canDevice.value && source.value === 'device') source.value = 'platform'
}

// 迷你日历：有录像日期高亮（REC-02）
const calOpen = ref(false)
const calMonth = ref(new Date())
const recDays = ref<Set<string>>(new Set())
function dayKey(d: Date) { return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}` }
async function loadRecDays() {
  recDays.value = new Set()
  if (!channelId.value) return
  const m = calMonth.value
  const start = new Date(m.getFullYear(), m.getMonth(), 1).getTime()
  const end = new Date(m.getFullYear(), m.getMonth() + 1, 0, 23, 59, 59).getTime()
  try {
    const res: any = await api.get(`/channels/${channelId.value}/records/days`, { start, end, source: source.value })
    const days = res.days || res.items || []
    recDays.value = new Set(days.map((d: any) => (typeof d === 'string' ? d : dayKey(new Date(Number(d.day || d.date || d.ts))))))
  } catch { /* 端点缺失时静默 */ }
}
watch(calMonth, loadRecDays)
watch([channelId, source], loadRecDays)
const calDays = computed(() => {
  const m = calMonth.value
  const startIdx = (new Date(m.getFullYear(), m.getMonth(), 1).getDay() + 6) % 7
  const count = new Date(m.getFullYear(), m.getMonth() + 1, 0).getDate()
  const out: (Date | null)[] = Array(startIdx).fill(null)
  for (let i = 1; i <= count; i++) out.push(new Date(m.getFullYear(), m.getMonth(), i))
  return out
})
function pickDate(d: Date) { dateVal.value = d; calOpen.value = false }
function shiftDay(n: number) { dateVal.value = new Date(dayStart.value + n * DAY) }

// ---------- 录像段与时间轴（三色 + 类型过滤 + 滚轮缩放 24h→10min） ----------
const segments = ref<{ s: number; e: number; type: string }[]>([])
const TYPE_COLOR: Record<string, string> = { timer: 'var(--color-rec-timer)', event: 'var(--color-rec-event)', manual: 'var(--color-rec-manual)' }
const TYPE_NAME: Record<string, string> = { timer: 'live.playback.typeTimerFull', event: 'live.playback.typeEventFull', manual: 'live.playback.typeManualFull' }
const typeFilter = reactive({ timer: true, event: true, manual: true })
const shownSegs = computed(() => segments.value.filter((s) => typeFilter[s.type] !== false))

async function loadRecords() {
  segments.value = []
  if (!channelId.value) return
  try {
    const res: any = await api.get(`/channels/${channelId.value}/records`, {
      start: dayStart.value, end: dayStart.value + DAY, source: source.value
    })
    segments.value = res.segments || []
  } catch (e: any) { toastApiError(e, t('live.msg.recordLoadFailed')) }
}

const view = reactive({ s: 0, e: DAY })
watch([dayStart, channelId], () => { view.s = 0; view.e = DAY })
const viewLen = computed(() => view.e - view.s)
const tlEl = ref<HTMLElement>()
function onWheel(e: WheelEvent) {
  e.preventDefault()
  const el = tlEl.value
  if (!el) return
  const ratio = Math.min(1, Math.max(0, (e.clientX - el.getBoundingClientRect().left) / el.getBoundingClientRect().width))
  const center = view.s + ratio * viewLen.value
  let len = viewLen.value * (e.deltaY > 0 ? 1.5 : 1 / 1.5)
  len = Math.min(DAY, Math.max(600000, len))
  let s = center - ratio * len
  let en = s + len
  if (s < 0) { s = 0; en = len }
  if (en > DAY) { en = DAY; s = Math.max(0, en - len) }
  view.s = s; view.e = en
}
function segStyle(seg: { s: number; e: number }) {
  const l = Math.min(100, Math.max(0, ((seg.s - dayStart.value - view.s) / viewLen.value) * 100))
  const r = Math.min(100, Math.max(0, ((seg.e - dayStart.value - view.s) / viewLen.value) * 100))
  return { left: l + '%', width: Math.max(0.15, r - l) + '%' }
}
const ticks = computed(() => {
  const out: { label: string; left: number }[] = []
  const step = viewLen.value > 6 * 3600_000 ? 3600_000 : viewLen.value > 3600_000 ? 600_000 : 300_000
  for (let t = Math.ceil(view.s / step) * step; t <= view.e; t += step) {
    const d = new Date(dayStart.value + t)
    out.push({
      label: viewLen.value > 3600_000 ? String(d.getHours()) : `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`,
      left: ((t - view.s) / viewLen.value) * 100
    })
  }
  return out
})
const zoomLabel = computed(() => {
  const h = viewLen.value / 3600_000
  return h >= 23.9 ? '24h' : h >= 0.99 ? Math.round(h) + 'h' : Math.round(viewLen.value / 60000) + 'min'
})
function resetZoom() { view.s = 0; view.e = DAY }

// 框选下载（REC-08 → 任务中心）
const selectMode = ref(false)
const dragSel = reactive({ active: false, a: 0, b: 0 })
function tsFromEvent(e: MouseEvent) {
  const el = tlEl.value
  if (!el) return null
  const rect = el.getBoundingClientRect()
  return dayStart.value + view.s + Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width)) * viewLen.value
}
function onTlDown(e: MouseEvent) {
  if (!selectMode.value) return
  const t = tsFromEvent(e); if (t == null) return
  dragSel.active = true; dragSel.a = t; dragSel.b = t
}
function onTlMove(e: MouseEvent) {
  if (!dragSel.active) return
  const t = tsFromEvent(e); if (t != null) dragSel.b = t
}
function onTlUp() {
  if (!dragSel.active) return
  dragSel.active = false
  const a = Math.min(dragSel.a, dragSel.b), b = Math.max(dragSel.a, dragSel.b)
  if (b - a < 30_000) { toast.warning(t('live.msg.rangeTooSmall')); return }
  downloadRange(a, b)
}
async function downloadRange(a: number, b: number) {
  const ok = await confirmBox.ask({ title: t('live.playback.downloadTitle'), message: t('live.playback.downloadConfirm', { start: fmt(a), end: fmt(b) }), confirmText: t('live.playback.downloadConfirmText') })
  if (!ok) return
  const tid = 'dl_' + Date.now()
  upsertTask({ id: tid, type: 'download', title: t('live.playback.downloadTaskTitle', { name: curChannelName.value, n: Math.round((b - a) / 60000) }), status: 'running', progress: 0 })
  taskOpen.value = true
  try {
    const res: any = await api.post(`/channels/${channelId.value}/records/download`, { start: Math.round(a), end: Math.round(b), source: source.value })
    upsertTask({ id: tid, status: 'success', progress: 100, detail: res?.file ? t('live.playback.downloadFileReady') : t('live.playback.downloadSubmitted') })
    toast.success(t('live.msg.downloadCreated'))
  } catch (e: any) {
    upsertTask({ id: tid, status: 'failed', detail: e?.msg || t('live.msg.downloadCreateFailed') })
    toastApiError(e, t('live.msg.downloadFailed'))
  }
}
const dragStyle = computed(() => {
  if (!dragSel.a || !dragSel.b) return {}
  const a = Math.min(dragSel.a, dragSel.b), b = Math.max(dragSel.a, dragSel.b)
  return {
    left: Math.max(0, ((a - dayStart.value - view.s) / viewLen.value) * 100) + '%',
    width: Math.max(0, Math.min(100, ((b - a) / viewLen.value) * 100)) + '%'
  }
})

// ---------- 回放会话与控制（保留已验证的双源 seek 逻辑） ----------
const session = ref<any>(null)
const paused = ref(false)
const muted = ref(true)
const speed = ref(1)
const startTs = ref(0)
const speeds = [1 / 16, 1 / 8, 1 / 4, 0.5, 1, 2, 4, 8, 16]
let pendingTs = 0

async function onTimelineClick(e: MouseEvent) {
  if (selectMode.value) return
  if (!channelId.value) { toast.warning(t('live.msg.pickChannelFirst')); return }
  const ts = normalizeTs(tsFromEvent(e) ?? NaN)
  if (ts == null) return
  if (!session.value) { startPlay(ts); return }
  if (sessionSource.value === 'platform') {
    if (currentSeg.value && ts >= currentSeg.value.s && ts <= currentSeg.value.e) seekLocal(ts)
    else if (segCovering(ts)) { closeSession(); startPlay(ts) }
    else toast.warning(t('live.msg.noPlatformRecord'))
    return
  }
  seekTo(ts)
}

async function startPlay(ts: number) {
  const t = normalizeTs(ts)
  if (t == null) return
  let seg: { s: number; e: number } | null = null
  if (source.value === 'platform') {
    seg = segCovering(t)
    if (!seg) { toast.warning(t('live.msg.noPlatformRecord')); return }
  }
  try {
    const res: any = await api.post(`/channels/${channelId.value}/playback`, { start: t, end: dayStart.value + DAY, source: source.value })
    startTs.value = t
    session.value = res
    paused.value = false
    speed.value = 1
    currentSeg.value = seg
    curTs.value = t
    if ((res.source || source.value) === 'platform') pendingVideoSeek = t
    else startDeviceTick()
  } catch (e: any) {
    if (e.code === 'E6003') toast.error({ title: t('live.msg.devicePlaybackUnsupported') })
    else toastApiError(e, t('live.msg.playbackStartFailed'))
  }
}

async function seekTo(ts: number) {
  if (!session.value) return
  try {
    await api.put(`/playback/${session.value.sessionId}`, { op: 'seek', seekTs: ts, baseTs: startTs.value })
    startTs.value = ts; curTs.value = ts
  } catch (e: any) {
    toastApiError(e, t('live.msg.seekFailed'))
    if (String(e.code) === 'E0404' || String(e.msg || '').includes('E0404')) { session.value = null; stopDeviceTick() }
  }
}

async function togglePause() {
  if (!session.value) return
  if (sessionSource.value === 'platform') {
    const v = videoEl.value
    if (!v) return
    try { if (v.paused) { await v.play(); paused.value = false } else { v.pause(); paused.value = true } } catch {}
    return
  }
  try {
    await api.put(`/playback/${session.value.sessionId}`, { op: paused.value ? 'resume' : 'pause' })
    paused.value = !paused.value
  } catch (e: any) { toastApiError(e, t('live.msg.opFailed')) }
}

async function onSpeedChange(v: any) {
  const nv = Number(v) || 1
  if (!session.value) { speed.value = nv; return }
  if (sessionSource.value === 'platform') {
    const vid = videoEl.value
    if (!vid) return
    const old = vid.playbackRate || 1
    try { vid.playbackRate = nv } catch {
      vid.playbackRate = old; speed.value = old
      toast.warning(t('live.msg.speedUnsupported'))
    }
    return
  }
  try { await api.put(`/playback/${session.value.sessionId}`, { op: 'speed', speed: nv }) } catch (e: any) { toastApiError(e, t('live.msg.speedFailed')) }
}

function forward30() {
  const base = curTs.value || startTs.value
  if (!base) return
  const t = base + 30_000
  if (sessionSource.value === 'platform') {
    if (segCovering(t)) seekLocal(t); else { closeSession(); startPlay(t) }
  } else seekTo(Math.min(t, dayStart.value + DAY - 1000))
}

async function closeSession() {
  if (!session.value) return
  const sid = session.value.sessionId
  session.value = null
  paused.value = false
  currentSeg.value = null
  curTs.value = 0
  pendingVideoSeek = 0
  stopDeviceTick()
  // 尽力关闭服务端会话；失败也不能阻塞组件卸载，服务端有超时回收兜底
  try { await api.del(`/playback/${sid}`) } catch {}
}
onBeforeUnmount(() => { closeSession() })

watch([channelId, dateVal, source], async () => {
  closeSession()
  await loadCapability()
  await loadRecords()
  if (pendingTs) { startPlay(pendingTs); pendingTs = 0 }
})

// ---------- 渲染源 ----------
const sessionSource = computed(() => session.value?.source || source.value)
const deviceUrl = computed(() => {
  if (!session.value || sessionSource.value !== 'device') return ''
  const s = session.value
  return location.protocol === 'https:' ? s.wssFlv || s.wsFlv || '' : s.wsFlv || s.wssFlv || ''
})
// platform 源经同源 /media 代理（COEP require-corp 阻断跨源 MP4）
const platformUrl = computed(() => {
  const u = session.value && sessionSource.value === 'platform' ? session.value.url || '' : ''
  if (!u) return ''
  try {
    const p = new URL(u)
    if (p.pathname.startsWith('/record/')) return '/media' + p.pathname + p.search
  } catch {
    // 地址不是合法 URL：回落用原始地址，交给播放器判断
  }
  return u
})

const videoEl = ref<HTMLVideoElement | null>(null)
const players0 = ref<any>(null)
const currentSeg = ref<{ s: number; e: number } | null>(null)
const curTs = ref(0)
let pendingVideoSeek = 0
let deviceTick: ReturnType<typeof setInterval> | null = null

function normalizeTs(ts: number): number | null {
  if (!Number.isFinite(ts)) return null
  return Math.min(Math.max(Math.round(ts / 1000) * 1000, dayStart.value), dayStart.value + DAY - 1000)
}
function segCovering(ts: number) { return shownSegs.value.find((s) => ts >= s.s && ts <= s.e) || null }
function seekLocal(ts: number) {
  const v = videoEl.value
  if (!v || !currentSeg.value) return
  const maxOff = Number.isFinite(v.duration) ? Math.max(0, v.duration - 0.25) : 0
  const off = Math.min(Math.max((ts - currentSeg.value.s) / 1000, 0), maxOff)
  try { v.currentTime = off; curTs.value = currentSeg.value.s + off * 1000 } catch { toast.error({ title: t('live.msg.seekRetry') }) }
}
function onVideoMeta() { if (pendingVideoSeek && currentSeg.value) { seekLocal(pendingVideoSeek); pendingVideoSeek = 0 } }
function onVideoTime() { const v = videoEl.value; if (v && currentSeg.value) curTs.value = currentSeg.value.s + v.currentTime * 1000 }
function onVideoErr() { if (session.value && sessionSource.value === 'platform') toast.error({ title: t('live.msg.recordFileFailed'), suggest: t('live.msg.recordFileFailedSuggest') }) }
function toggleMute() { muted.value = !muted.value; if (videoEl.value) videoEl.value.muted = muted.value }
function doSnapshot() {
  if (sessionSource.value === 'device') players0.value?.snapshot?.()
  else toast.info(t('live.msg.platformSnapshotHint'))
}
function startDeviceTick() {
  stopDeviceTick()
  deviceTick = setInterval(() => { if (session.value && !paused.value) curTs.value += 250 * (speed.value || 1) }, 250)
}
function stopDeviceTick() { if (deviceTick) { clearInterval(deviceTick); deviceTick = null } }
const curLeft = computed(() => {
  if (!curTs.value) return '0%'
  return Math.min(100, Math.max(0, ((curTs.value - dayStart.value - view.s) / viewLen.value) * 100)) + '%'
})
const curLabel = computed(() => curTs.value ? new Date(curTs.value).toLocaleTimeString('zh-CN', { hour12: false }) : '')

onMounted(async () => {
  try {
    const [dRes, cRes]: any[] = await Promise.all([api.get('/devices'), api.get('/channels')])
    devices.value = dRes.items || dRes || []
    channels.value = cRes.items || cRes || []
  } catch (e: any) { toastApiError(e, t('live.msg.treeLoadFailed')) }
  // 消息中心"回放此刻"跳转：/playback?channelId=xx&ts=
  const q: any = route.query
  if (q.channelId) channelId.value = String(q.channelId)
  if (q.ts) {
    const ts = Number(q.ts)
    if (ts > 0) { pendingTs = ts; dateVal.value = new Date(ts) }
  }
})
</script>

<template>
  <div class="flex h-[calc(100vh-84px)] gap-3">
    <!-- 左：通道树 -->
    <!-- 收起态只留一条窄边，点击展开；展开态窄屏用较窄宽度 -->
    <button
      v-if="treeCollapsed"
      type="button"
      class="flex w-8 shrink-0 flex-col items-center justify-center gap-2 rounded-signal border border-line bg-surface text-muted transition-colors hover:border-primary hover:text-primary"
      :aria-label="t('live.tree.expand')"
      :aria-expanded="false"
      @click="treeCollapsed = false"
    >
      <Icon name="chevron-right" :size="14" />
      <span class="text-[11px] [writing-mode:vertical-rl]">{{ t('live.tree.collapsedLabel') }}</span>
    </button>
    <div v-else class="flex w-48 shrink-0 flex-col overflow-hidden rounded-signal border border-line bg-surface lg:w-tree">
      <div class="border-b border-line-soft p-3">
        <div class="mb-2 flex items-center justify-between">
          <p class="text-sm font-semibold text-ink">{{ t('live.tree.pickTitle') }}</p>
          <button
            type="button"
            class="rounded-chrome p-0.5 text-placeholder transition-colors hover:text-primary"
            :aria-label="t('live.tree.collapse')"
            :aria-expanded="true"
            @click="treeCollapsed = true"
          >
            <Icon name="chevron-left" :size="14" />
          </button>
        </div>
        <UiInput v-model="treeSearch" :placeholder="t('live.tree.searchPlaceholder')" size="sm" clearable>
          <template #prefix><Icon name="search" :size="13" class="text-placeholder" /></template>
        </UiInput>
      </div>
      <div class="min-h-0 flex-1 overflow-y-auto p-2">
        <UiTree :nodes="treeData" :search="treeSearch" :selected="channelId" @select="onNodeClick">
          <template #node="{ node }"><span class="truncate">{{ node.label }}</span></template>
        </UiTree>
        <UiEmptyState v-if="!treeData.length" :text="t('live.tree.empty')" icon="video" />
      </div>
    </div>

    <div class="flex min-w-0 flex-1 flex-col gap-2.5">
      <!-- 工具条 -->
      <div class="flex flex-wrap items-center gap-3 rounded-signal border border-line bg-surface px-3 py-2">
        <span class="text-sm font-semibold text-ink">{{ curChannelName }}</span>
        <div class="flex items-center gap-1">
          <button type="button" class="flex h-7 w-7 items-center justify-center rounded-chrome border border-line text-muted hover:border-primary hover:text-primary" :aria-label="t('live.playback.prevDay')" @click="shiftDay(-1)"><Icon name="chevron-left" :size="14" /></button>
          <UiPopover v-model:open="calOpen" width="w-64">
            <template #trigger>
              <button class="flex h-7 items-center gap-1.5 rounded-chrome border border-line bg-surface px-2.5 text-sm text-body hover:border-primary">
                <Icon name="calendar" :size="13" class="text-placeholder" />{{ new Date(dayStart).toLocaleDateString('zh-CN') }}
              </button>
            </template>
            <div>
              <div class="mb-1 flex items-center justify-between">
                <button type="button" class="rounded-chrome p-1 text-muted hover:bg-zone" :aria-label="t('live.playback.prevMonth')" @click="calMonth = new Date(calMonth.getFullYear(), calMonth.getMonth() - 1, 1)"><Icon name="chevron-left" :size="14" /></button>
                <span class="text-sm font-medium text-ink">{{ t('live.playback.calMonthLabel', { y: calMonth.getFullYear(), m: calMonth.getMonth() + 1 }) }}</span>
                <button type="button" class="rounded-chrome p-1 text-muted hover:bg-zone" :aria-label="t('live.playback.nextMonth')" @click="calMonth = new Date(calMonth.getFullYear(), calMonth.getMonth() + 1, 1)"><Icon name="chevron-right" :size="14" /></button>
              </div>
              <div class="grid grid-cols-7 gap-0.5 text-center text-[11px] text-placeholder">
                <span v-for="w in ['live.playback.weekMon', 'live.playback.weekTue', 'live.playback.weekWed', 'live.playback.weekThu', 'live.playback.weekFri', 'live.playback.weekSat', 'live.playback.weekSun']" :key="w" class="py-1">{{ t(w) }}</span>
              </div>
              <div class="grid grid-cols-7 gap-0.5">
                <template v-for="(d, i) in calDays" :key="i">
                  <button
                    v-if="d"
                    class="relative flex h-7 items-center justify-center rounded-chrome text-[13px] transition-colors hover:bg-primary-soft"
                    :class="dayStart === d.getTime() ? 'bg-primary font-medium text-white hover:bg-primary' : recDays.has(dayKey(d)) ? 'font-medium text-primary' : 'text-body'"
                    @click="pickDate(d)"
                  >
                    {{ d.getDate() }}
                    <span v-if="recDays.has(dayKey(d)) && dayStart !== d.getTime()" class="absolute bottom-0.5 h-1 w-1 rounded-full bg-primary" />
                  </button>
                  <span v-else />
                </template>
              </div>
              <p class="mt-1.5 border-t border-line-soft pt-1.5 text-[11px] text-placeholder">
                <span class="mr-1 inline-block h-1.5 w-1.5 rounded-full bg-primary align-middle" />{{ t('live.playback.calDotHint') }}
              </p>
            </div>
          </UiPopover>
          <button type="button" class="flex h-7 w-7 items-center justify-center rounded-chrome border border-line text-muted hover:border-primary hover:text-primary" :aria-label="t('live.playback.nextDay')" @click="shiftDay(1)"><Icon name="chevron-right" :size="14" /></button>
        </div>
        <UiSegmented
          :model-value="source" @update:model-value="source = $event as any"
          :items="[...(canDevice ? [{ label: t('live.playback.srcDevice'), value: 'device' }] : []), { label: t('live.playback.srcPlatform'), value: 'platform' }]"
        />
        <div class="ml-auto flex items-center gap-2">
          <button
            class="flex h-7 items-center gap-1 rounded-chrome border px-2 text-xs transition-colors"
            :class="selectMode ? 'border-primary bg-primary-soft text-primary' : 'border-line text-muted hover:border-primary hover:text-primary'"
            @click="selectMode = !selectMode"
          ><Icon name="sliders" :size="13" />{{ t('live.playback.rangeDownload') }}</button>
        </div>
      </div>

      <!-- 24h 时间轴（Hero：深槽 + 三色实体色块，语义见 REC-02） -->
      <div class="rounded-signal border border-line bg-surface px-3 pb-2.5 pt-2.5">
        <div class="relative mb-1 h-4">
          <span v-for="t in ticks" :key="t.left" class="absolute -translate-x-1/2 font-mono text-[10px] text-placeholder" :style="{ left: t.left + '%' }">{{ t.label }}</span>
        </div>
        <div
          ref="tlEl" class="relative h-9 rounded-signal border border-line-soft bg-canvas shadow-[inset_0_1px_4px_rgba(0,0,0,0.6)]"
          :class="selectMode ? 'cursor-crosshair' : 'cursor-pointer'"
          @click="onTimelineClick" @wheel="onWheel" @mousedown="onTlDown" @mousemove="onTlMove" @mouseup="onTlUp" @mouseleave="onTlUp"
        >
          <div
            v-for="(seg, i) in shownSegs" :key="i" class="absolute bottom-1.5 top-1.5 rounded-signal transition-opacity hover:opacity-85"
            :style="{ ...segStyle(seg), background: TYPE_COLOR[seg.type] || 'var(--color-rec-timer)' }"
            :title="t('live.playback.segTitle', { start: fmt(seg.s), end: fmt(seg.e), type: TYPE_NAME[seg.type] ? t(TYPE_NAME[seg.type]) : seg.type })"
          />
          <div v-if="dragStyle.left" class="pointer-events-none absolute bottom-0 top-0 rounded-signal border border-primary bg-primary/25" :style="dragStyle" />
          <div v-if="curTs" class="pointer-events-none absolute -bottom-1.5 -top-1.5 w-[2px] bg-primary shadow-[0_0_6px_var(--color-primary)]" :style="{ left: curLeft }">
            <span class="absolute -top-6 left-1/2 -translate-x-1/2 whitespace-nowrap rounded-chrome border border-primary/50 bg-surface-2 px-1.5 py-0.5 font-mono text-[11px] text-ink shadow-pop">{{ curLabel }}</span>
          </div>
        </div>
        <!-- 缩放/倍速：从属于时间轴本身，不做成独立工具栏 -->
        <div class="mt-2.5 flex flex-wrap items-center gap-4 border-t border-line-soft pt-2.5">
          <div class="flex items-center gap-4">
            <UiCheckbox v-model="typeFilter.timer">
              <span class="inline-flex items-center gap-1.5"><span class="h-1.5 w-1.5 rounded-full" :style="{ background: TYPE_COLOR.timer }" />{{ t('live.playback.typeTimer') }}</span>
            </UiCheckbox>
            <UiCheckbox v-model="typeFilter.event">
              <span class="inline-flex items-center gap-1.5"><span class="h-1.5 w-1.5 rounded-full" :style="{ background: TYPE_COLOR.event }" />{{ t('live.playback.typeEvent') }}</span>
            </UiCheckbox>
            <UiCheckbox v-model="typeFilter.manual">
              <span class="inline-flex items-center gap-1.5"><span class="h-1.5 w-1.5 rounded-full" :style="{ background: TYPE_COLOR.manual }" />{{ t('live.playback.typeManual') }}</span>
            </UiCheckbox>
          </div>
          <div class="h-3.5 w-px bg-line-soft" />
          <div class="flex items-center gap-1.5 text-xs text-placeholder">
            <Icon name="zoom-in" :size="13" />{{ t('live.playback.wheelZoom') }}
            <span class="font-mono text-body">{{ zoomLabel }}</span>
            <button v-if="zoomLabel !== '24h'" class="text-primary hover:underline" @click="resetZoom">{{ t('live.playback.resetZoom') }}</button>
          </div>
          <div class="h-3.5 w-px bg-line-soft" />
          <div class="flex items-center gap-2">
            <span class="text-xs text-placeholder">{{ t('live.playback.speed') }}</span>
            <UiSelect
              :model-value="String(speed)" width="w-24" size="sm" :disabled="!session"
              :options="speeds.map((s) => ({ label: s + 'x', value: String(s) }))" @update:model-value="onSpeedChange"
            />
          </div>
          <span class="ml-auto text-[11px] text-placeholder">{{ t('live.playback.segCount', { n: shownSegs.length }) }}</span>
        </div>
      </div>

      <!-- 播放器 -->
      <div class="relative min-h-0 flex-1 overflow-hidden rounded-signal border border-line bg-black">
        <H265Player v-if="deviceUrl" ref="players0" :url="deviceUrl" :title="curChannelName" :muted="muted" />
        <video
          v-else-if="platformUrl" ref="videoEl" :src="platformUrl" controls autoplay preload="metadata"
          class="h-full w-full bg-black object-contain" :muted="muted"
          @loadedmetadata="onVideoMeta" @timeupdate="onVideoTime" @error="onVideoErr"
        />
        <div v-else class="absolute inset-0 flex flex-col items-center justify-center gap-2 text-placeholder">
          <Icon name="film" :size="30" :stroke="1.4" />
          <span class="text-sm">{{ channelId ? t('live.playback.emptyPickSeg') : t('live.playback.emptyPickChannel') }}</span>
        </div>
        <div v-if="session" class="absolute left-2 top-2">
          <UiTag :color="sessionSource === 'platform' ? 'primary' : 'success'" plain>
            {{ sessionSource === 'platform' ? t('live.playback.tagPlatform') : t('live.playback.tagDevice') }}
          </UiTag>
        </div>
      </div>

      <!-- 控制条（REC-03：9 档倍速——控件随时间轴放置于其正下方 / 30s 快进 / 静音 / 截图） -->
      <div class="flex flex-wrap items-center gap-2 rounded-signal border border-line bg-surface px-3 py-2">
        <UiButton size="sm" :disabled="!session" @click="togglePause">
          <Icon :name="paused ? 'play' : 'pause'" :size="13" />{{ paused ? t('live.playback.resume') : t('live.playback.pause') }}
        </UiButton>
        <UiButton size="sm" :disabled="!session" @click="forward30"><Icon name="fast-forward" :size="13" />30s</UiButton>
        <UiButton size="sm" :disabled="!session" @click="toggleMute">
          <Icon :name="muted ? 'volume-x' : 'volume-2'" :size="13" />{{ muted ? t('live.playback.unmute') : t('live.playback.mute') }}
        </UiButton>
        <UiButton size="sm" :disabled="!session" @click="doSnapshot"><Icon name="camera" :size="13" />{{ t('live.playback.snapshot') }}</UiButton>
        <span v-if="curTs" class="ml-1 font-mono text-xs text-muted">{{ fmt(curTs) }}</span>
        <span class="ml-auto text-xs text-placeholder">{{ t('live.playback.sessionId', { id: session?.sessionId || '—' }) }}</span>
        <UiButton size="sm" variant="dangerText" :disabled="!session" @click="closeSession"><Icon name="x" :size="13" />{{ t('live.playback.closeSession') }}</UiButton>
      </div>
    </div>
  </div>
</template>
