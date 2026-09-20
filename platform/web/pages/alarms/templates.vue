<script setup lang="ts">
// 布防模板管理（ALM-01/05，PRD §4 独立二级菜单 /alarms/templates）
// 7x24h 网格编辑器 + 级联修改提示（A7）+ 内置模板保护
const api = useApi()
const toast = useToast()
const confirm = useConfirm()
const { t } = useI18n()


// ---------- 列表 ----------
const items = ref<any[]>([])
const loading = ref(false)

// 引用规则与通道
const rules = ref<any[]>([])
const channels = ref<any[]>([])
const channelMap = computed(() => Object.fromEntries(channels.value.map((c: any) => [c.id, c.name])))
const refCount = computed(() => {
  const m: Record<string, number> = {}
  rules.value.forEach((r) => { m[r.templateId] = (m[r.templateId] || 0) + 1 })
  return m
})
function refChannels(id: string) {
  return rules.value
    .filter((r) => r.templateId === id)
    .map((r) => channelMap.value[r.channelId] || r.channelId)
    .join(t('alarm.list.sep'))
}

async function load() {
  loading.value = true
  try {
    const res: any = await api.get('/alarm-templates')
    items.value = res.items || []
  } catch (e: any) {
    toastApiError(e, t('alarm.msg.loadTemplatesFailed'))
  } finally {
    loading.value = false
  }
}

async function loadRefs() {
  try {
    const [rRes, cRes]: any[] = await Promise.all([api.get('/alarm-rules'), api.get('/channels')])
    rules.value = rRes.items || []
    channels.value = cRes.items || cRes || []
  } catch (e: any) {
    // 引用计数取不到会让"被引用中"的模板看起来可以随意删除（删除保护失效），
    // 必须让用户知道这份数据不可靠。
    toastApiError(e, t('alarm.msg.loadRefsFailed'))
  }
}

// ---------- 新建 / 编辑 ----------
const dlg = ref(false)
const saving = ref(false)
const editing = ref<any>(null)
const form = reactive({
  name: '',
  schedule: { days: [] as number[], ranges: [] as string[][] }
})

function openDlg(row?: any) {
  if (row?.builtin) return
  editing.value = row || null
  form.name = row?.name || ''
  form.schedule = row?.schedule
    ? { days: [...(row.schedule.days || [])], ranges: (row.schedule.ranges || []).map((r: string[]) => [...r]) }
    : { days: [1, 2, 3, 4, 5, 6, 7], ranges: [['00:00', '24:00']] }
  dlg.value = true
}

async function save() {
  if (!form.name.trim()) { toast.warning(t('alarm.msg.nameRequired')); return }
  const target = editing.value
  if (target) {
    const n = refCount.value[target.id] || 0
    if (n > 0) {
      const ok = await confirm.ask({
        title: t('alarm.msg.tplCascadeTitle'),
        message: t('alarm.msg.tplCascadeMessageRules', { n }),
        detail: t('alarm.msg.tplCascadeDetail', { channels: refChannels(target.id) }),
        confirmText: t('alarm.msg.tplCascadeConfirm')
      })
      if (!ok) return
    }
  }
  saving.value = true
  try {
    if (target) {
      await api.put(`/alarm-templates/${target.id}`, { name: form.name.trim(), schedule: form.schedule })
      toast.success(t('alarm.msg.tplUpdatedFull'))
    } else {
      await api.post('/alarm-templates', { name: form.name.trim(), schedule: form.schedule })
      toast.success(t('alarm.msg.tplCreatedFull'))
    }
    dlg.value = false
    load()
    loadRefs()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    saving.value = false
  }
}

async function del(row: any) {
  if (row.builtin) return
  const n = refCount.value[row.id] || 0
  if (n > 0) {
    toast.warning(t('alarm.msg.tplInUse', { n }))
    return
  }
  const ok = await confirm.ask({
    title: t('alarm.msg.deleteTplTitle'),
    message: t('alarm.msg.deleteTplMessage', { name: row.name }),
    danger: true,
    confirmText: t('common.delete')
  })
  if (!ok) return
  try {
    await api.del(`/alarm-templates/${row.id}`)
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
      <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
        <div class="flex items-center gap-2">
          <UiButton variant="primary" @click="openDlg()">
            <UiIcon name="plus" :size="14" />{{ t('alarm.templates.create') }}
          </UiButton>
          <UiButton @click="load(); loadRefs()">
            <UiIcon name="refresh" :size="14" />{{ t('common.refresh') }}
          </UiButton>
        </div>
        <div class="text-xs text-muted">
          {{ t('alarm.templates.hint') }}
        </div>
      </div>

      <UiTable
        :columns="[
          { key: 'name', label: t('alarm.templates.colName'), width: '200px' },
          { key: 'schedule', label: t('alarm.templates.colSchedule'), width: '320px' },
          { key: 'ref', label: t('alarm.templates.colRef'), width: '120px', align: 'center' },
          { key: 'builtin', label: t('alarm.templates.colBuiltin'), width: '90px', align: 'center' },
          { key: 'ops', label: t('common.action'), width: '150px', align: 'center', ellipsis: false }
        ]"
        :rows="pgItems"
        :loading="loading"
        :row-key="'id'"
        :empty="t('alarm.templates.empty')"
      >
        <template #name="{ row }">
          <span class="font-medium text-ink">{{ row.name }}</span>
        </template>
        <template #schedule="{ row }">
          <span class="text-xs text-body">{{ fmtSchedule(row.schedule) }}</span>
        </template>
        <template #ref="{ row }">
          <UiTooltip v-if="refCount[row.id]" :label="refChannels(row.id)">
            <span class="cursor-pointer text-primary underline decoration-dotted">{{ t('alarm.templates.refChannels', { n: refCount[row.id] }) }}</span>
          </UiTooltip>
          <span v-else class="text-placeholder">0</span>
        </template>
        <template #builtin="{ row }">
          <UiTag v-if="row.builtin" color="info">{{ t('alarm.templates.builtin') }}</UiTag>
          <span v-else class="text-placeholder">—</span>
        </template>
        <template #ops="{ row }">
          <div class="flex items-center justify-center gap-1">
            <UiButton variant="text" size="sm" :disabled="row.builtin" @click="openDlg(row)">{{ t('common.edit') }}</UiButton>
            <UiButton variant="dangerText" size="sm" :disabled="row.builtin" @click="del(row)">{{ t('common.delete') }}</UiButton>
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

    <!-- 编辑抽屉/对话框 -->
    <UiDialog v-model:open="dlg" :title="editing ? t('alarm.templates.editTitle') : t('alarm.templates.createTitle')" width="max-w-3xl">
      <div class="space-y-4">
        <div>
          <label class="mb-1 block text-xs font-medium text-muted">{{ t('alarm.templates.nameLabel') }}</label>
          <UiInput v-model="form.name" :placeholder="t('alarm.templates.namePlaceholder')" />
        </div>

        <div>
          <div class="mb-1 flex items-center justify-between">
            <label class="text-xs font-medium text-muted">{{ t('alarm.templates.scheduleLabel') }}</label>
            <span class="text-xs text-placeholder">{{ t('alarm.templates.scheduleHint') }}</span>
          </div>
          <ScheduleGrid v-model="form.schedule" />
        </div>

        <div class="flex justify-end gap-2 pt-2 border-t border-line">
          <UiButton @click="dlg = false">{{ t('common.cancel') }}</UiButton>
          <UiButton variant="primary" :loading="saving" @click="save">{{ t('common.save') }}</UiButton>
        </div>
      </div>
    </UiDialog>
  </div>
</template>
