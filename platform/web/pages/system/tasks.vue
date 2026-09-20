<script setup lang="ts">
// 任务中心（P-18）：长任务统一管理入口——筛选、批量清理、取消进行中任务、获取下载产物
const api = useApi()
const toast = useToast()
const confirm = useConfirm()
const { currentProject, loadMe } = useAuth()
const { tasks: drawerTasks, dropLocal } = useTasks()

const filters = reactive({ status: '', type: '', keyword: '' })
const page = ref(1)
const pageSize = ref(20)
const total = ref(0)
const runningTotal = ref(0)
const items = ref<any[]>([])
const selection = ref<any[]>([])
const loading = ref(false)

const STATUS_OPTIONS = [
  { label: '全部状态', value: '' },
  { label: '进行中', value: 'running' },
  { label: '已完成', value: 'finished' },
  { label: '失败', value: 'failed' },
  { label: '已取消', value: 'canceled' }
]
const TYPE_OPTIONS = [
  { label: '全部类型', value: '' },
  { label: '录像下载', value: 'download' },
  { label: '设备发现', value: 'discover' },
  { label: '批量导入', value: 'import' },
  { label: '固件升级', value: 'upgrade' },
  { label: '数据导出', value: 'export' }
]

async function loadTasks() {
  loading.value = true
  try {
    const res: any = await api.get('/tasks', {
      page: page.value,
      pageSize: pageSize.value,
      status: filters.status || undefined,
      type: filters.type || undefined,
      keyword: filters.keyword.trim() || undefined
    })
    items.value = res?.items || []
    total.value = res?.total ?? 0
    runningTotal.value = res?.running ?? 0
    selection.value = []
  } catch (e: any) {
    toastApiError(e, '加载任务失败')
  } finally {
    loading.value = false
  }
}

function search() {
  page.value = 1
  loadTasks()
}

function resetFilters() {
  filters.status = ''
  filters.type = ''
  filters.keyword = ''
  search()
}

// 删除选中的任务（仅终态可删，进行中的会被服务端跳过）
async function removeSelected() {
  const ids = selection.value.map((r: any) => r.id ?? r)
  if (!ids.length) return
  const blocked = items.value.filter((t) => ids.includes(t.id) && !isTaskFinal(t.status)).length
  const okd = await confirm.ask({
    title: '删除任务',
    message: `确认删除选中的 ${ids.length} 条任务记录？`,
    detail: blocked ? `其中 ${blocked} 条仍在进行中，将被跳过（需先取消）。` : undefined,
    danger: true
  })
  if (!okd) return
  try {
    const res: any = await api.post('/tasks/clear', { scope: 'all', ids })
    const n = Number(res?.deleted || 0)
    dropLocal(ids)
    toast.success(n > 0 ? `已删除 ${n} 条任务` : '没有可删除的任务')
    await loadTasks()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

// 按范围清理
async function clearScope(scope: 'finished' | 'failed' | 'all') {
  const label = { finished: '已完成', failed: '失败与已取消', all: '全部已结束' }[scope]
  const okd = await confirm.ask({
    title: '清理任务',
    message: `确认清理${label}的任务记录？`,
    detail: '进行中的任务不会被清理。',
    confirmText: '确认清理',
    danger: true
  })
  if (!okd) return
  try {
    const res: any = await api.post('/tasks/clear', { scope })
    toast.success(`已清理 ${Number(res?.deleted || 0)} 条任务`)
    await loadTasks()
  } catch (e: any) {
    toastApiError(e, '清理失败')
  }
}

async function removeOne(row: any) {
  const okd = await confirm.ask({
    title: '删除任务',
    message: `确认删除任务「${row.title || row.id}」？`,
    danger: true
  })
  if (!okd) return
  try {
    await api.del(`/tasks/${row.id}`)
    dropLocal([row.id])
    toast.success('已删除')
    await loadTasks()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

async function cancelOne(row: any) {
  try {
    await api.post(`/tasks/${row.id}/cancel`, {})
    toast.success('已取消')
    await loadTasks()
  } catch (e: any) {
    toastApiError(e, '取消失败')
  }
}

const fmtTime = (ts: number) => (ts ? new Date(ts).toLocaleString() : '-')

const cols = [
  { key: 'createdAt', label: '创建时间', width: '170px' },
  { key: 'type', label: '类型', width: '110px' },
  { key: 'title', label: '任务', width: '200px', ellipsis: true },
  { key: 'status', label: '状态', width: '100px', align: 'center' as const },
  { key: 'progress', label: '进度', width: '140px' },
  { key: 'detail', label: '详情', ellipsis: true },
  { key: 'ops', label: '操作', width: '120px', align: 'center' as const }
]

// WS 实时刷新：当前页存在的任务直接就地更新进度
useWs((ev: any) => {
  if (ev.type !== 'task.progress') return
  const d = ev.data || {}
  const id = d.taskId || d.id
  if (!id) return
  const i = items.value.findIndex((x) => x.id === id)
  if (i >= 0) items.value[i] = { ...items.value[i], ...d, id }
  else if (page.value === 1 && !filters.status && !filters.type) loadTasks()
})

// 抽屉里执行删除/清理后（条目变少），本页表格同步刷新，避免显示已被删除的记录
watch(
  () => drawerTasks.value.length,
  (n, o) => { if (n < o && !loading.value) loadTasks() }
)

onMounted(async () => {
  if (!currentProject.value) await loadMe()
  await loadTasks()
})
</script>

<template>
  <div class="space-y-3">
    <UiCard title="任务中心" flat>
      <template #extra>
        <div class="flex items-center gap-2">
          <span v-if="runningTotal" class="text-xs text-muted">{{ runningTotal }} 个任务进行中</span>
          <UiButton size="sm" :disabled="!selection.length" @click="removeSelected">
            <UiIcon name="trash" :size="13" />删除选中
          </UiButton>
          <UiButton size="sm" @click="clearScope('finished')">清理已完成</UiButton>
          <UiButton size="sm" variant="danger" @click="clearScope('all')">清理全部</UiButton>
        </div>
      </template>

      <!-- 筛选条件 -->
      <div class="mb-3 flex flex-wrap items-center gap-2">
        <UiSelect v-model="filters.status" :options="STATUS_OPTIONS" size="sm" width="w-32" />
        <UiSelect v-model="filters.type" :options="TYPE_OPTIONS" size="sm" width="w-32" />
        <UiInput
          v-model="filters.keyword" placeholder="任务名称或 ID" clearable
          size="sm" width="w-52" @enter="search" @clear="search"
        />
        <UiButton variant="primary" size="sm" @click="search">
          <UiIcon name="search" :size="13" />查询
        </UiButton>
        <UiButton size="sm" @click="resetFilters">重置</UiButton>
        <UiButton size="sm" @click="loadTasks">
          <UiIcon name="refresh" :size="13" />刷新
        </UiButton>
      </div>

      <UiEmptyState v-if="!loading && !items.length" text="暂无任务记录" icon="list" />
      <UiTable
        v-else :columns="cols" :rows="items" :loading="loading"
        selectable :selection="selection" @update:selection="selection = $event"
      >
        <template #createdAt="{ row }">{{ fmtTime(row.createdAt) }}</template>
        <template #type="{ row }">{{ taskTypeText(row.type) }}</template>
        <template #title="{ row }">
          <span class="block truncate" :title="row.id">{{ row.title || row.id }}</span>
        </template>
        <template #status="{ row }">
          <UiTag :color="taskTagColor(row.status)">{{ taskStatusText(row.status) }}</UiTag>
        </template>
        <template #progress="{ row }">
          <div class="flex items-center gap-2">
            <div class="h-1 flex-1 overflow-hidden rounded-full bg-line">
              <div class="h-full rounded-full bg-primary transition-all" :style="{ width: (row.progress || 0) + '%' }" />
            </div>
            <span class="w-9 shrink-0 text-right text-xs text-muted">{{ row.progress || 0 }}%</span>
          </div>
        </template>
        <template #detail="{ row }">
          <div class="flex items-center gap-2">
            <span class="truncate text-muted">{{ row.detail || '-' }}</span>
            <a
              v-for="(f, i) in (row.type === 'download' ? row.result?.files || [] : [])" :key="i"
              :href="f.url" target="_blank" download
              class="shrink-0 text-xs text-primary hover:underline"
            >片段{{ i + 1 }}</a>
          </div>
        </template>
        <template #ops="{ row }">
          <div class="flex items-center justify-center gap-2">
            <button
              v-if="!isTaskFinal(row.status)"
              class="text-muted transition hover:text-warning" title="取消"
              @click="cancelOne(row)"
            ><UiIcon name="x" :size="14" /></button>
            <button
              v-else
              class="text-muted transition hover:text-danger" title="删除"
              @click="removeOne(row)"
            ><UiIcon name="trash" :size="14" /></button>
          </div>
        </template>
      </UiTable>

      <UiPagination
        v-model:page="page" v-model:page-size="pageSize" :total="total"
        @update:page="loadTasks" @update:page-size="search"
      />
    </UiCard>
  </div>
</template>
