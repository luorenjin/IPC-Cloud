<script setup lang="ts">
// 告警规则（ALM-02，三步向导）+ 布防模板（ALM-01，含级联提示 A7）
const api = useApi()
const toast = useToast()
const confirm = useConfirm()
const { t } = useI18n()

// 事件类型（ALM-02：六类，按通道能力集过滤置灰）映射见 utils/enums.ts
const KIND_OPTIONS = ALARM_KIND_OPTIONS
// 走词条键，随语言切换；alarmKindName 只是中文兜底
const kindName = (k: string) => t(alarmKindKey(k))

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
    toastApiError(e, t('alarm.msg.loadBaseFailed'))
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
    toastApiError(e, t('alarm.msg.loadRulesFailed'))
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
    toastApiError(e, t('alarm.msg.actionFailed'))
  }
}

async function delRule(row: any) {
  const ok = await confirm.ask({
    title: t('alarm.msg.deleteRuleTitle'),
    message: t('alarm.msg.deleteRuleMessage', { name: channelMap.value[row.channelId] || row.channelId }),
    detail: t('alarm.msg.deleteRuleDetail'),
    danger: true, confirmText: t('common.delete')
  })
  if (!ok) return
  try {
    await api.del(`/alarm-rules/${row.id}`)
    toast.success(t('common.deletedOk'))
    loadRules()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
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
  if (!editRuleDlg.templateId) { toast.warning(t('alarm.msg.pickTemplate')); return }
  editRuleDlg.saving = true
  try {
    await api.put(`/alarm-rules/${editRuleDlg.row.id}`, { templateId: editRuleDlg.templateId })
    toast.success(t('alarm.msg.armingUpdated'))
    editRuleDlg.show = false
    loadRules()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
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
    if (!ruleForm.channelIds.length) { toast.warning(t('alarm.msg.pickChannel')); return }
    // 进入第二步时剔除已选通道不支持的类型
    ruleForm.kinds = ruleForm.kinds.filter((k) => kindEnabled(k))
  } else if (step.value === 1) {
    if (!ruleForm.kinds.length) { toast.warning(t('alarm.msg.pickKind')); return }
  }
  step.value++
}
function prevStep() {
  if (step.value > 0) step.value--
}

async function saveRule() {
  if (!ruleForm.channelIds.length) { toast.warning(t('alarm.msg.pickChannel')); return }
  if (!ruleForm.kinds.length) { toast.warning(t('alarm.msg.pickKind')); return }
  if (!ruleForm.templateId) { toast.warning(t('alarm.msg.pickTemplate')); return }
  ruleSaving.value = true
  try {
    await api.post('/alarm-rules', {
      channelIds: ruleForm.channelIds,
      kinds: ruleForm.kinds,
      templateId: ruleForm.templateId
    })
    toast.success(t('alarm.msg.ruleCreated'))
    ruleDlg.value = false
    loadRules()
  } catch (e: any) {
    toastApiError(e, t('alarm.msg.createRuleFailed'))
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
    toastApiError(e, t('alarm.msg.loadTemplatesFailed'))
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
  if (!tplForm.name.trim()) { toast.warning(t('alarm.msg.nameRequired')); return }
  // A7：修改自定义模板前，级联提示将同步影响引用该模板的通道
  const editing = tplEditing.value
  if (editing) {
    const n = tplRefCount.value[editing.id] || 0
    if (n > 0) {
      const usedChannels = rules.value
        .filter((r) => r.templateId === editing.id)
        .map((r) => channelMap.value[r.channelId] || r.channelId)
        .join(t('alarm.list.sep'))
      const ok = await confirm.ask({
        title: t('alarm.msg.tplCascadeTitle'),
        message: t('alarm.msg.tplCascadeMessage', { n }),
        detail: t('alarm.msg.tplCascadeDetail', { channels: usedChannels }),
        confirmText: t('alarm.msg.tplCascadeConfirm')
      })
      if (!ok) return
    }
  }
  tplSaving.value = true
  try {
    if (editing) {
      await api.put(`/alarm-templates/${editing.id}`, { name: tplForm.name.trim(), schedule: tplForm.schedule })
      toast.success(t('alarm.msg.tplUpdated'))
    } else {
      await api.post('/alarm-templates', { name: tplForm.name.trim(), schedule: tplForm.schedule })
      toast.success(t('alarm.msg.tplCreated'))
    }
    tplDlg.value = false
    loadTemplates()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    tplSaving.value = false
  }
}

async function delTpl(row: any) {
  if (row.builtin) return // 内置模板不可删
  const n = tplRefCount.value[row.id] || 0
  if (n > 0) {
    toast.warning(t('alarm.msg.tplInUse', { n }))
    return
  }
  const ok = await confirm.ask({
    title: t('alarm.msg.deleteTplTitle'),
    message: t('alarm.msg.deleteTplMessage', { name: row.name }),
    danger: true, confirmText: t('common.delete')
  })
  if (!ok) return
  try {
    await api.del(`/alarm-templates/${row.id}`)
    toast.success(t('common.deletedOk'))
    loadTemplates()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
  }
}

onMounted(async () => {
  await loadBase()
  loadRules()
})

watch(tab, (v) => { if (v === 'templates') loadTemplates() })

/* 客户端分页（E7）：该列表接口一次性返回全部数据，此前全量渲染。
   服务端分页需后端配合，属后续工作。 */
const { page: pgPage, pageSize: pgSize, total: pgTotal, pageItems: pgItems } = useClientPage(rules)
</script>

<template>
  <div class="space-y-3">
    <UiCard flat>
      <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
        <div class="flex items-center gap-2">
          <UiButton variant="primary" @click="openRuleDlg">
            <UiIcon name="plus" :size="14" />{{ t('alarm.rules.createRule') }}
          </UiButton>
          <UiButton @click="loadRules">
            <UiIcon name="refresh" :size="14" />{{ t('common.refresh') }}
          </UiButton>
          <UiButton @click="navigateTo('/alarms/templates')">
            <UiIcon name="calendar" :size="14" />{{ t('alarm.rules.manageTemplates') }}
          </UiButton>
        </div>
        <span class="text-xs text-muted">{{ t('alarm.rules.formula') }}</span>
      </div>

      <UiTable
        :columns="[
          { key: 'channel', label: t('alarm.rules.colChannel'), width: '180px' },
          { key: 'kinds', label: t('alarm.rules.colKinds'), width: '260px', ellipsis: false },
          { key: 'template', label: t('alarm.rules.colTemplate'), width: '200px' },
          { key: 'enabled', label: t('alarm.rules.colEnabled'), width: '70px', align: 'center' },
          { key: 'ops', label: t('common.action'), width: '150px', align: 'center', ellipsis: false }
        ]"
        :rows="pgItems" :loading="rulesLoading" :row-key="'id'" :empty="t('alarm.rules.empty')"
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
            <div class="text-xs text-placeholder">{{ fmtSchedule(templates.find(tpl => tpl.id === row.templateId)?.schedule, t('alarm.rules.notArmed')) }}</div>
          </div>
        </template>
        <template #enabled="{ row }">
          <UiSwitch :model-value="!!row.enabled" size="sm" :aria-label="t('alarm.rules.toggleAria', { name: channelMap[row.channelId] || row.id })" @update:model-value="toggleRule(row, $event)" />
        </template>
        <template #ops="{ row }">
          <div class="flex items-center justify-center gap-1">
            <UiButton variant="text" size="sm" @click="openEditRule(row)">{{ t('alarm.rules.editArming') }}</UiButton>
            <UiButton variant="dangerText" size="sm" @click="delRule(row)">{{ t('common.delete') }}</UiButton>
          </div>
        </template>
      </UiTable>
      <div v-if="pgTotal > pgSize" class="mt-3 flex justify-end">
        <UiPagination
          v-model:page="pgPage"
          v-model:page-size="pgSize"
          :total="pgTotal"
        />
      </div>
    </UiCard>

    <!-- 新建告警规则（ALM-02）：触发条件/联动动作/生效时间三块内容，字段间非严格线性依赖，用 Tab 而非编号步骤器，允许自由切换 -->
    <UiDialog v-model:open="ruleDlg" :title="t('alarm.rules.dialogTitle')" width="max-w-xl">
      <UiTabs
        :model-value="String(step)"
        :items="[
          { label: t('alarm.rules.stepTrigger'), value: '0' },
          { label: t('alarm.rules.stepAction'), value: '1' },
          { label: t('alarm.rules.stepSchedule'), value: '2' }
        ]"
        @update:model-value="step = Number($event)"
      >
        <div class="pt-4">
          <!-- 触发条件：通道多选（分组树 + 复选） -->
          <div v-if="step === 0">
            <div class="mb-2 flex items-center justify-between">
              <span class="text-xs text-muted">{{ t('alarm.rules.channelHint') }}</span>
              <span class="text-xs text-primary">{{ t('alarm.rules.selectedChannels', { n: ruleForm.channelIds.length }) }}</span>
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
              <div v-if="!channelTree.length" class="py-6 text-center text-sm text-placeholder">{{ t('alarm.rules.noChannels') }}</div>
            </div>
          </div>

          <!-- 联动动作：事件类型复选（按能力集过滤） -->
          <div v-else-if="step === 1">
            <p class="mb-2 text-xs text-muted">{{ t('alarm.rules.kindHint') }}</p>
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
                <span v-if="!kindEnabled(o.value)" class="ml-auto text-xs">{{ t('alarm.rules.kindUnsupported') }}</span>
              </div>
            </div>
          </div>

          <!-- 生效时间：布防模板单选 -->
          <div v-else>
            <p class="mb-2 text-xs text-muted">{{ t('alarm.rules.templateHint') }}</p>
            <div class="max-h-72 space-y-2 overflow-y-auto">
              <div
                v-for="tpl in templates" :key="tpl.id"
                class="flex cursor-pointer items-center gap-2.5 rounded border px-3 py-2.5 transition-colors"
                :class="ruleForm.templateId === tpl.id ? 'border-primary bg-primary-soft' : 'border-line hover:border-primary'"
                @click="ruleForm.templateId = tpl.id"
              >
                <span class="flex h-4 w-4 shrink-0 items-center justify-center rounded-full border" :class="ruleForm.templateId === tpl.id ? 'border-primary' : 'border-line'">
                  <span v-if="ruleForm.templateId === tpl.id" class="h-2 w-2 rounded-full bg-primary" />
                </span>
                <div class="min-w-0">
                  <div class="flex items-center gap-1.5 text-sm text-ink">
                    {{ tpl.name }}
                    <UiTag v-if="tpl.builtin" color="info">{{ t('alarm.rules.builtin') }}</UiTag>
                  </div>
                  <div class="text-xs text-placeholder">{{ fmtSchedule(tpl.schedule, t('alarm.rules.notArmed')) }}</div>
                </div>
              </div>
              <div v-if="!templates.length" class="py-6 text-center text-sm text-placeholder">{{ t('alarm.rules.noTemplates') }}</div>
            </div>
          </div>
        </div>
      </UiTabs>

      <template #footer>
        <div class="flex w-full items-center justify-between">
          <UiButton :disabled="step === 0" @click="prevStep">{{ t('alarm.rules.prevStep') }}</UiButton>
          <div class="flex items-center gap-2">
            <UiButton @click="ruleDlg = false">{{ t('common.cancel') }}</UiButton>
            <UiButton v-if="step < 2" variant="primary" @click="nextStep">{{ t('alarm.rules.nextStep') }}</UiButton>
            <UiButton v-else variant="primary" :disabled="ruleSaving" @click="saveRule">
              <UiIcon v-if="!ruleSaving" name="check" :size="14" />
              <UiIcon v-else name="refresh" :size="14" class="ipc-spin" />
              {{ t('common.confirm') }}
            </UiButton>
          </div>
        </div>
      </template>
    </UiDialog>

    <!-- 修改布防 -->
    <UiDialog v-model:open="editRuleDlg.show" :title="t('alarm.rules.editArmingTitle')" width="max-w-md">
      <div class="space-y-3">
        <div class="flex items-center gap-2 text-sm">
          <span class="text-muted">{{ t('alarm.rules.channel') }}</span>
          <span class="text-ink">{{ channelMap[editRuleDlg.row?.channelId] || editRuleDlg.row?.channelId }}</span>
        </div>
        <div>
          <p class="mb-1.5 text-sm text-muted">{{ t('alarm.rules.template') }}</p>
          <UiSelect v-model="editRuleDlg.templateId" :placeholder="t('alarm.rules.templatePlaceholder')" :options="templates.map(tpl => ({ label: t('alarm.rules.templateOption', { name: tpl.name, schedule: fmtSchedule(tpl.schedule, t('alarm.rules.notArmed')) }), value: tpl.id }))" />
        </div>
      </div>
      <template #footer>
        <UiButton @click="editRuleDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="editRuleDlg.saving" @click="saveEditRule">{{ t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 新建 / 编辑布防模板 -->
    <UiDialog v-model:open="tplDlg" :title="tplEditing ? t('alarm.rules.tplEditTitle') : t('alarm.rules.tplCreateTitle')" width="max-w-2xl">
      <div class="space-y-4">
        <div class="flex items-center gap-3">
          <label class="shrink-0 text-sm text-body"><span class="text-danger">*</span> {{ t('alarm.rules.tplName') }}</label>
          <UiInput v-model="tplForm.name" :placeholder="t('alarm.rules.tplNamePlaceholder')" :maxlength="30" width="w-64" />
        </div>
        <div>
          <p class="mb-1.5 text-sm text-muted">{{ t('alarm.rules.tplSchedule') }}</p>
          <ScheduleGrid v-model="tplForm.schedule" />
        </div>
        <p v-if="tplEditing && (tplRefCount[tplEditing.id] || 0) > 0" class="flex items-center gap-1.5 rounded bg-warning-soft px-3 py-2 text-xs text-warning">
          <UiIcon name="alert-triangle" :size="13" />
          {{ t('alarm.rules.tplRefWarn', { n: tplRefCount[tplEditing.id] }) }}
        </p>
      </div>
      <template #footer>
        <UiButton @click="tplDlg = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="tplSaving" @click="saveTpl">{{ t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
