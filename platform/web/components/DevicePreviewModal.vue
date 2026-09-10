<script setup lang="ts">
// 设备预览与回放入口弹窗（LIVE-01/02）：预览 Tab 真实拉流 + 抓拍；回放 Tab 跳转到回放页（完整回放见 Task 18）
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

// ==================== 回放入口（跳转到回放页，完整回放见 Task 18） ====================
function openPlaybackPage() {
  const id = curChannel.value?.id
  if (!id) return
  close()
  navigateTo('/playback?channelId=' + id)
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
    } else {
      stopLiveStream()
      liveStreamUrl.value = ''
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
            @click="activeTab = 'playback'; stopLiveStream(); liveStreamUrl = ''"
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

      <!-- ==================== 回放 Tab：说明 + 跳转按钮（完整回放见 Task 18） ==================== -->
      <div v-else class="relative aspect-video w-full flex flex-col items-center justify-center gap-3 bg-zone text-center">
        <Icon name="video" :size="30" class="text-placeholder" :stroke="1.4" />
        <p class="max-w-xs text-xs text-muted">{{ t('live.preview.playbackHint', { name: curChannel?.name || t('live.preview.playbackFallbackName') }) }}</p>
        <UiButton variant="primary" size="sm" :disabled="!curChannel?.id" @click="openPlaybackPage">
          {{ t('live.preview.openPlayback') }}
        </UiButton>
      </div>

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
    </div>
  </div>
</template>
