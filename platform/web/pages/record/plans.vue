<script setup lang="ts">
// 录像设置（REC-06）：通道录像计划管理（三步向导：选通道 → 选模板 → 选码流）
const api = useApi()
const toast = useToast()
const confirm = useConfirm()
const { t } = useI18n()

/** 码流展示名（主/子码流），随语言切换 */
const profileName = (p: string) => (p === 'sub' ? t('record.profile.sub') : p === 'main' ? t('record.profile.main') : p)

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
    toastApiError(e, t('record.msg.loadBaseFailed'))
  }
}

// ---------- 存储概览（REC-07）----------
// 后端 /storage/overview 已实现（用量、总量、片段数、保留天数），此前前端零调用。
const storage = ref<any>(null)
const storageErr = ref('')

async function loadStorage() {
  storageErr.value = ''
  try {
    storage.value = await api.get('/storage/overview')
  } catch (e: any) {
    // 不弹 toast：这是页面上的一张辅助卡片，失败就地显示原因即可
    storage.value = null
    storageErr.value = e?.msg || t('record.storage.loadFailed')
  }
}

/** 用量条颜色：越接近上限越警戒；90% 以上后端会产生 disk_full 告警 */
const storageColor = computed(() => {
  const pct = Number(storage.value?.percent) || 0
  if (pct >= 90) return 'var(--color-danger)'
  if (pct >= 75) return 'var(--color-warning)'
  return 'var(--color-primary)'
})

// ---------- 计划列表 ----------
const plans = ref<any[]>([])
const loading = ref(false)

async function loadPlans() {
  loading.value = true
  try {
    const res: any = await api.get('/record-plans')
    plans.value = res.items || []
  } catch (e: any) {
    toastApiError(e, t('record.msg.loadPlansFailed'))
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
    toastApiError(e, t('record.msg.actionFailed'))
  }
}

async function delPlan(row: any) {
  const ok = await confirm.ask({
    title: t('record.msg.deletePlanTitle'),
    message: t('record.msg.deletePlanMessage', { name: channelMap.value[row.channelId] || row.channelId }),
    detail: t('record.msg.deletePlanDetail'),
    danger: true, confirmText: t('common.delete')
  })
  if (!ok) return
  try {
    await api.del(`/record-plans/${row.id}`)
    toast.success(t('common.deletedOk'))
    loadPlans()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
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
  if (step.value === 0 && !form.channelIds.length) { toast.warning(t('record.msg.pickChannel')); return }
  if (step.value === 1 && !form.templateId) { toast.warning(t('record.msg.pickTemplate')); return }
  step.value++
}
function prevStep() {
  if (step.value > 0) step.value--
}

async function save() {
  if (!form.channelIds.length) { toast.warning(t('record.msg.pickChannel')); return }
  if (!form.templateId) { toast.warning(t('record.msg.pickTemplate')); return }
  saving.value = true
  try {
    await api.post('/record-plans', {
      channelIds: form.channelIds,
      templateId: form.templateId,
      profile: form.profile
    })
    toast.success(t('record.msg.planCreated'))
    dlg.value = false
    loadPlans()
  } catch (e: any) {
    toastApiError(e, t('record.msg.createPlanFailed'))
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
  if (!editTplDlg.templateId) { toast.warning(t('record.msg.pickTemplate')); return }
  editTplDlg.saving = true
  try {
    await api.put(`/record-plans/${editTplDlg.row.id}`, { templateId: editTplDlg.templateId })
    toast.success(t('record.msg.planUpdated'))
    editTplDlg.show = false
    loadPlans()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
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
    toast.success(t('record.msg.profileUpdated'))
    editProfDlg.show = false
    loadPlans()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    editProfDlg.saving = false
  }
}

onMounted(() => {
  loadStorage()
  loadBase()
  loadPlans()
})

/* 客户端分页（E7）：该列表接口一次性返回全部数据，此前全量渲染。
   服务端分页需后端配合，属后续工作。 */
const { page: pgPage, pageSize: pgSize, total: pgTotal, pageItems: pgItems } = useClientPage(plans)
</script>

<template>
  <div class="space-y-3">
    <!-- 存储概览（REC-07）：录像占了多少空间、还能存多久 -->
    <div class="rounded-signal border border-line bg-surface px-4 py-3">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div class="flex items-center gap-2">
          <Icon name="hard-drive" :size="15" class="text-muted" />
          <span class="text-sm font-medium text-ink">{{ t('record.storage.title') }}</span>
        </div>
        <button
          type="button"
          class="rounded-chrome text-xs text-muted transition-colors hover:text-primary"
          @click="loadStorage"
        >{{ t('record.storage.recalc') }}</button>
      </div>

      <p v-if="storageErr" class="mt-2 text-xs text-danger">{{ storageErr }}</p>

      <template v-else-if="storage">
        <div class="mt-2.5 h-1.5 overflow-hidden rounded-full bg-line">
          <div
            class="h-full rounded-full transition-all"
            :style="{ width: Math.min(100, Number(storage.percent) || 0) + '%', background: storageColor }"
          />
        </div>
        <div class="mt-2 flex flex-wrap items-center gap-x-6 gap-y-1 text-xs">
          <span class="text-body">
            {{ t('record.storage.used') }} <span class="font-mono text-ink">{{ fmtBytes(storage.usedBytes) }}</span>
            / <span class="font-mono">{{ fmtBytes(storage.totalBytes) }}</span>
            <span class="ml-1 text-placeholder">{{ t('record.storage.percent', { n: storage.percent ?? 0 }) }}</span>
          </span>
          <span class="text-placeholder">{{ t('record.storage.segments') }} <span class="font-mono text-body">{{ storage.segments ?? 0 }}</span> {{ t('record.storage.segmentsUnit') }}</span>
          <span class="text-placeholder">{{ t('record.storage.keep') }} <span class="font-mono text-body">{{ storage.keepDays ?? 30 }}</span> {{ t('record.storage.keepUnit') }}</span>
        </div>
        <p v-if="Number(storage.percent) >= 90" class="mt-1.5 text-xs text-danger">
          {{ t('record.storage.nearFull') }}
        </p>
      </template>

      <p v-else class="mt-2 text-xs text-placeholder">{{ t('record.storage.calculating') }}</p>
    </div>

    <UiCard flat>
      <template #header>
        <div class="flex w-full items-center justify-between">
          <span class="text-[15px] font-semibold text-ink">{{ t('record.plans.title') }}</span>
          <div class="flex items-center gap-2">
            <UiButton size="sm" @click="() => { loadBase(); loadPlans() }"><UiIcon name="refresh" :size="13" />{{ t('common.refresh') }}</UiButton>
            <UiButton variant="primary" size="sm" @click="openDlg"><UiIcon name="plus" :size="14" />{{ t('record.plans.create') }}</UiButton>
          </div>
        </div>
      </template>

      <UiTable
        :columns="[
          { key: 'channel', label: t('record.plans.colChannel'), width: '200px' },
          { key: 'template', label: t('record.plans.colTemplate'), width: '200px' },
          { key: 'profile', label: t('record.plans.colProfile'), width: '100px' },
          { key: 'enabled', label: t('record.plans.colEnabled'), width: '70px', align: 'center' },
          { key: 'ops', label: t('common.action'), width: '220px', align: 'center', ellipsis: false }
        ]"
        :rows="pgItems" :loading="loading" :row-key="'id'" :empty="t('record.plans.empty')"
      >
        <template #channel="{ row }">{{ channelMap[row.channelId] || row.channelId }}</template>
        <template #template="{ row }">{{ templateMap[row.templateId] || row.templateId || '—' }}</template>
        <template #profile="{ row }">
          <UiTag :color="row.profile === 'sub' ? 'info' : 'primary'" plain>{{ profileName(row.profile) }}</UiTag>
        </template>
        <template #enabled="{ row }">
          <UiSwitch :model-value="!!row.enabled" size="sm" :aria-label="t('record.plans.toggleAria', { name: row.name || row.id })" @update:model-value="togglePlan(row, $event)" />
        </template>
        <template #ops="{ row }">
          <div class="flex items-center justify-center gap-1">
            <UiButton variant="text" size="sm" @click="openEditTpl(row)">{{ t('record.plans.editPlan') }}</UiButton>
            <UiButton variant="text" size="sm" @click="openEditProfile(row)">{{ t('record.plans.editProfile') }}</UiButton>
            <UiButton variant="dangerText" size="sm" @click="delPlan(row)">{{ t('common.delete') }}</UiButton>
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

    <!-- 新建录像设置：三步向导（REC-06） -->
    <UiDialog v-model:open="dlg" :title="t('record.plans.dialogTitle')" width="max-w-xl">
      <div class="mb-5 flex justify-center">
        <UiSteps :steps="[t('record.plans.stepChannel'), t('record.plans.stepTemplate'), t('record.plans.stepProfile')]" :current="step" />
      </div>

      <!-- 第一步：通道多选 -->
      <div v-if="step === 0">
        <div class="mb-2 flex items-center justify-between">
          <span class="text-xs text-muted">{{ t('record.plans.channelHint') }}</span>
          <span class="text-xs text-primary">{{ t('record.plans.selectedChannels', { n: form.channelIds.length }) }}</span>
        </div>
        <div class="max-h-72 overflow-y-auto rounded-signal border border-line p-2">
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
          <div v-if="!channelTree.length" class="py-6 text-center text-sm text-placeholder">{{ t('record.plans.noChannels') }}</div>
        </div>
      </div>

      <!-- 第二步：模板单选 -->
      <div v-else-if="step === 1">
        <p class="mb-2 text-xs text-muted">{{ t('record.plans.templateHint') }}</p>
        <div class="max-h-72 space-y-2 overflow-y-auto">
          <div
            v-for="tpl in templates" :key="tpl.id"
            class="flex cursor-pointer items-center gap-2.5 rounded-signal border px-3 py-2.5 transition-colors"
            :class="form.templateId === tpl.id ? 'border-primary bg-primary-soft' : 'border-line hover:border-primary'"
            @click="form.templateId = tpl.id"
          >
            <span class="flex h-4 w-4 shrink-0 items-center justify-center rounded-full border" :class="form.templateId === tpl.id ? 'border-primary' : 'border-line'">
              <span v-if="form.templateId === tpl.id" class="h-2 w-2 rounded-full bg-primary" />
            </span>
            <div class="min-w-0">
              <div class="flex items-center gap-1.5 text-sm text-ink">
                {{ tpl.name }}
                <!-- 录像三色语义（REC-02）：定时=primary/信号青，事件=success/绿，与 playback.vue 一致 -->
                <UiTag :color="tpl.kind === 'event' ? 'success' : 'primary'" plain>{{ tpl.kind === 'event' ? t('record.kind.event') : t('record.kind.timer') }}</UiTag>
                <UiTag v-if="tpl.builtin" color="info">{{ t('record.plans.builtin') }}</UiTag>
              </div>
            </div>
          </div>
          <div v-if="!templates.length" class="py-6 text-center text-sm text-placeholder">{{ t('record.plans.noTemplates') }}</div>
        </div>
      </div>

      <!-- 第三步：码流 -->
      <div v-else>
        <p class="mb-2 text-xs text-muted">{{ t('record.plans.profileHint') }}</p>
        <div class="grid grid-cols-2 gap-2">
          <div
            v-for="prof in ['main', 'sub']" :key="prof"
            class="flex cursor-pointer items-center gap-2.5 rounded-signal border px-3 py-3 transition-colors"
            :class="form.profile === prof ? 'border-primary bg-primary-soft' : 'border-line hover:border-primary'"
            @click="form.profile = prof"
          >
            <span class="flex h-4 w-4 shrink-0 items-center justify-center rounded-full border" :class="form.profile === prof ? 'border-primary' : 'border-line'">
              <span v-if="form.profile === prof" class="h-2 w-2 rounded-full bg-primary" />
            </span>
            <div>
              <div class="text-sm text-ink">{{ profileName(prof) }}</div>
              <div class="text-xs text-placeholder">{{ prof === 'main' ? t('record.plans.mainDesc') : t('record.plans.subDesc') }}</div>
            </div>
          </div>
        </div>
        <div class="mt-4 rounded-signal bg-zone px-3 py-2.5 text-xs text-muted">
          {{ t('record.plans.summary', { count: form.channelIds.length, template: templateMap[form.templateId] || '—', profile: profileName(form.profile) }) }}
        </div>
      </div>

      <template #footer>
        <div class="flex w-full items-center justify-between">
          <UiButton :disabled="step === 0" @click="prevStep">{{ t('record.plans.prevStep') }}</UiButton>
          <div class="flex items-center gap-2">
            <UiButton @click="dlg = false">{{ t('common.cancel') }}</UiButton>
            <UiButton v-if="step < 2" variant="primary" @click="nextStep">{{ t('record.plans.nextStep') }}</UiButton>
            <UiButton v-else variant="primary" :disabled="saving" @click="save">
              <UiIcon v-if="!saving" name="check" :size="14" />
              <UiIcon v-else name="refresh" :size="14" class="ipc-spin" />
              {{ t('common.confirm') }}
            </UiButton>
          </div>
        </div>
      </template>
    </UiDialog>

    <!-- 修改计划（更换模板） -->
    <UiDialog v-model:open="editTplDlg.show" :title="t('record.plans.editPlanTitle')" width="max-w-md">
      <div class="space-y-3">
        <div class="flex items-center gap-2 text-sm">
          <span class="text-muted">{{ t('record.plans.channel') }}</span>
          <span class="text-ink">{{ channelMap[editTplDlg.row?.channelId] || editTplDlg.row?.channelId }}</span>
        </div>
        <div>
          <p class="mb-1.5 text-sm text-muted">{{ t('record.plans.template') }}</p>
          <UiSelect v-model="editTplDlg.templateId" :placeholder="t('record.plans.templatePlaceholder')" :options="templates.map(tpl => ({ label: tpl.name, value: tpl.id }))" />
        </div>
      </div>
      <template #footer>
        <UiButton @click="editTplDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="editTplDlg.saving" @click="saveEditTpl">{{ t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 修改码流 -->
    <UiDialog v-model:open="editProfDlg.show" :title="t('record.plans.editProfileTitle')" width="max-w-sm">
      <div class="space-y-3">
        <div class="flex items-center gap-2 text-sm">
          <span class="text-muted">{{ t('record.plans.channel') }}</span>
          <span class="text-ink">{{ channelMap[editProfDlg.row?.channelId] || editProfDlg.row?.channelId }}</span>
        </div>
        <UiSegmented v-model="editProfDlg.profile" :items="[{ label: t('record.profile.main'), value: 'main' }, { label: t('record.profile.sub'), value: 'sub' }]" />
      </div>
      <template #footer>
        <UiButton @click="editProfDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="editProfDlg.saving" @click="saveEditProfile">{{ t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
