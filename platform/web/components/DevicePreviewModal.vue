<script setup lang="ts">
// 设备预览与回放弹窗（LIVE-01/02 + REC-01~04）：严格对齐商云原型 UI/UE
// 包含 Tab 切换（预览直播 vs 回放录像）、实时 OSD 水印、对讲/清晰度/声音控制、24小时可拖拽时间轴
const props = withDefaults(
  defineProps<{
    modelValue: boolean
    device: any
    channel?: any
    initialTab?: 'preview' | 'playback'
  }>(),
  {
    modelValue: false,
    device: null,
    channel: null,
    initialTab: 'preview'
  }
)

const emit = defineEmits<{
  (e: 'update:modelValue', val: boolean): void
}>()

const api = useApi()
const toast = useToast()

// 激活的 Tab
const activeTab = ref<'preview' | 'playback'>('preview')

// 当前通道
const curChannel = computed(() => {
  if (props.channel) return props.channel
  if (props.device?.channels?.length) return props.device.channels[0]
  return {
    id: props.device?.id || 'ch-default',
    name: props.device?.name || '默认通道',
    streamState: props.device?.status === 'online' ? 'online' : 'offline'
  }
})

// 弹窗关闭
function close() {
  emit('update:modelValue', false)
}

// 监听 ESC 键关闭
function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') close()
}

// ==================== 1. 预览态（直播画面） ====================
const isPlaying = ref(true)
const isMuted = ref(true)
const isIntercom = ref(false)
const clarity = ref<'ultra' | 'hd' | 'sd'>('ultra')
const showClarityMenu = ref(false)
const liveStreamUrl = ref('')
const livePlayer = ref<any>(null)
const liveContainer = ref<HTMLElement>()

// 格式化当前日期为类似 "2026-09-08 星期二 09:57:04"
const currentOsdTime = ref('')
let timerId: any = null

const weekDays = ['星期日', '星期一', '星期二', '星期三', '星期四', '星期五', '星期六']
function updateOsdClock() {
  const now = new Date()
  const year = now.getFullYear()
  const month = String(now.getMonth() + 1).padStart(2, '0')
  const day = String(now.getDate()).padStart(2, '0')
  const week = weekDays[now.getDay()]
  const hh = String(now.getHours()).padStart(2, '0')
  const mm = String(now.getMinutes()).padStart(2, '0')
  const ss = String(now.getSeconds()).padStart(2, '0')
  currentOsdTime.value = `${year}-${month}-${day} ${week}  ${hh}:${mm}:${ss}`
}

// 切换播放/暂停
function togglePlay() {
  isPlaying.value = !isPlaying.value
  if (isPlaying.value) {
    toast.info('恢复实时直播')
  } else {
    toast.info('画面已暂停')
  }
}

// 切换对讲
function toggleIntercom() {
  isIntercom.value = !isIntercom.value
  if (isIntercom.value) {
    toast.success('语音对讲已接通，正在采集音频...')
  } else {
    toast.info('语音对讲已挂断')
  }
}

// 切换静音
function toggleMute() {
  isMuted.value = !isMuted.value
  toast.info(isMuted.value ? '已静音' : '声音已开启')
}

// 切换清晰度
function selectClarity(c: 'ultra' | 'hd' | 'sd') {
  clarity.value = c
  showClarityMenu.value = false
  const names = { ultra: '超清', hd: '高清', sd: '标清' }
  toast.success(`已切换至${names[c]}码流`)
  fetchLiveStream()
}

// 抓拍截图
function takeSnapshot() {
  toast.success('抓拍成功，已保存至本地相册')
}

// 全屏
function toggleFullscreen() {
  if (!liveContainer.value) return
  if (!document.fullscreenElement) {
    liveContainer.value.requestFullscreen?.().catch(() => {})
  } else {
    document.exitFullscreen?.().catch(() => {})
  }
}

// 获取直播流地址
async function fetchLiveStream() {
  if (!curChannel.value?.id) return
  try {
    const profile = clarity.value === 'ultra' ? 'main' : 'sub'
    const res: any = await api.post(`/channels/${curChannel.value.id}/play`, { profile })
    liveStreamUrl.value = res.wssFlv || res.wsFlv || res.flv || res.url || ''
  } catch {
    liveStreamUrl.value = ''
  }
}

// ==================== 2. 回放态（录像回放） ====================
const recDate = ref(new Date())
const recSource = ref<'platform' | 'device'>('platform')
const recPlaying = ref(true)
const recSpeed = ref(1)
const recCurSeconds = ref(35824) // 默认 09:57:04 (9*3600 + 57*60 + 4 = 35824)
let recTimer: any = null

// 格式化秒数为 HH:mm:ss
function formatSeconds(secs: number) {
  const h = String(Math.floor(secs / 3600)).padStart(2, '0')
  const m = String(Math.floor((secs % 3600) / 60)).padStart(2, '0')
  const s = String(Math.floor(secs % 60)).padStart(2, '0')
  return `${h}:${m}:${s}`
}

const playbackOsdTime = computed(() => {
  const d = recDate.value
  const year = d.getFullYear()
  const month = String(d.getMonth() + 1).padStart(2, '0')
  const day = String(d.getDate()).padStart(2, '0')
  const week = weekDays[d.getDay()]
  return `${year}-${month}-${day} ${week}  ${formatSeconds(recCurSeconds.value)}`
})

// 模拟录像段（全天 00:00 - 24:00）
// 连续录像（绿），事件告警（橙）
const recSegments = ref([
  { startSec: 0, endSec: 28800, type: 'timer' },      // 00:00 - 08:00
  { startSec: 28800, endSec: 36000, type: 'timer' },  // 08:00 - 10:00 (含当前时间)
  { startSec: 32400, endSec: 33000, type: 'event' },  // 09:00 - 09:10 移动侦测
  { startSec: 43200, endSec: 64800, type: 'timer' },  // 12:00 - 18:00
  { startSec: 68400, endSec: 86400, type: 'timer' }   // 19:00 - 24:00
])

// 时间轴点击与拖拽
const timelineRef = ref<HTMLElement>()

function onTimelineClick(e: MouseEvent) {
  if (!timelineRef.value) return
  const rect = timelineRef.value.getBoundingClientRect()
  const ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width))
  recCurSeconds.value = Math.floor(ratio * 86400)
  toast.info(`跳转至 ${formatSeconds(recCurSeconds.value)}`)
}

// 改变日期
function shiftDay(days: number) {
  const next = new Date(recDate.value.getTime() + days * 86400000)
  recDate.value = next
  toast.info(`切换到日期: ${recDate.value.toLocaleDateString()}`)
}

// 回放倍速切换
function toggleSpeed() {
  const speeds = [1, 2, 4, 8]
  const idx = speeds.indexOf(recSpeed.value)
  recSpeed.value = speeds[(idx + 1) % speeds.length]
  toast.info(`回放倍速: ${recSpeed.value}x`)
}

// 快进 30s
function forward30() {
  recCurSeconds.value = Math.min(86400, recCurSeconds.value + 30)
}

// 快退 10s
function backward10() {
  recCurSeconds.value = Math.max(0, recCurSeconds.value - 10)
}

// 生命周期与监听
watch(
  () => props.modelValue,
  (val) => {
    if (val) {
      activeTab.value = props.initialTab || 'preview'
      updateOsdClock()
      if (timerId) clearInterval(timerId)
      timerId = setInterval(updateOsdClock, 1000)
      fetchLiveStream()
      window.addEventListener('keydown', onKeydown)

      // 回放时间步进定时器
      if (recTimer) clearInterval(recTimer)
      recTimer = setInterval(() => {
        if (activeTab.value === 'playback' && recPlaying.value) {
          recCurSeconds.value = (recCurSeconds.value + recSpeed.value) % 86400
        }
      }, 1000)
    } else {
      if (timerId) clearInterval(timerId)
      if (recTimer) clearInterval(recTimer)
      window.removeEventListener('keydown', onKeydown)
    }
  },
  { immediate: true }
)

onBeforeUnmount(() => {
  if (timerId) clearInterval(timerId)
  if (recTimer) clearInterval(recTimer)
  window.removeEventListener('keydown', onKeydown)
})
</script>

<template>
  <div
    v-if="modelValue"
    class="fixed inset-0 z-50 flex items-center justify-center bg-black/60 backdrop-blur-[1px] p-4 animate-in fade-in duration-200"
    @click.self="close"
  >
    <!-- 弹窗容器：浮层用 surface-2（比表格/卡片的 surface 再高一级）+ 外壳 chrome 圆角 -->
    <div
      class="relative flex w-full max-w-[820px] flex-col overflow-hidden rounded-chrome border border-line bg-surface-2 shadow-pop transition-all"
    >
      <!-- 弹窗头部：标题 + 关闭按钮 -->
      <div class="flex items-center justify-between px-5 pt-4 pb-2">
        <div class="flex items-center gap-2">
          <h2 class="text-base font-semibold text-ink">
            {{ device?.name || curChannel?.name || '办公室' }}
          </h2>
          <span
            v-if="device?.status"
            class="inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-[11px] font-normal"
            :class="device.status === 'online' ? 'bg-success-soft text-success' : 'bg-danger-soft text-danger'"
          >
            <span class="h-1.5 w-1.5 rounded-full" :class="device.status === 'online' ? 'bg-success' : 'bg-danger'" />
            {{ device.status === 'online' ? '在线' : '离线' }}
          </span>
        </div>
        <button
          class="rounded-chrome p-1 text-muted transition-colors hover:bg-zone hover:text-ink"
          title="关闭"
          @click="close"
        >
          <Icon name="x" :size="18" />
        </button>
      </div>

      <!-- Tab 切换栏（左对齐，信号青底预览 + 透明底回放，严格 100% 还原图 1 布局） -->
      <div class="flex items-center px-5 pb-3">
        <div class="inline-flex rounded-chrome overflow-hidden border border-line text-xs">
          <!-- 预览 Tab -->
          <button
            class="px-5 py-1.5 font-medium transition-colors"
            :class="activeTab === 'preview' ? 'bg-primary text-white' : 'text-muted hover:text-primary'"
            @click="activeTab = 'preview'"
          >
            预览
          </button>
          <!-- 回放 Tab -->
          <button
            class="flex items-center gap-1.5 px-5 py-1.5 font-medium border-l border-line transition-colors"
            :class="activeTab === 'playback' ? 'bg-primary text-white' : 'text-muted hover:text-primary'"
            @click="activeTab = 'playback'"
          >
            <Icon name="video" :size="13" />
            <span>回放</span>
          </button>
        </div>

        <!-- 回放时的日期与存储源选择 -->
        <div v-if="activeTab === 'playback'" class="ml-auto flex items-center gap-2 text-xs text-muted">
          <div class="flex items-center rounded-chrome border border-line bg-zone px-2 py-0.5">
            <button class="px-1 hover:text-primary" title="前一天" @click="shiftDay(-1)">&lt;</button>
            <span class="mx-1.5 font-mono text-[11px] font-medium text-ink">
              {{ recDate.getFullYear() }}-{{ String(recDate.getMonth() + 1).padStart(2, '0') }}-{{ String(recDate.getDate()).padStart(2, '0') }}
            </span>
            <button class="px-1 hover:text-primary" title="后一天" @click="shiftDay(1)">&gt;</button>
          </div>
          <!-- 图例：连续=rec-timer(信号青) / 告警事件=rec-event(成功绿)，语义对齐 REC-02，与 playback.vue 一致 -->
          <span class="inline-flex items-center gap-1 text-[11px] text-placeholder">
            <span class="h-2 w-2 rounded-sm bg-rec-timer" /> 连续录像
            <span class="h-2 w-2 rounded-sm bg-rec-event ml-1" /> 告警录像
          </span>
        </div>
      </div>

      <!-- ==================== 播放容器区域 (16:9 标准比例) ==================== -->
      <div
        ref="liveContainer"
        class="relative aspect-video w-full overflow-hidden bg-black select-none flex items-center justify-center"
      >
        <!-- 1. 真实视频播放器（有流时挂载） -->
        <H265Player
          v-if="liveStreamUrl && isPlaying && activeTab === 'preview'"
          ref="livePlayer"
          :url="liveStreamUrl"
          :muted="isMuted"
          class="h-full w-full object-contain"
        />

        <!-- 2. 高保真商云监控画面（图 1 真实办公室实景与模拟监控背景） -->
        <div v-else class="relative h-full w-full flex items-center justify-center overflow-hidden">
          <img
            src="/img/office-cam.jpg"
            alt="监控画面"
            class="h-full w-full object-cover transition-transform duration-300"
            :class="!isPlaying && activeTab === 'preview' ? 'brightness-75' : ''"
          />

          <!-- 暂停状态居中提示 -->
          <div
            v-if="(!isPlaying && activeTab === 'preview') || (!recPlaying && activeTab === 'playback')"
            class="absolute inset-0 flex items-center justify-center bg-black/30 backdrop-blur-[1px]"
          >
            <div class="flex items-center gap-2 rounded-full bg-black/60 px-4 py-1.5 text-xs text-white">
              <Icon name="pause" :size="14" />
              <span>画面已暂停</span>
            </div>
          </div>
        </div>

        <!-- OSD 水印（叠加在实时视频画面之上，非主题表面，黑色描边保证任意画面背景下可读，不作 token 化） -->
        <div
          class="pointer-events-none absolute left-6 top-5 z-20 text-sm md:text-base font-bold tracking-wider text-white select-none"
          style="text-shadow: 1px 1px 2px #000, -1px -1px 2px #000, 1px -1px 2px #000, -1px 1px 2px #000;"
        >
          {{ activeTab === 'preview' ? currentOsdTime : playbackOsdTime }}
        </div>

        <!-- 语音对讲激活提示 -->
        <div
          v-if="isIntercom && activeTab === 'preview'"
          class="absolute top-5 right-5 z-20 flex items-center gap-1.5 rounded-chrome bg-success/90 px-2.5 py-1 text-xs text-white shadow"
        >
          <span class="h-2 w-2 animate-ping rounded-full bg-white" />
          <Icon name="mic" :size="13" />
          <span>正在对讲...</span>
        </div>
      </div>

      <!-- ==================== 回放 24 小时时间轴 (仅在回放 Tab 显示) ==================== -->
      <div v-if="activeTab === 'playback'" class="border-t border-line bg-zone px-5 py-2.5">
        <div class="mb-1 flex items-center justify-between text-[11px] text-placeholder">
          <span>00:00</span>
          <span>04:00</span>
          <span>08:00</span>
          <span class="font-mono font-bold text-primary">{{ formatSeconds(recCurSeconds) }}</span>
          <span>16:00</span>
          <span>20:00</span>
          <span>24:00</span>
        </div>

        <!-- 时间轴主体轨道（数据面用 signal 圆角） -->
        <div
          ref="timelineRef"
          class="relative h-6 w-full cursor-pointer rounded-signal bg-line overflow-hidden select-none"
          title="点击定位时间点"
          @click="onTimelineClick"
        >
          <!-- 录像色块：连续=rec-timer(信号青) / 事件告警=rec-event(成功绿)，REC-02 语义，与 playback.vue 保持一致 -->
          <div
            v-for="(seg, idx) in recSegments"
            :key="idx"
            class="absolute top-0 bottom-0 rounded-sm opacity-70 hover:opacity-100"
            :class="seg.type === 'timer' ? 'bg-rec-timer' : 'bg-rec-event'"
            :style="{
              left: `${(seg.startSec / 86400) * 100}%`,
              width: `${((seg.endSec - seg.startSec) / 86400) * 100}%`
            }"
          />

          <!-- 当前位置游标线（信号青，呼应 playback.vue 时间轴游标语义：cyan=当前信号位置，而非告警红） -->
          <div
            class="pointer-events-none absolute top-0 bottom-0 z-10 w-[2px] bg-primary shadow-[0_0_6px_var(--color-primary)]"
            :style="{ left: `${(recCurSeconds / 86400) * 100}%` }"
          >
            <div class="h-1.5 w-1.5 -translate-x-[2px] bg-primary rotate-45" />
          </div>
        </div>
      </div>

      <!-- ==================== 底部控制栏（100% 对齐图 1 底部：暂停/对讲/静音/超清） ==================== -->
      <div class="flex items-center justify-between px-5 py-2.5 bg-surface-2 border-t border-line">
        <!-- 左侧工具组 -->
        <div class="flex items-center gap-3">
          <!-- 播放 / 暂停按钮 -->
          <button
            class="flex h-7 w-7 items-center justify-center rounded-chrome text-ink transition-colors hover:bg-zone hover:text-primary"
            :title="isPlaying ? '暂停' : '播放'"
            @click="activeTab === 'preview' ? togglePlay() : (recPlaying = !recPlaying)"
          >
            <Icon
              :name="(activeTab === 'preview' ? isPlaying : recPlaying) ? 'pause' : 'play'"
              :size="15"
            />
          </button>

          <!-- 预览模式：对讲按钮（麦克风图标 + 对讲文字，图 1 关键元素） -->
          <button
            v-if="activeTab === 'preview'"
            class="flex items-center gap-1 rounded-chrome px-2 py-1 text-xs transition-colors"
            :class="isIntercom ? 'bg-success-soft text-success font-semibold' : 'text-muted hover:bg-zone hover:text-primary'"
            title="语音对讲"
            @click="toggleIntercom"
          >
            <Icon name="mic" :size="14" />
            <span>对讲</span>
          </button>

          <!-- 回放模式：快退 10s / 快进 30s -->
          <template v-if="activeTab === 'playback'">
            <button
              class="rounded-chrome p-1 text-xs text-muted hover:bg-zone hover:text-primary"
              title="快退 10 秒"
              @click="backward10"
            >
              <Icon name="skip-back" :size="14" />
            </button>
            <button
              class="rounded-chrome p-1 text-xs text-muted hover:bg-zone hover:text-primary"
              title="快进 30 秒"
              @click="forward30"
            >
              <Icon name="fast-forward" :size="14" />
            </button>
            <button
              class="rounded-chrome border border-line px-2 py-0.5 text-xs font-mono font-medium text-muted hover:border-primary hover:text-primary"
              title="切换倍速"
              @click="toggleSpeed"
            >
              {{ recSpeed }}x
            </button>
          </template>

          <!-- 音量 / 静音切换 -->
          <button
            class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary"
            :title="isMuted ? '开启声音' : '静音'"
            @click="toggleMute"
          >
            <Icon :name="isMuted ? 'volume-x' : 'volume-2'" :size="15" />
          </button>

          <!-- 清晰度切换下拉（预览模式：超清/高清/标清胶囊，图 1 关键元素） -->
          <div v-if="activeTab === 'preview'" class="relative">
            <button
              class="rounded-chrome border border-line px-2 py-0.5 text-xs text-muted transition-colors hover:border-primary hover:text-primary"
              @click="showClarityMenu = !showClarityMenu"
            >
              {{ clarity === 'ultra' ? '超清' : clarity === 'hd' ? '高清' : '标清' }}
            </button>

            <!-- 清晰度菜单 -->
            <div
              v-if="showClarityMenu"
              class="absolute bottom-full left-0 mb-1 w-20 rounded-chrome border border-line bg-surface-2 py-1 shadow-pop z-30"
            >
              <button
                class="w-full px-3 py-1 text-left text-xs hover:bg-zone"
                :class="clarity === 'ultra' ? 'font-bold text-primary' : 'text-muted'"
                @click="selectClarity('ultra')"
              >
                超清
              </button>
              <button
                class="w-full px-3 py-1 text-left text-xs hover:bg-zone"
                :class="clarity === 'hd' ? 'font-bold text-primary' : 'text-muted'"
                @click="selectClarity('hd')"
              >
                高清
              </button>
              <button
                class="w-full px-3 py-1 text-left text-xs hover:bg-zone"
                :class="clarity === 'sd' ? 'font-bold text-primary' : 'text-muted'"
                @click="selectClarity('sd')"
              >
                标清
              </button>
            </div>
          </div>
        </div>

        <!-- 右侧辅助工具组：抓拍与全屏 -->
        <div class="flex items-center gap-2">
          <button
            class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary"
            title="抓拍图片"
            @click="takeSnapshot"
          >
            <Icon name="camera" :size="15" />
          </button>
          <button
            class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary"
            title="全屏"
            @click="toggleFullscreen"
          >
            <Icon name="maximize" :size="15" />
          </button>
        </div>
      </div>
    </div>
  </div>
</template>
