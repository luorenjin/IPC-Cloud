<script setup lang="ts">
// 操作日志（ACC-08）：筛选（时间范围/操作者/对象类型/动作）、分页与导出 CSV
const api = useApi()
const toast = useToast()
const { t } = useI18n()
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

/*
 * 动作中文名见 utils/enums.ts 的 AUDIT_ACTION_MAP（唯一的动作词条来源）。
 * 旧实现是页内自建映射，键名写成 `device.add` / `user.create` 这类「资源.动词」，
 * 而后端 AuditLog.Action 存的是**动词本身**（api/audit.go：路径命中 auditVerbs 取该段，
 * 否则按 HTTP 方法回落 create/update/delete），资源类型在 Target（如 `device:<id>`）里
 * → 旧映射永远匹配不上，动作列一直靠原样显示。
 */
const actionLabel = (a: string) => (a ? t(auditActionKey(a)) : '-')

/* 对象类型（由动作前缀推断） */
const TARGET_TYPES = computed(() => [
  { label: t('common.all'), value: '' },
  { label: t('system.audit.typeDevice'), value: 'device' },
  { label: t('system.audit.typeUser'), value: 'user' },
  { label: t('system.audit.typeRole'), value: 'role' },
  { label: t('system.audit.typeGroup'), value: 'group' },
  { label: t('system.audit.typeNode'), value: 'node' },
  { label: t('system.audit.typeSettings'), value: 'settings' },
  { label: t('system.audit.typeSession'), value: 'login' }
])
function targetTypeOf(action: string): string {
  const a = String(action || '')
  if (a.startsWith('device.')) return t('system.audit.typeDevice')
  if (a.startsWith('user.')) return t('system.audit.typeUser')
  if (a.startsWith('role.')) return t('system.audit.typeRole')
  if (a.startsWith('group.')) return t('system.audit.typeGroup')
  if (a.startsWith('node.') || a.startsWith('media-node')) return t('system.audit.typeNode')
  if (a.startsWith('settings.') || a.startsWith('alarm.') || a.startsWith('record.')) return t('system.audit.typeSettings')
  if (a === 'login' || a === 'logout') return t('system.audit.typeSession')
  return t('system.audit.typeOther')
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
        const label = TARGET_TYPES.value.find((x) => x.value === filters.targetType)?.label
        return t === label || String(r.action || '').startsWith(filters.targetType + '.')
      })
    }
    items.value = list
    total.value = res?.total || 0
  } catch (e: any) {
    toastApiError(e, t('system.msg.auditLoadFailed'))
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
    toast.success(t('system.msg.auditExportStarted'))
  } catch (e: any) {
    toastApiError(e, t('system.msg.auditExportFailed'))
  }
}


/* 结果标签映射见 utils/enums.ts */
function resultTag(r: string) {
  const i = resultInfo(r)
  return { text: t(i.labelKey), color: i.color }
}

const cols = computed(() => [
  { key: 'ts', label: t('common.time'), width: '170px' },
  { key: 'username', label: t('system.audit.operator'), width: '120px' },
  { key: 'targetType', label: t('system.audit.targetType'), width: '90px', align: 'center' as const },
  { key: 'target', label: t('system.audit.colTarget'), width: '200px' },
  { key: 'action', label: t('system.audit.colAction'), width: '140px' },
  { key: 'result', label: t('system.audit.colResult'), width: '80px', align: 'center' as const },
  { key: 'ip', label: t('system.audit.colIp'), width: '140px' }
])

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await loadLogs()
})
</script>

<template>
  <div class="space-y-3">
    <UiCard :title="t('system.audit.title')" flat>
      <template #extra>
        <UiButton variant="primary" size="sm" @click="exportLogs">
          <UiIcon name="download" :size="14" />{{ t('system.audit.exportCsv') }}
        </UiButton>
      </template>

      <!-- 筛选条件 -->
      <div class="mb-3 flex flex-wrap items-center gap-2 rounded-signal border border-line-soft bg-zone px-3 py-2.5">
        <span class="flex items-center gap-1 text-xs text-placeholder"><UiIcon name="calendar" :size="13" />{{ t('system.audit.timeLabel') }}</span>
        <UiInput v-model="filters.dateFrom" type="date" size="sm" width="w-40" @enter="search" />
        <span class="text-placeholder">{{ t('system.audit.dateTo') }}</span>
        <UiInput v-model="filters.dateTo" type="date" size="sm" width="w-40" @enter="search" />
        <UiInput
          v-model="filters.username" :placeholder="t('system.audit.operator')" clearable
          size="sm" width="w-36" @enter="search" @clear="search"
        />
        <UiSelect v-model="filters.targetType" :options="TARGET_TYPES" size="sm" width="w-32" :placeholder="t('system.audit.targetType')" />
        <UiInput
          v-model="filters.action" :placeholder="t('system.audit.actionPlaceholder')" clearable
          size="sm" width="w-45" @enter="search" @clear="search"
        />
        <UiButton variant="primary" size="sm" @click="search">
          <UiIcon name="search" :size="13" />{{ t('common.query') }}
        </UiButton>
        <UiButton size="sm" @click="resetFilters">{{ t('common.reset') }}</UiButton>
      </div>

      <UiEmptyState v-if="!loading && !items.length" :text="t('system.audit.empty')" icon="history" />
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
