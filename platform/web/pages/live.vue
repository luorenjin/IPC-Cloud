<script setup lang="ts">
// 实时预览（LIVE-01~07）：通道树（搜索/拖拽/双击）+ 1/4 分屏 + 清晰度 + 抓图 + PTZ（含预置位）
const api = useApi()
const toast = useToast()
const route = useRoute()

// ---------- 通道树数据（两级：设备 → 通道） ----------
interface Channel {
  id: string
  name: string
  deviceId: string
  streamState: string
  enabled: boolean
  caps?: string[]
}
const devices = ref<any[]>([])
const channels = ref<Channel[]>([])
const treeSearch = ref('')

const treeData = computed(() =>
  devices.value
    .map((d) => ({
      label: d.name,
      value: 'dev-' + d.id,
      raw: d,
      children: channels.value
        .filter((c) => c.deviceId === d.id && c.enabled !== false)
        .map((c) => ({ label: c.name, value: c.id, channel: c }))
    }))
    .filter((n) => n.children.length)
)

function isOnline(c: Channel) {
  return ['online', 'active', 'on', 'streaming', 'playing'].includes(String(c.streamState || '').toLowerCase())
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
    toastApiError(e, '通道加载失败')
  }
}

// ---------- 分屏与格子状态 ----------
interface Cell {
  channel: Channel | null
  url: string
  profile: 'main' | 'sub'
  bitrate: number
  latency: number
}
const blank = (): Cell => ({ channel: null, url: '', profile: 'main', bitrate: 0, latency: 0 })
const grid = ref<1 | 4 | 9>(1)
const cells = ref<Cell[]>([blank()])
const selected = ref(0)
const players = ref<any[]>([])
const wrap = ref<HTMLElement>()

const curCell = computed(() => cells.value[selected.value] || cells.value[0])

function applyGrid(g: 1 | 4 | 9) {
  const old = cells.value
  if (g === 1) {
    const keep = old.slice(0, 1)
    cells.value = [keep[0] || blank()]
  } else {
    cells.value = Array.from({ length: g }, (_, i) => old[i] || blank())
    // 多分屏时新接入默认选择子码流以保护性能
    cells.value.forEach((c) => { if (!c.channel) c.profile = 'sub' })
  }
  if (selected.value >= g) selected.value = 0
}

// 布局与通道记忆（LIVE-04）
function saveLayout() {
  try {
    localStorage.setItem('ipc_live_layout', String(grid.value))
    localStorage.setItem('ipc_live_cells', JSON.stringify(cells.value.map((c) => ({ ch: c.channel?.id || '', p: c.profile }))))
  } catch {}
}
watch(grid, (g) => { applyGrid(g); saveLayout() })

onMounted(async () => {
  try {
    const saved = Number(localStorage.getItem('ipc_live_layout'))
    if ([1, 4, 9].includes(saved)) grid.value = saved as any
  } catch {}
  applyGrid(grid.value)
  await loadTree()
  // 恢复上次通道
  try {
    const saved = JSON.parse(localStorage.getItem('ipc_live_cells') || '[]')
    for (const s of saved) {
      const ch = channels.value.find((c) => c.id === s.ch)
      if (ch) {
        const idx = cells.value.findIndex((c) => !c.channel)
        if (idx >= 0) { cells.value[idx].profile = s.p || 'main'; playInto(cells.value[idx], ch) }
      }
    }
  } catch {}
  // 支持从设备列表跳转直接播放（/live?channel=xxx）
  if (route.query.channel) {
    const ch = channels.value.find((c) => c.id === route.query.channel)
    if (ch) playInto(curCell.value, ch)
  }
})

// ---------- 播放：点/拖拽/双击通道起流 ----------
function pickFlv(res: any): string {
  const https = location.protocol === 'https:'
  return https ? res.wssFlv || res.wsFlv || '' : res.wsFlv || res.wssFlv || ''
}

async function playInto(cell: Cell, ch: Channel) {
  try {
    const res: any = await api.post(`/channels/${ch.id}/play`, { profile: cell.profile })
    cell.channel = ch
    cell.url = pickFlv(res)
    saveLayout()
  } catch (e: any) {
    cell.channel = null
    cell.url = ''
    toastApiError(e, '起流失败')
  }
}

// 单击树节点：在选中格播放；双击：同样（LIVE-03）
function onNodeClick(node: any) {
  if (node.channel && curCell.value) playInto(curCell.value, node.channel)
}

// 拖拽到指定格（LIVE-03：离线灰显不可拖）
function onTreeDragStart(e: DragEvent, node: any) {
  if (!node.channel) { e.preventDefault?.(); return }
  if (!isOnline(node.channel)) { e.preventDefault?.(); return }
  e.dataTransfer?.setData('text/channel', node.channel.id)
}
function onCellDrop(e: DragEvent, i: number) {
  const id = e.dataTransfer?.getData('text/channel')
  if (!id) return
  const ch = channels.value.find((c) => c.id === id)
  if (ch) { selected.value = i; playInto(cells.value[i], ch) }
}

// 清晰度切换（LIVE-05，能力不足置灰）
function canSub(cell: Cell) {
  const caps = cell.channel?.caps || []
  return !caps.length || caps.some((c) => c.startsWith('live.sub') || c === 'live')
}
function switchProfile(cell: Cell, v: any) {
  cell.profile = v === 'sub' ? 'sub' : 'main'
  if (cell.channel) playInto(cell, cell.channel)
}

function closeCell(cell: Cell) {
  cell.channel = null
  cell.url = ''
  saveLayout()
}

// ---------- 工具栏 ----------
function fullscreen() {
  try { wrap.value?.requestFullscreen?.() } catch {}
}
function doSnapshot() {
  const p = players.value[selected.value]
  if (p?.snapshot) p.snapshot()
  else toast.warning('当前画面不可截图')
}
function closeAll() {
  cells.value.forEach((c) => { c.channel = null; c.url = '' })
  saveLayout()
}

// ---------- PTZ 云台控制（LIVE-07） ----------
const ptzPanel = ref(true)
const ptzSpeed = ref(4)
const dirs = [
  { icon: 'arrow-up', rot: -45, pan: -1, tilt: 1, cls: 'nw' },
  { icon: 'arrow-up', rot: 0, pan: 0, tilt: 1, cls: 'n' },
  { icon: 'arrow-up', rot: 45, pan: 1, tilt: 1, cls: 'ne' },
  { icon: 'arrow-left', rot: 0, pan: -1, tilt: 0, cls: 'w' },
  { icon: 'arrow-right', rot: 0, pan: 1, tilt: 0, cls: 'e' },
  { icon: 'arrow-down', rot: 45, pan: -1, tilt: -1, cls: 'sw' },
  { icon: 'arrow-down', rot: 0, pan: 0, tilt: -1, cls: 's' },
  { icon: 'arrow-down', rot: -45, pan: 1, tilt: -1, cls: 'se' }
]

async function ptzCmd(body: any) {
  const ch = curCell.value?.channel
  if (!ch) return
  try {
    await api.post(`/channels/${ch.id}/ptz`, body)
  } catch (e: any) {
    toastApiError(e, '云台指令失败')
  }
}
function ptzStart(d: any) {
  ptzCmd({ op: 'move', pan: d.pan, tilt: d.tilt, speed: ptzSpeed.value })
}
function ptzStop() {
  ptzCmd({ op: 'stop' })
}
function ptzZoom(step: number) {
  ptzCmd({ op: 'move', zoom: step })
}
function ptzFocus(step: number) {
  ptzCmd({ op: 'move', focus: step })
}

// 预置位（LIVE-07：增删改调用 ≤255）
const presets = ref<any[]>([])
const presetName = ref('')
async function loadPresets() {
  const ch = curCell.value?.channel
  if (!ch) { presets.value = []; return }
  try {
    const res: any = await api.get(`/channels/${ch.id}/ptz/presets`)
    presets.value = res.items || res.presets || []
  } catch { presets.value = [] }
}
watch(() => curCell.value?.channel?.id, loadPresets)
async function addPreset() {
  const ch = curCell.value?.channel
  if (!ch) return
  try {
    await api.post(`/channels/${ch.id}/ptz/presets`, { name: presetName.value || ('预置位' + (presets.value.length + 1)) })
    toast.success('预置位已保存')
    presetName.value = ''
    loadPresets()
  } catch (e: any) { toastApiError(e, '保存失败') }
}
async function gotoPreset(p: any) {
  const ch = curCell.value?.channel
  if (!ch) return
  try { await api.post(`/channels/${ch.id}/ptz/preset/goto`, { id: p.id ?? p.index }) } catch (e: any) { toastApiError(e, '调用失败') }
}
async function delPreset(p: any) {
  const ch = curCell.value?.channel
  if (!ch) return
  const ok = await useConfirm().ask({ title: '删除预置位', message: `确定删除「${p.name || p.id}」？`, danger: true })
  if (!ok) return
  try { await api.request(`/channels/${ch.id}/ptz/presets/${p.id ?? p.index}`, { method: 'DELETE' }); loadPresets() } catch (e: any) { toastApiError(e, '删除失败') }
}
</script>

<template>
  <div class="flex h-[calc(100vh-84px)] gap-3">
    <!-- 左：通道树（搜索 + 在线圆点 + 拖拽） -->
    <div class="flex w-58 shrink-0 flex-col overflow-hidden rounded border border-line bg-surface" style="width: 232px">
      <div class="border-b border-line-soft p-3">
        <p class="mb-2 text-sm font-semibold text-ink">通道列表</p>
        <UiInput v-model="treeSearch" placeholder="搜索通道" size="sm" clearable><template #prefix><Icon name="search" :size="13" class="text-placeholder" /></template></UiInput>
      </div>
      <div class="min-h-0 flex-1 overflow-y-auto p-2">
        <UiTree :nodes="treeData" :search="treeSearch" @select="onNodeClick">
          <template #node="{ node }">
            <span
              class="flex min-w-0 items-center gap-1.5"
              :draggable="!!node.channel && isOnline(node.channel)"
              @dragstart="onTreeDragStart($event, node)"
            >
              <span v-if="node.channel" class="h-2 w-2 shrink-0 rounded-full" :class="isOnline(node.channel) ? 'bg-success' : 'bg-[#c9cdd4]'" />
              <span class="truncate" :class="node.channel && !isOnline(node.channel) ? 'text-placeholder' : ''">{{ node.label }}</span>
            </span>
          </template>
        </UiTree>
        <UiEmptyState v-if="!treeData.length" text="暂无通道" icon="video" />
      </div>
    </div>

    <!-- 右：工具栏 + 分屏网格 + PTZ -->
    <div class="flex min-w-0 flex-1 flex-col">
      <div class="mb-2.5 flex flex-wrap items-center gap-2">
        <UiSegmented
          :model-value="String(grid)" @update:model-value="grid = Number($event) as any"
          :items="[{ label: '1 分屏', value: '1' }, { label: '4 分屏', value: '4' }, { label: '9 分屏', value: '9' }]"
        />
        <UiButton size="sm" @click="fullscreen"><Icon name="maximize" :size="13" />全屏</UiButton>
        <UiButton size="sm" variant="primary" :disabled="!curCell?.channel" @click="doSnapshot"><Icon name="camera" :size="13" />抓图</UiButton>
        <UiButton size="sm" @click="closeAll"><Icon name="x" :size="13" />全部关闭</UiButton>
        <UiButton size="sm" :disabled="!curCell?.channel" @click="ptzPanel = !ptzPanel">
          <Icon name="crosshair" :size="13" />云台
        </UiButton>
        <span class="ml-auto text-xs text-placeholder">双击或拖拽通道到画面格播放</span>
      </div>

      <div ref="wrap" class="relative min-h-0 flex-1 overflow-hidden rounded border border-line bg-black">
        <div class="grid h-full gap-0.5" :class="grid === 9 ? 'grid-cols-3 grid-rows-3' : grid === 4 ? 'grid-cols-2 grid-rows-2' : 'grid-cols-1 grid-rows-1'">
          <div
            v-for="(cell, i) in cells" :key="i"
            class="group/cell relative cursor-pointer bg-black"
            :class="i === selected ? 'outline outline-2 -outline-offset-2 outline-primary' : ''"
            @click="selected = i"
            @dragover.prevent
            @drop="onCellDrop($event, i)"
          >
            <H265Player
              v-if="cell.url" :ref="(el: any) => (players[i] = el)" :url="cell.url" muted
              @retry="cell.channel && playInto(cell, cell.channel)"
            />
            <div v-else class="flex h-full flex-col items-center justify-center gap-2 text-[#4e5969]">
              <Icon name="video" :size="30" :stroke="1.4" />
              <span class="text-xs">双击左侧通道或拖拽到此处播放</span>
            </div>

            <!-- 标题条（LIVE-01：通道名 + 码流 + 关闭） -->
            <div v-if="cell.channel" class="absolute inset-x-0 top-0 flex items-center justify-between bg-gradient-to-b from-black/60 to-transparent px-2.5 py-1.5 opacity-0 transition-opacity group-hover/cell:opacity-100" :class="i === selected ? 'opacity-100' : ''">
              <span class="truncate text-xs text-white">{{ cell.channel.name }}<span class="ml-1.5 text-[#c9cdd4]">{{ cell.profile === 'main' ? '主码流' : '子码流' }}</span></span>
              <button class="rounded p-0.5 text-[#c9cdd4] hover:bg-white/15 hover:text-white" @click.stop="closeCell(cell)">
                <Icon name="x" :size="13" />
              </button>
            </div>
            <!-- 底部工具（清晰度切换，LIVE-05） -->
            <div v-if="cell.channel && i === selected" class="absolute inset-x-0 bottom-0 flex items-center justify-end gap-1 bg-gradient-to-t from-black/60 to-transparent px-2.5 py-1.5">
              <button
                class="rounded border border-[#4e5969] px-2 py-0.5 text-[11px] text-white transition-colors hover:border-primary"
                :class="!canSub(cell) ? 'cursor-not-allowed opacity-40' : cell.profile === 'sub' ? 'border-primary bg-primary/20' : ''"
                :disabled="!canSub(cell)"
                @click.stop="switchProfile(cell, cell.profile === 'main' ? 'sub' : 'main')"
              >{{ cell.profile === 'main' ? '子码流' : '主码流' }}</button>
            </div>
          </div>
        </div>

        <!-- PTZ 浮动面板 -->
        <div v-if="ptzPanel && curCell?.channel" class="absolute right-3 top-3 z-10 w-44 rounded border border-line bg-surface p-3 shadow-pop">
          <div class="mb-2 flex items-center justify-between">
            <span class="truncate text-xs font-medium text-ink">云台 · {{ curCell.channel.name }}</span>
            <button class="text-placeholder hover:text-body" @click="ptzPanel = false"><Icon name="x" :size="13" /></button>
          </div>
          <div class="mx-auto grid w-fit grid-cols-3 gap-1">
            <template v-for="d in dirs" :key="d.cls">
              <button
                v-if="d.cls !== 'c'"
                class="flex h-8 w-9 items-center justify-center rounded border border-line bg-canvas text-muted transition-colors hover:border-primary hover:bg-primary-soft hover:text-primary active:border-primary active:bg-primary active:text-white"
                @mousedown.prevent="ptzStart(d)" @mouseup="ptzStop()" @mouseleave="ptzStop()"
              >
                <Icon :name="d.icon" :size="14" :style="{ transform: `rotate(${d.rot}deg)` }" />
              </button>
            </template>
            <span class="flex h-8 w-9 items-center justify-center text-[9px] text-placeholder">PTZ</span>
          </div>
          <div class="mt-2 grid grid-cols-2 gap-1">
            <button class="h-6 rounded border border-line text-[11px] text-muted transition-colors hover:border-primary hover:text-primary" @mousedown.prevent="ptzZoom(1)" @mouseup="ptzStop()" @mouseleave="ptzStop()">变倍 +</button>
            <button class="h-6 rounded border border-line text-[11px] text-muted transition-colors hover:border-primary hover:text-primary" @mousedown.prevent="ptzZoom(-1)" @mouseup="ptzStop()" @mouseleave="ptzStop()">变倍 −</button>
            <button class="h-6 rounded border border-line text-[11px] text-muted transition-colors hover:border-primary hover:text-primary" @mousedown.prevent="ptzFocus(1)" @mouseup="ptzStop()" @mouseleave="ptzStop()">聚焦 +</button>
            <button class="h-6 rounded border border-line text-[11px] text-muted transition-colors hover:border-primary hover:text-primary" @mousedown.prevent="ptzFocus(-1)" @mouseup="ptzStop()" @mouseleave="ptzStop()">聚焦 −</button>
          </div>
          <div class="mt-2 flex items-center gap-2">
            <span class="shrink-0 text-[11px] text-muted">速度 {{ ptzSpeed }}</span>
            <UiSlider v-model="ptzSpeed" :min="1" :max="10" :step="1" />
          </div>
          <!-- 预置位 -->
          <div class="mt-2 border-t border-line-soft pt-2">
            <p class="mb-1 text-[11px] text-placeholder">预置位</p>
            <div class="mb-1.5 flex gap-1">
              <UiInput v-model="presetName" placeholder="名称" size="sm" />
              <UiButton size="sm" variant="primary" @click="addPreset"><Icon name="plus" :size="12" /></UiButton>
            </div>
            <div v-if="!presets.length" class="py-1 text-center text-[11px] text-placeholder">暂无预置位</div>
            <ul v-else class="max-h-24 space-y-0.5 overflow-y-auto">
              <li v-for="p in presets" :key="p.id ?? p.index" class="flex items-center gap-1 rounded px-1 py-0.5 text-[11px] hover:bg-zone">
                <button class="min-w-0 flex-1 truncate text-left text-body hover:text-primary" @click="gotoPreset(p)">{{ p.name || ('预置位' + (p.index ?? p.id)) }}</button>
                <button class="text-placeholder hover:text-danger" @click="delPreset(p)"><Icon name="trash" :size="11" /></button>
              </li>
            </ul>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
