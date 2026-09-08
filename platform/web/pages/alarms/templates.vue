<script setup lang="ts">
// 布防模板管理（ALM-01/05，PRD §4 独立二级菜单 /alarms/templates）
// 7x24h 网格编辑器 + 级联修改提示（A7）+ 内置模板保护
const api = useApi()
const toast = useToast()
const confirm = useConfirm()

const DAY_NAMES = ['周一', '周二', '周三', '周四', '周五', '周六', '周日']

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
    .join('、')
}

async function load() {
  loading.value = true
  try {
    const res: any = await api.get('/alarm-templates')
    items.value = res.items || []
  } catch (e: any) {
    toastApiError(e, '加载布防模板失败')
  } finally {
    loading.value = false
  }
}

async function loadRefs() {
  try {
    const [rRes, cRes]: any[] = await Promise.all([api.get('/alarm-rules'), api.get('/channels')])
    rules.value = rRes.items || []
    channels.value = cRes.items || cRes || []
  } catch {}
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
  if (!form.name.trim()) { toast.warning('请填写模板名称'); return }
  const target = editing.value
  if (target) {
    const n = refCount.value[target.id] || 0
    if (n > 0) {
      const ok = await confirm.ask({
        title: '修改布防模板',
        message: `修改后将同步更新使用该模板的 ${n} 个通道规则`,
        detail: `受影响通道：${refChannels(target.id)}`,
        confirmText: '继续保存'
      })
      if (!ok) return
    }
  }
  saving.value = true
  try {
    if (target) {
      await api.put(`/alarm-templates/${target.id}`, { name: form.name.trim(), schedule: form.schedule })
      toast.success('布防模板已更新')
    } else {
      await api.post('/alarm-templates', { name: form.name.trim(), schedule: form.schedule })
      toast.success('布防模板已创建')
    }
    dlg.value = false
    load()
    loadRefs()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    saving.value = false
  }
}

async function del(row: any) {
  if (row.builtin) return
  const n = refCount.value[row.id] || 0
  if (n > 0) {
    toast.warning(`该模板正被 ${n} 条规则使用，请先解绑后再删除`)
    return
  }
  const ok = await confirm.ask({
    title: '删除确认',
    message: `确定删除布防模板「${row.name}」？`,
    danger: true,
    confirmText: '删除'
  })
  if (!ok) return
  try {
    await api.del(`/alarm-templates/${row.id}`)
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
      <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
        <div class="flex items-center gap-2">
          <UiButton variant="primary" @click="openDlg()">
            <UiIcon name="plus" :size="14" />新建模板
          </UiButton>
          <UiButton @click="load(); loadRefs()">
            <UiIcon name="refresh" :size="14" />刷新
          </UiButton>
        </div>
        <div class="text-xs text-muted">
          提示：布防时间决定该时间段内是否对侦测事件产生告警。内置模板不可删除与直接修改。
        </div>
      </div>

      <UiTable
        :columns="[
          { key: 'name', label: '模板名称', width: '200px' },
          { key: 'schedule', label: '布防时间', width: '320px' },
          { key: 'ref', label: '引用规则数', width: '120px', align: 'center' },
          { key: 'builtin', label: '内置', width: '90px', align: 'center' },
          { key: 'ops', label: '操作', width: '150px', align: 'center', ellipsis: false }
        ]"
        :rows="items"
        :loading="loading"
        :row-key="'id'"
        empty="暂无布防模板"
      >
        <template #name="{ row }">
          <span class="font-medium text-ink">{{ row.name }}</span>
        </template>
        <template #schedule="{ row }">
          <span class="text-xs text-body">{{ fmtSchedule(row.schedule) }}</span>
        </template>
        <template #ref="{ row }">
          <UiTooltip v-if="refCount[row.id]" :label="refChannels(row.id)">
            <span class="cursor-pointer text-primary underline decoration-dotted">{{ refCount[row.id] }} 个通道</span>
          </UiTooltip>
          <span v-else class="text-placeholder">0</span>
        </template>
        <template #builtin="{ row }">
          <UiTag v-if="row.builtin" color="info">内置</UiTag>
          <span v-else class="text-placeholder">—</span>
        </template>
        <template #ops="{ row }">
          <div class="flex items-center justify-center gap-1">
            <UiButton variant="text" size="sm" :disabled="row.builtin" @click="openDlg(row)">编辑</UiButton>
            <UiButton variant="dangerText" size="sm" :disabled="row.builtin" @click="del(row)">删除</UiButton>
          </div>
        </template>
      </UiTable>
    </UiCard>

    <!-- 编辑抽屉/对话框 -->
    <UiDialog v-model:open="dlg" :title="editing ? '编辑布防模板' : '新建布防模板'" width="max-w-3xl">
      <div class="space-y-4">
        <div>
          <label class="mb-1 block text-xs font-medium text-muted">模板名称 *</label>
          <UiInput v-model="form.name" placeholder="如：全天布防、工作日布防" />
        </div>

        <div>
          <div class="mb-1 flex items-center justify-between">
            <label class="text-xs font-medium text-muted">布防时间段（周 × 24h）*</label>
            <span class="text-xs text-placeholder">拖拽或点击网格选择布防生效时间</span>
          </div>
          <ScheduleGrid v-model="form.schedule" />
        </div>

        <div class="flex justify-end gap-2 pt-2 border-t border-line">
          <UiButton @click="dlg = false">取消</UiButton>
          <UiButton variant="primary" :loading="saving" @click="save">保存</UiButton>
        </div>
      </div>
    </UiDialog>
  </div>
</template>
