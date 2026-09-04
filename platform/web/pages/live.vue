<script setup lang="ts">
// 实时预览（LIVE-01~07）：左侧通道树 + 1/4 分屏网格 + 抓图 + PTZ 云台面板
const api = useApi()

// ---------- 通道树数据（两级：设备 → 通道） ----------
interface Channel {
  id: string
  name: string
  deviceId: string
  streamState: string
  enabled: boolean
}
const devices = ref<any[]>([])
const channels = ref<Channel[]>([])

const treeData = computed(() =>
  devices.value.map((d) => ({
    key: 'dev-' + d.id,
    label: d.name,
    isChannel: false,
    children: channels.value
      .filter((c) => c.deviceId === d.id)
      .map((c) => ({ key: c.id, label: c.name, isChannel: true, channel: c }))
  }))
)

// 在线圆点判定（按 streamState）
function isOnline(c: Channel) {
  return ['online', 'active', 'on', 'streaming', 'playing'].includes(
    String(c.streamState || '').toLowerCase()
  )
}

async function loadTree() {
  try {
    const pid = api.project().value
    const [dRes, cRes]: any[] = await Promise.all([
      api.get('/devices'),
      api.get('/channels', pid ? { projectId: pid } : undefined)
    ])
    devices.value = dRes.items || dRes || []
    channels.value = cRes.items || cRes || []
  } catch (e: any) {
    ElMessage.error(`${e.code} ${e.msg} ${e.suggest || ''}`)
  }
}

// ---------- 分屏与格子状态 ----------
interface Cell {
  channel: Channel | null
  url: string
  profile: 'main' | 'sub'
}
const blank = (): Cell => ({ channel: null, url: '', profile: 'main' })
const grid = ref<1 | 4>(1)
const cells = ref<Cell[]>([blank()])
const selected = ref(0)
const players = ref<any[]>([])
const wrap = ref<HTMLElement>()

const curCell = computed(() => cells.value[selected.value] || cells.value[0])

// 分屏数量变化时保留已有画面
function applyGrid(g: 1 | 4) {
  const old = cells.value
  if (g === 1) {
    cells.value = [old[0] || blank()]
  } else {
    cells.value = Array.from({ length: 4 }, (_, i) => old[i] || blank())
  }
  if (selected.value >= g) selected.value = 0
}

// 布局记忆（localStorage）
watch(grid, (g) => {
  applyGrid(g)
  try { localStorage.setItem('ipc_live_layout', String(g)) } catch {}
})

onMounted(() => {
  try {
    if (Number(localStorage.getItem('ipc_live_layout')) === 4) grid.value = 4
  } catch {}
  loadTree()
})

// ---------- 播放：点通道起流 ----------
// 按 https/wss 与 http/ws 选择拉流地址
function pickFlv(res: any): string {
  const https = location.protocol === 'https:'
  return https ? res.wssFlv || res.wsFlv || '' : res.wsFlv || res.wssFlv || ''
}

async function playInto(cell: Cell, ch: Channel) {
  try {
    const res: any = await api.post(`/channels/${ch.id}/play`, { profile: cell.profile })
    cell.channel = ch
    cell.url = pickFlv(res)
  } catch (e: any) {
    cell.channel = null
    cell.url = ''
    ElMessage.error(`${e.code} ${e.msg} ${e.suggest || ''}`)
  }
}

function onNodeClick(data: any) {
  if (data.isChannel && curCell.value) playInto(curCell.value, data.channel)
}

// 清晰度切换（主/子码流）：切完重新起流
function switchProfile(cell: Cell, v: any) {
  cell.profile = v === 'sub' ? 'sub' : 'main'
  if (cell.channel) playInto(cell, cell.channel)
}

// ---------- 工具栏 ----------
function fullscreen() {
  try { wrap.value?.requestFullscreen?.() } catch {}
}
function doSnapshot() {
  players.value[selected.value]?.snapshot?.()
}

// ---------- PTZ 云台控制 ----------
const ptzSpeed = ref(4)
const dirs = [
  { label: '↖', pan: -1, tilt: 1, cls: 'nw' },
  { label: '↑', pan: 0, tilt: 1, cls: 'n' },
  { label: '↗', pan: 1, tilt: 1, cls: 'ne' },
  { label: '←', pan: -1, tilt: 0, cls: 'w' },
  { label: '→', pan: 1, tilt: 0, cls: 'e' },
  { label: '↙', pan: -1, tilt: -1, cls: 'sw' },
  { label: '↓', pan: 0, tilt: -1, cls: 's' },
  { label: '↘', pan: 1, tilt: -1, cls: 'se' }
]

async function ptzCmd(body: any) {
  const ch = curCell.value?.channel
  if (!ch) return
  try {
    await api.post(`/channels/${ch.id}/ptz`, body)
  } catch (e: any) {
    ElMessage.error(`${e.code} ${e.msg} ${e.suggest || ''}`)
  }
}
// 按住移动
function ptzStart(d: any) {
  ptzCmd({ op: 'move', pan: d.pan, tilt: d.tilt, speed: ptzSpeed.value })
}
// 松开停止
function ptzStop() {
  ptzCmd({ op: 'stop' })
}
// 变倍 ±1（单步）
function ptzZoom(step: number) {
  ptzCmd({ op: 'move', zoom: step })
}
</script>

<template>
  <div class="live-page">
    <!-- 左侧通道树 -->
    <el-aside width="230px" class="ch-tree">
      <div class="tree-head">通道列表</div>
      <el-tree
        :data="treeData" node-key="key" default-expand-all
        :props="{ label: 'label', children: 'children' }" @node-click="onNodeClick"
      >
        <template #default="{ data }">
          <span class="node">
            <span v-if="data.isChannel" class="dot" :class="{ on: isOnline(data.channel) }" />
            {{ data.label }}
          </span>
        </template>
      </el-tree>
    </el-aside>

    <!-- 右侧：工具栏 + 分屏网格 -->
    <div ref="wrap" class="stage">
      <div class="toolbar">
        <el-radio-group v-model="grid" size="small">
          <el-radio-button :value="1">1 分屏</el-radio-button>
          <el-radio-button :value="4">4 分屏</el-radio-button>
        </el-radio-group>
        <el-button size="small" @click="fullscreen">全屏</el-button>
        <el-button size="small" type="primary" :disabled="!curCell?.channel" @click="doSnapshot">
          抓图
        </el-button>
      </div>

      <div class="grid-wrap">
        <div class="grid" :class="{ four: grid === 4 }">
          <div
            v-for="(cell, i) in cells" :key="i"
            class="cell" :class="{ active: i === selected }" @click="selected = i"
          >
            <H265Player :ref="(el: any) => (players[i] = el)" :url="cell.url" muted />
            <!-- 标题条：通道名 + 主/子码流切换 -->
            <div v-if="cell.channel" class="cell-bar">
              <span class="cell-name">
                {{ cell.channel.name }}（{{ cell.profile === 'main' ? '主码流' : '子码流' }}）
              </span>
              <el-switch
                size="small" :model-value="cell.profile"
                active-value="sub" inactive-value="main"
                @change="switchProfile(cell, $event)"
              />
            </div>
          </div>
        </div>

        <!-- PTZ 浮动面板（当前选中格有通道时显示） -->
        <div v-if="curCell?.channel" class="ptz">
          <div class="ptz-title">云台 · {{ curCell.channel.name }}</div>
          <div class="pad">
            <button
              v-for="d in dirs" :key="d.cls" class="pz" :class="d.cls"
              @mousedown.prevent="ptzStart(d)" @mouseup="ptzStop()" @mouseleave="ptzStop()"
            >{{ d.label }}</button>
            <span class="pad-center" />
          </div>
          <div class="ptz-row">
            <el-button size="small" @click="ptzZoom(1)">变倍 +</el-button>
            <el-button size="small" @click="ptzZoom(-1)">变倍 -</el-button>
          </div>
          <div class="ptz-row speed">
            <span class="sp-label">速度 {{ ptzSpeed }}</span>
            <el-slider v-model="ptzSpeed" :min="1" :max="10" :step="1" style="width: 110px" />
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.live-page {
  display: flex; gap: 12px;
  height: calc(100vh - 84px);
}
.ch-tree {
  background: #fff; border-radius: 6px; border: 1px solid #e4e7ed;
  overflow: auto; padding-bottom: 8px;
}
.tree-head {
  font-weight: 600; color: #303133; padding: 12px 14px 8px; font-size: 14px;
}
.node { font-size: 13px; }
.dot {
  display: inline-block; width: 8px; height: 8px; border-radius: 50%;
  background: #c0c4cc; margin-right: 6px;
}
.dot.on { background: #67c23a; }

.stage { flex: 1; min-width: 0; display: flex; flex-direction: column; }
.toolbar {
  display: flex; align-items: center; gap: 10px; margin-bottom: 10px;
}
.grid-wrap {
  position: relative; flex: 1; min-height: 0;
  background: #000; border-radius: 6px; overflow: hidden;
}
.grid {
  height: 100%; display: grid; gap: 2px;
  grid-template-columns: 1fr; grid-template-rows: 1fr;
}
.grid.four { grid-template-columns: 1fr 1fr; grid-template-rows: 1fr 1fr; }
.cell { position: relative; background: #000; border: 1px solid transparent; cursor: pointer; }
.cell.active { border-color: #409eff; }
.cell-bar {
  position: absolute; left: 0; right: 0; bottom: 0;
  display: flex; justify-content: space-between; align-items: center;
  padding: 3px 8px; background: rgba(0, 0, 0, 0.45); font-size: 12px;
}
.cell-name { color: #fff; }

/* PTZ 面板 */
.ptz {
  position: absolute; right: 12px; top: 12px; z-index: 10; width: 158px;
  background: #fff; border-radius: 8px; padding: 10px;
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.25);
}
.ptz-title { font-size: 12px; color: #303133; margin-bottom: 8px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.pad {
  display: grid; gap: 4px;
  grid-template-areas: 'nw n ne' 'w c e' 'sw s se';
  grid-template-columns: repeat(3, 34px); grid-template-rows: repeat(3, 28px);
  justify-content: center;
}
.pz {
  border: 1px solid #dcdfe6; background: #f5f7fa; border-radius: 4px;
  cursor: pointer; font-size: 13px; color: #606266; padding: 0;
}
.pz:active { background: #409eff; color: #fff; border-color: #409eff; }
.nw { grid-area: nw; } .n { grid-area: n; } .ne { grid-area: ne; }
.w { grid-area: w; } .e { grid-area: e; }
.sw { grid-area: sw; } .s { grid-area: s; } .se { grid-area: se; }
.pad-center { grid-area: c; }
.ptz-row { display: flex; justify-content: center; gap: 6px; margin-top: 8px; }
.ptz-row.speed { align-items: center; }
.sp-label { font-size: 12px; color: #606266; white-space: nowrap; }
</style>