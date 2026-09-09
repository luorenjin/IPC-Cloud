<script setup lang="ts">
// 首次设置向导（ACC-02）：创建项目 → 添加媒体节点（自检）→ 完成
definePageMeta({ layout: 'auth' })
const api = useApi()
const { loadMe, setupDone } = useAuth()
const toast = useToast()

const step = ref(0)
const projectId = ref<string>(((useCookie('ipc_project').value as any) || '') as string)

// 已完成设置的账户不再进入向导（A22）
onMounted(() => {
  if (setupDone.value && projectId.value) {
    toast.info('您已完成初始设置')
    navigateTo('/')
  }
})

// ---- 第 1 步：创建项目 ----
const projectName = ref('')
const creating = ref(false)
const err1 = ref('')

async function createProject() {
  if (!projectName.value.trim()) { err1.value = '请输入项目名称'; return }
  err1.value = ''
  creating.value = true
  try {
    const p: any = await api.post('/projects', { name: projectName.value.trim() })
    projectId.value = p.id
    useCookie('ipc_project', { maxAge: 60 * 60 * 24 * 30 }).value = p.id
    await loadMe()
    toast.success('项目创建成功')
    step.value = 1
  } catch (e: any) {
    err1.value = e?.msg || '创建失败'
  } finally {
    creating.value = false
  }
}

// ---- 第 2 步：添加媒体节点（自检失败显示具体原因，ACC-02） ----
const nodeForm = reactive({ name: '', apiUrl: '', secret: '', publicHost: '' })
const addingNode = ref(false)
const err2 = ref<{ msg: string; reason?: string } | null>(null)
const selfcheckTip = ref(false)

async function addNode() {
  if (!nodeForm.name.trim() || !nodeForm.apiUrl.trim()) {
    err2.value = { msg: '请填写节点名称和 API 地址' }
    return
  }
  err2.value = null
  addingNode.value = true
  try {
    const n: any = await api.post('/media-nodes', { ...nodeForm })
    // 同步自检，失败给出具体原因（不可达 / secret 错误 / 版本过低）
    try {
      const chk: any = await api.post(`/media-nodes/${n.id}/selfcheck`)
      if (chk?.ok === false) {
        err2.value = { msg: '节点自检未通过', reason: chk?.reason || chk?.detail || '请检查地址与密钥' }
      } else {
        selfcheckTip.value = true
      }
    } catch {
      selfcheckTip.value = true
    }
  } catch (e: any) {
    err2.value = { msg: e?.msg || '添加失败', reason: e?.suggest }
  } finally {
    addingNode.value = false
  }
}

// ---- 进入完成页：标记项目 setupDone ----
async function finish() {
  step.value = 2
  if (!projectId.value) return
  try {
    await api.put(`/projects/${projectId.value}`, { setupDone: true })
  } catch (e: any) {
    toastApiError(e, '完成状态保存失败，下次登录仍会进入向导')
  }
}
</script>

<template>
  <div class="w-[580px] max-w-[calc(100vw-32px)] rounded-signal border border-line bg-surface p-8 shadow-pop">
    <h1 class="mb-6 text-center text-lg font-bold text-ink">首次设置向导</h1>
    <UiSteps :steps="['创建项目', '添加媒体节点', '完成']" :current="step" class="mb-8 justify-center" />

    <!-- 第 1 步 -->
    <div v-if="step === 0">
      <div class="grid grid-cols-[90px_1fr] items-center gap-x-3">
        <label class="text-right text-sm text-body"><span class="text-danger">*</span> 项目名称</label>
        <UiInput v-model="projectName" placeholder="如：园区监控" maxlength="32" @enter="createProject" />
      </div>
      <div v-if="err1" class="mt-4 flex items-start gap-2 rounded-chrome border border-danger/30 bg-danger-soft px-3 py-2.5 text-sm text-danger">
        <Icon name="alert-circle" :size="15" class="mt-0.5 shrink-0" />{{ err1 }}
      </div>
      <div class="mt-6 flex justify-center gap-2">
        <UiButton @click="step = 1">跳过</UiButton>
        <UiButton variant="primary" :disabled="creating" @click="createProject">创建项目</UiButton>
      </div>
    </div>

    <!-- 第 2 步 -->
    <div v-if="step === 1">
      <div class="grid grid-cols-[90px_1fr] items-center gap-x-3 gap-y-3">
        <label class="text-right text-sm text-body"><span class="text-danger">*</span> 节点名称</label>
        <UiInput v-model="nodeForm.name" placeholder="如：主媒体节点" />
        <label class="text-right text-sm text-body"><span class="text-danger">*</span> API 地址</label>
        <UiInput v-model="nodeForm.apiUrl" placeholder="如：http://1.2.3.4:8080" />
        <label class="text-right text-sm text-muted">通信密钥</label>
        <UiInput v-model="nodeForm.secret" placeholder="节点通信密钥" type="password" />
        <label class="text-right text-sm text-muted">公网地址</label>
        <UiInput v-model="nodeForm.publicHost" placeholder="如：stream.example.com" />
      </div>
      <div v-if="err2" class="mt-4 flex items-start gap-2 rounded-chrome border border-danger/30 bg-danger-soft px-3 py-2.5 text-sm text-danger">
        <Icon name="alert-circle" :size="15" class="mt-0.5 shrink-0" />
        <div>
          <p>{{ err2.msg }}</p>
          <p v-if="err2.reason" class="mt-0.5 text-xs opacity-80">{{ err2.reason }}</p>
        </div>
      </div>
      <div v-if="selfcheckTip" class="mt-4 flex items-start gap-2 rounded-chrome border border-success/30 bg-success-soft px-3 py-2.5 text-sm text-success">
        <Icon name="check-circle" :size="15" class="mt-0.5 shrink-0" />
        <div>
          <p>节点已添加，自检通过</p>
          <p class="mt-0.5 text-xs opacity-80">详细状态可在【系统 → 媒体节点】页面查看。</p>
        </div>
      </div>
      <div class="mt-6 flex justify-center gap-2">
        <UiButton @click="finish">跳过</UiButton>
        <UiButton v-if="!selfcheckTip" variant="primary" :disabled="addingNode" @click="addNode">添加节点</UiButton>
        <UiButton v-else variant="primary" @click="finish">下一步</UiButton>
      </div>
    </div>

    <!-- 第 3 步 -->
    <div v-if="step === 2" class="flex flex-col items-center gap-3 py-6">
      <span class="flex h-14 w-14 items-center justify-center rounded-full bg-success-soft text-success">
        <Icon name="check" :size="28" :stroke="3" />
      </span>
      <p class="text-base font-semibold text-ink">初始设置完成</p>
      <p class="text-sm text-muted">接下来可以添加您的第一批摄像头设备。</p>
      <UiButton variant="primary" size="lg" class="mt-2" @click="navigateTo('/devices')">去添加设备</UiButton>
    </div>
  </div>
</template>
