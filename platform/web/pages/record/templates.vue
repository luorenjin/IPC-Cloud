<script setup lang="ts">
// 录像计划模板（REC-05）：定时/事件模板管理（编辑/复制/删除 + 级联提示 A7）
const api = useApi()
const toast = useToast()
const confirm = useConfirm()
const { t } = useI18n()

/** 录像类型展示名（定时/事件），随语言切换 */
const kindName = (k: string) => (k === 'event' ? t('record.kind.event') : k === 'timer' ? t('record.kind.timer') : k)

// ---------- 列表 ----------
const items = ref<any[]>([])
const loading = ref(false)

// 通道引用数（来自录像计划）与通道名称
const plans = ref<any[]>([])
const channels = ref<any[]>([])
const channelMap = computed(() => Object.fromEntries(channels.value.map((c: any) => [c.id, c.name])))
const refCount = computed(() => {
  const m: Record<string, number> = {}
  plans.value.forEach((p) => { m[p.templateId] = (m[p.templateId] || 0) + 1 })
  return m
})
function refChannels(id: string) {
  return plans.value
    .filter((p) => p.templateId === id)
    .map((p) => channelMap.value[p.channelId] || p.channelId)
    .join(t('record.msg.listSep'))
}

async function load() {
  loading.value = true
  try {
    const res: any = await api.get('/record-templates')
    items.value = res.items || []
  } catch (e: any) {
    toastApiError(e, t('record.msg.loadTemplatesFailed'))
  } finally {
    loading.value = false
  }
}

const refsLoadFailed = ref(false)

async function loadRefs() {
  try {
    const [pRes, cRes]: any[] = await Promise.all([api.get('/record-plans'), api.get('/channels')])
    plans.value = pRes.items || []
    channels.value = cRes.items || cRes || []
    refsLoadFailed.value = false
  } catch (e: any) {
    refsLoadFailed.value = true
    toastApiError(e, t('record.msg.loadRefsFailed'))
  }
}

// ---------- 新建 / 编辑模板 ----------
const dlg = ref(false)
const saving = ref(false)
const editing = ref<any>(null) // null=新建
const form = reactive({
  name: '',
  kind: 'timer',
  schedule: { days: [] as number[], ranges: [] as string[][] }
})

function openDlg(row?: any) {
  if (row?.builtin) return // 内置模板不可编辑
  editing.value = row || null
  form.name = row?.name || ''
  form.kind = row?.kind || 'timer'
  form.schedule = row?.schedule
    ? { days: [...(row.schedule.days || [])], ranges: (row.schedule.ranges || []).map((r: string[]) => [...r]) }
    : { days: [], ranges: [] }
  dlg.value = true
}

async function save() {
  if (!form.name.trim()) { toast.warning(t('record.msg.nameRequired')); return }
  // A7：修改自定义模板前，级联提示将同步影响引用该模板的通道
  const target = editing.value
  if (target) {
    const n = refCount.value[target.id] || 0
    if (n > 0) {
      const ok = await confirm.ask({
        title: t('record.msg.tplCascadeTitle'),
        message: t('record.msg.tplCascadeMessage', { n }),
        detail: t('record.msg.tplCascadeDetail', { channels: refChannels(target.id) }),
        confirmText: t('record.msg.tplCascadeConfirm')
      })
      if (!ok) return
    }
  }
  saving.value = true
  try {
    if (target) {
      await api.put(`/record-templates/${target.id}`, {
        name: form.name.trim(),
        kind: form.kind,
        schedule: form.schedule
      })
      toast.success(t('record.msg.tplUpdated'))
    } else {
      await api.post('/record-templates', {
        name: form.name.trim(),
        kind: form.kind,
        schedule: form.schedule
      })
      toast.success(t('record.msg.tplCreated'))
    }
    dlg.value = false
    load()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    saving.value = false
  }
}

// 复制：以现有模板内容新建副本（服务端仅有创建接口）
async function copy(row: any) {
  const name = t('record.templates.copySuffix', { name: row.name })
  try {
    await api.post('/record-templates', { name, kind: row.kind, schedule: row.schedule })
    toast.success(t('record.msg.copiedOk', { name }))
    load()
  } catch (e: any) {
    toastApiError(e, t('record.msg.copyFailed'))
  }
}

// ---------- 删除模板 ----------
async function del(row: any) {
  if (row.builtin) return // 内置模板不可删
  const n = refCount.value[row.id] || 0
  if (n > 0) {
    toast.warning(t('record.msg.tplInUse', { n }))
    return
  }
  const ok = await confirm.ask({
    title: t('record.msg.deleteTplTitle'),
    message: t('record.msg.deleteTplMessage', { name: row.name }),
    detail: t('record.msg.deleteTplDetail'),
    danger: true, confirmText: t('common.delete')
  })
  if (!ok) return
  try {
    await api.del(`/record-templates/${row.id}`)
    toast.success(t('common.deletedOk'))
    load()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
  }
}

onMounted(() => {
  load()
  loadRefs()
})

/* 客户端分页（E7）：该列表接口一次性返回全部数据，此前全量渲染。
   服务端分页需后端配合，属后续工作。 */
const { page: pgPage, pageSize: pgSize, total: pgTotal, pageItems: pgItems } = useClientPage(items)
</script>

<template>
  <div class="space-y-3">
    <UiCard flat>
      <template #header>
        <div class="flex w-full items-center justify-between">
          <span class="text-[15px] font-semibold text-ink">{{ t('record.templates.title') }}</span>
          <div class="flex items-center gap-2">
            <UiButton size="sm" @click="() => { load(); loadRefs() }"><UiIcon name="refresh" :size="13" />{{ t('common.refresh') }}</UiButton>
            <UiButton variant="primary" size="sm" @click="openDlg()"><UiIcon name="plus" :size="14" />{{ t('record.templates.create') }}</UiButton>
          </div>
        </div>
      </template>

      <UiTable
        :columns="[
          { key: 'name', label: t('record.templates.colName'), width: '180px' },
          { key: 'kind', label: t('record.templates.colKind'), width: '110px' },
          { key: 'schedule', label: t('record.templates.colSchedule'), width: '280px' },
          { key: 'builtin', label: t('record.templates.colBuiltin'), width: '80px', align: 'center' },
          { key: 'ref', label: t('record.templates.colRef'), width: '110px', align: 'center' },
          { key: 'ops', label: t('common.action'), width: '180px', align: 'center', ellipsis: false }
        ]"
        :rows="pgItems" :loading="loading" :row-key="'id'" :empty="t('record.templates.empty')"
      >
        <template #kind="{ row }">
          <!-- 录像三色语义（REC-02）：定时=primary/信号青，事件=success/绿，与 playback.vue 一致 -->
          <UiTag :color="row.kind === 'event' ? 'success' : 'primary'" plain>{{ kindName(row.kind) }}</UiTag>
        </template>
        <template #schedule="{ row }">{{ fmtSchedule(row.schedule) }}</template>
        <template #builtin="{ row }">
          <UiTag v-if="row.builtin" color="info">{{ t('record.templates.builtin') }}</UiTag>
          <span v-else class="text-placeholder">—</span>
        </template>
        <template #ref="{ row }">
          <span :class="refCount[row.id] ? 'text-ink' : 'text-placeholder'">{{ refCount[row.id] || 0 }}</span>
        </template>
        <template #ops="{ row }">
          <div class="flex items-center justify-center gap-1">
            <!-- 内置模板：编辑/删除真正禁用且视觉置灰 -->
            <UiButton variant="text" size="sm" :disabled="row.builtin" @click="openDlg(row)">{{ t('common.edit') }}</UiButton>
            <UiButton variant="text" size="sm" @click="copy(row)">{{ t('record.templates.copy') }}</UiButton>
            <UiTooltip v-if="!row.builtin && refsLoadFailed" :label="t('record.templates.deleteBlocked')">
              <span><UiButton variant="dangerText" size="sm" disabled>{{ t('common.delete') }}</UiButton></span>
            </UiTooltip>
            <UiButton v-else variant="dangerText" size="sm" :disabled="row.builtin" @click="del(row)">{{ t('common.delete') }}</UiButton>
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

    <!-- 新建 / 编辑模板 -->
    <UiDialog v-model:open="dlg" :title="editing ? t('record.templates.editTitle') : t('record.templates.createTitle')" width="max-w-2xl">
      <div class="space-y-4">
        <div class="flex items-center gap-3">
          <label class="shrink-0 text-sm text-body"><span class="text-danger">*</span> {{ t('record.templates.nameLabel') }}</label>
          <UiInput v-model="form.name" :placeholder="t('record.templates.namePlaceholder')" :maxlength="30" width="w-64" />
        </div>
        <div class="flex items-center gap-3">
          <label class="shrink-0 text-sm text-body">{{ t('record.templates.kindLabel') }}</label>
          <UiSegmented v-model="form.kind" :items="[{ label: t('record.kind.timer'), value: 'timer' }, { label: t('record.kind.event'), value: 'event' }]" />
        </div>
        <div>
          <p class="mb-1.5 text-sm text-muted">{{ form.kind === 'event' ? t('record.templates.scheduleEvent') : t('record.templates.scheduleTimer') }}</p>
          <ScheduleGrid v-model="form.schedule" :kind="form.kind" />
        </div>
        <p v-if="editing && (refCount[editing.id] || 0) > 0" class="flex items-center gap-1.5 rounded-signal bg-warning-soft px-3 py-2 text-xs text-warning">
          <UiIcon name="alert-triangle" :size="13" />
          {{ t('record.templates.refWarn', { n: refCount[editing.id] }) }}
        </p>
      </div>
      <template #footer>
        <UiButton @click="dlg = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="saving" @click="save">{{ t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
