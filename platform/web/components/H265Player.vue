<script setup lang="ts">
// h265web.js 播放器封装（LIVE-01）：webcodec_hevc 优先，探测失败自动 wasm_hevc。
// 部署时将 h265web.js 发行包置于 /vendor/，缺库时显示错误卡片。
const props = defineProps<{
  url?: string
  title?: string
  muted?: boolean
}>()

const state = ref<'idle' | 'loading' | 'playing' | 'error'>('idle')
const errMsg = ref('')
const container = ref<HTMLDivElement>()
// @ts-expect-error 全局 h265web
let player: any = null

async function ensureLib(): Promise<any> {
  if ((window as any).H265WEB) return (window as any).H265WEB
  if (document.querySelector('script[src="/vendor/h265web.js"]')) {
    await new Promise((r) => setTimeout(r, 1500))
    return (window as any).H265WEB
  }
  await new Promise<void>((resolve, reject) => {
    const s = document.createElement('script')
    s.src = '/vendor/h265web.js'
    s.onload = () => resolve()
    s.onerror = () => reject(new Error('h265web.js 未部署（/vendor/h265web.js）'))
    document.head.appendChild(s)
  })
  return (window as any).H265WEB
}

async function play(url?: string) {
  if (!url) { state.value = 'idle'; return }
  state.value = 'loading'
  errMsg.value = ''
  try {
    const lib = await ensureLib()
    destroy()
    player = new lib.m7sPlayer({
      core: 'webcodec_hevc', // 探测失败由库内部自动降级 wasm_hevc
      extInfo: { wasm: '/vendor/dec' }
    })
    player.mount(container.value, container.value!.clientWidth, container.value!.clientHeight)
    player.on('streamEnd', () => { state.value = 'error'; errMsg.value = '流已结束' })
    player.on('error', (e: any) => { state.value = 'error'; errMsg.value = String(e) })
    player.load(url)
    state.value = 'playing'
  } catch (e: any) {
    state.value = 'error'
    errMsg.value = e?.message || String(e)
  }
}

function destroy() {
  try { player?.destroy?.() } catch {}
  player = null
}

function snapshot() {
  try { player?.shot?.() } catch {}
}

watch(() => props.url, (u) => play(u))
onBeforeUnmount(destroy)
defineExpose({ snapshot })

const timeoutTimer = ref<any>(null)
watch(state, (s) => {
  clearTimeout(timeoutTimer.value)
  if (s === 'loading') {
    // 起流超时 10s（PRD LIVE-01）
    timeoutTimer.value = setTimeout(() => {
      if (state.value === 'loading') {
        state.value = 'error'
        errMsg.value = 'E4002 起流超时'
      }
    }, 10000)
  }
})
</script>

<template>
  <div class="player-box" ref="container">
    <div v-if="state === 'idle'" class="overlay">未播放</div>
    <div v-else-if="state === 'loading'" class="overlay">
      <el-icon class="is-loading"><Loading /></el-icon> 正在连接…
    </div>
    <div v-else-if="state === 'error'" class="overlay error">
      <div>{{ errMsg || '播放失败' }}</div>
      <el-button size="small" style="margin-top: 8px" @click="play(props.url)">重试</el-button>
    </div>
    <div v-if="title && state === 'playing'" class="title">{{ title }}</div>
  </div>
</template>

<style scoped>
.player-box {
  position: relative; width: 100%; height: 100%; background: #000;
  display: flex; align-items: center; justify-content: center;
}
.overlay { color: #909399; text-align: center; }
.overlay.error { color: #f56c6c; display: flex; flex-direction: column; align-items: center; }
.title {
  position: absolute; top: 0; left: 0; right: 0; padding: 4px 8px;
  background: rgba(0,0,0,.45); color: #fff; font-size: 12px;
}
</style>
