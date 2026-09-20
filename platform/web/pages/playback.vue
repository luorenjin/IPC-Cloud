<script setup lang="ts">
// 录像回放（REC-01~04/08）：通道树 + 日期(录像高亮) + 存储位置 + 24h 时间轴(三色/缩放/框选下载) + 双源控制
const api = useApi()
const route = useRoute()
const toast = useToast()
const confirmBox = useConfirm()
const { upsert: upsertTask, dropLocal: dropLocalTasks, open: taskOpen } = useTasks()

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
const curChannelName = computed(() => channels.value.find((c) => c.id === channelId.value)?.name || '未选择通道')
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
  } catch {}
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
const TYPE_NAME: Record<string, string> = { timer: '定时录像', event: '事件录像', manual: '手动录像' }
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
  } catch (e: any) { toastApiError(e, '录像检索失败') }
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
  if (b - a < 30_000) { toast.warning('框选范围过小（至少 30 秒）'); return }
  downloadRange(a, b)
}
async function downloadRange(a: number, b: number) {
  const ok = await confirmBox.ask({ title: '下载录像片段', message: `将 ${fmt(a)} ~ ${fmt(b)} 的录像加入下载任务？`, confirmText: '加入任务' })
  if (!ok) return
  // 先用临时 ID 占位反馈，服务端返回真实 taskId 后替换，避免本地留下无法管理的僵尸条目
  const tmpId = 'dl_' + Date.now()
  const title = `下载 ${curChannelName.value} ${Math.round((b - a) / 60000)} 分钟片段`
  upsertTask({ id: tmpId, type: 'download', title, status: 'running', progress: 0 })
  taskOpen.value = true
  try {
    const res: any = await api.post(`/channels/${channelId.value}/records/download`, { start: Math.round(a), end: Math.round(b), source: source.value })
    dropLocalTasks([tmpId])
    const files = res?.files || []
    upsertTask({
      id: res?.taskId || tmpId,
      type: 'download',
      title,
      status: 'success',
      progress: 100,
      detail: `${files.length} 个文件`,
      result: { files },
      createdAt: Date.now()
    })
    toast.success(`下载任务已创建，共 ${files.length} 个文件`)
  } catch (e: any) {
    upsertTask({ id: tmpId, status: 'failed', detail: e?.msg || '创建失败' })
    toastApiError(e, '下载失败')
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
  if (!channelId.value) { toast.warning('请先选择通道'); return }
  const ts = normalizeTs(tsFromEvent(e) ?? NaN)
  if (ts == null) return
  if (!session.value) { startPlay(ts); return }
  if (sessionSource.value === 'platform') {
    if (currentSeg.value && ts >= currentSeg.value.s && ts <= currentSeg.value.e) seekLocal(ts)
    else if (segCovering(ts)) { closeSession(); startPlay(ts) }
    else toast.warning('该时间点无平台录像，请点击录像色块')
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
    if (!seg) { toast.warning('该时间点无平台录像，请点击录像色块'); return }
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
    if (e.code === 'E6003') toast.error({ title: '该设备不支持设备端回放' })
    else toastApiError(e, '起播失败')
  }
}

async function seekTo(ts: number) {
  if (!session.value) return
  try {
    await api.put(`/playback/${session.value.sessionId}`, { op: 'seek', seekTs: ts, baseTs: startTs.value })
    startTs.value = ts; curTs.value = ts
  } catch (e: any) {
    toastApiError(e, '定位失败')
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
  } catch (e: any) { toastApiError(e, '操作失败') }
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
      toast.warning('当前浏览器不支持该倍速')
    }
    return
  }
  try { await api.put(`/playback/${session.value.sessionId}`, { op: 'speed', speed: nv }) } catch (e: any) { toastApiError(e, '倍速设置失败') }
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
  } catch {}
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
  try { v.currentTime = off; curTs.value = currentSeg.value.s + off * 1000 } catch { toast.error({ title: '定位失败，请重试' }) }
}
function onVideoMeta() { if (pendingVideoSeek && currentSeg.value) { seekLocal(pendingVideoSeek); pendingVideoSeek = 0 } }
function onVideoTime() { const v = videoEl.value; if (v && currentSeg.value) curTs.value = currentSeg.value.s + v.currentTime * 1000 }
function onVideoErr() { if (session.value && sessionSource.value === 'platform') toast.error({ title: '录像文件加载失败', suggest: '请重试或选择其他时间段' }) }
function toggleMute() { muted.value = !muted.value; if (videoEl.value) videoEl.value.muted = muted.value }
function doSnapshot() {
  if (sessionSource.value === 'device') players0.value?.snapshot?.()
  else toast.info('平台回放请使用播放器原生截图')
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
  } catch (e: any) { toastApiError(e, '通道加载失败') }
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
    <div class="flex w-58 shrink-0 flex-col overflow-hidden rounded border border-line bg-surface" style="width: 232px">
      <div class="border-b border-line-soft p-3">
        <p class="mb-2 text-sm font-semibold text-ink">选择通道</p>
        <UiInput v-model="treeSearch" placeholder="搜索通道" size="sm" clearable>
          <template #prefix><Icon name="search" :size="13" class="text-placeholder" /></template>
        </UiInput>
      </div>
      <div class="min-h-0 flex-1 overflow-y-auto p-2">
        <UiTree :nodes="treeData" :search="treeSearch" :selected="channelId" @select="onNodeClick">
          <template #node="{ node }"><span class="truncate">{{ node.label }}</span></template>
        </UiTree>
        <UiEmptyState v-if="!treeData.length" text="暂无通道" icon="video" />
      </div>
    </div>

    <div class="flex min-w-0 flex-1 flex-col gap-2.5">
      <!-- 工具条 -->
      <div class="flex flex-wrap items-center gap-3 rounded border border-line bg-surface px-3 py-2">
        <span class="text-sm font-semibold text-ink">{{ curChannelName }}</span>
        <div class="flex items-center gap-1">
          <button class="flex h-7 w-7 items-center justify-center rounded border border-line text-muted hover:border-primary hover:text-primary" @click="shiftDay(-1)"><Icon name="chevron-left" :size="14" /></button>
          <UiPopover v-model:open="calOpen" width="w-64">
            <template #trigger>
              <button class="flex h-7 items-center gap-1.5 rounded border border-line bg-surface px-2.5 text-sm text-body hover:border-primary">
                <Icon name="calendar" :size="13" class="text-placeholder" />{{ new Date(dayStart).toLocaleDateString('zh-CN') }}
              </button>
            </template>
            <div>
              <div class="mb-1 flex items-center justify-between">
                <button class="rounded p-1 text-muted hover:bg-zone" @click="calMonth = new Date(calMonth.getFullYear(), calMonth.getMonth() - 1, 1)"><Icon name="chevron-left" :size="14" /></button>
                <span class="text-sm font-medium text-ink">{{ calMonth.getFullYear() }} 年 {{ calMonth.getMonth() + 1 }} 月</span>
                <button class="rounded p-1 text-muted hover:bg-zone" @click="calMonth = new Date(calMonth.getFullYear(), calMonth.getMonth() + 1, 1)"><Icon name="chevron-right" :size="14" /></button>
              </div>
              <div class="grid grid-cols-7 gap-0.5 text-center text-[11px] text-placeholder">
                <span v-for="w in ['一', '二', '三', '四', '五', '六', '日']" :key="w" class="py-1">{{ w }}</span>
              </div>
              <div class="grid grid-cols-7 gap-0.5">
                <template v-for="(d, i) in calDays" :key="i">
                  <button
                    v-if="d"
                    class="relative flex h-7 items-center justify-center rounded text-[13px] transition-colors hover:bg-primary-soft"
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
                <span class="mr-1 inline-block h-1.5 w-1.5 rounded-full bg-primary align-middle" />圆点表示当天有录像
              </p>
            </div>
          </UiPopover>
          <button class="flex h-7 w-7 items-center justify-center rounded border border-line text-muted hover:border-primary hover:text-primary" @click="shiftDay(1)"><Icon name="chevron-right" :size="14" /></button>
        </div>
        <UiSegmented
          :model-value="source" @update:model-value="source = $event as any"
          :items="[...(canDevice ? [{ label: '设备存储', value: 'device' }] : []), { label: '平台存储', value: 'platform' }]"
        />
        <div class="ml-auto flex items-center gap-2">
          <button
            class="flex h-7 items-center gap-1 rounded border px-2 text-xs transition-colors"
            :class="selectMode ? 'border-primary bg-primary-soft text-primary' : 'border-line text-muted hover:border-primary hover:text-primary'"
            @click="selectMode = !selectMode"
          ><Icon name="sliders" :size="13" />框选下载</button>
          <span class="text-xs text-placeholder">滚轮缩放 · {{ zoomLabel }}</span>
          <button v-if="zoomLabel !== '24h'" class="text-xs text-primary hover:underline" @click="resetZoom">重置</button>
        </div>
      </div>

      <!-- 24h 时间轴 -->
      <div class="rounded border border-line bg-surface px-3 pb-3 pt-2">
        <div class="relative mb-0.5 h-4">
          <span v-for="t in ticks" :key="t.left" class="absolute -translate-x-1/2 text-[10px] text-placeholder" :style="{ left: t.left + '%' }">{{ t.label }}</span>
        </div>
        <div
          ref="tlEl" class="relative h-7 rounded bg-zone" :class="selectMode ? 'cursor-crosshair' : 'cursor-pointer'"
          @click="onTimelineClick" @wheel="onWheel" @mousedown="onTlDown" @mousemove="onTlMove" @mouseup="onTlUp" @mouseleave="onTlUp"
        >
          <div
            v-for="(seg, i) in shownSegs" :key="i" class="absolute bottom-1 top-1 rounded-sm"
            :style="{ ...segStyle(seg), background: TYPE_COLOR[seg.type] || 'var(--color-rec-timer)' }"
            :title="`${fmt(seg.s)} ~ ${fmt(seg.e)}（${TYPE_NAME[seg.type] || seg.type}）`"
          />
          <div v-if="dragStyle.left" class="pointer-events-none absolute bottom-0 top-0 rounded-sm border border-primary bg-primary/25" :style="dragStyle" />
          <div v-if="curTs" class="pointer-events-none absolute -bottom-1 -top-1 w-0.5 rounded bg-danger" :style="{ left: curLeft }">
            <span class="absolute -top-5 left-1/2 -translate-x-1/2 whitespace-nowrap rounded bg-ink px-1 py-px text-[10px] text-white">{{ curLabel }}</span>
          </div>
        </div>
        <div class="mt-2 flex items-center gap-4">
          <UiCheckbox v-model="typeFilter.timer" label="定时" />
          <UiCheckbox v-model="typeFilter.event" label="事件" />
          <UiCheckbox v-model="typeFilter.manual" label="手动" />
          <span class="ml-auto text-[11px] text-placeholder">共 {{ shownSegs.length }} 段</span>
        </div>
      </div>

      <!-- 播放器 -->
      <div class="relative min-h-0 flex-1 overflow-hidden rounded border border-line bg-black">
        <H265Player v-if="deviceUrl" ref="players0" :url="deviceUrl" :title="curChannelName" :muted="muted" />
        <video
          v-else-if="platformUrl" ref="videoEl" :src="platformUrl" controls autoplay preload="metadata"
          class="h-full w-full bg-black object-contain" :muted="muted"
          @loadedmetadata="onVideoMeta" @timeupdate="onVideoTime" @error="onVideoErr"
        />
        <div v-else class="absolute inset-0 flex flex-col items-center justify-center gap-2 text-placeholder">
          <Icon name="film" :size="30" :stroke="1.4" />
          <span class="text-sm">{{ channelId ? '点击上方时间轴录像段开始回放' : '请先在左侧选择通道' }}</span>
        </div>
        <div v-if="session" class="absolute left-2 top-2">
          <UiTag :color="sessionSource === 'platform' ? 'primary' : 'success'" plain>
            {{ sessionSource === 'platform' ? '平台录像' : '设备录像' }}
          </UiTag>
        </div>
      </div>

      <!-- 控制条（REC-03：9 档倍速 / 30s 快进 / 静音 / 截图） -->
      <div class="flex flex-wrap items-center gap-2 rounded border border-line bg-surface px-3 py-2">
        <UiButton size="sm" :disabled="!session" @click="togglePause">
          <Icon :name="paused ? 'play' : 'pause'" :size="13" />{{ paused ? '继续' : '暂停' }}
        </UiButton>
        <UiButton size="sm" :disabled="!session" @click="forward30"><Icon name="fast-forward" :size="13" />30s</UiButton>
        <UiSelect
          :model-value="String(speed)" width="w-24" size="sm" :disabled="!session"
          :options="speeds.map((s) => ({ label: s + 'x', value: String(s) }))" @update:model-value="onSpeedChange"
        />
        <UiButton size="sm" :disabled="!session" @click="toggleMute">
          <Icon :name="muted ? 'volume-x' : 'volume-2'" :size="13" />{{ muted ? '取消静音' : '静音' }}
        </UiButton>
        <UiButton size="sm" :disabled="!session" @click="doSnapshot"><Icon name="camera" :size="13" />截图</UiButton>
        <span v-if="curTs" class="ml-1 font-mono text-xs text-muted">{{ fmt(curTs) }}</span>
        <span class="ml-auto text-xs text-placeholder">会话 {{ session?.sessionId || '—' }}</span>
        <UiButton size="sm" variant="dangerText" :disabled="!session" @click="closeSession"><Icon name="x" :size="13" />关闭回放</UiButton>
      </div>
    </div>
  </div>
</template>
