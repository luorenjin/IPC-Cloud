<script setup lang="ts">
// 录像设置（REC-06）：通道录像计划管理（三步向导：选通道 → 选模板 → 选码流）
const api = useApi()
const toast = useToast()
const confirm = useConfirm()

const PROFILE_MAP: Record<string, string> = { main: '主码流', sub: '子码流' }

// ---------- 基础数据（设备 / 通道 / 模板映射） ----------
const devices = ref<any[]>([])
const channels = ref<any[]>([])
const templates = ref<any[]>([])
const channelMap = computed(() => Object.fromEntries(channels.value.map((c) => [c.id, c.name])))
const templateMap = computed(() => Object.fromEntries(templates.value.map((t) => [t.id, t.name])))

async function loadBase() {
  try {
    const [dRes, cRes, tRes]: any[] = await Promise.all([
      api.get('/devices'), api.get('/channels'), api.get('/record-templates')
    ])
    devices.value = dRes.items || dRes || []
    channels.value = cRes.items || cRes || []
    templates.value = tRes.items || tRes || []
  } catch (e: any) {
    toastApiError(e, '加载基础数据失败')
  }
}

// ---------- 计划列表 ----------
const plans = ref<any[]>([])
const loading = ref(false)

async function loadPlans() {
  loading.value = true
  try {
    const res: any = await api.get('/record-plans')
    plans.value = res.items || []
  } catch (e: any) {
    toastApiError(e, '加载录像计划失败')
  } finally {
    loading.value = false
  }
}

async function togglePlan(row: any, v: boolean) {
  const prev = row.enabled
  row.enabled = v
  try {
    await api.put(`/record-plans/${row.id}`, { enabled: !!v })
  } catch (e: any) {
    row.enabled = prev // 失败回滚
    toastApiError(e, '操作失败')
  }
}

async function delPlan(row: any) {
  const ok = await confirm.ask({
    title: '删除确认',
    message: `确定删除通道「${channelMap.value[row.channelId] || row.channelId}」的录像计划？`,
    detail: '删除后该通道将停止按计划录像，已存储的录像文件不受影响。',
    danger: true, confirmText: '删除'
  })
  if (!ok) return
  try {
    await api.del(`/record-plans/${row.id}`)
    toast.success('已删除')
    loadPlans()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

// ---------- 新建计划：三步向导（REC-06） ----------
const dlg = ref(false)
const saving = ref(false)
const step = ref(0) // 0 选通道 / 1 选模板 / 2 选码流
const form = reactive({
  channelIds: [] as string[],
  templateId: '',
  profile: 'main'
})

// 通道树：按设备分组
const channelTree = computed(() => {
  const byDev: Record<string, any[]> = {}
  channels.value.forEach((c) => {
    ;(byDev[c.deviceId] = byDev[c.deviceId] || []).push(c)
  })
  return devices.value
    .filter((d: any) => (byDev[d.id] || []).length)
    .map((d: any) => ({
      label: d.name,
      value: 'dev:' + d.id,
      children: byDev[d.id].map((c: any) => ({ label: c.name, value: c.id }))
    }))
})

function toggleChannel(id: string) {
  const i = form.channelIds.indexOf(id)
  if (i >= 0) form.channelIds.splice(i, 1)
  else form.channelIds.push(id)
}
function toggleDevGroup(node: any) {
  const ids = (node.children || []).map((c: any) => c.value)
  const all = ids.length && ids.every((id: string) => form.channelIds.includes(id))
  ids.forEach((id: string) => {
    const i = form.channelIds.indexOf(id)
    if (all && i >= 0) form.channelIds.splice(i, 1)
    else if (!all && i < 0) form.channelIds.push(id)
  })
}
function onTreeSelect(node: any) {
  if (String(node.value).startsWith('dev:')) toggleDevGroup(node)
  else toggleChannel(node.value)
}
function groupChecked(node: any) {
  const ids = (node.children || []).map((c: any) => c.value)
  return ids.length > 0 && ids.every((id: string) => form.channelIds.includes(id))
}

function openDlg() {
  form.channelIds = []
  form.templateId = ''
  form.profile = 'main'
  step.value = 0
  dlg.value = true
}
function nextStep() {
  if (step.value === 0 && !form.channelIds.length) { toast.warning('请选择通道'); return }
  if (step.value === 1 && !form.templateId) { toast.warning('请选择录像模板'); return }
  step.value++
}
function prevStep() {
  if (step.value > 0) step.value--
}

async function save() {
  if (!form.channelIds.length) { toast.warning('请选择通道'); return }
  if (!form.templateId) { toast.warning('请选择录像模板'); return }
  saving.value = true
  try {
    await api.post('/record-plans', {
      channelIds: form.channelIds,
      templateId: form.templateId,
      profile: form.profile
    })
    toast.success('录像计划已创建')
    dlg.value = false
    loadPlans()
  } catch (e: any) {
    toastApiError(e, '创建录像计划失败')
  } finally {
    saving.value = false
  }
}

// ---------- 修改计划（更换录像模板） ----------
const editTplDlg = reactive({ show: false, row: null as any, templateId: '', saving: false })
function openEditTpl(row: any) {
  editTplDlg.row = row
  editTplDlg.templateId = row.templateId || ''
  editTplDlg.show = true
}
async function saveEditTpl() {
  if (!editTplDlg.templateId) { toast.warning('请选择录像模板'); return }
  editTplDlg.saving = true
  try {
    await api.put(`/record-plans/${editTplDlg.row.id}`, { templateId: editTplDlg.templateId })
    toast.success('录像计划已更新')
    editTplDlg.show = false
    loadPlans()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    editTplDlg.saving = false
  }
}

// ---------- 修改码流 ----------
const editProfDlg = reactive({ show: false, row: null as any, profile: 'main', saving: false })
function openEditProfile(row: any) {
  editProfDlg.row = row
  editProfDlg.profile = row.profile || 'main'
  editProfDlg.show = true
}
async function saveEditProfile() {
  editProfDlg.saving = true
  try {
    await api.put(`/record-plans/${editProfDlg.row.id}`, { profile: editProfDlg.profile })
    toast.success('码流已更新')
    editProfDlg.show = false
    loadPlans()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    editProfDlg.saving = false
  }
}

onMounted(() => {
  loadBase()
  loadPlans()
})
</script>

<template>
  <div class="space-y-3">
    <UiCard flat>
      <template #header>
        <div class="flex w-full items-center justify-between">
          <span class="text-[15px] font-semibold text-ink">录像计划</span>
          <div class="flex items-center gap-2">
            <UiButton size="sm" @click="() => { loadBase(); loadPlans() }"><UiIcon name="refresh" :size="13" />刷新</UiButton>
            <UiButton variant="primary" size="sm" @click="openDlg"><UiIcon name="plus" :size="14" />新建录像设置</UiButton>
          </div>
        </div>
      </template>

      <UiTable
        :columns="[
          { key: 'channel', label: '通道', width: '200px' },
          { key: 'template', label: '录像模板', width: '200px' },
          { key: 'profile', label: '码流', width: '100px' },
          { key: 'enabled', label: '启用', width: '70px', align: 'center' },
          { key: 'ops', label: '操作', width: '220px', align: 'center', ellipsis: false }
        ]"
        :rows="plans" :loading="loading" :row-key="'id'" empty="暂无录像计划，点击「新建录像设置」开始配置"
      >
        <template #channel="{ row }">{{ channelMap[row.channelId] || row.channelId }}</template>
        <template #template="{ row }">{{ templateMap[row.templateId] || row.templateId || '—' }}</template>
        <template #profile="{ row }">
          <UiTag :color="row.profile === 'sub' ? 'info' : 'primary'" plain>{{ PROFILE_MAP[row.profile] || row.profile }}</UiTag>
        </template>
        <template #enabled="{ row }">
          <UiSwitch :model-value="!!row.enabled" size="sm" @update:model-value="togglePlan(row, $event)" />
        </template>
        <template #ops="{ row }">
          <div class="flex items-center justify-center gap-1">
            <UiButton variant="text" size="sm" @click="openEditTpl(row)">修改计划</UiButton>
            <UiButton variant="text" size="sm" @click="openEditProfile(row)">修改码流</UiButton>
            <UiButton variant="dangerText" size="sm" @click="delPlan(row)">删除</UiButton>
          </div>
        </template>
      </UiTable>
    </UiCard>

    <!-- 新建录像设置：三步向导（REC-06） -->
    <UiDialog v-model:open="dlg" title="新建录像设置" width="max-w-xl">
      <div class="mb-5 flex justify-center">
        <UiSteps :steps="['选择通道', '选择模板', '选择码流']" :current="step" />
      </div>

      <!-- 第一步：通道多选 -->
      <div v-if="step === 0">
        <div class="mb-2 flex items-center justify-between">
          <span class="text-xs text-muted">按设备分组，可勾选设备整组或单个通道</span>
          <span class="text-xs text-primary">已选 {{ form.channelIds.length }} 个通道</span>
        </div>
        <div class="max-h-72 overflow-y-auto rounded border border-line p-2">
          <UiTree :nodes="channelTree" @select="onTreeSelect">
            <template #node="{ node }">
              <span class="flex items-center gap-1.5">
                <span
                  class="flex h-4 w-4 shrink-0 items-center justify-center rounded-sm border bg-surface"
                  :class="String(node.value).startsWith('dev:')
                    ? (groupChecked(node) ? 'border-primary bg-primary text-white' : 'border-line')
                    : (form.channelIds.includes(node.value) ? 'border-primary bg-primary text-white' : 'border-line')"
                >
                  <UiIcon
                    v-if="String(node.value).startsWith('dev:') ? groupChecked(node) : form.channelIds.includes(node.value)"
                    name="check" :size="10" :stroke="3"
                  />
                </span>
                <UiIcon :name="String(node.value).startsWith('dev:') ? 'video' : 'camera'" :size="13" class="text-placeholder" />
                <span class="truncate">{{ node.label }}</span>
                <span v-if="String(node.value).startsWith('dev:')" class="text-xs text-placeholder">({{ node.children?.length || 0 }})</span>
              </span>
            </template>
          </UiTree>
          <div v-if="!channelTree.length" class="py-6 text-center text-sm text-placeholder">暂无通道，请先在设备管理中接入设备</div>
        </div>
      </div>

      <!-- 第二步：模板单选 -->
      <div v-else-if="step === 1">
        <p class="mb-2 text-xs text-muted">选择录像计划模板（模板可在「录像模板」页维护）</p>
        <div class="max-h-72 space-y-2 overflow-y-auto">
          <div
            v-for="t in templates" :key="t.id"
            class="flex cursor-pointer items-center gap-2.5 rounded border px-3 py-2.5 transition-colors"
            :class="form.templateId === t.id ? 'border-primary bg-primary-soft' : 'border-line hover:border-primary'"
            @click="form.templateId = t.id"
          >
            <span class="flex h-4 w-4 shrink-0 items-center justify-center rounded-full border" :class="form.templateId === t.id ? 'border-primary' : 'border-line'">
              <span v-if="form.templateId === t.id" class="h-2 w-2 rounded-full bg-primary" />
            </span>
            <div class="min-w-0">
              <div class="flex items-center gap-1.5 text-sm text-ink">
                {{ t.name }}
                <UiTag :color="t.kind === 'event' ? 'warning' : 'primary'" plain>{{ t.kind === 'event' ? '事件录像' : '定时录像' }}</UiTag>
                <UiTag v-if="t.builtin" color="info">内置</UiTag>
              </div>
            </div>
          </div>
          <div v-if="!templates.length" class="py-6 text-center text-sm text-placeholder">暂无录像模板，请先在「录像模板」页创建</div>
        </div>
      </div>

      <!-- 第三步：码流 -->
      <div v-else>
        <p class="mb-2 text-xs text-muted">选择录像使用的码流；主码流画质高、占用存储大，子码流反之</p>
        <div class="grid grid-cols-2 gap-2">
          <div
            v-for="p in ['main', 'sub']" :key="p"
            class="flex cursor-pointer items-center gap-2.5 rounded border px-3 py-3 transition-colors"
            :class="form.profile === p ? 'border-primary bg-primary-soft' : 'border-line hover:border-primary'"
            @click="form.profile = p"
          >
            <span class="flex h-4 w-4 shrink-0 items-center justify-center rounded-full border" :class="form.profile === p ? 'border-primary' : 'border-line'">
              <span v-if="form.profile === p" class="h-2 w-2 rounded-full bg-primary" />
            </span>
            <div>
              <div class="text-sm text-ink">{{ PROFILE_MAP[p] }}</div>
              <div class="text-xs text-placeholder">{{ p === 'main' ? '高清录像，占用存储大' : '流畅录像，占用存储小' }}</div>
            </div>
          </div>
        </div>
        <div class="mt-4 rounded bg-zone px-3 py-2.5 text-xs text-muted">
          即将为 <span class="font-medium text-primary">{{ form.channelIds.length }}</span> 个通道创建
          <span class="font-medium text-ink">{{ templateMap[form.templateId] || '—' }}</span>
          （{{ PROFILE_MAP[form.profile] }}）录像计划
        </div>
      </div>

      <template #footer>
        <div class="flex w-full items-center justify-between">
          <UiButton :disabled="step === 0" @click="prevStep">上一步</UiButton>
          <div class="flex items-center gap-2">
            <UiButton @click="dlg = false">取消</UiButton>
            <UiButton v-if="step < 2" variant="primary" @click="nextStep">下一步</UiButton>
            <UiButton v-else variant="primary" :disabled="saving" @click="save">
              <UiIcon v-if="!saving" name="check" :size="14" />
              <UiIcon v-else name="refresh" :size="14" class="ipc-spin" />
              确定
            </UiButton>
          </div>
        </div>
      </template>
    </UiDialog>

    <!-- 修改计划（更换模板） -->
    <UiDialog v-model:open="editTplDlg.show" title="修改录像计划" width="max-w-md">
      <div class="space-y-3">
        <div class="flex items-center gap-2 text-sm">
          <span class="text-muted">通道</span>
          <span class="text-ink">{{ channelMap[editTplDlg.row?.channelId] || editTplDlg.row?.channelId }}</span>
        </div>
        <div>
          <p class="mb-1.5 text-sm text-muted">录像模板</p>
          <UiSelect v-model="editTplDlg.templateId" placeholder="选择录像模板" :options="templates.map(t => ({ label: t.name, value: t.id }))" />
        </div>
      </div>
      <template #footer>
        <UiButton @click="editTplDlg.show = false">取消</UiButton>
        <UiButton variant="primary" :disabled="editTplDlg.saving" @click="saveEditTpl">确定</UiButton>
      </template>
    </UiDialog>

    <!-- 修改码流 -->
    <UiDialog v-model:open="editProfDlg.show" title="修改码流" width="max-w-sm">
      <div class="space-y-3">
        <div class="flex items-center gap-2 text-sm">
          <span class="text-muted">通道</span>
          <span class="text-ink">{{ channelMap[editProfDlg.row?.channelId] || editProfDlg.row?.channelId }}</span>
        </div>
        <UiSegmented v-model="editProfDlg.profile" :items="[{ label: '主码流', value: 'main' }, { label: '子码流', value: 'sub' }]" />
      </div>
      <template #footer>
        <UiButton @click="editProfDlg.show = false">取消</UiButton>
        <UiButton variant="primary" :disabled="editProfDlg.saving" @click="saveEditProfile">确定</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
