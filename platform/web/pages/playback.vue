<script setup lang="ts">
// 录像回放（REC-01~04）：通道选择 + 24h 时间轴 + 设备/平台双源回放控制
const api = useApi()
const route = useRoute()

const DAY = 86400000
const fmt = (ts: any) => new Date(Number(ts)).toLocaleString('zh-CN', { hour12: false })

// ---------- 通道树（同 live 简化：设备 → 通道） ----------
const channels = ref<any[]>([])
const devices = ref<any[]>([])
const treeData = computed(() =>
  devices.value.map((d) => ({
    key: 'dev-' + d.id,
    label: d.name,
    children: channels.value
      .filter((c) => c.deviceId === d.id)
      .map((c) => ({ key: c.id, label: c.name, id: c.id }))
  }))
)
const channelId = ref('')
const deviceId = computed(() => channels.value.find((c) => c.id === channelId.value)?.deviceId || '')
const curChannelName = computed(() => channels.value.find((c) => c.id === channelId.value)?.name || '未选择通道')

function onNodeClick(data: any) {
  if (data.id) channelId.value = data.id
}

// ---------- 日期 / 存储位置 ----------
const dateVal = ref<Date>(new Date())
const dayStart = computed(() => new Date(dateVal.value).setHours(0, 0, 0, 0))
const source = ref<'device' | 'platform'>('platform')
const canDevice = ref(false)

// 设备能力：含 record.device.query 才显示"设备存储"（REC-02）
async function loadCapability() {
  canDevice.value = false
  if (!deviceId.value) return
  try {
    const d: any = await api.get(`/devices/${deviceId.value}`)
    canDevice.value = (d.capabilities || []).includes('record.device.query')
  } catch {}
  if (!canDevice.value) source.value = 'platform'
}

// ---------- 录像段与时间轴 ----------
const segments = ref<{ s: number; e: number; type: string }[]>([])
const TYPE_COLOR: Record<string, string> = { timer: '#409eff', event: '#e6a23c', manual: '#67c23a' }
const TYPE_NAME: Record<string, string> = { timer: '定时录像', event: '事件录像', manual: '手动录像' }

async function loadRecords() {
  segments.value = []
  if (!channelId.value) return
  try {
    const res: any = await api.get(`/channels/${channelId.value}/records`, {
      start: dayStart.value, end: dayStart.value + DAY, source: source.value
    })
    segments.value = res.segments || []
  } catch (e: any) {
    ElMessage.error(`${e.code} ${e.msg} ${e.suggest || ''}`)
  }
}

// 色块按 (s-startDay)/(end-startDay) 百分比定位
function segStyle(seg: { s: number; e: number }) {
  const left = Math.min(100, Math.max(0, ((seg.s - dayStart.value) / DAY) * 100))
  const right = Math.min(100, Math.max(0, ((seg.e - dayStart.value) / DAY) * 100))
  return { left: left + '%', width: Math.max(0.2, right - left) + '%' }
}

// ---------- 回放会话与控制 ----------
const session = ref<any>(null)
const paused = ref(false)
const speed = ref(1)
const startTs = ref(0)
const speeds = [0.25, 0.5, 1, 2, 4, 8, 16]
let pendingTs = 0 // 从告警页跳转时自动回放的时间点

function errText(e: any) {
  return `${e.code} ${e.msg} ${e.suggest || ''}`
}

// 点击时间轴：精确换算点击位置为墙钟时间，按会话模式分流定位
async function onTimelineClick(e: MouseEvent) {
  if (!channelId.value) {
    ElMessage.warning('请先选择通道')
    return
  }
  const el = e.currentTarget as HTMLElement
  const rect = el.getBoundingClientRect()
  const ratio = (e.clientX - rect.left) / rect.width
  if (!Number.isFinite(ratio)) return
  const ts = normalizeTs(dayStart.value + ratio * DAY)
  if (ts == null) return
  if (!session.value) {
    startPlay(ts)
    return
  }
  if (sessionSource.value === 'platform') {
    // 平台回放：同段内本地精确定位；跨段重开会话；空白处提示不破坏当前会话
    if (currentSeg.value && ts >= currentSeg.value.s && ts <= currentSeg.value.e) {
      seekLocal(ts)
    } else if (segCovering(ts)) {
      closeSession()
      startPlay(ts)
    } else {
      ElMessage.warning('该时间点无平台录像，请点击蓝色时间段')
    }
    return
  }
  seekTo(ts) // 设备端回放走 seek 指令（无 seek 能力的设备报 E0403 由 catch 提示）
}

async function startPlay(ts: number) {
  const t = normalizeTs(ts)
  if (t == null) return
  // 平台回放预检覆盖段：避免注定失败的会话请求，并记录段用于精确起始偏移
  let seg: { s: number; e: number } | null = null
  if (source.value === 'platform') {
    seg = segCovering(t)
    if (!seg) {
      ElMessage.warning('该时间点无平台录像，请点击蓝色时间段')
      return
    }
  }
  try {
    const res: any = await api.post(`/channels/${channelId.value}/playback`, {
      start: t, end: dayStart.value + DAY, source: source.value
    })
    startTs.value = t
    session.value = res
    paused.value = false
    speed.value = 1
    currentSeg.value = seg
    curTs.value = t
    if ((res.source || source.value) === 'platform') {
      pendingVideoSeek = t // loadedmetadata 后把 video 定位到段内偏移（而非从头播）
    } else {
      startDeviceTick()
    }
  } catch (e: any) {
    if (e.code === 'E6003') ElMessage.error('该设备不支持设备端回放')
    else ElMessage.error(errText(e))
  }
}

async function seekTo(ts: number) {
  if (!session.value) return
  try {
    await api.put(`/playback/${session.value.sessionId}`, { op: 'seek', seekTs: ts, baseTs: startTs.value })
    startTs.value = ts
    curTs.value = ts
  } catch (e: any) {
    ElMessage.error(errText(e))
    // 会话已过期（E0404 可能被网关包装为 E5000）：清掉死会话，下次点击重新起播
    if (String(e.code) === 'E0404' || String(e.msg || '').includes('E0404')) {
      session.value = null
      stopDeviceTick()
    }
  }
}

async function togglePause() {
  if (!session.value) return
  // 平台录像由播放器本地控制（后端 ctrl 对 platform 源为 no-op）
  if (sessionSource.value === 'platform') {
    const v = videoEl.value
    if (!v) return
    try {
      if (v.paused) { await v.play(); paused.value = false }
      else { v.pause(); paused.value = true }
    } catch {}
    return
  }
  const op = paused.value ? 'resume' : 'pause'
  try {
    await api.put(`/playback/${session.value.sessionId}`, { op })
    paused.value = !paused.value
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

async function onSpeedChange(v: any) {
  const nv = Number(v) || 1
  if (!session.value) { speed.value = nv; return }
  // 平台录像本地倍速（HTML5 playbackRate；超出浏览器支持区间会抛异常）
  if (sessionSource.value === 'platform') {
    const vid = videoEl.value
    if (!vid) return
    const old = vid.playbackRate || 1
    try { vid.playbackRate = nv } catch {
      vid.playbackRate = old
      speed.value = old
      ElMessage.warning('当前浏览器不支持该倍速')
    }
    return
  }
  try {
    await api.put(`/playback/${session.value.sessionId}`, { op: 'speed', speed: nv })
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
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

// 通道 / 日期 / 存储位置变化：关旧会话 → 拉能力与录像段
watch([channelId, dateVal, source], async () => {
  closeSession()
  await loadCapability()
  await loadRecords()
  if (pendingTs) {
    startPlay(pendingTs)
    pendingTs = 0
  }
})

// ---------- 渲染源选择 ----------
// device 源走 H265Player（ws/wss-flv），platform 源直接 <video>
const sessionSource = computed(() => session.value?.source || source.value)
const deviceUrl = computed(() => {
  if (!session.value || sessionSource.value !== 'device') return ''
  const s = session.value
  return location.protocol === 'https:' ? s.wssFlv || s.wsFlv || '' : s.wsFlv || s.wssFlv || ''
})
// platform 源直接 <video>；ZLM 绝对 URL 重写为同源 /media 代理路径：
// 页面 COEP require-corp 会阻断无 CORP 头的跨源媒体（ERR_BLOCKED_BY_RESPONSE…Coep），
// 同源代理（server/routes/media）透传 Range，点播拖动不受影响。
const platformUrl = computed(() => {
  const u = session.value && sessionSource.value === 'platform' ? session.value.url || '' : ''
  if (!u) return ''
  try {
    const p = new URL(u)
    if (p.pathname.startsWith('/record/')) return '/media' + p.pathname + p.search
  } catch {}
  return u
})

// ---------- 播放定位（REC-04）：点击时间轴 → 精确起播/段内拖动 ----------
const videoEl = ref<HTMLVideoElement | null>(null)
const currentSeg = ref<{ s: number; e: number } | null>(null) // 平台回放当前 MP4 覆盖的时间段
const curTs = ref(0) // 播放位置（墙钟 ms，驱动时间轴光标）
let pendingVideoSeek = 0 // 待 loadedmetadata 应用的起始墙钟时间
let deviceTick: ReturnType<typeof setInterval> | null = null

// 规整时间点：非法值过滤 + 当日边界钳制；无效返回 null
function normalizeTs(ts: number): number | null {
  if (!Number.isFinite(ts)) return null
  return Math.min(Math.max(Math.round(ts / 1000) * 1000, dayStart.value), dayStart.value + DAY - 1000)
}

// 覆盖 ts 的录像段（与后端选段条件一致：start_ts <= ts <= end_ts）
function segCovering(ts: number): { s: number; e: number } | null {
  return segments.value.find((s) => ts >= s.s && ts <= s.e) || null
}

// 平台回放段内定位：墙钟时间 → MP4 文件偏移，钳制到 [0, duration)
function seekLocal(ts: number) {
  const v = videoEl.value
  if (!v || !currentSeg.value) return
  const maxOff = Number.isFinite(v.duration) ? Math.max(0, v.duration - 0.25) : 0
  const off = Math.min(Math.max((ts - currentSeg.value.s) / 1000, 0), maxOff)
  try {
    v.currentTime = off
    curTs.value = currentSeg.value.s + off * 1000
  } catch {
    ElMessage.error('定位失败，请重试')
  }
}

// 元数据就绪后应用起始偏移（修正"点击 ts 却从头播放"）
function onVideoMeta() {
  if (pendingVideoSeek && currentSeg.value) {
    seekLocal(pendingVideoSeek)
    pendingVideoSeek = 0
  }
}
// 播放位置 → 时间轴光标（平台回放以 video 时间为权威）
function onVideoTime() {
  const v = videoEl.value
  if (v && currentSeg.value) curTs.value = currentSeg.value.s + v.currentTime * 1000
}
function onVideoErr() {
  if (session.value && sessionSource.value === 'platform') {
    ElMessage.error('录像文件加载失败，请重试或选择其他时间段')
  }
}

// 设备端回放：FLV 流读不到墙钟时间，光标按 speed 推算推进
function startDeviceTick() {
  stopDeviceTick()
  deviceTick = setInterval(() => {
    if (session.value && !paused.value) curTs.value += 250 * (speed.value || 1)
  }, 250)
}
function stopDeviceTick() {
  if (deviceTick) { clearInterval(deviceTick); deviceTick = null }
}
const curLeft = computed(() => {
  if (!curTs.value) return '0%'
  const p = ((curTs.value - dayStart.value) / DAY) * 100
  return Math.min(100, Math.max(0, p)) + '%'
})

onMounted(async () => {
  try {
    const [dRes, cRes]: any[] = await Promise.all([api.get('/devices'), api.get('/channels')])
    devices.value = dRes.items || dRes || []
    channels.value = cRes.items || cRes || []
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
  // 支持消息中心"回放此刻"跳转：/playback?channelId=xx&ts=
  const q: any = route.query
  if (q.channelId) channelId.value = String(q.channelId)
  if (q.ts) {
    const ts = Number(q.ts)
    if (ts > 0) {
      pendingTs = ts
      dateVal.value = new Date(ts)
    }
  }
})
</script>

<template>
  <div class="rec-page">
    <!-- 左侧通道树 -->
    <el-aside width="230px" class="ch-tree">
      <div class="tree-head">选择通道</div>
      <el-tree
        :data="treeData" node-key="key" default-expand-all
        :props="{ label: 'label', children: 'children' }" @node-click="onNodeClick"
      >
        <template #default="{ data }">
          <span class="node">{{ data.label }}</span>
        </template>
      </el-tree>
    </el-aside>

    <div class="panel">
      <!-- 工具条：通道 / 日期 / 存储位置 -->
      <div class="ctrl">
        <span class="ch-name">{{ curChannelName }}</span>
        <el-date-picker
          v-model="dateVal" type="date" :clearable="false"
          placeholder="选择日期" style="width: 140px"
        />
        <el-radio-group v-model="source" size="small">
          <el-radio-button v-if="canDevice" value="device">设备存储</el-radio-button>
          <el-radio-button value="platform">平台存储</el-radio-button>
        </el-radio-group>
      </div>

      <!-- 24h 时间轴 -->
      <div class="tl-box">
        <div class="tl-hours">
          <span v-for="h in 24" :key="h" class="tl-h">{{ h - 1 }}</span>
        </div>
        <div class="timeline" @click="onTimelineClick">
          <div
            v-for="(seg, i) in segments" :key="i" class="tl-seg"
            :style="{ ...segStyle(seg), background: TYPE_COLOR[seg.type] || '#409eff' }"
            :title="`${fmt(seg.s)} ~ ${fmt(seg.e)}（${TYPE_NAME[seg.type] || seg.type}）`"
          />
          <div v-if="curTs" class="tl-cur" :style="{ left: curLeft }" :title="'播放位置 ' + fmt(curTs)" />
        </div>
      </div>

      <!-- 播放器区域 -->
      <div class="player-area">
        <H265Player v-if="deviceUrl" :url="deviceUrl" :title="curChannelName" muted />
        <video
          v-else-if="platformUrl" ref="videoEl" :src="platformUrl" controls autoplay
          preload="metadata" class="video"
          @loadedmetadata="onVideoMeta" @timeupdate="onVideoTime" @error="onVideoErr"
        />
        <div v-else class="placeholder">
          {{ channelId ? '点击上方时间轴任意位置开始回放' : '请先在左侧选择通道' }}
        </div>
      </div>

      <!-- 控制条 -->
      <div class="ctrl bottom">
        <el-button size="small" :disabled="!session" @click="togglePause">
          {{ paused ? '继续' : '暂停' }}
        </el-button>
        <el-select
          v-model="speed" size="small" style="width: 90px"
          :disabled="!session" @change="onSpeedChange"
        >
          <el-option v-for="s in speeds" :key="s" :label="s + 'x'" :value="s" />
        </el-select>
        <span v-if="session" class="hint">会话 {{ session.sessionId }}</span>
        <el-button size="small" type="danger" plain :disabled="!session" @click="closeSession">
          关闭回放
        </el-button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.rec-page {
  display: flex; gap: 12px;
  height: calc(100vh - 84px);
}
.ch-tree {
  background: #fff; border-radius: 6px; border: 1px solid #e4e7ed;
  overflow: auto; padding-bottom: 8px;
}
.tree-head { font-weight: 600; color: #303133; padding: 12px 14px 8px; font-size: 14px; }
.node { font-size: 13px; }

.panel {
  flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 10px;
}
.ctrl {
  display: flex; align-items: center; gap: 12px; flex-wrap: wrap;
  background: #fff; border: 1px solid #e4e7ed; border-radius: 6px; padding: 10px 12px;
}
.ch-name { font-weight: 600; color: #303133; }
.hint { color: #909399; font-size: 12px; flex: 1; }

/* 时间轴 */
.tl-box { background: #fff; border: 1px solid #e4e7ed; border-radius: 6px; padding: 8px 12px 12px; }
.tl-hours { display: flex; margin-bottom: 2px; }
.tl-h { flex: 1; font-size: 10px; color: #909399; text-align: left; }
.timeline {
  position: relative; height: 26px; border-radius: 3px; cursor: pointer;
  background-color: #f0f2f5;
  background-image: repeating-linear-gradient(
    to right, transparent 0, transparent calc(100% / 24 - 1px),
    #e4e7ed calc(100% / 24 - 1px), #e4e7ed calc(100% / 24)
  );
}
.tl-seg { position: absolute; top: 3px; bottom: 3px; border-radius: 2px; }
.tl-cur {
  position: absolute; top: -3px; bottom: -3px; width: 2px;
  background: #f56c6c; border-radius: 1px; pointer-events: none;
}

/* 播放器 */
.player-area {
  flex: 1; min-height: 0; position: relative;
  background: #000; border-radius: 6px; overflow: hidden;
}
.video { width: 100%; height: 100%; object-fit: contain; background: #000; }
.placeholder {
  position: absolute; inset: 0; display: flex; align-items: center; justify-content: center;
  color: #909399; font-size: 14px;
}
.ctrl.bottom { justify-content: flex-start; }
</style>