<script setup lang="ts">
// 操作日志（ACC-08）：筛选（时间范围/操作者/对象类型/动作）、分页与导出 CSV
const api = useApi()
const toast = useToast()
const { currentProject, loadMe } = useAuth()

const filters = reactive({
  action: '',
  username: '',
  targetType: '',
  dateFrom: '',
  dateTo: ''
})
const page = ref(1)
const pageSize = ref(20)
const total = ref(0)
const items = ref<any[]>([])
const loading = ref(false)

/* 动作中文映射（未匹配的动作原样展示） */
const ACTION_MAP: Record<string, string> = {
  'device.add': '添加设备',
  'device.delete': '删除设备',
  'device.transfer': '转移设备',
  'user.create': '创建成员',
  'user.delete': '删除成员',
  'role.create': '创建角色',
  'role.update': '修改角色',
  'role.delete': '删除角色',
  'group.create': '创建分组',
  'group.delete': '删除分组',
  'node.create': '创建节点',
  'node.delete': '删除节点',
  'settings.update': '修改设置',
  'login': '登录',
  'logout': '退出登录'
}
const actionLabel = (a: string) => ACTION_MAP[a] || a || '-'

/* 对象类型（由动作前缀推断） */
const TARGET_TYPES = [
  { label: '全部', value: '' },
  { label: '设备', value: 'device' },
  { label: '成员', value: 'user' },
  { label: '角色', value: 'role' },
  { label: '分组', value: 'group' },
  { label: '节点', value: 'node' },
  { label: '设置', value: 'settings' },
  { label: '会话', value: 'login' }
]
function targetTypeOf(action: string): string {
  const a = String(action || '')
  if (a.startsWith('device.')) return '设备'
  if (a.startsWith('user.')) return '成员'
  if (a.startsWith('role.')) return '角色'
  if (a.startsWith('group.')) return '分组'
  if (a.startsWith('node.') || a.startsWith('media-node')) return '节点'
  if (a.startsWith('settings.') || a.startsWith('alarm.') || a.startsWith('record.')) return '设置'
  if (a === 'login' || a === 'logout') return '会话'
  return '其他'
}

async function loadLogs() {
  loading.value = true
  try {
    const params: any = {
      page: page.value,
      pageSize: pageSize.value,
      action: filters.action.trim() || undefined,
      username: filters.username.trim() || undefined
    }
    // 时间范围 -> 后端 ts 过滤（以毫秒时间戳传递，未支持时由后端忽略）
    if (filters.dateFrom) params.start = Date.parse(filters.dateFrom + 'T00:00:00')
    if (filters.dateTo) params.end = Date.parse(filters.dateTo + 'T23:59:59')
    const res: any = await api.get('/audit-logs', params)
    let list = res?.items || []
    // 对象类型为前端派生字段，做客户端过滤
    if (filters.targetType) {
      list = list.filter((r: any) => {
        const t = r.targetType || targetTypeOf(r.action)
        const label = TARGET_TYPES.find((x) => x.value === filters.targetType)?.label
        return t === label || String(r.action || '').startsWith(filters.targetType + '.')
      })
    }
    items.value = list
    total.value = res?.total || 0
  } catch (e: any) {
    toastApiError(e, '加载操作日志失败')
  } finally {
    loading.value = false
  }
}

function search() {
  page.value = 1
  loadLogs()
}

function resetFilters() {
  filters.action = ''
  filters.username = ''
  filters.targetType = ''
  filters.dateFrom = ''
  filters.dateTo = ''
  search()
}

/* 导出 CSV（带当前筛选条件；后端 /audit-logs/export 输出 CSV） */
async function exportLogs() {
  const params: any = {}
  if (filters.action.trim()) params.action = filters.action.trim()
  if (filters.username.trim()) params.username = filters.username.trim()
  if (filters.dateFrom) params.start = String(Date.parse(filters.dateFrom + 'T00:00:00'))
  if (filters.dateTo) params.end = String(Date.parse(filters.dateTo + 'T23:59:59'))
  try {
    await api.download('/audit-logs/export', params)
    toast.success('已开始导出（最多 10000 条）')
  } catch (e: any) {
    toastApiError(e, '导出失败')
  }
}


/* 结果标签映射见 utils/enums.ts */
function resultTag(r: string) {
  const i = resultInfo(r)
  return { text: i.label, color: i.color }
}

const cols = [
  { key: 'ts', label: '时间', width: '170px' },
  { key: 'username', label: '操作者', width: '120px' },
  { key: 'targetType', label: '对象类型', width: '90px', align: 'center' as const },
  { key: 'target', label: '对象', width: '200px' },
  { key: 'action', label: '动作', width: '140px' },
  { key: 'result', label: '结果', width: '80px', align: 'center' as const },
  { key: 'ip', label: 'IP', width: '140px' }
]

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await loadLogs()
})
</script>

<template>
  <div class="space-y-3">
    <UiCard title="操作日志" flat>
      <template #extra>
        <UiButton variant="primary" size="sm" @click="exportLogs">
          <UiIcon name="download" :size="14" />导出 CSV
        </UiButton>
      </template>

      <!-- 筛选条件 -->
      <div class="mb-3 flex flex-wrap items-center gap-2 rounded-signal border border-line-soft bg-zone px-3 py-2.5">
        <span class="flex items-center gap-1 text-xs text-placeholder"><UiIcon name="calendar" :size="13" />时间</span>
        <UiInput v-model="filters.dateFrom" type="date" size="sm" width="w-40" @enter="search" />
        <span class="text-placeholder">至</span>
        <UiInput v-model="filters.dateTo" type="date" size="sm" width="w-40" @enter="search" />
        <UiInput
          v-model="filters.username" placeholder="操作者" clearable
          size="sm" width="w-36" @enter="search" @clear="search"
        />
        <UiSelect v-model="filters.targetType" :options="TARGET_TYPES" size="sm" width="w-32" placeholder="对象类型" />
        <UiInput
          v-model="filters.action" placeholder="动作，如 device.add" clearable
          size="sm" width="w-45" @enter="search" @clear="search"
        />
        <UiButton variant="primary" size="sm" @click="search">
          <UiIcon name="search" :size="13" />查询
        </UiButton>
        <UiButton size="sm" @click="resetFilters">重置</UiButton>
      </div>

      <UiEmptyState v-if="!loading && !items.length" text="暂无操作日志" icon="history" />
      <UiTable v-else :columns="cols" :rows="items" :loading="loading">
        <template #ts="{ row }"><span class="font-mono text-xs text-body">{{ fmtTime(row.ts) }}</span></template>
        <template #username="{ row }">{{ row.username || '-' }}</template>
        <template #targetType="{ row }">{{ row.targetType || targetTypeOf(row.action) }}</template>
        <template #target="{ row }">
          <span class="block truncate" :title="row.target">{{ row.target || '-' }}</span>
        </template>
        <template #action="{ row }">
          <div class="leading-tight">
            <div class="text-body">{{ actionLabel(row.action) }}</div>
            <div class="font-mono text-[11px] text-placeholder">{{ row.action || '-' }}</div>
          </div>
        </template>
        <template #result="{ row }">
          <UiTag :color="resultTag(row.result).color">{{ resultTag(row.result).text }}</UiTag>
        </template>
        <template #ip="{ row }"><span class="font-mono text-xs text-muted">{{ row.ip || '-' }}</span></template>
      </UiTable>

      <!-- 分页 -->
      <UiPagination
        v-model:page="page" v-model:page-size="pageSize" :total="total"
        @update:page="loadLogs" @update:page-size="search"
      />
    </UiCard>
  </div>
</template>
