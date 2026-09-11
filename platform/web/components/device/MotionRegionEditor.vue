<script setup lang="ts">
/**
 * 移动侦测区域框选（alarm.motion.regions）。
 *
 * 值的形状与固件/模拟器契约一致：**元组数组** `[[x, y, w, h], …]`，坐标为归一化 0–1，
 * 与《IpcCloud设备接入规范》事件 payload 里的 regions 用同一种写法（见 simulator/idp/config.go
 * 的 coerceRegions，三处逐字对齐）。
 *
 * 用 DOM 百分比定位而不是 Canvas 绘制：区域框天然跟着底图缩放，不需要重算像素，
 * 也让每个框都能成为一个可聚焦的宿主元素——键盘与读屏用户才有等价的操作路径。
 */
const props = withDefaults(defineProps<{
  modelValue: number[][]
  maxRegions?: number
  /** 底图（设备抓拍的一帧）。没有时退化为等比网格，不假装有图 */
  posterSrc?: string
  disabled?: boolean
}>(), { maxRegions: 4, posterSrc: '', disabled: false })

const emit = defineEmits<{ 'update:modelValue': [v: number[][]] }>()
const { t } = useI18n()

/** 最小边长：再小就点不中、也看不出画了个什么 */
const MIN = 0.02
/** 单击空白处（没有拖动）时新建的默认边长 */
const DEFAULT_SIZE = 0.25

const canvasEl = ref<HTMLElement | null>(null)
const list = ref<number[][]>([])

const round3 = (n: number) => Math.round(n * 1000) / 1000
/** 夹在画布内：拖到画布外时收在边界上，而不是生成一个越界的坐标让设备拒绝 */
function clampRect(r: number[]): number[] {
  const w = Math.min(Math.max(r[2], MIN), 1)
  const h = Math.min(Math.max(r[3], MIN), 1)
  const x = Math.min(Math.max(r[0], 0), 1 - w)
  const y = Math.min(Math.max(r[1], 0), 1 - h)
  return [round3(x), round3(y), round3(w), round3(h)]
}

function normalize(v: unknown): number[][] {
  if (!Array.isArray(v)) return []
  return v
    .filter((r) => Array.isArray(r) && r.length === 4)
    .slice(0, props.maxRegions)
    .map((r) => clampRect((r as any[]).map((n) => Number(n) || 0)))
}

// 只按引用比较重建本地副本：deep watch 会在拖拽过程中被自己 emit 出去的值打断
watch(() => props.modelValue, (v) => {
  if (JSON.stringify(v || []) !== JSON.stringify(list.value)) list.value = normalize(v)
}, { immediate: true })

const canAdd = computed(() => !props.disabled && list.value.length < props.maxRegions)

function emitChange() {
  emit('update:modelValue', list.value.map((r) => [...r]))
}

// ---------------- 拖拽 ----------------
type DragMode = 'create' | 'move' | 'resize'
const drag = reactive({ mode: '' as '' | DragMode, index: -1, anchor: [0, 0], start: [0, 0], origin: [0, 0, 0, 0], moved: false })

/** 指针位置 → 画布内的归一化坐标 */
function toNorm(e: PointerEvent) {
  const box = canvasEl.value?.getBoundingClientRect()
  if (!box || !box.width || !box.height) return { x: 0, y: 0 }
  return {
    x: Math.min(Math.max((e.clientX - box.left) / box.width, 0), 1),
    y: Math.min(Math.max((e.clientY - box.top) / box.height, 0), 1)
  }
}

function onCanvasDown(e: PointerEvent) {
  if (!canAdd.value) return
  const p = toNorm(e)
  drag.mode = 'create'
  drag.index = list.value.length
  drag.anchor = [p.x, p.y]
  drag.moved = false
  // 先在按下点放一个最小框：拖动时会被撑开，单击则在抬手时换成默认大小
  list.value = [...list.value, [round3(p.x), round3(p.y), MIN, MIN]]
  watchPointer()
}

function onRectDown(e: PointerEvent, i: number, mode: 'move' | 'resize') {
  if (props.disabled) return
  const p = toNorm(e)
  drag.mode = mode
  drag.index = i
  drag.start = [p.x, p.y]
  drag.origin = [...list.value[i]]
  watchPointer()
}

function onPointerMove(e: PointerEvent) {
  if (!drag.mode || drag.index < 0) return
  const p = toNorm(e)
  if (drag.mode === 'create') {
    const [ax, ay] = drag.anchor
    if (Math.hypot(p.x - ax, p.y - ay) > 0.02) drag.moved = true
    list.value[drag.index] = clampRect([
      Math.min(ax, p.x), Math.min(ay, p.y),
      Math.max(Math.abs(p.x - ax), MIN), Math.max(Math.abs(p.y - ay), MIN)
    ])
  } else if (drag.mode === 'move') {
    const o = drag.origin
    list.value[drag.index] = clampRect([o[0] + (p.x - drag.start[0]), o[1] + (p.y - drag.start[1]), o[2], o[3]])
  } else {
    const o = drag.origin
    list.value[drag.index] = clampRect([o[0], o[1], Math.max(o[2] + (p.x - drag.start[0]), MIN), Math.max(o[3] + (p.y - drag.start[1]), MIN)])
  }
}

function onPointerUp() {
  if (!drag.mode) return
  const i = drag.index
  // 单击空白（没有真正拖动）：给一个默认大小的框，中心落在点击处，
  // 否则用户点一下只会得到一个看不见的小方块
  if (drag.mode === 'create' && !drag.moved && i >= 0) {
    const [ax, ay] = drag.anchor
    list.value[i] = clampRect([ax - DEFAULT_SIZE / 2, ay - DEFAULT_SIZE / 2, DEFAULT_SIZE, DEFAULT_SIZE])
  }
  drag.mode = ''
  drag.index = -1
  stopPointer()
  emitChange()
}

/** 挂到 window 而不是元素上：指针拖出画布时不会丢事件 */
function watchPointer() {
  window.addEventListener('pointermove', onPointerMove)
  window.addEventListener('pointerup', onPointerUp)
}
function stopPointer() {
  window.removeEventListener('pointermove', onPointerMove)
  window.removeEventListener('pointerup', onPointerUp)
}
onUnmounted(stopPointer)

// ---------------- 增删与键盘 ----------------
function removeAt(i: number) {
  list.value = list.value.filter((_, idx) => idx !== i)
  emitChange()
}
function addDefault() {
  if (!canAdd.value) return
  const half = DEFAULT_SIZE / 2
  // 依次错开落点，连续添加时不会完全重叠成一个
  const offset = (list.value.length % 4) * 0.05
  list.value = [...list.value, clampRect([0.5 - half + offset, 0.5 - half + offset, DEFAULT_SIZE, DEFAULT_SIZE])]
  emitChange()
}

function onRectKeydown(e: KeyboardEvent, i: number) {
  if (props.disabled) return
  if (e.key === 'Delete' || e.key === 'Backspace') {
    e.preventDefault()
    removeAt(i)
    return
  }
  const step = e.shiftKey ? 0.05 : 0.01
  const r = [...list.value[i]]
  if (e.key === 'ArrowLeft') r[0] -= step
  else if (e.key === 'ArrowRight') r[0] += step
  else if (e.key === 'ArrowUp') r[1] -= step
  else if (e.key === 'ArrowDown') r[1] += step
  else return
  e.preventDefault()
  list.value[i] = clampRect(r)
  emitChange()
}

const rectStyle = (r: number[]) => ({
  left: `${r[0] * 100}%`, top: `${r[1] * 100}%`, width: `${r[2] * 100}%`, height: `${r[3] * 100}%`
})
/** 坐标只展示到 3 位小数：与设备回读的精度一致，免得界面上显示的数看着像被改过 */
const fmtRect = (r: number[]) => r.map((n) => round3(n).toFixed(3)).join(' · ')
</script>

<template>
  <div>
    <div
      ref="canvasEl"
      class="relative aspect-video w-full touch-none overflow-hidden rounded-signal border border-line bg-zone select-none"
      @pointerdown="onCanvasDown"
    >
      <img
        v-if="posterSrc" :src="posterSrc" alt=""
        class="pointer-events-none absolute inset-0 h-full w-full object-contain" draggable="false"
      >
      <!-- 没有底图时用等比网格代替：明确是"没有图"，而不是给一张假图 -->
      <div
        v-else class="pointer-events-none absolute inset-0"
        style="background-image: linear-gradient(to right, var(--color-line-soft) 1px, transparent 1px), linear-gradient(to bottom, var(--color-line-soft) 1px, transparent 1px); background-size: 10% 10%"
      />

      <div
        v-for="(r, i) in list" :key="i"
        class="absolute cursor-move border-2 border-primary bg-primary/20 outline-none ipc-focus-ring"
        :style="rectStyle(r)"
        role="group" tabindex="0"
        :aria-label="t('device.config.regionAria', { n: i + 1, x: round3(r[0]), y: round3(r[1]), w: round3(r[2]), h: round3(r[3]) })"
        @pointerdown.stop="onRectDown($event, i, 'move')"
        @keydown="onRectKeydown($event, i)"
      >
        <span class="absolute left-0 top-0 rounded-br-chrome bg-primary px-1 text-[10px] leading-4 text-white">{{ i + 1 }}</span>
        <button
          type="button"
          class="absolute -right-2 -top-2 h-4 w-4 rounded-full border border-line bg-surface text-[10px] leading-none text-danger"
          :aria-label="t('device.config.regionDelete', { n: i + 1 })"
          @pointerdown.stop @click.stop="removeAt(i)"
        >×</button>
        <span
          class="absolute -bottom-1 -right-1 h-2.5 w-2.5 cursor-nwse-resize rounded-full border border-primary bg-surface"
          @pointerdown.stop="onRectDown($event, i, 'resize')"
        />
      </div>
    </div>

    <div class="mt-3 space-y-2">
      <p v-if="posterSrc" class="text-xs text-placeholder">{{ t('device.config.regionPosterHint') }}</p>
      <p v-if="!list.length" class="text-xs text-placeholder">{{ t('device.config.regionEmpty') }}</p>
      <div v-for="(r, i) in list" :key="i" class="flex flex-wrap items-center gap-3 text-xs">
        <span class="w-16 shrink-0 text-muted">{{ t('device.config.regionN', { n: i + 1 }) }}</span>
        <span class="font-mono tabular-nums text-body">{{ fmtRect(r) }}</span>
        <UiButton variant="text" size="sm" :disabled="disabled" @click="removeAt(i)">
          {{ t('device.config.regionRemove') }}
        </UiButton>
      </div>
      <div class="flex flex-wrap items-center gap-2">
        <UiButton size="sm" :disabled="!canAdd" @click="addDefault">
          {{ t('device.config.regionAdd') }}
        </UiButton>
        <span class="text-xs text-placeholder">{{ t('device.config.regionMax', { n: maxRegions }) }}</span>
      </div>
    </div>
  </div>
</template>
