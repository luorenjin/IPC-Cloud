<script lang="ts">
/**
 * 固定叠加项（通道名 / 时间）：位置与字号都可调。
 * pos 是设备侧的归一化二元组 [x, y]，x/y 为**文字区域左上角**在画面中的比例；
 * fontPx 是主码流分辨率下的像素高度（与自定义文字的 font_px 同义同区间）。
 */
export interface OsdPosItem {
  /** 位置键（osd.time.pos / osd.channelName.pos） */
  key: string
  /** 字号键（osd.time.fontPx / osd.channelName.fontPx） */
  fontKey: string
  /** 已 t() 过的名称，用于 aria 与列表 */
  label: string
  /** 预览里显示的文字（时间样例 / 通道名） */
  text: string
  /** text 只是占位示例（通道名取不到）时为真：渲染成虚线弱化样式 */
  sample?: boolean
  pos: number[]
  fontPx: number
}

/**
 * 一条自定义文字叠加（`osd.text.regions` 的数组元素）。
 * 形状与固件规则表注释、模拟器 coerceOsdTexts 三处逐字一致：{text, x, y, font_px}。
 */
export interface OsdTextItem {
  text: string
  x: number
  y: number
  /** 字号：主码流分辨率下的像素高度（与 hal_osd_cfg_t.font_px 同名同义） */
  font_px: number
}

/** 与固件/模拟器逐字对齐的字号区间：数值只写在这里，避免三处各写一个数 */
export const OSD_FONT_MIN = 12
export const OSD_FONT_MAX = 72
export const OSD_FONT_STEP = 2
export const OSD_FONT_DEFAULT = 32
/** HAL_OSD_TEXT_MAX：设备端 char[HAL_OSD_TEXT_MAX] 是按**字节**算的 */
export const OSD_TEXT_MAX_BYTES = 64
</script>

<script setup lang="ts">
/**
 * OSD 画面贴合编辑器：三类叠加项（通道名 / 时间 / 自定义文字）都可拖动定位、拖右下角缩放字号。
 *
 * 为什么用 DOM 百分比定位而不是把文字烧进 JPEG：底图是设备抓拍的一帧（/channels/:id/snapshot），
 * 叠加层用 `left: x*100%` 绝对定位后天然跟着底图缩放，不需要像素换算，也不依赖任何图片处理库；
 * 与移动侦测的 MotionRegionEditor 是同一套做法（那边画框、这边摆字），两处交互语言保持一致。
 *
 * 坐标语义严格照抄设备侧：`[x, y]` 是文字**左上角**的归一化坐标（见 hal_osd_cfg_t 的 pos），
 * 所以拖动时以元素本身的落点为锚，不做事后居中补偿——否则平台显示的位置会比设备实际渲染的偏移半个字宽。
 * 画面镜像/翻转（image.mirror / image.flip）不影响这里的坐标：OSD 是在镜像之后叠加到输出画面上的。
 *
 * 字号是这里唯一能真实还原的量：fontPx/font_px 就是主码流分辨率下的像素高度，按底图高度等比折算即可，
 * 所以预览里看到的大小与设备上一致（字形仍由设备决定）。
 */
const props = withDefaults(defineProps<{
  /** 固定叠加项，已由调用方按“开关已开 + 设备支持”过滤 */
  items: OsdPosItem[]
  /** 自定义文字列表，数组本身就是要下发给设备的值 */
  texts: OsdTextItem[]
  /** 底图（设备抓拍的一帧）。没有时退化为等比网格，不假装有图 */
  posterSrc?: string
  /** 主码流高度，用来把 font_px 折算成预览字号（取设备上的 video.0.main.h） */
  sourceHeight?: number
  /** 固定叠加项已占用的 OSD 区域数（用于说明自定义文字还能加几条） */
  chromeRegions?: number
  /** 设备每通道的 OSD 区域上限 */
  regionMax?: number
  /** 不能再添加时的原因（空串=可添加）。禁用必须给原因，否则用户不知道是自己哪里没做对 */
  addBlockedHint?: string
  disabled?: boolean
}>(), { posterSrc: '', sourceHeight: 1080, chromeRegions: 0, regionMax: 4, addBlockedHint: '', disabled: false })

const emit = defineEmits<{
  move: [key: string, pos: number[]]
  /** 固定叠加项的字号改动（key 是它的字号键） */
  resize: [key: string, fontPx: number]
  'update:texts': [texts: OsdTextItem[]]
}>()
const { t } = useI18n()

const canvasEl = ref<HTMLElement | null>(null)

const round3 = (n: number) => Math.round(n * 1000) / 1000
/** 夹在画面内：拖到画面外时收在边界上，而不是生成一个越界坐标让设备拒绝 */
function clampPos(x: number, y: number): number[] {
  return [round3(Math.min(Math.max(x, 0), 1)), round3(Math.min(Math.max(y, 0), 1))]
}
/** 取值兜底：回读值缺失或形状不对时按左上角处理，不让整块预览消失 */
function posOf(item: OsdPosItem): number[] {
  const p = item.pos
  if (!Array.isArray(p) || p.length !== 2) return [0, 0]
  return clampPos(Number(p[0]) || 0, Number(p[1]) || 0)
}
/** 自定义文字的取值兜底：只影响渲染，不回写——否则一进页面就会把“设备值”改脏 */
const xOf = (tx: OsdTextItem) => round3(Math.min(Math.max(Number(tx?.x) || 0, 0), 1))
const yOf = (tx: OsdTextItem) => round3(Math.min(Math.max(Number(tx?.y) || 0, 0), 1))
/** 字号取值兜底（三类叠加项共用）：缺失按默认档，越界夹到区间内 */
const clampFont = (px: unknown) => Math.min(Math.max(Number(px) || OSD_FONT_DEFAULT, OSD_FONT_MIN), OSD_FONT_MAX)
const fontOf = (tx: OsdTextItem) => clampFont(tx?.font_px)
const chromeFontOf = (item: OsdPosItem) => clampFont(item?.fontPx)

/**
 * 预览字号：font_px 是主码流像素高度，按画布高度等比折算后就是它在底图上应有的视觉大小。
 * 用 ResizeObserver 而不是固定值：窗口尺寸与侧栏折叠都会改变画布高度，字号必须跟着变，
 * 否则“调到 32 看着没变化”会让人以为字号没生效。
 */
const canvasH = ref(0)
let ro: ResizeObserver | null = null
onMounted(() => {
  if (!canvasEl.value || typeof ResizeObserver === 'undefined') return
  ro = new ResizeObserver((entries) => {
    const h = entries[0]?.contentRect?.height || 0
    if (h) canvasH.value = h
  })
  ro.observe(canvasEl.value)
})
onUnmounted(() => {
  ro?.disconnect()
  stopPointer()
})
/** 画面高度缺失/为 0 时按 1080p 折算，并把下限压到 9px，保证小窗口下文字仍可点击 */
function fontStyles(px: number) {
  const src = props.sourceHeight > 0 ? props.sourceHeight : 1080
  const size = canvasH.value ? Math.max(9, Math.round((px / src) * canvasH.value)) : 12
  return { fontSize: `${size}px` }
}

/** 指针位置 → 画面内的归一化坐标 */
function toNorm(e: { clientX: number; clientY: number }) {
  const box = canvasEl.value?.getBoundingClientRect()
  if (!box || !box.width || !box.height) return { x: 0, y: 0 }
  return {
    x: Math.min(Math.max((e.clientX - box.left) / box.width, 0), 1),
    y: Math.min(Math.max((e.clientY - box.top) / box.height, 0), 1)
  }
}

// ---------------- 拖拽定位 ----------------
// dx/dy 是按下点相对文字左上角的偏移（同样归一化）：没有它的话，点文字中间会让整块文字瞬间左移半个身位，
// 用户以为是自己拖歪了。两个目标（固定项 / 自定义文字）共用同一套拖拽状态，靠 kind 区分回写目标。
const drag = reactive({ kind: '' as '' | 'chrome' | 'text', ref: '', index: -1, dx: 0, dy: 0 })

function startDrag(e: { clientX: number; clientY: number }, kind: 'chrome' | 'text', ref: string, index: number, x: number, y: number) {
  if (props.disabled) return
  const p = toNorm(e)
  drag.kind = kind
  drag.ref = ref
  drag.index = index
  drag.dx = p.x - x
  drag.dy = p.y - y
  watchPointer()
}

// ---------------- 拖拽缩放字号 ----------------
// 字号用一个“相对把手”的比值来算：拖到离锚点是原来的两倍远，字号就是两倍。
// 用「到左上角锚点的距离」而不是只看横向/纵向位移：文字是成比例缩放的，
// 只认单个轴会让斜着拖时字号跟不上手，用户会觉得“拖了半天没反应”。
// 三类叠加项共用同一套手势状态，靠 kind 区分回写目标（固定项写字号键，自定义文字写数组元素）。
const resize = reactive({
  kind: '' as '' | 'chrome' | 'text',
  ref: '', index: -1,
  anchorX: 0, anchorY: 0, startDist: 0, startFont: 0
})

function startResize(e: PointerEvent, kind: 'chrome' | 'text', ref: string, index: number, startFont: number) {
  if (props.disabled) return
  // 把手是文字块的子元素：父元素的 rect 就是当前文字块的实际渲染尺寸（含行高），
  // 不依赖任何字号到像素的换算假设，拖动比值因此天然自洽。
  const box = (e.currentTarget as HTMLElement)?.parentElement?.getBoundingClientRect()
  if (!box) return
  resize.kind = kind
  resize.ref = ref
  resize.index = index
  resize.anchorX = box.left
  resize.anchorY = box.top
  resize.startDist = Math.max(4, Math.hypot(e.clientX - box.left, e.clientY - box.top))
  resize.startFont = startFont
  drag.kind = ''
  watchPointer()
}

/** 字号吸附到与滑块同一个步进网格上，避免“拖出来的值滑杆显示不出来” */
function snapFont(px: number) {
  const snapped = Math.round(px / OSD_FONT_STEP) * OSD_FONT_STEP
  return Math.min(Math.max(snapped, OSD_FONT_MIN), OSD_FONT_MAX)
}

/** 字号回写：固定项按字号键 emit 给页面，自定义文字就地改数组元素 */
function applyFont(kind: 'chrome' | 'text', ref: string, index: number, px: number) {
  if (kind === 'chrome') emit('resize', ref, px)
  else patchText(index, { font_px: px })
}

/** 缩放把手的键盘等价路径：上下键微调字号，Shift 加速（读屏用户不靠拖拽也能调） */
function onResizeKeydown(e: KeyboardEvent, kind: 'chrome' | 'text', ref: string, index: number, cur: number) {
  if (props.disabled) return
  const step = e.shiftKey ? OSD_FONT_STEP * 3 : OSD_FONT_STEP
  if (e.key === 'ArrowUp' || e.key === 'ArrowRight') {
    e.preventDefault()
    applyFont(kind, ref, index, snapFont(Math.min(cur + step, OSD_FONT_MAX)))
  } else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') {
    e.preventDefault()
    applyFont(kind, ref, index, snapFont(Math.max(cur - step, OSD_FONT_MIN)))
  }
}

function onPointerMove(e: PointerEvent) {
  if (resize.kind) {
    const dist = Math.hypot(e.clientX - resize.anchorX, e.clientY - resize.anchorY)
    applyFont(resize.kind, resize.ref, resize.index, snapFont(resize.startFont * (dist / resize.startDist)))
    return
  }
  if (!drag.kind) return
  const p = toNorm(e)
  const pos = clampPos(p.x - drag.dx, p.y - drag.dy)
  if (drag.kind === 'chrome') emit('move', drag.ref, pos)
  else patchText(drag.index, { x: pos[0], y: pos[1] })
}

function onPointerUp() {
  drag.kind = ''
  drag.index = -1
  resize.kind = ''
  resize.index = -1
  stopPointer()
}

/** 挂到 window 而不是元素上：指针拖出画面时不会丢事件（同 MotionRegionEditor） */
function watchPointer() {
  window.addEventListener('pointermove', onPointerMove)
  window.addEventListener('pointerup', onPointerUp)
}
function stopPointer() {
  window.removeEventListener('pointermove', onPointerMove)
  window.removeEventListener('pointerup', onPointerUp)
}

/** 方向键微调：拖拽之外必须有一条等价路径，键盘与读屏用户才用得上这个编辑器 */
function onKeydown(e: KeyboardEvent, kind: 'chrome' | 'text', ref: string, index: number, x: number, y: number) {
  if (props.disabled) return
  const step = e.shiftKey ? 0.05 : 0.01
  let nx = x
  let ny = y
  if (e.key === 'ArrowLeft') nx = x - step
  else if (e.key === 'ArrowRight') nx = x + step
  else if (e.key === 'ArrowUp') ny = y - step
  else if (e.key === 'ArrowDown') ny = y + step
  else return
  e.preventDefault()
  const pos = clampPos(nx, ny)
  if (kind === 'chrome') emit('move', ref, pos)
  else patchText(index, { x: pos[0], y: pos[1] })
}

// ---------------- 自定义文字的增删改 ----------------
/**
 * 就地改一条并整体回传。整数组回传而不是“按条 emit”：设备侧的键就是这一个数组，
 * 调用方（页面）拿到后原样写进 cfg.data，脏值判定、保存与回读三处因此共用同一份数据形状。
 */
function patchText(index: number, changes: Partial<OsdTextItem>) {
  if (index < 0 || index >= props.texts.length) return
  emit('update:texts', props.texts.map((tx, i) => (i === index ? { ...tx, ...changes } : tx)))
}

function addText() {
  if (props.disabled || props.addBlockedHint) return
  // 新条落点依次错开：连着加两条时不会完全叠在一起，用户还得先拉开才能看清
  const offset = (props.texts.length % 3) * 0.08
  emit('update:texts', [...props.texts, { text: '', x: 0.05, y: round3(0.25 + offset), font_px: OSD_FONT_DEFAULT }])
}

function removeText(index: number) {
  emit('update:texts', props.texts.filter((_, i) => i !== index))
}

/** UTF-8 字节数：设备端 char[] 按字节装，提示也必须是字节口径 */
function byteLen(s: string) {
  return new TextEncoder().encode(s || '').length
}
/** 单条的即时校验：空内容与超长都在行内说清，不要等设备 rejected 才让用户知道 */
function rowError(tx: OsdTextItem, i: number) {
  if (!tx.text) return t('device.config.osdTextEmptyRow', { n: i + 1 })
  if (byteLen(tx.text) > OSD_TEXT_MAX_BYTES) return t('device.config.osdTextTooLong', { n: byteLen(tx.text), max: OSD_TEXT_MAX_BYTES })
  return ''
}

const textStyle = (tx: OsdTextItem) => ({ left: `${xOf(tx) * 100}%`, top: `${yOf(tx) * 100}%` })
</script>

<template>
  <div>
    <div
      ref="canvasEl"
      class="relative aspect-video w-full touch-none overflow-hidden rounded-signal border border-line bg-zone select-none"
    >
      <img
        v-if="posterSrc" :src="posterSrc" alt=""
        class="pointer-events-none absolute inset-0 h-full w-full object-contain" draggable="false"
      >
      <div
        v-else class="pointer-events-none absolute inset-0"
        style="background-image: linear-gradient(to right, var(--color-line-soft) 1px, transparent 1px), linear-gradient(to bottom, var(--color-line-soft) 1px, transparent 1px); background-size: 10% 10%"
      />

      <!-- 固定叠加项：文字块本身是拖拽手柄，右下角圆点是缩放把手（与自定义文字同一套操作语言） -->
      <div
        v-for="item in items" :key="item.key"
        class="absolute cursor-move whitespace-nowrap rounded-[2px] px-0.5 leading-tight text-white outline-none transition-shadow ipc-focus-ring"
        :class="[
          item.sample ? 'border border-dashed border-white/60' : '',
          disabled ? 'cursor-not-allowed opacity-70' : 'hover:ring-1 hover:ring-primary focus-visible:ring-1 focus-visible:ring-primary',
          drag.kind === 'chrome' && drag.ref === item.key ? 'ring-1 ring-primary' : ''
        ]"
        :style="[{ left: `${posOf(item)[0] * 100}%`, top: `${posOf(item)[1] * 100}%` }, fontStyles(chromeFontOf(item)), { textShadow: '0 0 3px rgba(0,0,0,.9)' }]"
        :data-osd-key="item.key"
        role="group" tabindex="0"
        :aria-label="t('device.config.osdOverlayAria', { name: item.label, x: posOf(item)[0], y: posOf(item)[1], font: chromeFontOf(item) })"
        @pointerdown.stop.prevent="startDrag($event, 'chrome', item.key, -1, posOf(item)[0], posOf(item)[1])"
        @keydown="onKeydown($event, 'chrome', item.key, -1, posOf(item)[0], posOf(item)[1])"
      >
        {{ item.text }}
        <span
          class="absolute -bottom-1 -right-1 h-2.5 w-2.5 cursor-nwse-resize rounded-full border border-primary bg-surface"
          :class="disabled ? 'cursor-not-allowed opacity-50' : ''"
          role="slider" tabindex="0"
          :aria-label="t('device.config.osdResizeAria', { name: item.label })"
          :aria-valuemin="OSD_FONT_MIN" :aria-valuemax="OSD_FONT_MAX" :aria-valuenow="chromeFontOf(item)"
          :data-osd-font-resize="item.fontKey"
          @pointerdown.stop.prevent="startResize($event, 'chrome', item.fontKey, -1, chromeFontOf(item))"
          @keydown.stop="onResizeKeydown($event, 'chrome', item.fontKey, -1, chromeFontOf(item))"
        />
        <span
          v-if="resize.kind === 'chrome' && resize.ref === item.fontKey"
          class="pointer-events-none absolute -top-5 right-0 whitespace-nowrap rounded-chrome bg-primary px-1 text-[10px] leading-4 text-white"
        >{{ chromeFontOf(item) }} px</span>
      </div>

      <!-- 自定义文字：每条一个区域，字号按设备上的 font_px 折算；空内容用虚线占位块表达“这里还没写字” -->
      <div
        v-for="(tx, i) in texts" :key="`tx${i}`"
        class="absolute cursor-move whitespace-nowrap rounded-[2px] px-0.5 leading-tight text-white outline-none transition-shadow ipc-focus-ring"
        :class="[
          tx.text ? '' : 'border border-dashed border-white/60',
          disabled ? 'cursor-not-allowed opacity-70' : 'hover:ring-1 hover:ring-primary focus-visible:ring-1 focus-visible:ring-primary',
          drag.kind === 'text' && drag.index === i ? 'ring-1 ring-primary' : ''
        ]"
        :style="[textStyle(tx), fontStyles(fontOf(tx)), { textShadow: '0 0 3px rgba(0,0,0,.9)' }]"
        :data-osd-text-index="i"
        role="group" tabindex="0"
        :aria-label="t('device.config.osdTextAria', { n: i + 1, x: xOf(tx), y: yOf(tx), font: fontOf(tx) })"
        @pointerdown.stop.prevent="startDrag($event, 'text', '', i, xOf(tx), yOf(tx))"
        @keydown="onKeydown($event, 'text', '', i, xOf(tx), yOf(tx))"
      >
        {{ tx.text || t('device.config.osdTextPlaceholder', { n: i + 1 }) }}
        <!-- 缩放把手：拖它改字号（右下角，与移动侦测的区域缩放同一位置语言）。
             它是文字块的子元素，因此不破坏块自身的拖拽定位——按住文字移动、按住圆点缩放。 -->
        <span
          class="absolute -bottom-1 -right-1 h-2.5 w-2.5 cursor-nwse-resize rounded-full border border-primary bg-surface"
          :class="disabled ? 'cursor-not-allowed opacity-50' : ''"
          role="slider" tabindex="0"
          :aria-label="t('device.config.osdResizeAria', { name: t('device.config.osdTextN', { n: i + 1 }) })"
          :aria-valuemin="OSD_FONT_MIN" :aria-valuemax="OSD_FONT_MAX" :aria-valuenow="fontOf(tx)"
          :data-osd-text-resize="i"
          @pointerdown.stop.prevent="startResize($event, 'text', '', i, fontOf(tx))"
          @keydown.stop="onResizeKeydown($event, 'text', '', i, fontOf(tx))"
        />
        <!-- 缩放过程实时读数：不给的话用户只能盯着下方数字框猜拖到了多少 -->
        <span
          v-if="resize.index === i"
          class="pointer-events-none absolute -top-5 right-0 whitespace-nowrap rounded-chrome bg-primary px-1 text-[10px] leading-4 text-white"
        >{{ fontOf(tx) }} px</span>
      </div>
    </div>

    <div class="mt-3 space-y-3">
      <p v-if="posterSrc" class="text-xs text-placeholder">{{ t('device.config.osdOverlayPosterHint') }}</p>
      <p v-if="!items.length" class="text-xs text-placeholder">{{ t('device.config.osdOverlayEmpty') }}</p>
      <p v-else class="text-xs text-placeholder">{{ t('device.config.osdOverlayHint') }}</p>

      <!-- 区域用量：OSD 区域是每通道共享的资源，不说清楚就会出现“为什么加不了第 3 条” -->
      <div class="flex flex-wrap items-center gap-3 text-xs">
        <span class="w-20 shrink-0 text-muted">{{ t('device.config.osdRegionUsage') }}</span>
        <span class="font-mono tabular-nums text-body">{{ chromeRegions + texts.length }} / {{ regionMax }}</span>
        <span class="text-placeholder">{{ t('device.config.osdRegionUsageHint', { n: regionMax }) }}</span>
      </div>

      <!-- 自定义文字：列表即设备上的数组，读数与字号同行，避免“图上摆好了还要滚下去调字号” -->
      <div v-for="(tx, i) in texts" :key="`row${i}`" class="space-y-2 rounded-signal border border-line-soft p-2">
        <div class="flex flex-wrap items-center gap-2">
          <span class="w-16 shrink-0 text-xs text-muted">{{ t('device.config.osdTextN', { n: i + 1 }) }}</span>
          <UiInput
            size="sm" width="w-56" :maxlength="64" :disabled="disabled"
            :placeholder="t('device.config.osdTextContentPlaceholder')"
            :model-value="tx.text" @update:model-value="patchText(i, { text: $event })"
          />
          <span class="font-mono text-xs tabular-nums text-body">{{ xOf(tx).toFixed(3) }} · {{ yOf(tx).toFixed(3) }}</span>
          <UiButton variant="text" size="sm" :disabled="disabled" @click="removeText(i)">
            {{ t('device.config.osdTextRemove') }}
          </UiButton>
        </div>
        <div class="flex flex-wrap items-center gap-2">
          <span class="w-16 shrink-0 text-xs text-muted">{{ t('device.config.osdTextSize') }}</span>
          <UiSlider
            class="max-w-52 flex-1" :model-value="fontOf(tx)" :min="OSD_FONT_MIN" :max="OSD_FONT_MAX" :step="OSD_FONT_STEP"
            :disabled="disabled" @update:model-value="patchText(i, { font_px: $event })"
          />
          <UiInput
            type="number" size="sm" width="w-20" :disabled="disabled"
            :model-value="String(tx.font_px ?? '')" @update:model-value="patchText(i, { font_px: Number($event) })"
          />
          <span class="text-xs text-placeholder">{{ t('device.config.osdTextSizeUnit', { min: OSD_FONT_MIN, max: OSD_FONT_MAX }) }}</span>
        </div>
        <p v-if="rowError(tx, i)" class="text-xs text-danger md:pl-[4.5rem]">{{ rowError(tx, i) }}</p>
      </div>

      <div class="flex flex-wrap items-center gap-2">
        <UiButton size="sm" :disabled="disabled || !!addBlockedHint" @click="addText">
          {{ t('device.config.osdTextAdd') }}
        </UiButton>
        <span v-if="addBlockedHint" class="text-xs text-placeholder">{{ addBlockedHint }}</span>
        <span v-else class="text-xs text-placeholder">{{ t('device.config.osdTextAddHint') }}</span>
      </div>
    </div>
  </div>
</template>
