<script setup lang="ts">
// 国标待确认设备（ADD-05）：注册后需人工确认入组
const api = useApi()

const items = ref<any[]>([])
const loading = ref(false)

// 分组（确认入组时选择）
const groups = ref<any[]>([])
const groupOptions = computed(() => groups.value.map((g: any) => ({ value: g.id, label: g.name })))

async function load() {
  loading.value = true
  try {
    const res: any = await api.get('/devices/gb28181/pending')
    items.value = res.items || []
  } catch (e: any) {
    ElMessage.error(e.msg || '加载失败')
  } finally {
    loading.value = false
  }
}

async function loadGroups() {
  try {
    const res: any = await api.get('/groups')
    groups.value = res.items || []
  } catch {}
}

// 确认入组
const cfmDlg = reactive({ show: false, row: null as any, groupId: '', name: '' })
function askConfirm(row: any) {
  cfmDlg.row = row
  cfmDlg.groupId = ''
  cfmDlg.name = row.gbId || ''
  cfmDlg.show = true
}
async function doConfirm() {
  if (!cfmDlg.groupId) { ElMessage.warning('请选择分组'); return }
  try {
    await api.post(`/devices/gb28181/pending/${cfmDlg.row.id}/confirm`, {
      groupId: cfmDlg.groupId, name: cfmDlg.name
    })
    ElMessage.success('设备已确认入组')
    cfmDlg.show = false
    load()
  } catch (e: any) {
    ElMessage.error(e.msg || '操作失败')
  }
}

// 拒绝注册
async function doReject(row: any) {
  try { await ElMessageBox.confirm(`确定拒绝设备 ${row.gbId} 的注册请求？`, '拒绝设备', { type: 'warning' }) } catch { return }
  try {
    await api.post(`/devices/gb28181/pending/${row.id}/reject`)
    ElMessage.success('已拒绝')
    load()
  } catch (e: any) {
    ElMessage.error(e.msg || '操作失败')
  }
}

// 首次注册时间格式化
function fmt(ts?: number) {
  if (!ts) return '-'
  return new Date(ts).toLocaleString('zh-CN', { hour12: false })
}

onMounted(() => {
  load()
  loadGroups()
})

// 实时事件：新的国标注册请求 → 刷新列表
useWs((ev: any) => {
  if (ev.type === 'gb.pending') load()
})
</script>

<template>
  <div class="page">
    <el-card shadow="never">
      <template #header>
        <div class="head">
          <span>国标待确认设备</span>
          <el-button size="small" @click="load">刷新</el-button>
        </div>
      </template>
      <el-table :data="items" v-loading="loading">
        <el-table-column prop="gbId" label="国标 ID" min-width="180" />
        <el-table-column prop="ip" label="IP" width="140" />
        <el-table-column prop="vendor" label="厂商" width="120" />
        <el-table-column prop="model" label="型号" width="140" />
        <el-table-column label="首次注册时间" width="180">
          <template #default="{ row }">{{ fmt(row.firstSeen) }}</template>
        </el-table-column>
        <el-table-column label="操作" width="160">
          <template #default="{ row }">
            <el-button type="primary" size="small" @click="askConfirm(row)">确认入组</el-button>
            <el-button type="danger" plain size="small" @click="doReject(row)">拒绝</el-button>
          </template>
        </el-table-column>
      </el-table>
      <div v-if="!items.length && !loading" class="empty">暂无待确认的国标设备</div>
    </el-card>

    <!-- 确认入组弹窗 -->
    <el-dialog v-model="cfmDlg.show" title="确认入组" width="420px">
      <el-form label-width="80px">
        <el-form-item label="国标 ID">
          <span>{{ cfmDlg.row?.gbId }}</span>
        </el-form-item>
        <el-form-item label="设备名称" required>
          <el-input v-model="cfmDlg.name" placeholder="设备显示名称" />
        </el-form-item>
        <el-form-item label="分组" required>
          <el-select v-model="cfmDlg.groupId" placeholder="选择分组" style="width: 100%">
            <el-option v-for="g in groupOptions" :key="g.value" :label="g.label" :value="g.value" />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="cfmDlg.show = false">取消</el-button>
        <el-button type="primary" @click="doConfirm">确认</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.head { display: flex; justify-content: space-between; align-items: center; }
.empty { color: #909399; font-size: 13px; text-align: center; padding: 24px 0; }
</style>