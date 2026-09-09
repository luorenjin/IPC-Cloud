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

// 纯展示性状态文案：随扫描状态机派生，不引入新状态、不改变原逻辑
const scanStatusText = computed(() => {
  if (deviceFound.value) return `识别成功：设备型号 ${deviceFound.value.model || '标准 IPC'}`
  if (lookingUp.value) return '识别中…'
  if (errorMsg.value) return ''
  if (cameraActive.value && scanning.value) return '将镜头对准设备机身或包装盒上的二维码'
  return '摄像头未开启，可在下方手动输入设备信息'
})

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
    const res: any = await api.post('/devices/idp/lookup', { deviceId: form.deviceId })
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
    await api.post('/devices/idp/bind', {
      deviceId: form.deviceId.toUpperCase(),
      verifyCode: form.verifyCode,
      groupId: form.groupId,
      name: form.name.trim() || undefined
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
  <div class="flex min-h-screen flex-col bg-sidebar">
    <!-- 移动端顶栏 -->
    <header class="flex h-12 shrink-0 items-center justify-between border-b border-line px-4">
      <button class="flex items-center gap-1 text-sm text-sidebar-text transition-colors hover:text-ink" @click="router.push('/devices')">
        <Icon name="chevron-left" :size="18" />返回
      </button>
      <span class="text-base font-medium text-ink">扫描设备二维码</span>
      <div class="w-12 text-right">
        <button v-if="cameraActive" class="text-xs text-primary hover:text-primary-deep" @click="stopCamera">手动</button>
        <button v-else class="text-xs text-primary hover:text-primary-deep" @click="startCamera">重新扫码</button>
      </div>
    </header>

    <!-- 摄像头视窗区 -->
    <div class="relative flex flex-1 flex-col items-center justify-center p-4">
      <div v-show="cameraActive" class="relative aspect-square w-full max-w-xs overflow-hidden rounded-signal border border-line bg-black">
        <video ref="videoRef" class="h-full w-full object-cover" playsinline muted autoplay></video>
        <canvas ref="canvasRef" class="hidden"></canvas>
        <!-- 取景角标：四角直角描边，呼应"摄像头对焦框"语义——不用圆角矩形边框 -->
        <div class="pointer-events-none absolute inset-0 flex flex-col items-center justify-center">
          <div class="relative h-56 w-56 max-w-[80%]">
            <span class="absolute -top-px -left-px h-7 w-7 border-t-2 border-l-2 border-primary"></span>
            <span class="absolute -top-px -right-px h-7 w-7 border-t-2 border-r-2 border-primary"></span>
            <span class="absolute -bottom-px -left-px h-7 w-7 border-b-2 border-l-2 border-primary"></span>
            <span class="absolute -bottom-px -right-px h-7 w-7 border-b-2 border-r-2 border-primary"></span>
            <div v-if="scanning" class="absolute inset-x-3 top-1/2 h-0.5 -translate-y-1/2 bg-primary/80 shadow-[0_0_8px_var(--color-primary)] animate-pulse"></div>
          </div>
        </div>
      </div>

      <!-- 状态文案：随扫描状态机更新（对准二维码 → 识别中 → 识别成功：设备型号 XXX） -->
      <p class="mt-4 min-h-4 text-center text-xs" :class="deviceFound ? 'text-success' : 'text-sidebar-text'">{{ scanStatusText }}</p>

      <div v-if="errorMsg" class="my-4 max-w-sm rounded-chrome border border-danger/30 bg-danger-soft px-3 py-2.5 text-center text-xs text-danger">
        {{ errorMsg }}
      </div>

      <!-- 识别结果与手动确认录入表单 -->
      <div class="mt-4 w-full max-w-md space-y-3 rounded-signal border border-line bg-surface p-5">
        <div class="text-sm font-medium text-ink">设备接入信息</div>

        <div>
          <label class="mb-1 block text-xs text-muted">设备 ID (17位) *</label>
          <div class="flex gap-2">
            <UiInput v-model="form.deviceId" placeholder="扫描或手动输入 DeviceID" class="flex-1 uppercase" />
            <UiButton size="sm" :loading="lookingUp" @click="lookupDevice">查找</UiButton>
          </div>
        </div>

        <div v-if="deviceFound" class="flex items-center justify-between rounded-signal border border-primary/30 bg-primary-soft p-2.5 text-xs text-primary">
          <div class="space-y-0.5">
            <div>型号：<span class="font-medium text-ink">{{ deviceFound.model || '标准IPC' }}</span></div>
            <div>状态：<span :class="deviceFound.online ? 'text-success' : 'text-danger'">{{ deviceFound.online ? '在线' : '离线' }}</span></div>
          </div>
          <UiTag :color="deviceFound.bound ? 'warning' : 'success'">
            {{ deviceFound.bound ? '已绑定' : '可绑定' }}
          </UiTag>
        </div>

        <div>
          <label class="mb-1 block text-xs text-muted">设备验证码 (6位) *</label>
          <UiInput v-model="form.verifyCode" placeholder="机身标签 6 位验证码" />
        </div>

        <div>
          <label class="mb-1 block text-xs text-muted">设备名称 (选填)</label>
          <UiInput v-model="form.name" placeholder="如：正门摄像头、库房监控" />
        </div>

        <div>
          <label class="mb-1 block text-xs text-muted">所属分组 *</label>
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
