<script setup lang="ts">
// 设备预览与回放入口弹窗（LIVE-01/02 + REC-01~03）：预览 Tab 真实拉流 + 抓拍；
// 回放 Tab 内联单通道回放，核心逻辑与 pages/playback.vue 共用 usePlaybackSession
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
const { t } = useI18n()

// 激活的 Tab
const activeTab = ref<'preview' | 'playback'>('preview')

// 当前通道
const curChannel = computed(() => {
  if (props.channel) return props.channel
  if (props.device?.channels?.length) return props.device.channels[0]
  return {
    id: props.device?.id || '',
    name: props.device?.name || t('live.preview.defaultChannel'),
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

// ==================== 预览态（直播画面） ====================
const isMuted = ref(true)
const profile = ref<'main' | 'sub'>('main')
const liveStreamUrl = ref('')
const liveLoading = ref(false)
const liveContainer = ref<HTMLElement>()
const snapshotUrl = ref('')
const snapshotLoading = ref(false)

// 切换静音
function toggleMute() {
  isMuted.value = !isMuted.value
  toast.info(isMuted.value ? t('live.msg.muted') : t('live.msg.unmuted'))
}

// 切换码流（主码流 / 子码流）
function selectProfile(p: 'main' | 'sub') {
  if (profile.value === p) return
  profile.value = p
  fetchLiveStream()
}

// 抓拍截图
async function takeSnapshot() {
  if (!curChannel.value?.id) return
  snapshotLoading.value = true
  try {
    const res: any = await api.post(`/channels/${curChannel.value.id}/snapshot`)
    if (res?.url) {
      snapshotUrl.value = res.url
      toast.success(t('live.msg.snapshotOk'))
    } else {
      toast.warning(t('live.msg.snapshotNoImage'))
    }
  } catch (e: any) {
    toastApiError(e, t('live.msg.snapshotFailed'))
  } finally {
    snapshotLoading.value = false
  }
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
  liveLoading.value = true
  liveStreamUrl.value = ''
  try {
    const res: any = await api.post(`/channels/${curChannel.value.id}/play`, { profile: profile.value })
    liveStreamUrl.value = pickFlv(res)
  } catch (e: any) {
    liveStreamUrl.value = ''
    toastApiError(e, t('live.msg.playFailed'))
  } finally {
    liveLoading.value = false
  }
}

function stopLiveStream() {
  if (!curChannel.value?.id) return
  api.post(`/channels/${curChannel.value.id}/stop`).catch((e: any) => console.warn('停流失败', curChannel.value.id, e))
}

// ==================== 回放态（Tab 内联播放，REC-01~03） ====================
// channelId/deviceId 用独立 ref 而非直接绑定 curChannel：只在真正进入回放 Tab 时才赋值，
// 离开时清空，借助 usePlaybackSession 内部 watch(channelId,...) 的变化触发实现懒加载，
// 避免每次打开预览弹窗都顺带请求录像能力/记录（该弹窗不会真正卸载，见 close 分支说明）
const pbChannelId = ref('')
const pbDeviceId = ref('')
const {
  dayStart, shiftDay, pickDate,
  calOpen, calMonth, recDays, dayKey, calDays,
  source, canDevice,
  TYPE_COLOR, TYPE_NAME, typeFilter, shownSegs,
  tlEl, onWheel, segStyle, ticks,
  session, paused, muted: pbMuted, speed, speeds, sessionSource, deviceUrl, platformUrl,
  videoEl, curTs, curLeft,
  onTimelineClick, togglePause, onSpeedChange, forward30, closeSession,
  toggleMute: togglePbMute, doSnapshot: doPbSnapshot, onVideoMeta, onVideoTime, onVideoErr
} = usePlaybackSession(pbChannelId, pbDeviceId)

const fmtSeg = (ts: any) => new Date(Number(ts)).toLocaleString('zh-CN', { hour12: false })

// 电子放大（P1，简单点击循环 1x/2x/4x，CSS transform 缩放画面）
const zoomLevel = ref(1)
function cycleZoom() {
  zoomLevel.value = zoomLevel.value >= 4 ? 1 : zoomLevel.value * 2
}

function enterPlayback() {
  pbChannelId.value = curChannel.value?.id || ''
  pbDeviceId.value = props.device?.id || ''
}
function leavePlayback() {
  pbChannelId.value = ''
  pbDeviceId.value = ''
  zoomLevel.value = 1
}

// 生命周期与监听
watch(
  () => props.modelValue,
  (val) => {
    if (val) {
      activeTab.value = props.initialTab || 'preview'
      snapshotUrl.value = ''
      window.addEventListener('keydown', onKeydown)
      if (activeTab.value === 'preview') fetchLiveStream()
      else enterPlayback()
    } else {
      stopLiveStream()
      liveStreamUrl.value = ''
      leavePlayback()
      window.removeEventListener('keydown', onKeydown)
    }
  },
  { immediate: true }
)

onBeforeUnmount(() => {
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
            {{ device?.name || curChannel?.name || t('live.preview.title') }}
          </h2>
          <span
            v-if="device?.status"
            class="inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-[11px] font-normal"
            :class="device.status === 'online' ? 'bg-success-soft text-success' : 'bg-danger-soft text-danger'"
          >
            <span class="h-1.5 w-1.5 rounded-full" :class="device.status === 'online' ? 'bg-success' : 'bg-danger'" />
            {{ device.status === 'online' ? t('common.online') : t('common.offline') }}
          </span>
        </div>
        <button
          class="rounded-chrome p-1 text-muted transition-colors hover:bg-zone hover:text-ink"
          :title="t('common.close')"
          :aria-label="t('common.close')"
          @click="close"
        >
          <Icon name="x" :size="18" />
        </button>
      </div>

      <!-- Tab 切换栏 -->
      <div class="flex items-center px-5 pb-3">
        <div class="inline-flex rounded-chrome overflow-hidden border border-line text-xs">
          <!-- 预览 Tab -->
          <button
            class="px-5 py-1.5 font-medium transition-colors"
            :class="activeTab === 'preview' ? 'bg-primary text-white' : 'text-muted hover:text-primary'"
            @click="activeTab = 'preview'; fetchLiveStream()"
          >
            {{ t('live.preview.tabPreview') }}
          </button>
          <!-- 回放 Tab -->
          <button
            class="flex items-center gap-1.5 px-5 py-1.5 font-medium border-l border-line transition-colors"
            :class="activeTab === 'playback' ? 'bg-primary text-white' : 'text-muted hover:text-primary'"
            @click="activeTab = 'playback'; stopLiveStream(); liveStreamUrl = ''; enterPlayback()"
          >
            <Icon name="video" :size="13" />
            <span>{{ t('live.preview.tabPlayback') }}</span>
          </button>
        </div>
      </div>

      <!-- ==================== 预览 Tab：播放容器区域 (16:9 标准比例) ==================== -->
      <div
        v-if="activeTab === 'preview'"
        ref="liveContainer"
        class="relative aspect-video w-full overflow-hidden bg-black select-none flex items-center justify-center"
      >
        <H265Player
          v-if="liveStreamUrl"
          :url="liveStreamUrl"
          :muted="isMuted"
          class="h-full w-full object-contain"
        />

        <!-- 无流占位：与 live.vue 一致的图标 + 文案 -->
        <div v-else class="flex flex-col items-center justify-center gap-2 text-placeholder">
          <Icon :name="liveLoading ? 'refresh' : 'video'" :size="30" :stroke="1.4" :class="liveLoading ? 'ipc-spin' : ''" />
          <span class="text-xs">{{ liveLoading ? t('live.preview.loading') : t('live.preview.noStream') }}</span>
        </div>
      </div>

      <!-- ==================== 回放 Tab：单通道内联回放（REC-01~03） ==================== -->
      <template v-else>
        <div class="relative aspect-video w-full overflow-hidden bg-black select-none flex items-center justify-center">
          <div class="h-full w-full overflow-hidden">
            <div class="h-full w-full transition-transform duration-200" :style="{ transform: `scale(${zoomLevel})` }">
              <H265Player v-if="deviceUrl" :url="deviceUrl" :muted="pbMuted" class="h-full w-full object-contain" />
              <video
                v-else-if="platformUrl"
                ref="videoEl"
                :src="platformUrl"
                autoplay
                preload="metadata"
                class="h-full w-full bg-black object-contain"
                :muted="pbMuted"
                @loadedmetadata="onVideoMeta"
                @timeupdate="onVideoTime"
                @error="onVideoErr"
              />
              <div v-else class="flex h-full flex-col items-center justify-center gap-2 text-placeholder">
                <Icon name="film" :size="30" :stroke="1.4" />
                <span class="text-xs">{{ pbChannelId ? t('live.playback.emptyPickSeg') : t('live.playback.emptyPickChannel') }}</span>
              </div>
            </div>
          </div>

          <!-- OSD：当前回放时间戳 -->
          <div
            v-if="curTs"
            class="pointer-events-none absolute left-2 top-2 rounded-chrome bg-black/50 px-2 py-0.5 font-mono text-[11px] text-white"
          >
            {{ new Date(curTs).toLocaleString('zh-CN', { hour12: false }) }}
          </div>
        </div>

        <!-- 日期 / 存储位置 -->
        <div class="flex flex-wrap items-center gap-2 border-t border-line bg-surface-2 px-5 py-2">
          <button
            type="button"
            class="flex h-7 w-7 items-center justify-center rounded-chrome border border-line text-muted hover:border-primary hover:text-primary"
            :aria-label="t('live.playback.prevDay')"
            @click="shiftDay(-1)"
          >
            <Icon name="chevron-left" :size="14" />
          </button>
          <UiPopover v-model:open="calOpen" width="w-64">
            <template #trigger>
              <button class="flex h-7 items-center gap-1.5 rounded-chrome border border-line bg-surface px-2.5 text-sm text-body hover:border-primary">
                <Icon name="calendar" :size="13" class="text-placeholder" />{{ new Date(dayStart).toLocaleDateString('zh-CN') }}
              </button>
            </template>
            <div>
              <div class="mb-1 flex items-center justify-between">
                <button type="button" class="rounded-chrome p-1 text-muted hover:bg-zone" :aria-label="t('live.playback.prevMonth')" @click="calMonth = new Date(calMonth.getFullYear(), calMonth.getMonth() - 1, 1)"><Icon name="chevron-left" :size="14" /></button>
                <span class="text-sm font-medium text-ink">{{ t('live.playback.calMonthLabel', { y: calMonth.getFullYear(), m: calMonth.getMonth() + 1 }) }}</span>
                <button type="button" class="rounded-chrome p-1 text-muted hover:bg-zone" :aria-label="t('live.playback.nextMonth')" @click="calMonth = new Date(calMonth.getFullYear(), calMonth.getMonth() + 1, 1)"><Icon name="chevron-right" :size="14" /></button>
              </div>
              <div class="grid grid-cols-7 gap-0.5 text-center text-[11px] text-placeholder">
                <span v-for="w in ['live.playback.weekMon', 'live.playback.weekTue', 'live.playback.weekWed', 'live.playback.weekThu', 'live.playback.weekFri', 'live.playback.weekSat', 'live.playback.weekSun']" :key="w" class="py-1">{{ t(w) }}</span>
              </div>
              <div class="grid grid-cols-7 gap-0.5">
                <template v-for="(d, i) in calDays" :key="i">
                  <button
                    v-if="d"
                    class="relative flex h-7 items-center justify-center rounded-chrome text-[13px] transition-colors hover:bg-primary-soft"
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
                <span class="mr-1 inline-block h-1.5 w-1.5 rounded-full bg-primary align-middle" />{{ t('live.playback.calDotHint') }}
              </p>
            </div>
          </UiPopover>
          <button
            type="button"
            class="flex h-7 w-7 items-center justify-center rounded-chrome border border-line text-muted hover:border-primary hover:text-primary"
            :aria-label="t('live.playback.nextDay')"
            @click="shiftDay(1)"
          >
            <Icon name="chevron-right" :size="14" />
          </button>
          <UiSegmented
            :model-value="source"
            @update:model-value="source = $event as any"
            :items="[...(canDevice ? [{ label: t('live.playback.srcDevice'), value: 'device' }] : []), { label: t('live.playback.srcPlatform'), value: 'platform' }]"
          />
          <span v-if="session" class="ml-auto">
            <UiTag :color="sessionSource === 'platform' ? 'primary' : 'success'" plain>
              {{ sessionSource === 'platform' ? t('live.playback.tagPlatform') : t('live.playback.tagDevice') }}
            </UiTag>
          </span>
        </div>

        <!-- 24h 时间轴 -->
        <div class="border-t border-line bg-surface-2 px-5 py-2">
          <div class="relative mb-1 h-3.5">
            <span v-for="tk in ticks" :key="tk.left" class="absolute -translate-x-1/2 font-mono text-[10px] text-placeholder" :style="{ left: tk.left + '%' }">{{ tk.label }}</span>
          </div>
          <div
            ref="tlEl"
            class="relative h-7 cursor-pointer rounded-signal border border-line-soft bg-canvas shadow-[inset_0_1px_4px_rgba(0,0,0,0.6)]"
            @click="onTimelineClick"
            @wheel="onWheel"
          >
            <div
              v-for="(seg, i) in shownSegs"
              :key="i"
              class="absolute bottom-1 top-1 rounded-signal transition-opacity hover:opacity-85"
              :style="{ ...segStyle(seg), background: TYPE_COLOR[seg.type] || 'var(--color-rec-timer)' }"
              :title="t('live.playback.segTitle', { start: fmtSeg(seg.s), end: fmtSeg(seg.e), type: TYPE_NAME[seg.type] ? t(TYPE_NAME[seg.type]) : seg.type })"
            />
            <div v-if="curTs" class="pointer-events-none absolute -bottom-1 -top-1 w-[2px] bg-primary shadow-[0_0_6px_var(--color-primary)]" :style="{ left: curLeft }" />
          </div>
          <div class="mt-1.5 flex flex-wrap items-center gap-3">
            <UiCheckbox v-model="typeFilter.timer">
              <span class="inline-flex items-center gap-1.5 text-xs"><span class="h-1.5 w-1.5 rounded-full" :style="{ background: TYPE_COLOR.timer }" />{{ t('live.playback.typeTimer') }}</span>
            </UiCheckbox>
            <UiCheckbox v-model="typeFilter.event">
              <span class="inline-flex items-center gap-1.5 text-xs"><span class="h-1.5 w-1.5 rounded-full" :style="{ background: TYPE_COLOR.event }" />{{ t('live.playback.typeEvent') }}</span>
            </UiCheckbox>
            <UiCheckbox v-model="typeFilter.manual">
              <span class="inline-flex items-center gap-1.5 text-xs"><span class="h-1.5 w-1.5 rounded-full" :style="{ background: TYPE_COLOR.manual }" />{{ t('live.playback.typeManual') }}</span>
            </UiCheckbox>
            <span class="ml-auto text-[11px] text-placeholder">{{ t('live.playback.segCount', { n: shownSegs.length }) }}</span>
          </div>
        </div>
      </template>

      <!-- ==================== 底部控制栏（仅预览 Tab） ==================== -->
      <div v-if="activeTab === 'preview'" class="flex items-center justify-between px-5 py-2.5 bg-surface-2 border-t border-line">
        <!-- 左侧工具组 -->
        <div class="flex items-center gap-3">
          <!-- 音量 / 静音切换 -->
          <button
            class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary"
            :title="isMuted ? t('live.preview.unmute') : t('live.preview.mute')"
            :aria-label="isMuted ? t('live.preview.unmute') : t('live.preview.mute')"
            @click="toggleMute"
          >
            <Icon :name="isMuted ? 'volume-x' : 'volume-2'" :size="15" />
          </button>

          <!-- 码流切换：主码流 / 子码流 -->
          <div class="inline-flex rounded-chrome border border-line overflow-hidden text-xs">
            <button
              class="px-2 py-0.5 transition-colors"
              :class="profile === 'main' ? 'bg-primary-soft text-primary font-medium' : 'text-muted hover:text-primary'"
              @click="selectProfile('main')"
            >
              {{ t('live.player.mainStream') }}
            </button>
            <button
              class="px-2 py-0.5 border-l border-line transition-colors"
              :class="profile === 'sub' ? 'bg-primary-soft text-primary font-medium' : 'text-muted hover:text-primary'"
              @click="selectProfile('sub')"
            >
              {{ t('live.player.subStream') }}
            </button>
          </div>
        </div>

        <!-- 右侧辅助工具组：抓拍与全屏 -->
        <div class="flex items-center gap-2">
          <a
            v-if="snapshotUrl"
            :href="snapshotUrl"
            target="_blank"
            rel="noopener"
            class="text-xs text-primary hover:underline"
          >{{ t('live.preview.viewSnapshot') }}</a>
          <button
            class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary disabled:cursor-not-allowed disabled:opacity-40"
            :title="t('live.preview.snapshot')"
            :aria-label="t('live.preview.snapshot')"
            :disabled="snapshotLoading || !liveStreamUrl"
            @click="takeSnapshot"
          >
            <Icon :name="snapshotLoading ? 'refresh' : 'camera'" :size="15" :class="snapshotLoading ? 'ipc-spin' : ''" />
          </button>
          <button
            class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary"
            :title="t('live.preview.fullscreen')"
            :aria-label="t('live.preview.fullscreen')"
            @click="toggleFullscreen"
          >
            <Icon name="maximize" :size="15" />
          </button>
        </div>
      </div>

      <!-- ==================== 底部控制栏（仅回放 Tab） ==================== -->
      <div v-else-if="activeTab === 'playback'" class="flex flex-wrap items-center gap-2 bg-surface-2 border-t border-line px-5 py-2.5">
        <button
          class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary disabled:cursor-not-allowed disabled:opacity-40"
          :title="paused ? t('live.playback.resume') : t('live.playback.pause')"
          :aria-label="paused ? t('live.playback.resume') : t('live.playback.pause')"
          :disabled="!session"
          @click="togglePause"
        >
          <Icon :name="paused ? 'play' : 'pause'" :size="15" />
        </button>
        <button
          class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary disabled:cursor-not-allowed disabled:opacity-40"
          :title="'30s'"
          :aria-label="'30s'"
          :disabled="!session"
          @click="forward30"
        >
          <Icon name="fast-forward" :size="15" />
        </button>
        <button
          class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary"
          :title="pbMuted ? t('live.playback.unmute') : t('live.playback.mute')"
          :aria-label="pbMuted ? t('live.playback.unmute') : t('live.playback.mute')"
          @click="togglePbMute"
        >
          <Icon :name="pbMuted ? 'volume-x' : 'volume-2'" :size="15" />
        </button>
        <UiSelect
          :model-value="String(speed)" width="w-20" size="sm" :disabled="!session"
          :options="speeds.map((s: number) => ({ label: s + 'x', value: String(s) }))" @update:model-value="onSpeedChange"
        />
        <button
          class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-primary"
          :title="t('live.playback.snapshot')"
          :aria-label="t('live.playback.snapshot')"
          @click="doPbSnapshot"
        >
          <Icon name="camera" :size="15" />
        </button>
        <!-- 电子放大：简单点击循环 1x → 2x → 4x → 1x -->
        <button
          class="flex h-7 items-center gap-1 rounded-chrome px-1.5 text-xs text-muted transition-colors hover:bg-zone hover:text-primary"
          :title="t('live.preview.zoomLevel', { level: zoomLevel })"
          :aria-label="t('live.preview.zoomLevel', { level: zoomLevel })"
          @click="cycleZoom"
        >
          <Icon name="zoom-in" :size="15" /><span class="font-mono">{{ zoomLevel }}x</span>
        </button>

        <span class="ml-auto" />
        <button
          class="flex h-7 w-7 items-center justify-center rounded-chrome text-muted transition-colors hover:bg-zone hover:text-danger disabled:cursor-not-allowed disabled:opacity-40"
          :title="t('live.playback.closeSession')"
          :aria-label="t('live.playback.closeSession')"
          :disabled="!session"
          @click="closeSession"
        >
          <Icon name="x" :size="15" />
        </button>
      </div>
    </div>
  </div>
</template>
