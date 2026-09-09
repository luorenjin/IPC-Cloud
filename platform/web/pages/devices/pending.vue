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
  <!-- 待处理清单：卡片化呈现，刻意区别于常规设备列表（devices/index.vue）的表格视觉 -->
  <UiCard flat body-class="p-4">
    <template #header>
      <div class="flex w-full items-center justify-between">
        <div class="flex items-center gap-2">
          <span class="text-sm font-semibold text-ink">国标待确认设备</span>
          <UiTag v-if="items.length" color="warning" dot>{{ items.length }} 台待处理</UiTag>
        </div>
        <UiButton size="sm" @click="load"><Icon name="refresh" :size="13" />刷新</UiButton>
      </div>
    </template>

    <UiLoading :loading="loading">
      <UiEmptyState
        v-if="!items.length"
        icon="clipboard"
        text="暂无待确认的国标设备。设备按 GB28181 注册且不在白名单时，会出现在这里。"
      />
      <div v-else class="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
        <div
          v-for="row in items" :key="row.id"
          class="relative flex flex-col gap-3 overflow-hidden rounded-signal border border-line bg-canvas p-4 pl-5 transition-colors hover:border-warning/50"
        >
          <!-- 左侧警示条：标记"待人工确认"的队列语义 -->
          <span class="absolute inset-y-0 left-0 w-1 bg-warning" />

          <div class="flex items-start justify-between gap-2">
            <div class="flex min-w-0 items-center gap-2.5">
              <span class="flex h-9 w-9 shrink-0 items-center justify-center rounded-signal bg-warning-soft text-warning">
                <Icon name="shield-alert" :size="17" />
              </span>
              <div class="min-w-0">
                <div class="truncate font-mono text-sm font-semibold text-ink" :title="row.gbId">{{ row.gbId }}</div>
                <div class="mt-0.5 text-xs text-placeholder">首次注册 {{ fmt(row.firstSeen) }}</div>
              </div>
            </div>
            <UiTag color="warning">待确认</UiTag>
          </div>

          <div class="grid grid-cols-3 gap-2 border-t border-line-soft pt-3 text-xs">
            <div class="min-w-0">
              <div class="text-placeholder">IP</div>
              <div class="mt-0.5 truncate font-mono text-body" :title="row.ip">{{ row.ip || '—' }}</div>
            </div>
            <div class="min-w-0">
              <div class="text-placeholder">厂商</div>
              <div class="mt-0.5 truncate text-body" :title="row.vendor">{{ row.vendor || '—' }}</div>
            </div>
            <div class="min-w-0">
              <div class="text-placeholder">型号</div>
              <div class="mt-0.5 truncate text-body" :title="row.model">{{ row.model || '—' }}</div>
            </div>
          </div>

          <div class="mt-1 flex items-center gap-2">
            <UiButton variant="primary" size="sm" class="flex-1 justify-center" @click="askConfirm(row)">
              <Icon name="check" :size="13" />确认入组
            </UiButton>
            <UiButton variant="dangerText" size="sm" class="flex-1 justify-center" @click="doReject(row)">
              <Icon name="x" :size="13" />拒绝
            </UiButton>
          </div>
        </div>
      </div>
    </UiLoading>
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
