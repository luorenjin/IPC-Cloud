<script setup lang="ts">
// 告警规则（ALM-02，三步向导）+ 布防模板（ALM-01，含级联提示 A7）
const api = useApi()
const toast = useToast()
const confirm = useConfirm()

const DAY_NAMES = ['周一', '周二', '周三', '周四', '周五', '周六', '周日']
// 事件类型（ALM-02：六类，按通道能力集过滤置灰）
const KIND_OPTIONS = [
  { value: 'motion', label: '移动侦测' },
  { value: 'humanoid', label: '人形侦测' },
  { value: 'intrusion', label: '区域入侵' },
  { value: 'linecross', label: '越界侦测' },
  { value: 'tamper', label: '视频遮挡' },
  { value: 'io', label: '外接IO' }
]
const kindName = (k: string) => KIND_OPTIONS.find((o) => o.value === k)?.label || k

// schedule 摘要：{days:[1,2],ranges:[["00:00","24:00"]]} → "周一、周二 00:00-24:00"
function fmtSchedule(s: any) {
  if (!s || !s.days?.length) return '未布防'
  const days = [...s.days]
    .sort((a: number, b: number) => a - b)
    .map((d: number) => DAY_NAMES[d - 1] || d)
    .join('、')
  const ranges = (s.ranges || []).map((r: string[]) => `${r[0]}-${r[1]}`).join('、')
  return `${days} ${ranges}`
}

// ---------- 基础数据（设备 / 通道 / 模板映射） ----------
const devices = ref<any[]>([])
const channels = ref<any[]>([])
const templates = ref<any[]>([])
const channelMap = computed(() => Object.fromEntries(channels.value.map((c) => [c.id, c.name])))
const templateMap = computed(() => Object.fromEntries(templates.value.map((t) => [t.id, t.name])))

async function loadBase() {
  try {
    const [dRes, cRes, tRes]: any[] = await Promise.all([
      api.get('/devices'), api.get('/channels'), api.get('/alarm-templates')
    ])
    devices.value = dRes.items || dRes || []
    channels.value = cRes.items || cRes || []
    templates.value = tRes.items || tRes || []
  } catch (e: any) {
    toastApiError(e, '加载基础数据失败')
  }
}

const tab = ref('rules')

// ---------- Tab A：告警规则 ----------
const rules = ref<any[]>([])
const rulesLoading = ref(false)

async function loadRules() {
  rulesLoading.value = true
  try {
    const res: any = await api.get('/alarm-rules')
    rules.value = res.items || []
  } catch (e: any) {
    toastApiError(e, '加载告警规则失败')
  } finally {
    rulesLoading.value = false
  }
}

async function toggleRule(row: any, v: boolean) {
  const prev = row.enabled
  row.enabled = v
  try {
    await api.put(`/alarm-rules/${row.id}`, { enabled: !!v })
  } catch (e: any) {
    row.enabled = prev // 失败回滚
    toastApiError(e, '操作失败')
  }
}

async function delRule(row: any) {
  const ok = await confirm.ask({
    title: '删除确认',
    message: `确定删除通道「${channelMap.value[row.channelId] || row.channelId}」的告警规则？`,
    detail: '删除后该通道将不再产生对应告警消息。',
    danger: true, confirmText: '删除'
  })
  if (!ok) return
  try {
    await api.del(`/alarm-rules/${row.id}`)
    toast.success('已删除')
    loadRules()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

// 修改布防（更换该规则引用的布防模板）
const editRuleDlg = reactive({ show: false, row: null as any, templateId: '', saving: false })
function openEditRule(row: any) {
  editRuleDlg.row = row
  editRuleDlg.templateId = row.templateId || ''
  editRuleDlg.show = true
}
async function saveEditRule() {
  if (!editRuleDlg.templateId) { toast.warning('请选择布防模板'); return }
  editRuleDlg.saving = true
  try {
    await api.put(`/alarm-rules/${editRuleDlg.row.id}`, { templateId: editRuleDlg.templateId })
    toast.success('布防已更新')
    editRuleDlg.show = false
    loadRules()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    editRuleDlg.saving = false
  }
}

// ---------- 新建规则：三步向导（ALM-02） ----------
const ruleDlg = ref(false)
const ruleSaving = ref(false)
const step = ref(0) // 0 选通道 / 1 选事件类型 / 2 选布防时间
const ruleForm = reactive({ channelIds: [] as string[], kinds: [] as string[], templateId: '' })

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

// 能力集判断：通道 caps 含 event.<kind>（或 io）才支持；能力集为空的通道视为未知、不置灰
function chSupportsKind(ch: any, k: string) {
  const caps: string[] = ch.capabilities || []
  if (!caps.length) return true
  if (k === 'io') return caps.includes('io') || caps.includes('event.io')
  return caps.includes(`event.${k}`) || caps.includes('event.all')
}
// 所选通道全部支持该事件类型才可选（交集）
function kindEnabled(k: string) {
  const sel = channels.value.filter((c) => ruleForm.channelIds.includes(c.id))
  if (!sel.length) return true
  return sel.every((c) => chSupportsKind(c, k))
}
function toggleKind(k: string) {
  if (!kindEnabled(k)) return
  const i = ruleForm.kinds.indexOf(k)
  if (i >= 0) ruleForm.kinds.splice(i, 1)
  else ruleForm.kinds.push(k)
}

function toggleChannel(id: string) {
  const i = ruleForm.channelIds.indexOf(id)
  if (i >= 0) ruleForm.channelIds.splice(i, 1)
  else ruleForm.channelIds.push(id)
}
function toggleDevGroup(node: any) {
  const ids = (node.children || []).map((c: any) => c.value)
  const all = ids.length && ids.every((id: string) => ruleForm.channelIds.includes(id))
  ids.forEach((id: string) => {
    const i = ruleForm.channelIds.indexOf(id)
    if (all && i >= 0) ruleForm.channelIds.splice(i, 1)
    else if (!all && i < 0) ruleForm.channelIds.push(id)
  })
}
function onTreeSelect(node: any) {
  if (String(node.value).startsWith('dev:')) toggleDevGroup(node)
  else toggleChannel(node.value)
}
function groupChecked(node: any) {
  const ids = (node.children || []).map((c: any) => c.value)
  return ids.length > 0 && ids.every((id: string) => ruleForm.channelIds.includes(id))
}

function openRuleDlg() {
  ruleForm.channelIds = []
  ruleForm.kinds = []
  ruleForm.templateId = ''
  step.value = 0
  ruleDlg.value = true
}
function nextStep() {
  if (step.value === 0) {
    if (!ruleForm.channelIds.length) { toast.warning('请选择通道'); return }
    // 进入第二步时剔除已选通道不支持的类型
    ruleForm.kinds = ruleForm.kinds.filter((k) => kindEnabled(k))
  } else if (step.value === 1) {
    if (!ruleForm.kinds.length) { toast.warning('请选择事件类型'); return }
  }
  step.value++
}
function prevStep() {
  if (step.value > 0) step.value--
}

async function saveRule() {
  if (!ruleForm.channelIds.length) { toast.warning('请选择通道'); return }
  if (!ruleForm.kinds.length) { toast.warning('请选择事件类型'); return }
  if (!ruleForm.templateId) { toast.warning('请选择布防模板'); return }
  ruleSaving.value = true
  try {
    await api.post('/alarm-rules', {
      channelIds: ruleForm.channelIds,
      kinds: ruleForm.kinds,
      templateId: ruleForm.templateId
    })
    toast.success('规则已创建')
    ruleDlg.value = false
    loadRules()
  } catch (e: any) {
    toastApiError(e, '创建规则失败')
  } finally {
    ruleSaving.value = false
  }
}

// ---------- Tab B：布防模板 ----------
const tplLoading = ref(false)
const tplDlg = ref(false)
const tplSaving = ref(false)
const tplEditing = ref<any>(null) // null=新建
const tplForm = reactive({
  name: '',
  schedule: { days: [] as number[], ranges: [] as string[][] }
})

// 模板被多少条规则（通道）引用
const tplRefCount = computed(() => {
  const m: Record<string, number> = {}
  rules.value.forEach((r) => { m[r.templateId] = (m[r.templateId] || 0) + 1 })
  return m
})

async function loadTemplates() {
  tplLoading.value = true
  try {
    const res: any = await api.get('/alarm-templates')
    templates.value = res.items || []
  } catch (e: any) {
    toastApiError(e, '加载布防模板失败')
  } finally {
    tplLoading.value = false
  }
}

function openTplDlg(row?: any) {
  tplEditing.value = row || null
  tplForm.name = row?.name || ''
  tplForm.schedule = row?.schedule
    ? { days: [...(row.schedule.days || [])], ranges: (row.schedule.ranges || []).map((r: string[]) => [...r]) }
    : { days: [], ranges: [] }
  tplDlg.value = true
}

async function saveTpl() {
  if (!tplForm.name.trim()) { toast.warning('请填写模板名称'); return }
  // A7：修改自定义模板前，级联提示将同步影响引用该模板的通道
  const editing = tplEditing.value
  if (editing) {
    const n = tplRefCount.value[editing.id] || 0
    if (n > 0) {
      const usedChannels = rules.value
        .filter((r) => r.templateId === editing.id)
        .map((r) => channelMap.value[r.channelId] || r.channelId)
        .join('、')
      const ok = await confirm.ask({
        title: '修改布防模板',
        message: `修改后将同步更新使用该模板的 ${n} 个通道`,
        detail: `受影响通道：${usedChannels}`,
        confirmText: '继续保存'
      })
      if (!ok) return
    }
  }
  tplSaving.value = true
  try {
    if (editing) {
      await api.put(`/alarm-templates/${editing.id}`, { name: tplForm.name.trim(), schedule: tplForm.schedule })
      toast.success('模板已更新')
    } else {
      await api.post('/alarm-templates', { name: tplForm.name.trim(), schedule: tplForm.schedule })
      toast.success('模板已创建')
    }
    tplDlg.value = false
    loadTemplates()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    tplSaving.value = false
  }
}

async function delTpl(row: any) {
  if (row.builtin) return // 内置模板不可删
  const n = tplRefCount.value[row.id] || 0
  if (n > 0) {
    toast.warning(`该模板正被 ${n} 条规则使用，请先解绑后再删除`)
    return
  }
  const ok = await confirm.ask({
    title: '删除确认',
    message: `确定删除布防模板「${row.name}」？`,
    danger: true, confirmText: '删除'
  })
  if (!ok) return
  try {
    await api.del(`/alarm-templates/${row.id}`)
    toast.success('已删除')
    loadTemplates()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

onMounted(async () => {
  await loadBase()
  loadRules()
})

watch(tab, (v) => { if (v === 'templates') loadTemplates() })
</script>

<template>
  <div class="space-y-3">
    <UiCard flat>
      <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
        <div class="flex items-center gap-2">
          <UiButton variant="primary" @click="openRuleDlg">
            <UiIcon name="plus" :size="14" />新建规则
          </UiButton>
          <UiButton @click="loadRules">
            <UiIcon name="refresh" :size="14" />刷新
          </UiButton>
          <UiButton variant="secondary" @click="navigateTo('/alarms/templates')">
            <UiIcon name="calendar" :size="14" />管理布防模板
          </UiButton>
        </div>
        <span class="text-xs text-muted">一条规则 = 通道 × 事件类型 × 布防时间</span>
      </div>

      <UiTable
        :columns="[
          { key: 'channel', label: '通道', width: '180px' },
          { key: 'kinds', label: '事件类型', width: '260px', ellipsis: false },
          { key: 'template', label: '布防模板', width: '200px' },
          { key: 'enabled', label: '启用', width: '70px', align: 'center' },
          { key: 'ops', label: '操作', width: '150px', align: 'center', ellipsis: false }
        ]"
        :rows="rules" :loading="rulesLoading" :row-key="'id'" empty="暂无告警规则，点击「新建规则」开始配置"
      >
        <template #channel="{ row }">{{ channelMap[row.channelId] || row.channelId }}</template>
        <template #kinds="{ row }">
          <div class="flex flex-wrap gap-1">
            <UiTag v-for="k in row.kinds" :key="k" color="primary" plain>{{ kindName(k) }}</UiTag>
          </div>
        </template>
        <template #template="{ row }">
          <div>
            <div class="text-body">{{ templateMap[row.templateId] || row.templateId || '—' }}</div>
            <div class="text-xs text-placeholder">{{ fmtSchedule(templates.find(t => t.id === row.templateId)?.schedule) }}</div>
          </div>
        </template>
        <template #enabled="{ row }">
          <UiSwitch :model-value="!!row.enabled" size="sm" @update:model-value="toggleRule(row, $event)" />
        </template>
        <template #ops="{ row }">
          <div class="flex items-center justify-center gap-1">
            <UiButton variant="text" size="sm" @click="openEditRule(row)">修改布防</UiButton>
            <UiButton variant="dangerText" size="sm" @click="delRule(row)">删除</UiButton>
          </div>
        </template>
      </UiTable>
    </UiCard>

    <!-- 新建告警规则：三步向导（ALM-02） -->
    <UiDialog v-model:open="ruleDlg" title="新建告警规则" width="max-w-xl">
      <div class="mb-5 flex justify-center">
        <UiSteps :steps="['选择通道', '选择事件类型', '选择布防时间']" :current="step" />
      </div>

      <!-- 第一步：通道多选（分组树 + 复选） -->
      <div v-if="step === 0">
        <div class="mb-2 flex items-center justify-between">
          <span class="text-xs text-muted">按设备分组，可勾选设备整组或单个通道</span>
          <span class="text-xs text-primary">已选 {{ ruleForm.channelIds.length }} 个通道</span>
        </div>
        <div class="max-h-72 overflow-y-auto rounded border border-line p-2">
          <UiTree :nodes="channelTree" @select="onTreeSelect">
            <template #node="{ node }">
              <span class="flex items-center gap-1.5">
                <span
                  class="flex h-4 w-4 shrink-0 items-center justify-center rounded-sm border bg-surface"
                  :class="String(node.value).startsWith('dev:')
                    ? (groupChecked(node) ? 'border-primary bg-primary text-white' : 'border-line')
                    : (ruleForm.channelIds.includes(node.value) ? 'border-primary bg-primary text-white' : 'border-line')"
                >
                  <UiIcon
                    v-if="String(node.value).startsWith('dev:') ? groupChecked(node) : ruleForm.channelIds.includes(node.value)"
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

      <!-- 第二步：事件类型复选（按能力集过滤） -->
      <div v-else-if="step === 1">
        <p class="mb-2 text-xs text-muted">仅所选通道全部支持的事件类型可选；灰色为设备能力不支持</p>
        <div class="grid grid-cols-2 gap-2">
          <div
            v-for="o in KIND_OPTIONS" :key="o.value"
            class="flex cursor-pointer items-center gap-2 rounded border px-3 py-2.5 text-sm transition-colors"
            :class="[
              !kindEnabled(o.value)
                ? 'cursor-not-allowed border-line-soft bg-zone text-placeholder'
                : ruleForm.kinds.includes(o.value)
                  ? 'border-primary bg-primary-soft text-primary'
                  : 'border-line text-body hover:border-primary'
            ]"
            @click="toggleKind(o.value)"
          >
            <span
              class="flex h-4 w-4 shrink-0 items-center justify-center rounded-sm border bg-surface"
              :class="ruleForm.kinds.includes(o.value) ? 'border-primary bg-primary text-white' : kindEnabled(o.value) ? 'border-line' : 'border-line-soft'"
            >
              <UiIcon v-if="ruleForm.kinds.includes(o.value)" name="check" :size="10" :stroke="3" />
            </span>
            {{ o.label }}
            <span v-if="!kindEnabled(o.value)" class="ml-auto text-xs">不支持</span>
          </div>
        </div>
      </div>

      <!-- 第三步：布防模板单选 -->
      <div v-else>
        <p class="mb-2 text-xs text-muted">选择规则生效的布防时间段（模板可在「布防模板」页维护）</p>
        <div class="max-h-72 space-y-2 overflow-y-auto">
          <div
            v-for="t in templates" :key="t.id"
            class="flex cursor-pointer items-center gap-2.5 rounded border px-3 py-2.5 transition-colors"
            :class="ruleForm.templateId === t.id ? 'border-primary bg-primary-soft' : 'border-line hover:border-primary'"
            @click="ruleForm.templateId = t.id"
          >
            <span class="flex h-4 w-4 shrink-0 items-center justify-center rounded-full border" :class="ruleForm.templateId === t.id ? 'border-primary' : 'border-line'">
              <span v-if="ruleForm.templateId === t.id" class="h-2 w-2 rounded-full bg-primary" />
            </span>
            <div class="min-w-0">
              <div class="flex items-center gap-1.5 text-sm text-ink">
                {{ t.name }}
                <UiTag v-if="t.builtin" color="info">内置</UiTag>
              </div>
              <div class="text-xs text-placeholder">{{ fmtSchedule(t.schedule) }}</div>
            </div>
          </div>
          <div v-if="!templates.length" class="py-6 text-center text-sm text-placeholder">暂无布防模板</div>
        </div>
      </div>

      <template #footer>
        <div class="flex w-full items-center justify-between">
          <UiButton :disabled="step === 0" @click="prevStep">上一步</UiButton>
          <div class="flex items-center gap-2">
            <UiButton @click="ruleDlg = false">取消</UiButton>
            <UiButton v-if="step < 2" variant="primary" @click="nextStep">下一步</UiButton>
            <UiButton v-else variant="primary" :disabled="ruleSaving" @click="saveRule">
              <UiIcon v-if="!ruleSaving" name="check" :size="14" />
              <UiIcon v-else name="refresh" :size="14" class="ipc-spin" />
              确定
            </UiButton>
          </div>
        </div>
      </template>
    </UiDialog>

    <!-- 修改布防 -->
    <UiDialog v-model:open="editRuleDlg.show" title="修改布防" width="max-w-md">
      <div class="space-y-3">
        <div class="flex items-center gap-2 text-sm">
          <span class="text-muted">通道</span>
          <span class="text-ink">{{ channelMap[editRuleDlg.row?.channelId] || editRuleDlg.row?.channelId }}</span>
        </div>
        <div>
          <p class="mb-1.5 text-sm text-muted">布防模板</p>
          <UiSelect v-model="editRuleDlg.templateId" placeholder="选择布防模板" :options="templates.map(t => ({ label: `${t.name}（${fmtSchedule(t.schedule)}）`, value: t.id }))" />
        </div>
      </div>
      <template #footer>
        <UiButton @click="editRuleDlg.show = false">取消</UiButton>
        <UiButton variant="primary" :disabled="editRuleDlg.saving" @click="saveEditRule">确定</UiButton>
      </template>
    </UiDialog>

    <!-- 新建 / 编辑布防模板 -->
    <UiDialog v-model:open="tplDlg" :title="tplEditing ? '编辑布防模板' : '新建布防模板'" width="max-w-2xl">
      <div class="space-y-4">
        <div class="flex items-center gap-3">
          <label class="shrink-0 text-sm text-body"><span class="text-danger">*</span> 模板名称</label>
          <UiInput v-model="tplForm.name" placeholder="如：夜间布防" :maxlength="30" width="w-64" />
        </div>
        <div>
          <p class="mb-1.5 text-sm text-muted">布防时间（在网格上拖选时段）</p>
          <ScheduleGrid v-model="tplForm.schedule" />
        </div>
        <p v-if="tplEditing && (tplRefCount[tplEditing.id] || 0) > 0" class="flex items-center gap-1.5 rounded bg-warning-soft px-3 py-2 text-xs text-warning">
          <UiIcon name="alert-triangle" :size="13" />
          该模板正被 {{ tplRefCount[tplEditing.id] }} 个通道使用，保存后将同步更新这些通道的布防时间
        </p>
      </div>
      <template #footer>
        <UiButton @click="tplDlg = false">取消</UiButton>
        <UiButton variant="primary" :disabled="tplSaving" @click="saveTpl">确定</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
