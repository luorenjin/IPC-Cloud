<script setup lang="ts">
// 录像计划模板（REC-05）：定时/事件模板管理（编辑/复制/删除 + 级联提示 A7）
const api = useApi()
const toast = useToast()
const confirm = useConfirm()

const KIND_MAP: Record<string, string> = { timer: '定时录像', event: '事件录像' }
const DAY_NAMES = ['周一', '周二', '周三', '周四', '周五', '周六', '周日']

// schedule 摘要：{days:[1,2],ranges:[["00:00","24:00"]]} → "周一、周二 00:00-24:00"
function fmtSchedule(s: any) {
  if (!s || !s.days?.length) return '未设置'
  const days = [...s.days]
    .sort((a: number, b: number) => a - b)
    .map((d: number) => DAY_NAMES[d - 1] || d)
    .join('、')
  const ranges = (s.ranges || []).map((r: string[]) => `${r[0]}-${r[1]}`).join('、')
  return `${days} ${ranges}`
}

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
    .join('、')
}

async function load() {
  loading.value = true
  try {
    const res: any = await api.get('/record-templates')
    items.value = res.items || []
  } catch (e: any) {
    toastApiError(e, '加载录像模板失败')
  } finally {
    loading.value = false
  }
}

async function loadRefs() {
  try {
    const [pRes, cRes]: any[] = await Promise.all([api.get('/record-plans'), api.get('/channels')])
    plans.value = pRes.items || []
    channels.value = cRes.items || cRes || []
  } catch {}
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
  if (!form.name.trim()) { toast.warning('请填写模板名称'); return }
  // A7：修改自定义模板前，级联提示将同步影响引用该模板的通道
  const target = editing.value
  if (target) {
    const n = refCount.value[target.id] || 0
    if (n > 0) {
      const ok = await confirm.ask({
        title: '修改录像模板',
        message: `修改后将同步更新使用该模板的 ${n} 个通道`,
        detail: `受影响通道：${refChannels(target.id)}`,
        confirmText: '继续保存'
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
      toast.success('模板已更新')
    } else {
      await api.post('/record-templates', {
        name: form.name.trim(),
        kind: form.kind,
        schedule: form.schedule
      })
      toast.success('模板已创建')
    }
    dlg.value = false
    load()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    saving.value = false
  }
}

// 复制：以现有模板内容新建副本（服务端仅有创建接口）
async function copy(row: any) {
  const name = `${row.name}（副本）`
  try {
    await api.post('/record-templates', { name, kind: row.kind, schedule: row.schedule })
    toast.success(`已复制为「${name}」`)
    load()
  } catch (e: any) {
    toastApiError(e, '复制失败')
  }
}

// ---------- 删除模板 ----------
async function del(row: any) {
  if (row.builtin) return // 内置模板不可删
  const n = refCount.value[row.id] || 0
  if (n > 0) {
    toast.warning(`该模板正被 ${n} 个通道使用，请先在录像计划中解绑后再删除`)
    return
  }
  const ok = await confirm.ask({
    title: '删除确认',
    message: `确定删除录像模板「${row.name}」？`,
    detail: '删除后该模板不可恢复，已产生的录像文件不受影响。',
    danger: true, confirmText: '删除'
  })
  if (!ok) return
  try {
    await api.del(`/record-templates/${row.id}`)
    toast.success('已删除')
    load()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

onMounted(() => {
  load()
  loadRefs()
})
</script>

<template>
  <div class="space-y-3">
    <UiCard flat>
      <template #header>
        <div class="flex w-full items-center justify-between">
          <span class="text-[15px] font-semibold text-ink">录像计划模板</span>
          <div class="flex items-center gap-2">
            <UiButton size="sm" @click="() => { load(); loadRefs() }"><UiIcon name="refresh" :size="13" />刷新</UiButton>
            <UiButton variant="primary" size="sm" @click="openDlg()"><UiIcon name="plus" :size="14" />新建模板</UiButton>
          </div>
        </div>
      </template>

      <UiTable
        :columns="[
          { key: 'name', label: '模板名称', width: '180px' },
          { key: 'kind', label: '类型', width: '110px' },
          { key: 'schedule', label: '计划时间', width: '280px' },
          { key: 'builtin', label: '内置', width: '80px', align: 'center' },
          { key: 'ref', label: '通道引用数', width: '110px', align: 'center' },
          { key: 'ops', label: '操作', width: '180px', align: 'center', ellipsis: false }
        ]"
        :rows="items" :loading="loading" :row-key="'id'" empty="暂无录像模板，点击「新建模板」开始配置"
      >
        <template #kind="{ row }">
          <UiTag :color="row.kind === 'event' ? 'warning' : 'primary'" plain>{{ KIND_MAP[row.kind] || row.kind }}</UiTag>
        </template>
        <template #schedule="{ row }">{{ fmtSchedule(row.schedule) }}</template>
        <template #builtin="{ row }">
          <UiTag v-if="row.builtin" color="info">内置</UiTag>
          <span v-else class="text-placeholder">—</span>
        </template>
        <template #ref="{ row }">
          <span :class="refCount[row.id] ? 'text-ink' : 'text-placeholder'">{{ refCount[row.id] || 0 }}</span>
        </template>
        <template #ops="{ row }">
          <div class="flex items-center justify-center gap-1">
            <!-- 内置模板：编辑/删除真正禁用且视觉置灰 -->
            <UiButton variant="text" size="sm" :disabled="row.builtin" @click="openDlg(row)">编辑</UiButton>
            <UiButton variant="text" size="sm" @click="copy(row)">复制</UiButton>
            <UiButton variant="dangerText" size="sm" :disabled="row.builtin" @click="del(row)">删除</UiButton>
          </div>
        </template>
      </UiTable>
    </UiCard>

    <!-- 新建 / 编辑模板 -->
    <UiDialog v-model:open="dlg" :title="editing ? '编辑录像模板' : '新建录像模板'" width="max-w-2xl">
      <div class="space-y-4">
        <div class="flex items-center gap-3">
          <label class="shrink-0 text-sm text-body"><span class="text-danger">*</span> 模板名称</label>
          <UiInput v-model="form.name" placeholder="如：全天定时录像" :maxlength="30" width="w-64" />
        </div>
        <div class="flex items-center gap-3">
          <label class="shrink-0 text-sm text-body">类型</label>
          <UiSegmented v-model="form.kind" :items="[{ label: '定时录像', value: 'timer' }, { label: '事件录像', value: 'event' }]" />
        </div>
        <div>
          <p class="mb-1.5 text-sm text-muted">{{ form.kind === 'event' ? '事件触发时段（事件发生时在时段内才录像）' : '录像时间（在网格上拖选时段）' }}</p>
          <ScheduleGrid v-model="form.schedule" />
        </div>
        <p v-if="editing && (refCount[editing.id] || 0) > 0" class="flex items-center gap-1.5 rounded bg-warning-soft px-3 py-2 text-xs text-warning">
          <UiIcon name="alert-triangle" :size="13" />
          该模板正被 {{ refCount[editing.id] }} 个通道使用，保存后将同步更新这些通道的录像计划
        </p>
      </div>
      <template #footer>
        <UiButton @click="dlg = false">取消</UiButton>
        <UiButton variant="primary" :disabled="saving" @click="save">确定</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
