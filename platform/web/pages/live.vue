<script setup lang="ts">
// 通道树收起状态：窄屏下画面优先，树可整体收起为一条竖边（响应式，PRD 无固定宽度要求）
const treeCollapsed = ref(false)

// 实时预览（LIVE-01~07）：通道树（搜索/拖拽/双击）+ 1/4 分屏 + 清晰度 + 抓图 + PTZ（含预置位）
const api = useApi()
const toast = useToast()
const route = useRoute()
const { t } = useI18n()

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
    toastApiError(e, t('live.msg.treeLoadFailed'))
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
// 通道树"信号灯"：标记当前已上屏（在任意画面格中播放）的通道，纯展示用，复用侧栏激活态手法
const onScreenIds = computed(() => new Set(cells.value.filter((c) => c.channel).map((c) => c.channel!.id)))

function stopChannel(ch: Channel | null) {
  if (!ch) return
  api.post(`/channels/${ch.id}/stop`).catch((e: any) => console.warn('停流失败', ch.id, e))
}

function applyGrid(g: 1 | 4 | 9) {
  const old = cells.value
  if (g === 1) {
    const keep = old.slice(0, 1)
    // 缩减为单屏时，被移出的格子对应通道需停流
    old.slice(1).forEach((c) => stopChannel(c.channel))
    cells.value = [keep[0] || blank()]
  } else {
    cells.value = Array.from({ length: g }, (_, i) => old[i] || blank())
    // 格数减少时，被移除的格子对应通道需停流
    old.slice(g).forEach((c) => stopChannel(c.channel))
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
  } catch {
    // 隐私模式下不可用：布局记不住，但不影响本次观看
  }
}
watch(grid, (g) => { applyGrid(g); saveLayout() })


/* 实时状态（E8）：通道树按设备分组，设备上下线要即时反映在树上 */
useWs((ev: any) => {
  if (ev.type !== 'device.online' && ev.type !== 'device.offline') return
  const d = devices.value.find((x: any) => x.id === ev.deviceId)
  if (d) d.status = ev.type === 'device.online' ? 'online' : 'offline'
})

onMounted(async () => {
  try {
    const saved = Number(localStorage.getItem('ipc_live_layout'))
    if ([1, 4, 9].includes(saved)) grid.value = saved as any
  } catch {
    // 读不到就用默认布局
  }
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
  } catch {
    // 恢复上次画面失败（记录损坏或通道已删除）：留空画面等用户自己选，不打扰
  }
  // 支持从设备列表跳转直接播放（/live?channel=xxx）
  if (route.query.channel) {
    const ch = channels.value.find((c) => c.id === route.query.channel)
    if (ch) playInto(curCell.value, ch)
  }
})

// ---------- 播放：点/拖拽/双击通道起流 ----------
// pickFlv：按页面协议选择流地址，定义于 utils/stream.ts（Nuxt 自动导入），与预览弹窗共用
async function playInto(cell: Cell, ch: Channel) {
  try {
    const res: any = await api.post(`/channels/${ch.id}/play`, { profile: cell.profile })
    cell.channel = ch
    cell.url = pickFlv(res)
    saveLayout()
  } catch (e: any) {
    cell.channel = null
    cell.url = ''
    toastApiError(e, t('live.msg.playFailed'))
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
  stopChannel(cell.channel)
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
  else toast.warning(t('live.msg.snapshotUnavailable'))
}
function closeAll() {
  cells.value.forEach((c) => { stopChannel(c.channel); c.channel = null; c.url = '' })
  saveLayout()
}

// ---------- PTZ 云台控制（LIVE-07） ----------
const ptzPanel = ref(true)
const ptzSpeed = ref(4)
// label 供读屏与 aria-label 使用（纯图标按钮必须有可访问名称）
const dirs = [
  { icon: 'arrow-up', rot: -45, pan: -1, tilt: 1, cls: 'nw', label: 'live.ptz.upLeft' },
  { icon: 'arrow-up', rot: 0, pan: 0, tilt: 1, cls: 'n', label: 'live.ptz.up' },
  { icon: 'arrow-up', rot: 45, pan: 1, tilt: 1, cls: 'ne', label: 'live.ptz.upRight' },
  { icon: 'arrow-left', rot: 0, pan: -1, tilt: 0, cls: 'w', label: 'live.ptz.left' },
  { icon: 'arrow-right', rot: 0, pan: 1, tilt: 0, cls: 'e', label: 'live.ptz.right' },
  { icon: 'arrow-down', rot: 45, pan: -1, tilt: -1, cls: 'sw', label: 'live.ptz.downLeft' },
  { icon: 'arrow-down', rot: 0, pan: 0, tilt: -1, cls: 's', label: 'live.ptz.down' },
  { icon: 'arrow-down', rot: -45, pan: 1, tilt: -1, cls: 'se', label: 'live.ptz.downRight' }
]
// 摇杆九宫格：显式声明方向→网格坐标，贴合真实云台控制器方向布局，不依赖 dirs 数组书写顺序（纯展示）
const dirGridPos: Record<string, { col: number; row: number }> = {
  nw: { col: 1, row: 1 }, n: { col: 2, row: 1 }, ne: { col: 3, row: 1 },
  w: { col: 1, row: 2 }, e: { col: 3, row: 2 },
  sw: { col: 1, row: 3 }, s: { col: 2, row: 3 }, se: { col: 3, row: 3 }
}

async function ptzCmd(body: any) {
  const ch = curCell.value?.channel
  if (!ch) return
  try {
    await api.post(`/channels/${ch.id}/ptz`, body)
  } catch (e: any) {
    toastApiError(e, t('live.msg.ptzFailed'))
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
    await api.post(`/channels/${ch.id}/ptz/presets`, { name: presetName.value || t('live.ptz.presetDefaultName', { n: presets.value.length + 1 }) })
    toast.success(t('live.msg.presetSaved'))
    presetName.value = ''
    loadPresets()
  } catch (e: any) { toastApiError(e, t('live.msg.presetSaveFailed')) }
}
async function gotoPreset(p: any) {
  const ch = curCell.value?.channel
  if (!ch) return
  try { await api.post(`/channels/${ch.id}/ptz/preset/goto`, { id: p.id ?? p.index }) } catch (e: any) { toastApiError(e, t('live.msg.presetGotoFailed')) }
}
async function delPreset(p: any) {
  const ch = curCell.value?.channel
  if (!ch) return
  const ok = await useConfirm().ask({ title: t('live.ptz.presetDeleteTitle'), message: t('live.ptz.presetDeleteConfirm', { name: p.name || p.id }), danger: true })
  if (!ok) return
  try { await api.request(`/channels/${ch.id}/ptz/presets/${p.id ?? p.index}`, { method: 'DELETE' }); loadPresets() } catch (e: any) { toastApiError(e, t('live.msg.presetDeleteFailed')) }
}
</script>

<template>
  <div class="flex h-[calc(100vh-84px)] gap-3">
    <!-- 左：通道树（搜索 + 在线圆点 + 拖拽 + 上屏信号灯） -->
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
          <p class="text-sm font-semibold text-ink">{{ t('live.tree.title') }}</p>
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
        <UiInput v-model="treeSearch" :placeholder="t('live.tree.searchPlaceholder')" size="sm" clearable><template #prefix><Icon name="search" :size="13" class="text-placeholder" /></template></UiInput>
      </div>
      <div class="min-h-0 flex-1 overflow-y-auto p-2">
        <UiTree :nodes="treeData" :search="treeSearch" @select="onNodeClick">
          <template #node="{ node }">
            <span
              class="relative flex min-w-0 items-center gap-1.5"
              :draggable="!!node.channel && isOnline(node.channel)"
              @dragstart="onTreeDragStart($event, node)"
            >
              <!-- 信号灯：该通道当前已上屏——复用侧栏激活态手法（细竖线 + 变色），而非整块高亮胶囊 -->
              <span v-if="node.channel && onScreenIds.has(node.channel.id)" class="absolute -left-2 top-1/2 h-3.5 w-0.5 -translate-y-1/2 rounded-full bg-primary" />
              <span v-if="node.channel" class="h-2 w-2 shrink-0 rounded-full" :class="isOnline(node.channel) ? 'bg-success' : 'bg-placeholder'" />
              <span
                class="truncate"
                :class="node.channel && !isOnline(node.channel) ? 'text-placeholder' : node.channel && onScreenIds.has(node.channel.id) ? 'font-medium text-primary' : ''"
              >{{ node.label }}</span>
            </span>
          </template>
        </UiTree>
        <UiEmptyState v-if="!treeData.length" :text="t('live.tree.empty')" icon="video" />
      </div>
    </div>

    <!-- 右：视频宫格（Hero，唯一允许"发光"的界面元素） + 底部机身工具条 -->
    <div class="flex min-w-0 flex-1 flex-col">
      <div ref="wrap" class="relative min-h-0 flex-1 overflow-hidden rounded-signal border border-line bg-black">
        <div class="grid h-full gap-0.5" :class="grid === 9 ? 'grid-cols-3 grid-rows-3' : grid === 4 ? 'grid-cols-2 grid-rows-2' : 'grid-cols-1 grid-rows-1'">
          <div
            v-for="(cell, i) in cells" :key="i"
            class="group/cell relative cursor-pointer bg-black"
            :class="i === selected ? 'shadow-[inset_0_0_0_2px_var(--color-primary),inset_0_0_22px_-4px_var(--color-primary)]' : ''"
            @click="selected = i"
            @dragover.prevent
            @drop="onCellDrop($event, i)"
          >
            <H265Player
              v-if="cell.url" :ref="(el: any) => (players[i] = el)" :url="cell.url" muted
              @retry="cell.channel && playInto(cell, cell.channel)"
            />
            <div v-else class="flex h-full flex-col items-center justify-center gap-2 text-placeholder">
              <Icon name="video" :size="30" :stroke="1.4" />
              <span class="text-xs">{{ t('live.player.dropHint') }}</span>
            </div>

            <!-- 标题条（LIVE-01：通道名 + 码流 + 关闭） -->
            <div v-if="cell.channel" class="absolute inset-x-0 top-0 flex items-center justify-between bg-gradient-to-b from-black/60 to-transparent px-2.5 py-1.5 opacity-0 transition-opacity group-hover/cell:opacity-100" :class="i === selected ? 'opacity-100' : ''">
              <span class="truncate text-xs text-white">{{ cell.channel.name }}<span class="ml-1.5 text-white/70">{{ cell.profile === 'main' ? t('live.player.mainStream') : t('live.player.subStream') }}</span></span>
              <button type="button" class="rounded-chrome p-0.5 text-white/70 hover:bg-white/15 hover:text-white" :aria-label="t('live.player.closeCell', { name: cell.channel.name })" @click.stop="closeCell(cell)">
                <Icon name="x" :size="13" />
              </button>
            </div>
          </div>
        </div>

        <!-- PTZ 浮动面板：九宫格摇杆造型 -->
        <div v-if="ptzPanel && curCell?.channel" class="absolute right-3 top-3 z-10 w-44 rounded-chrome border border-line bg-surface-2 p-3 shadow-pop">
          <div class="mb-2 flex items-center justify-between">
            <span class="truncate text-xs font-medium text-ink">{{ t('live.ptz.panelTitle', { name: curCell.channel.name }) }}</span>
            <button type="button" class="rounded-chrome text-placeholder hover:text-body" :aria-label="t('live.ptz.closePanel')" @click="ptzPanel = false"><Icon name="x" :size="13" /></button>
          </div>
          <!-- 摇杆：圆形裁切 + 3x3 显式坐标，贴合真实云台控制器方向布局；中心为停止钮 -->
          <div class="mx-auto grid h-28 w-28 grid-cols-3 grid-rows-3 overflow-hidden rounded-full border border-line bg-canvas">
            <button
              v-for="d in dirs" :key="d.cls"
              type="button"
              class="flex items-center justify-center border border-line-soft/70 text-muted transition-colors hover:bg-primary-soft hover:text-primary active:bg-primary active:text-white"
              :style="{ gridColumn: dirGridPos[d.cls].col, gridRow: dirGridPos[d.cls].row }"
              :aria-label="t('live.ptz.moveTo', { dir: t(d.label) })"
              @mousedown.prevent="ptzStart(d)" @mouseup="ptzStop()" @mouseleave="ptzStop()"
            >
              <Icon :name="d.icon" :size="13" :style="{ transform: `rotate(${d.rot}deg)` }" />
            </button>
            <button
              type="button"
              class="flex items-center justify-center border border-line-soft/70 bg-surface text-primary transition-colors hover:bg-primary-soft active:bg-primary active:text-white"
              style="grid-column: 2; grid-row: 2"
              :aria-label="t('live.ptz.stop')"
              :title="t('live.ptz.stopShort')"
              @click="ptzStop()"
            >
              <Icon name="crosshair" :size="14" />
            </button>
          </div>
          <div class="mt-2 grid grid-cols-2 gap-1">
            <button class="h-6 rounded-chrome border border-line text-[11px] text-muted transition-colors hover:border-primary hover:text-primary" @mousedown.prevent="ptzZoom(1)" @mouseup="ptzStop()" @mouseleave="ptzStop()">{{ t('live.ptz.zoomIn') }}</button>
            <button class="h-6 rounded-chrome border border-line text-[11px] text-muted transition-colors hover:border-primary hover:text-primary" @mousedown.prevent="ptzZoom(-1)" @mouseup="ptzStop()" @mouseleave="ptzStop()">{{ t('live.ptz.zoomOut') }}</button>
            <button class="h-6 rounded-chrome border border-line text-[11px] text-muted transition-colors hover:border-primary hover:text-primary" @mousedown.prevent="ptzFocus(1)" @mouseup="ptzStop()" @mouseleave="ptzStop()">{{ t('live.ptz.focusNear') }}</button>
            <button class="h-6 rounded-chrome border border-line text-[11px] text-muted transition-colors hover:border-primary hover:text-primary" @mousedown.prevent="ptzFocus(-1)" @mouseup="ptzStop()" @mouseleave="ptzStop()">{{ t('live.ptz.focusFar') }}</button>
          </div>
          <div class="mt-2 flex items-center gap-2">
            <span class="shrink-0 text-[11px] text-muted">{{ t('live.ptz.speed', { n: ptzSpeed }) }}</span>
            <UiSlider v-model="ptzSpeed" :min="1" :max="10" :step="1" />
          </div>
          <!-- 预置位 -->
          <div class="mt-2 border-t border-line-soft pt-2">
            <p class="mb-1 text-[11px] text-placeholder">{{ t('live.ptz.presets') }}</p>
            <div class="mb-1.5 flex gap-1">
              <UiInput v-model="presetName" :placeholder="t('live.ptz.presetNamePlaceholder')" size="sm" />
              <UiButton size="sm" variant="primary" @click="addPreset"><Icon name="plus" :size="12" /></UiButton>
            </div>
            <div v-if="!presets.length" class="py-1 text-center text-[11px] text-placeholder">{{ t('live.ptz.presetEmpty') }}</div>
            <ul v-else class="max-h-24 space-y-0.5 overflow-y-auto">
              <li v-for="p in presets" :key="p.id ?? p.index" class="flex items-center gap-1 rounded-chrome px-1 py-0.5 text-[11px] hover:bg-zone">
                <button class="min-w-0 flex-1 truncate text-left text-body hover:text-primary" @click="gotoPreset(p)">{{ p.name || t('live.ptz.presetDefaultName', { n: p.index ?? p.id }) }}</button>
                <button type="button" class="rounded-chrome text-placeholder hover:text-danger" :aria-label="t('live.ptz.presetDelete', { name: p.name || p.id })" @click="delPreset(p)"><Icon name="trash" :size="11" /></button>
              </li>
            </ul>
          </div>
        </div>
      </div>

      <!-- 底部工具条：监视器机身控制按钮式扁平图标条（分屏/清晰度/截图/云台/关闭） -->
      <div class="mt-2.5 flex h-11 shrink-0 items-center gap-1.5 overflow-x-auto rounded-chrome border border-line bg-surface-2 px-2.5">
        <UiSegmented
          :model-value="String(grid)" @update:model-value="grid = Number($event) as any"
          :items="[{ label: t('live.player.grid1'), value: '1' }, { label: t('live.player.grid4'), value: '4' }, { label: t('live.player.grid9'), value: '9' }]"
        />
        <span class="mx-1 h-5 w-px shrink-0 bg-line" />
        <button
          class="flex h-7 shrink-0 items-center gap-1.5 rounded-chrome px-2.5 text-xs transition-colors disabled:cursor-not-allowed disabled:opacity-40"
          :class="curCell?.profile === 'sub' ? 'bg-primary-soft text-primary' : 'text-muted hover:bg-zone hover:text-primary'"
          :disabled="!curCell?.channel || !canSub(curCell)"
          @click="switchProfile(curCell, curCell?.profile === 'main' ? 'sub' : 'main')"
        ><Icon name="sliders" :size="13" />{{ curCell?.profile === 'sub' ? t('live.player.subStream') : t('live.player.mainStream') }}</button>
        <button
          class="flex h-7 shrink-0 items-center gap-1.5 rounded-chrome px-2.5 text-xs text-muted transition-colors hover:bg-zone hover:text-primary disabled:cursor-not-allowed disabled:opacity-40 disabled:hover:bg-transparent disabled:hover:text-muted"
          :disabled="!curCell?.channel"
          @click="doSnapshot"
        ><Icon name="camera" :size="13" />{{ t('live.player.snapshot') }}</button>
        <button
          class="flex h-7 shrink-0 items-center gap-1.5 rounded-chrome px-2.5 text-xs text-muted transition-colors hover:bg-zone hover:text-primary"
          @click="fullscreen"
        ><Icon name="maximize" :size="13" />{{ t('live.player.fullscreen') }}</button>
        <span class="mx-1 h-5 w-px shrink-0 bg-line" />
        <button
          class="flex h-7 shrink-0 items-center gap-1.5 rounded-chrome px-2.5 text-xs transition-colors disabled:cursor-not-allowed disabled:opacity-40"
          :class="ptzPanel ? 'bg-primary-soft text-primary' : 'text-muted hover:bg-zone hover:text-primary'"
          :disabled="!curCell?.channel"
          @click="ptzPanel = !ptzPanel"
        ><Icon name="crosshair" :size="13" />{{ t('live.ptz.title') }}</button>
        <span class="mx-1 h-5 w-px shrink-0 bg-line" />
        <button
          class="flex h-7 shrink-0 items-center gap-1.5 rounded-chrome px-2.5 text-xs text-muted transition-colors hover:bg-zone hover:text-primary"
          @click="closeAll"
        ><Icon name="power" :size="13" />{{ t('live.player.closeAll') }}</button>
        <span class="ml-auto shrink-0 text-xs text-placeholder">{{ t('live.player.toolbarHint') }}</span>
      </div>
    </div>
  </div>
</template>
