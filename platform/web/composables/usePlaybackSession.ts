// 单通道录像回放核心逻辑（REC-01~03）：日期/存储位置/类型过滤、24h 时间轴缩放、
// 双源（设备/平台）回放会话控制。由 pages/playback.vue（含通道树、框选下载）与
// DevicePreviewModal.vue 的「回放」Tab（单通道、无通道树）共用，避免逻辑分叉。
export function usePlaybackSession(channelId: Ref<string>, deviceId: Ref<string>) {
  const api = useApi()
  const toast = useToast()
  const { t } = useI18n()

  const DAY = 86400000

  // ---------- 日期 / 存储位置 ----------
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
  const shownSegs = computed(() => segments.value.filter((s) => (typeFilter as any)[s.type] !== false))

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
  function tsFromEvent(e: MouseEvent) {
    const el = tlEl.value
    if (!el) return null
    const rect = el.getBoundingClientRect()
    return dayStart.value + view.s + Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width)) * viewLen.value
  }
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

  // ---------- 回放会话与控制（双源 seek 逻辑） ----------
  const session = ref<any>(null)
  const paused = ref(false)
  const muted = ref(true)
  const speed = ref(1)
  const startTs = ref(0)
  const speeds = [1 / 16, 1 / 8, 1 / 4, 0.5, 1, 2, 4, 8, 16]
  const pendingTs = ref(0)

  async function onTimelineClick(e: MouseEvent) {
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
    if (pendingTs.value) { const t = pendingTs.value; pendingTs.value = 0; startPlay(t) }
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

  return {
    dateVal, dayStart, shiftDay, pickDate,
    calOpen, calMonth, recDays, dayKey, calDays,
    source, canDevice, loadCapability,
    segments, TYPE_COLOR, TYPE_NAME, typeFilter, shownSegs, loadRecords,
    view, viewLen, tlEl, tsFromEvent, onWheel, segStyle, ticks, zoomLabel, resetZoom,
    session, paused, muted, speed, speeds, sessionSource, deviceUrl, platformUrl,
    videoEl, players0, currentSeg, curTs, curLeft, curLabel, pendingTs,
    onTimelineClick, startPlay, seekTo, togglePause, onSpeedChange, forward30, closeSession,
    toggleMute, doSnapshot, onVideoMeta, onVideoTime, onVideoErr
  }
}
