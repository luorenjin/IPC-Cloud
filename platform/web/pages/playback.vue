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

// 点击时间轴：已有会话则 seek，否则开新会话
async function onTimelineClick(e: MouseEvent) {
  if (!channelId.value) {
    ElMessage.warning('请先选择通道')
    return
  }
  const el = e.currentTarget as HTMLElement
  const ratio = (e.clientX - el.getBoundingClientRect().left) / el.clientWidth
  const ts = Math.round((dayStart.value + ratio * DAY) / 1000) * 1000
  if (session.value) seekTo(ts)
  else startPlay(ts)
}

async function startPlay(ts: number) {
  try {
    const res: any = await api.post(`/channels/${channelId.value}/playback`, {
      start: ts, end: dayStart.value + DAY, source: source.value
    })
    startTs.value = ts
    session.value = res
    paused.value = false
    speed.value = 1
  } catch (e: any) {
    if (e.code === 'E6003') ElMessage.error('该设备不支持设备端回放')
    else ElMessage.error(errText(e))
  }
}

async function seekTo(ts: number) {
  try {
    await api.put(`/playback/${session.value.sessionId}`, { op: 'seek', seekTs: ts, baseTs: startTs.value })
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

async function togglePause() {
  if (!session.value) return
  const op = paused.value ? 'resume' : 'pause'
  try {
    await api.put(`/playback/${session.value.sessionId}`, { op })
    paused.value = !paused.value
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

async function onSpeedChange(v: any) {
  if (!session.value) return
  try {
    await api.put(`/playback/${session.value.sessionId}`, { op: 'speed', speed: Number(v) })
  } catch (e: any) {
    ElMessage.error(errText(e))
  }
}

async function closeSession() {
  if (!session.value) return
  const sid = session.value.sessionId
  session.value = null
  paused.value = false
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
const platformUrl = computed(() =>
  session.value && sessionSource.value === 'platform' ? session.value.url || '' : ''
)

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
        </div>
      </div>

      <!-- 播放器区域 -->
      <div class="player-area">
        <H265Player v-if="deviceUrl" :url="deviceUrl" :title="curChannelName" muted />
        <video v-else-if="platformUrl" :src="platformUrl" controls autoplay class="video" />
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