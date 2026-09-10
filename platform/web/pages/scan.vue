<script setup lang="ts">
// 扫码页（P-19 / ADD-02）：移动端响应式，调用摄像头扫描设备二维码并快速绑定
definePageMeta({ layout: 'auth' }) // 移动端扫码使用独立轻量布局或全屏

const api = useApi()
const toast = useToast()
const router = useRouter()
const { t } = useI18n()

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
  if (deviceFound.value) return t('account.scan.statusFound', { model: deviceFound.value.model || t('account.scan.defaultModel') })
  if (lookingUp.value) return t('account.scan.statusLooking')
  if (errorMsg.value) return ''
  if (cameraActive.value && scanning.value) return t('account.scan.statusAiming')
  return t('account.scan.statusIdle')
})

async function loadGroups() {
  try {
    const res: any = await api.get('/groups')
    groups.value = res?.items || []
    if (groups.value.length) form.groupId = groups.value[0].id
  } catch (e: any) {
    toastApiError(e, t('account.msg.groupLoadFailed'))
  }
}

let stream: MediaStream | null = null
let scanTimer: any = null

async function startCamera() {
  errorMsg.value = ''
  cameraActive.value = false
  try {
    if (!navigator.mediaDevices?.getUserMedia) {
      errorMsg.value = t('account.msg.cameraUnsupported')
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
    errorMsg.value = t('account.msg.cameraFailed', { reason: e.message || t('account.msg.cameraPermission') })
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
  } catch {
    // 浏览器不支持这些码制：detector 保持 null，下方会回落到手动输入
  }
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
      } catch {
        // 逐帧识别，单帧失败继续下一帧即可
      }
    }
  }

  scanTimer = requestAnimationFrame(startDetection)
}

// 处理二维码文本：规则 IPC1:{deviceID}:{verifyCode} 或纯 deviceID
function handleQrResult(raw: string) {
  const text = raw.trim()
  if (!text) return

  stopCamera()
  toast.success(t('account.msg.scanOk'))

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
    toastApiError(e, t('account.msg.deviceNotFound'))
  } finally {
    lookingUp.value = false
  }
}

// 提交绑定
async function submitBind() {
  if (!form.deviceId) return toast.warning(t('account.msg.deviceIdRequired'))
  if (!form.verifyCode) return toast.warning(t('account.msg.verifyCodeRequired'))
  if (!form.groupId) return toast.warning(t('account.msg.groupRequired'))

  binding.value = true
  try {
    await api.post('/devices/idp/bind', {
      deviceId: form.deviceId.toUpperCase(),
      verifyCode: form.verifyCode,
      groupId: form.groupId,
      name: form.name.trim() || undefined
    })
    toast.success(t('account.msg.bindOk'))
    router.push('/devices')
  } catch (e: any) {
    toastApiError(e, t('account.msg.bindFailed'))
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
        <Icon name="chevron-left" :size="18" />{{ t('account.scan.back') }}
      </button>
      <span class="text-base font-medium text-ink">{{ t('account.scan.title') }}</span>
      <div class="w-12 text-right">
        <button v-if="cameraActive" class="text-xs text-primary hover:text-primary-deep" @click="stopCamera">{{ t('account.scan.manual') }}</button>
        <button v-else class="text-xs text-primary hover:text-primary-deep" @click="startCamera">{{ t('account.scan.rescan') }}</button>
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
        <div class="text-sm font-medium text-ink">{{ t('account.scan.formTitle') }}</div>

        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('account.scan.deviceIdLabel') }}</label>
          <div class="flex gap-2">
            <UiInput v-model="form.deviceId" :placeholder="t('account.scan.deviceIdPlaceholder')" class="flex-1 uppercase" />
            <UiButton size="sm" :loading="lookingUp" @click="lookupDevice">{{ t('account.scan.lookup') }}</UiButton>
          </div>
        </div>

        <div v-if="deviceFound" class="flex items-center justify-between rounded-signal border border-primary/30 bg-primary-soft p-2.5 text-xs text-primary">
          <div class="space-y-0.5">
            <div>{{ t('account.scan.model') }}<span class="font-medium text-ink">{{ deviceFound.model || t('account.scan.defaultModel') }}</span></div>
            <div>{{ t('account.scan.status') }}<span :class="deviceFound.online ? 'text-success' : 'text-danger'">{{ deviceFound.online ? t('common.online') : t('common.offline') }}</span></div>
          </div>
          <UiTag :color="deviceFound.bound ? 'warning' : 'success'">
            {{ deviceFound.bound ? t('account.scan.bound') : t('account.scan.bindable') }}
          </UiTag>
        </div>

        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('account.scan.verifyCodeLabel') }}</label>
          <UiInput v-model="form.verifyCode" :placeholder="t('account.scan.verifyCodePlaceholder')" />
        </div>

        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('account.scan.nameLabel') }}</label>
          <UiInput v-model="form.name" :placeholder="t('account.scan.namePlaceholder')" />
        </div>

        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('account.scan.groupLabel') }}</label>
          <UiSelect v-model="form.groupId" :options="groupOptions" class="w-full" />
        </div>

        <div class="pt-2">
          <UiButton variant="primary" class="w-full justify-center" :loading="binding" @click="submitBind">
            {{ t('account.scan.submit') }}
          </UiButton>
        </div>
      </div>
    </div>
  </div>
</template>
