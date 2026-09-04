<script setup lang="ts">
// 操作日志（ACC-08）：查询、筛选、分页与导出
const api = useApi()
const { currentProject, loadMe } = useAuth()

const filters = reactive({ action: '', username: '' })
const page = ref(1)
const pageSize = 20
const total = ref(0)
const items = ref<any[]>([])
const loading = ref(false)

/* 动作中文映射（未匹配的动作原样展示） */
const ACTION_MAP: Record<string, string> = {
  'device.add': '添加设备',
  'user.create': '创建成员',
  'login': '登录'
}
const actionLabel = (a: string) => ACTION_MAP[a] || a || '-'

async function loadLogs() {
  loading.value = true
  try {
    const res: any = await api.get('/audit-logs', {
      page: page.value,
      pageSize,
      action: filters.action.trim() || undefined,
      username: filters.username.trim() || undefined
    })
    items.value = res?.items || []
    total.value = res?.total || 0
  } catch (e: any) {
    ElMessage.error(e?.msg || '加载操作日志失败')
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
  search()
}

/* 导出操作日志 */
function exportLogs() {
  window.open('/api/v1/audit-logs/export')
}

const fmtTime = (ts: any) =>
  ts ? new Date(typeof ts === 'string' ? Date.parse(ts) : ts).toLocaleString() : '-'

/* 结果标签：success 绿 / fail 红 */
function resultTag(r: string) {
  if (r === 'success') return { text: '成功', type: 'success' as const }
  if (r === 'fail') return { text: '失败', type: 'danger' as const }
  return { text: r || '-', type: 'info' as const }
}

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await loadLogs()
})
</script>

<template>
  <div class="page">
    <el-card shadow="never">
      <template #header>
        <div class="head">
          <span>操作日志</span>
          <el-button type="primary" @click="exportLogs">导出</el-button>
        </div>
      </template>

      <!-- 筛选条件 -->
      <el-form inline @submit.prevent>
        <el-form-item label="动作">
          <el-input
            v-model="filters.action"
            placeholder="如 device.add"
            clearable
            style="width: 180px"
            @keyup.enter="search"
            @clear="search"
          />
        </el-form-item>
        <el-form-item label="操作者">
          <el-input
            v-model="filters.username"
            placeholder="用户名"
            clearable
            style="width: 160px"
            @keyup.enter="search"
            @clear="search"
          />
        </el-form-item>
        <el-form-item>
          <el-button type="primary" @click="search">查询</el-button>
          <el-button @click="resetFilters">重置</el-button>
        </el-form-item>
      </el-form>

      <el-table v-loading="loading" :data="items" border>
        <el-table-column label="时间" width="180">
          <template #default="{ row }">{{ fmtTime(row.ts) }}</template>
        </el-table-column>
        <el-table-column label="操作者" width="120">
          <template #default="{ row }">{{ row.username || '-' }}</template>
        </el-table-column>
        <el-table-column label="动作" min-width="140">
          <template #default="{ row }">{{ actionLabel(row.action) }}</template>
        </el-table-column>
        <el-table-column label="对象" min-width="180" show-overflow-tooltip>
          <template #default="{ row }">{{ row.target || '-' }}</template>
        </el-table-column>
        <el-table-column label="结果" width="90" align="center">
          <template #default="{ row }">
            <el-tag :type="resultTag(row.result).type" size="small">{{ resultTag(row.result).text }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="IP" width="150">
          <template #default="{ row }">{{ row.ip || '-' }}</template>
        </el-table-column>
      </el-table>

      <!-- 分页 -->
      <div class="pager">
        <el-pagination
          layout="total, prev, pager, next"
          :total="total"
          :page-size="pageSize"
          :current-page="page"
          @current-change="(p: number) => { page = p; loadLogs() }"
        />
      </div>
    </el-card>
  </div>
</template>

<style scoped>
.head { display: flex; justify-content: space-between; align-items: center; }
.pager { display: flex; justify-content: flex-end; margin-top: 12px; }
</style>