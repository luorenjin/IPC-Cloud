<script setup lang="ts">
// 国标待确认设备（ADD-05）：注册后需人工确认入组 / 拒绝（加入黑名单）
const api = useApi()
const toast = useToast()
const confirm = useConfirm()

const items = ref<any[]>([])
const loading = ref(false)

const groups = ref<any[]>([])
const groupOptions = computed(() => groups.value.map((g: any) => ({ value: g.id, label: g.name })))

async function load() {
  loading.value = true
  try {
    const res: any = await api.get('/devices/gb28181/pending')
    items.value = res.items || []
  } catch (e: any) {
    toastApiError(e, '加载失败')
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
  if (!cfmDlg.groupId) { toast.warning('请选择分组'); return }
  try {
    await api.post(`/devices/gb28181/pending/${cfmDlg.row.id}/confirm`, {
      groupId: cfmDlg.groupId, name: cfmDlg.name
    })
    toast.success('设备已确认入组，通道将自动同步')
    cfmDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, '操作失败')
  }
}

// 拒绝注册（加入黑名单）
async function doReject(row: any) {
  const ok = await confirm.ask({
    title: '拒绝设备',
    message: `确定拒绝设备 ${row.gbId} 的注册请求？`,
    detail: '拒绝后该国标 ID 将加入黑名单，后续注册将被自动丢弃。',
    danger: true, confirmText: '拒绝并拉黑'
  })
  if (!ok) return
  try {
    await api.post(`/devices/gb28181/pending/${row.id}/reject`)
    toast.success('已拒绝并加入黑名单')
    load()
  } catch (e: any) {
    toastApiError(e, '操作失败')
  }
}

function fmt(ts?: number) {
  if (!ts) return '—'
  return new Date(ts).toLocaleString('zh-CN', { hour12: false })
}

onMounted(() => {
  load()
  loadGroups()
})

useWs((ev: any) => {
  if (ev.type === 'gb.pending') load()
})
</script>

<template>
  <UiCard flat>
    <template #header>
      <div class="flex w-full items-center justify-between">
        <span class="text-sm font-semibold text-ink">国标待确认设备</span>
        <UiButton size="sm" @click="load"><Icon name="refresh" :size="13" />刷新</UiButton>
      </div>
    </template>
    <UiTable
      :columns="[
        { key: 'gbId', label: '国标 ID' },
        { key: 'ip', label: 'IP', width: '140px' },
        { key: 'vendor', label: '厂商', width: '120px' },
        { key: 'model', label: '型号', width: '140px' },
        { key: 'firstSeen', label: '首次注册时间', width: '180px' },
        { key: 'ops', label: '操作', width: '170px', ellipsis: false }
      ]"
      :rows="items" :loading="loading" :row-key="'id'"
      empty="暂无待确认的国标设备。设备按 GB28181 注册且不在白名单时，会出现在这里。"
    >
      <template #firstSeen="{ row }">{{ fmt(row.firstSeen) }}</template>
      <template #ops="{ row }">
        <UiButton variant="primary" size="sm" @click="askConfirm(row)">确认入组</UiButton>
        <UiButton variant="dangerText" size="sm" @click="doReject(row)">拒绝</UiButton>
      </template>
    </UiTable>
  </UiCard>

  <!-- 确认入组弹窗 -->
  <UiDialog v-model:open="cfmDlg.show" title="确认入组" width="max-w-md">
    <div class="grid grid-cols-[80px_1fr] items-center gap-x-3 gap-y-3">
      <label class="text-right text-sm text-muted">国标 ID</label>
      <span class="font-mono text-sm text-ink">{{ cfmDlg.row?.gbId }}</span>
      <label class="text-right text-sm text-body"><span class="text-danger">*</span> 设备名称</label>
      <UiInput v-model="cfmDlg.name" placeholder="设备显示名称" />
      <label class="text-right text-sm text-body"><span class="text-danger">*</span> 分组</label>
      <UiSelect v-model="cfmDlg.groupId" :options="groupOptions" placeholder="选择分组" />
    </div>
    <template #footer>
      <UiButton @click="cfmDlg.show = false">取消</UiButton>
      <UiButton variant="primary" @click="doConfirm">确认</UiButton>
    </template>
  </UiDialog>
</template>
