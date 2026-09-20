<script setup lang="ts">
// 首次设置向导（ACC-02）：创建项目 → 添加媒体节点（自检）→ 完成
definePageMeta({ layout: 'auth' })
const api = useApi()
const { loadMe, setupDone } = useAuth()
const toast = useToast()
const { t } = useI18n()

const step = ref(0)
const projectId = ref<string>(((useCookie('ipc_project').value as any) || '') as string)

// 已完成设置的账户不再进入向导（A22）
onMounted(() => {
  if (setupDone.value && projectId.value) {
    toast.info(t('account.msg.setupAlreadyDone'))
    navigateTo('/')
  }
})

// ---- 第 1 步：创建项目 ----
const projectName = ref('')
const creating = ref(false)
const err1 = ref('')

async function createProject() {
  if (!projectName.value.trim()) { err1.value = t('account.msg.projectNameRequired'); return }
  err1.value = ''
  creating.value = true
  try {
    const p: any = await api.post('/projects', { name: projectName.value.trim() })
    projectId.value = p.id
    useCookie('ipc_project', { maxAge: 60 * 60 * 24 * 30 }).value = p.id
    await loadMe()
    toast.success(t('account.msg.projectCreated'))
    step.value = 1
  } catch (e: any) {
    err1.value = e?.msg || t('account.msg.projectCreateFailed')
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
    err2.value = { msg: t('account.msg.nodeFieldsRequired') }
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
        err2.value = { msg: t('account.msg.selfcheckFailed'), reason: chk?.reason || chk?.detail || t('account.msg.selfcheckFailedReason') }
      } else {
        selfcheckTip.value = true
      }
    } catch {
      selfcheckTip.value = true
    }
  } catch (e: any) {
    err2.value = { msg: e?.msg || t('account.msg.nodeAddFailed'), reason: e?.suggest }
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
    toastApiError(e, t('account.msg.setupSaveFailed'))
  }
}
</script>

<template>
  <div class="w-[580px] max-w-[calc(100vw-32px)] rounded-signal border border-line bg-surface p-8 shadow-pop">
    <h1 class="mb-6 text-center text-lg font-bold text-ink">{{ t('account.wizard.title') }}</h1>
    <UiSteps :steps="[t('account.wizard.step1'), t('account.wizard.step2'), t('account.wizard.step3')]" :current="step" class="mb-8 justify-center" />

    <!-- 第 1 步 -->
    <div v-if="step === 0">
      <div class="grid grid-cols-[90px_1fr] items-center gap-x-3">
        <label class="text-right text-sm text-body"><span class="text-danger">*</span> {{ t('account.wizard.projectName') }}</label>
        <UiInput v-model="projectName" :placeholder="t('account.wizard.projectNamePlaceholder')" maxlength="32" @enter="createProject" />
      </div>
      <div v-if="err1" class="mt-4 flex items-start gap-2 rounded-chrome border border-danger/30 bg-danger-soft px-3 py-2.5 text-sm text-danger">
        <Icon name="alert-circle" :size="15" class="mt-0.5 shrink-0" />{{ err1 }}
      </div>
      <div class="mt-6 flex justify-center gap-2">
        <UiButton @click="step = 1">{{ t('account.wizard.skip') }}</UiButton>
        <UiButton variant="primary" :disabled="creating" @click="createProject">{{ t('account.wizard.createProject') }}</UiButton>
      </div>
    </div>

    <!-- 第 2 步 -->
    <div v-if="step === 1">
      <div class="grid grid-cols-[90px_1fr] items-center gap-x-3 gap-y-3">
        <label class="text-right text-sm text-body"><span class="text-danger">*</span> {{ t('account.wizard.nodeName') }}</label>
        <UiInput v-model="nodeForm.name" :placeholder="t('account.wizard.nodeNamePlaceholder')" />
        <label class="text-right text-sm text-body"><span class="text-danger">*</span> {{ t('account.wizard.nodeApiUrl') }}</label>
        <UiInput v-model="nodeForm.apiUrl" :placeholder="t('account.wizard.nodeApiUrlPlaceholder')" />
        <label class="text-right text-sm text-muted">{{ t('account.wizard.nodeSecret') }}</label>
        <UiInput v-model="nodeForm.secret" :placeholder="t('account.wizard.nodeSecretPlaceholder')" type="password" />
        <label class="text-right text-sm text-muted">{{ t('account.wizard.nodePublicHost') }}</label>
        <UiInput v-model="nodeForm.publicHost" :placeholder="t('account.wizard.nodePublicHostPlaceholder')" />
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
          <p>{{ t('account.wizard.selfcheckOk') }}</p>
          <p class="mt-0.5 text-xs opacity-80">{{ t('account.wizard.selfcheckOkHint') }}</p>
        </div>
      </div>
      <div class="mt-6 flex justify-center gap-2">
        <UiButton @click="finish">{{ t('account.wizard.skip') }}</UiButton>
        <UiButton v-if="!selfcheckTip" variant="primary" :disabled="addingNode" @click="addNode">{{ t('account.wizard.addNode') }}</UiButton>
        <UiButton v-else variant="primary" @click="finish">{{ t('account.wizard.next') }}</UiButton>
      </div>
    </div>

    <!-- 第 3 步 -->
    <div v-if="step === 2" class="flex flex-col items-center gap-3 py-6">
      <span class="flex h-14 w-14 items-center justify-center rounded-full bg-success-soft text-success">
        <Icon name="check" :size="28" :stroke="3" />
      </span>
      <p class="text-base font-semibold text-ink">{{ t('account.wizard.doneTitle') }}</p>
      <p class="text-sm text-muted">{{ t('account.wizard.doneHint') }}</p>
      <UiButton variant="primary" size="lg" class="mt-2" @click="navigateTo('/devices')">{{ t('account.wizard.goAddDevice') }}</UiButton>
    </div>
  </div>
</template>
