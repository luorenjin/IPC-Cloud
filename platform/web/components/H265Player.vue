<script setup lang="ts">
// h265web.js 播放器封装（LIVE-01）：core 自动探测（webcodec 优先，失败自动降级 wasm）。
// 发行包置于 /vendor/（public/vendor/，构建时随 .output/public 发布）。
// API 版本 v20260824：window.H265webjsPlayer 工厂 + build(config) + load_media(url)。
const props = defineProps<{
  url?: string
  title?: string
  muted?: boolean
}>()

const state = ref<'idle' | 'loading' | 'playing' | 'error'>('idle')
const errMsg = ref('')
const emit = defineEmits<{ (e: 'retry'): void }>()
const container = ref<HTMLDivElement>()
const containerId = `h265player-${Math.random().toString(36).slice(2, 9)}`
let player: any = null

async function ensureLib(): Promise<any> {
  const win = window as any
  if (win.H265webjsPlayer) return win.H265webjsPlayer
  const pending = document.querySelector<HTMLScriptElement>('script[data-h265web]')
  if (pending) {
    for (let i = 0; i < 50; i++) {
      if (win.H265webjsPlayer) return win.H265webjsPlayer
      await new Promise((r) => setTimeout(r, 100))
    }
    throw new Error('h265web.js 加载超时')
  }
  await new Promise<void>((resolve, reject) => {
    const s = document.createElement('script')
    s.src = '/vendor/h265web.js'
    s.dataset.h265web = '1'
    s.onload = () => resolve()
    s.onerror = () => reject(new Error('h265web.js 未部署（/vendor/h265web.js）'))
    document.head.appendChild(s)
  })
  const lib = win.H265webjsPlayer
  if (!lib) throw new Error('h265web.js 全局对象缺失（H265webjsPlayer）')
  return lib
}

async function play(url?: string) {
  if (!url) { state.value = 'idle'; return }
  state.value = 'loading'
  errMsg.value = ''
  try {
    const create = await ensureLib()
    destroy()
    const box = container.value
    if (!box) throw new Error('播放容器未就绪')
    box.id = containerId
    player = create()
    // wasm 渲染把解码帧拉伸铺满构建尺寸的视口（change_viewport 无宽高比保护），
    // 容器宽高比 ≠ 16:9 时画面变形 + 放大模糊。按 16:9（IPC 主码流标准）在容器内
    // 拟合画布尺寸，画布由容器 flex 居中 + CSS auto 尺寸显示。
    const bw = box.clientWidth || 640
    const bh = box.clientHeight || 360
    let cw = bw
    let ch = bh
    if (bw / bh > 16 / 9) {
      cw = Math.round(bh * 16 / 9)
    } else {
      ch = Math.round(bw * 9 / 16)
    }
    const ok = player.build({
      player_id: containerId,
      wasm_js_uri: '/vendor/h265web_wasm.js',
      wasm_wasm_uri: '/vendor/h265web_wasm.wasm',
      ext_src_js_uri: '/vendor/extjs.js',
      ext_wasm_js_uri: '/vendor/extwasm.js',
      width: cw,
      height: ch,
      color: 'black',
      auto_play: true,
      ignore_audio: false,
      // H264 直播强制 mse_mp4（flv.js 派生）：其 remux 以首帧 DTS 为基准归零
      // MSE 时间轴，规避"起始 PTS=流已推时长"导致的长时间黑屏等待；
      // H265 流由调用方换用专用播放器或后续按 codec 分流。
      core: 'mse_mp4',
      // 直播属性透传（flv.js 派生层）：起始 PTS ≠ 0 的常驻流需要 live 模式
      // 立即 append + 追帧，否则 MSE 侧按内部时钟丢弃"过期"段导致长时间黑屏。
      isLive: true,
      enableStashBuffer: false,
      liveBufferLatencyChasing: true,
      liveBufferLatencyMaxLatency: 1.5,
      liveBufferLatencyMinRemain: 0.5
    })
    if (!ok) throw new Error('播放器初始化失败')
    player.on_ready_show_done_callback = () => {
      if (state.value === 'loading') state.value = 'playing'
    }
    player.on_play_finished = () => {
      state.value = 'error'
      errMsg.value = '流已结束'
    }
    player.on_error_callback = (e: any) => {
      state.value = 'error'
      errMsg.value = typeof e === 'string' ? e : JSON.stringify(e)
    }
    player.load_media(url)
    startSeekPoll()
  } catch (e: any) {
    state.value = 'error'
    errMsg.value = e?.message || String(e)
  }
}

// MSE 时间戳起点修正：常驻推流的直播流首帧 PTS 可达数小时（流注册起算），
// video.currentTime 停在 0 永远追不上 buffer 起点，导致数据到达却永不渲染（E4002 假象）。
let seekPoll: ReturnType<typeof setInterval> | null = null
let seekPollStart = 0
function stopSeekPoll() {
  if (seekPoll) { clearInterval(seekPoll); seekPoll = null }
}
function startSeekPoll() {
  stopSeekPoll()
  seekPollStart = performance.now()
  // 持续轮询直至 playing（此前"首检后 early-return"的写法会在 readyState
  // 尚未就绪时永久放弃复查，导致数据已到却卡 loading → E4002）。
  seekPoll = setInterval(() => {
    const v = container.value?.querySelector('video') as HTMLVideoElement | null
    if (!v) return
    // 自动播放策略兜底：video 未静音时 play() 会被拒绝（NotAllowedError），
    // 画面数据到位也永不渲染——强制对齐 muted prop 并主动拉起播放。
    if (props.muted && !v.muted) { v.muted = true; v.volume = 0 }
    if (v.paused && state.value !== 'idle') {
      try { v.play()?.catch(() => {}) } catch {}
    }
    if (v.buffered.length > 0) {
      const start = v.buffered.start(0)
      if (seekPollStart) {
        console.log(`[H265Player] first buffered after ${(performance.now() - seekPollStart).toFixed(0)}ms, start=${start.toFixed(2)}, end=${v.buffered.end(0).toFixed(2)}, rs=${v.readyState}`)
        seekPollStart = 0
      }
      if (v.currentTime < start) v.currentTime = start
      if (v.readyState >= 2 || v.currentTime > 0) {
        state.value = 'playing'
        stopSeekPoll()
      }
    }
  }, 250)
}

function destroy() {
  stopSeekPoll()
  try { player?.release?.() } catch {}
  player = null
}

function snapshot() {
  try { player?.screenshot?.() } catch {}
}

// immediate：回放页组件因 v-if 在 url 就绪后才挂载，挂载后 url 不再变化，
// 无 immediate 时 watcher 永不触发 → 永远停留"未播放"（live 页先挂载后赋 url 不受影响）。
watch(() => props.url, (u) => play(u), { immediate: true })
onBeforeUnmount(destroy)
defineExpose({ snapshot })

const timeoutTimer = ref<any>(null)
watch(state, (s) => {
  clearTimeout(timeoutTimer.value)
  if (s === 'loading') {
    // 起流超时 30s（PRD LIVE-01 起流 10s 指后端出流；前端含弱网缓冲放宽）
    timeoutTimer.value = setTimeout(() => {
      if (state.value === 'loading') {
        state.value = 'error'
        errMsg.value = 'E4002 起流超时'
      }
    }, 30000)
  }
})

// 错误码解析（MGR-15：E4xxx 流媒体错误单独展示）
const errCode = computed(() => (/E\d{4}/.exec(errMsg.value || '') || [''])[0])
const errText = computed(() => (errMsg.value || '播放失败').replace(/E\d{4}\s*/, '') || '播放失败')
</script>

<template>
  <div ref="container" class="player-box">
    <div v-if="state === 'idle'" class="absolute inset-0 z-10 flex items-center justify-center text-sm text-placeholder">未播放</div>
    <div v-else-if="state === 'loading'" class="absolute inset-0 z-10 flex items-center justify-center gap-2 text-sm text-placeholder">
      <Icon name="refresh" :size="16" class="ipc-spin" />正在连接…
    </div>
    <div v-else-if="state === 'error'" class="absolute inset-0 z-10 flex items-center justify-center p-4">
      <UiErrorCard :code="errCode" :msg="errText" :suggest="'可点击重试重新起流；持续失败请在设备列表发起诊断'" @retry="emit('retry')" @diagnose="emit('retry')" />
    </div>
    <div v-if="title && state === 'playing'" class="absolute inset-x-0 top-0 bg-black/45 px-2 py-1 text-xs text-white">{{ title }}</div>
  </div>
</template>

<style scoped>
.player-box {
  position: relative; width: 100%; height: 100%; background: #000;
  display: flex; align-items: center; justify-content: center;
}
/* wasm 画布按 16:9 位图尺寸构建，CSS auto 显示（禁止库内联拉伸样式），
   由 flex 居中，保持视频固有宽高比不变形 */
.player-box :deep(canvas) {
  width: auto !important; height: auto !important;
  max-width: 100%; max-height: 100%;
}
</style>
