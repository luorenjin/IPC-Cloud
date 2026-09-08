<script setup lang="ts">
// 扫码页（P-19 / ADD-02）：移动端响应式，调用摄像头扫描设备二维码并快速绑定
definePageMeta({ layout: 'auth' }) // 移动端扫码使用独立轻量布局或全屏

const api = useApi()
const toast = useToast()
const router = useRouter()

const videoRef = ref<HTMLVideoElement>()
const canvasRef = ref<HTMLCanvasElement>()
const scanning = ref(false)
const cameraActive = ref(false)
const errorMsg = ref('')

// 解析出的设备信息
const form = reactive({
  deviceId: '',
  verifyCode: '',
  groupId: '',
  name: ''
})

const groups = ref<any[]>([])
const groupOptions = computed(() => groups.value.map((g: any) => ({ value: g.id, label: g.name })))

// 查找状态
const lookingUp = ref(false)
const deviceFound = ref<any>(null)
const binding = ref(false)

async function loadGroups() {
  try {
    const res: any = await api.get('/groups')
    groups.value = res?.items || []
    if (groups.value.length) form.groupId = groups.value[0].id
  } catch {}
}

let stream: MediaStream | null = null
let scanTimer: any = null

async function startCamera() {
  errorMsg.value = ''
  cameraActive.value = false
  try {
    if (!navigator.mediaDevices?.getUserMedia) {
      errorMsg.value = '当前浏览器或环境不支持调用摄像头，请使用手动输入'
      return
    }
    stream = await navigator.mediaDevices.getUserMedia({
      video: { facingMode: 'environment', width: { ideal: 1280 }, height: { ideal: 720 } },
      audio: false
    })
    if (videoRef.value) {
      videoRef.value.srcObject = stream
      await videoRef.value.play()
      cameraActive.value = true
      scanning.value = true
      startDetection()
    }
  } catch (e: any) {
    errorMsg.value = '无法打开摄像头：' + (e.message || '请检查摄像头权限')
  }
}

function stopCamera() {
  scanning.value = false
  cameraActive.value = false
  if (scanTimer) cancelAnimationFrame(scanTimer)
  if (stream) {
    stream.getTracks().forEach((track) => track.stop())
    stream = null
  }
}

// 条码检测支持
const hasBarcodeDetector = typeof window !== 'undefined' && 'BarcodeDetector' in window
let detector: any = null
if (hasBarcodeDetector) {
  try {
    // @ts-ignore
    detector = new window.BarcodeDetector({ formats: ['qr_code', 'code_128', 'data_matrix'] })
  } catch {}
}

async function startDetection() {
  if (!scanning.value || !videoRef.value || !canvasRef.value) return

  const video = videoRef.value
  if (video.readyState === video.HAVE_ENOUGH_DATA) {
    if (detector) {
      try {
        const barcodes = await detector.detect(video)
        if (barcodes.length > 0) {
          const text = barcodes[0].rawValue || ''
          handleQrResult(text)
          return
        }
      } catch {}
    }
  }

  scanTimer = requestAnimationFrame(startDetection)
}

// 处理二维码文本：规则 IPC1:{deviceID}:{verifyCode} 或纯 deviceID
function handleQrResult(raw: string) {
  const text = raw.trim()
  if (!text) return

  stopCamera()
  toast.success('扫描成功')

  if (text.startsWith('IPC1:')) {
    const parts = text.split(':')
    if (parts.length >= 2) form.deviceId = (parts[1] || '').toUpperCase()
    if (parts.length >= 3) form.verifyCode = parts[2] || ''
  } else if (/^[A-Za-z0-9]{17}$/.test(text)) {
    form.deviceId = text.toUpperCase()
  } else {
    // 兼容 JSON 格式二维码
    try {
      const parsed = JSON.parse(text)
      if (parsed.deviceId) form.deviceId = String(parsed.deviceId).toUpperCase()
      if (parsed.verifyCode) form.verifyCode = String(parsed.verifyCode)
    } catch {
      form.deviceId = text
    }
  }

  if (form.deviceId) {
    lookupDevice()
  }
}

// 查找设备
async function lookupDevice() {
  if (!form.deviceId) return
  lookingUp.value = true
  deviceFound.value = null
  try {
    const res: any = await api.get('/devices/idp/lookup', { deviceId: form.deviceId })
    deviceFound.value = res
  } catch (e: any) {
    toastApiError(e, '未找到该设备或设备未连网')
  } finally {
    lookingUp.value = false
  }
}

// 提交绑定
async function submitBind() {
  if (!form.deviceId) return toast.warning('请输入设备 ID')
  if (!form.verifyCode) return toast.warning('请输入设备验证码')
  if (!form.groupId) return toast.warning('请选择所属分组')

  binding.value = true
  try {
    await api.post('/devices', {
      source: 'idp',
      name: form.name.trim() || deviceFound.value?.model || form.deviceId,
      groupId: form.groupId,
      credentials: {
        deviceId: form.deviceId.toUpperCase(),
        verifyCode: form.verifyCode
      }
    })
    toast.success('设备绑定成功！')
    router.push('/devices')
  } catch (e: any) {
    toastApiError(e, '绑定失败')
  } finally {
    binding.value = false
  }
}

onMounted(() => {
  loadGroups()
  startCamera()
})

onBeforeUnmount(() => {
  stopCamera()
})
</script>

<template>
  <div class="flex min-h-screen flex-col bg-sidebar text-white">
    <!-- 移动端顶栏 -->
    <header class="flex h-12 items-center justify-between border-b border-white/10 px-4">
      <button class="flex items-center gap-1 text-sm text-white/80 hover:text-white" @click="router.push('/devices')">
        <UiIcon name="chevron-left" :size="18" />返回
      </button>
      <span class="font-medium text-base">扫描设备二维码</span>
      <div class="w-12 text-right">
        <button v-if="cameraActive" class="text-xs text-primary-light" @click="stopCamera">手动</button>
        <button v-else class="text-xs text-primary-light" @click="startCamera">重新扫码</button>
      </div>
    </header>

    <!-- 摄像头视窗区 -->
    <div class="relative flex flex-1 flex-col items-center justify-center p-4">
      <div v-show="cameraActive" class="relative aspect-square w-full max-w-xs overflow-hidden rounded-2xl border-2 border-primary/60 bg-black shadow-2xl">
        <video ref="videoRef" class="h-full w-full object-cover" playsinline muted autoplay></video>
        <canvas ref="canvasRef" class="hidden"></canvas>
        <!-- 扫码扫描框与动画 -->
        <div class="pointer-events-none absolute inset-0 flex flex-col items-center justify-center">
          <div class="relative h-56 w-56 rounded-lg border-2 border-primary">
            <div class="absolute -top-1 -left-1 h-4 w-4 border-t-2 border-l-2 border-primary-light"></div>
            <div class="absolute -top-1 -right-1 h-4 w-4 border-t-2 border-r-2 border-primary-light"></div>
            <div class="absolute -bottom-1 -left-1 h-4 w-4 border-b-2 border-l-2 border-primary-light"></div>
            <div class="absolute -bottom-1 -right-1 h-4 w-4 border-b-2 border-r-2 border-primary-light"></div>
            <div class="h-0.5 w-full bg-primary-light/80 shadow-[0_0_8px_#1785E6] animate-pulse"></div>
          </div>
        </div>
      </div>

      <div v-if="cameraActive" class="mt-4 text-center text-xs text-white/70">
        将镜头对准机身标签或包装盒上的「IPC1:」二维码
      </div>

      <div v-if="errorMsg" class="my-4 max-w-sm rounded-lg bg-danger/20 p-3 text-center text-xs text-danger-light">
        {{ errorMsg }}
      </div>

      <!-- 识别结果与手动确认录入表单 -->
      <div class="mt-4 w-full max-w-md space-y-3 rounded-xl bg-surface/10 p-5 backdrop-blur-md border border-white/10">
        <div class="text-sm font-medium text-white/90">设备接入信息</div>

        <div>
          <label class="mb-1 block text-xs text-white/60">设备 ID (17位) *</label>
          <div class="flex gap-2">
            <UiInput v-model="form.deviceId" placeholder="扫描或手动输入 DeviceID" class="flex-1 uppercase" />
            <UiButton size="sm" :loading="lookingUp" @click="lookupDevice">查找</UiButton>
          </div>
        </div>

        <div v-if="deviceFound" class="rounded bg-primary/20 p-2.5 text-xs text-primary-light flex items-center justify-between">
          <div>
            <div>型号：<span class="font-medium text-white">{{ deviceFound.model || '标准IPC' }}</span></div>
            <div>状态：<span :class="deviceFound.online ? 'text-success' : 'text-danger'">{{ deviceFound.online ? '在线' : '离线' }}</span></div>
          </div>
          <UiTag :color="deviceFound.bound ? 'warning' : 'success'">
            {{ deviceFound.bound ? '已绑定' : '可绑定' }}
          </UiTag>
        </div>

        <div>
          <label class="mb-1 block text-xs text-white/60">设备验证码 (6位) *</label>
          <UiInput v-model="form.verifyCode" placeholder="机身标签 6 位验证码" />
        </div>

        <div>
          <label class="mb-1 block text-xs text-white/60">设备名称 (选填)</label>
          <UiInput v-model="form.name" placeholder="如：正门摄像头、库房监控" />
        </div>

        <div>
          <label class="mb-1 block text-xs text-white/60">所属分组 *</label>
          <UiSelect v-model="form.groupId" :options="groupOptions" class="w-full" />
        </div>

        <div class="pt-2">
          <UiButton variant="primary" class="w-full justify-center" :loading="binding" @click="submitBind">
            确认绑定设备
          </UiButton>
        </div>
      </div>
    </div>
  </div>
</template>
