<script setup lang="ts">
// 国标待确认设备（ADD-05）：注册后需人工确认入组 / 拒绝（加入黑名单）
const api = useApi()
const toast = useToast()
const confirm = useConfirm()
const { t } = useI18n()

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
    toastApiError(e, t('device.msg.pendingLoadFailed'))
  } finally {
    loading.value = false
  }
}

async function loadGroups() {
  try {
    const res: any = await api.get('/groups')
    groups.value = res.items || []
  } catch (e: any) {
    toastApiError(e, t('device.msg.groupListFailed'))
  }
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
  if (!cfmDlg.groupId) { toast.warning(t('device.msg.pendingSelectGroup')); return }
  try {
    await api.post(`/devices/gb28181/pending/${cfmDlg.row.id}/confirm`, {
      groupId: cfmDlg.groupId, name: cfmDlg.name
    })
    toast.success(t('device.msg.pendingConfirmOk'))
    cfmDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, t('device.msg.opFailed'))
  }
}

// 拒绝注册（加入黑名单）
async function doReject(row: any) {
  const ok = await confirm.ask({
    title: t('device.pending.rejectTitle'),
    message: t('device.pending.rejectMsg', { id: row.gbId }),
    detail: t('device.pending.rejectDetail'),
    danger: true, confirmText: t('device.pending.rejectOk')
  })
  if (!ok) return
  try {
    await api.post(`/devices/gb28181/pending/${row.id}/reject`)
    toast.success(t('device.msg.pendingRejectOk'))
    load()
  } catch (e: any) {
    toastApiError(e, t('device.msg.opFailed'))
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
          <span class="text-sm font-semibold text-ink">{{ t('device.pending.title') }}</span>
          <UiTag v-if="items.length" color="warning" dot>{{ t('device.pending.count', { n: items.length }) }}</UiTag>
        </div>
        <UiButton size="sm" @click="load"><Icon name="refresh" :size="13" />{{ t('common.refresh') }}</UiButton>
      </div>
    </template>

    <UiLoading :loading="loading">
      <UiEmptyState
        v-if="!items.length"
        icon="clipboard"
        :text="t('device.pending.empty')"
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
                <div class="mt-0.5 text-xs text-placeholder">{{ t('device.pending.firstSeen', { time: fmt(row.firstSeen) }) }}</div>
              </div>
            </div>
            <UiTag color="warning">{{ t('device.pending.badge') }}</UiTag>
          </div>

          <div class="grid grid-cols-3 gap-2 border-t border-line-soft pt-3 text-xs">
            <div class="min-w-0">
              <div class="text-placeholder">IP</div>
              <div class="mt-0.5 truncate font-mono text-body" :title="row.ip">{{ row.ip || '—' }}</div>
            </div>
            <div class="min-w-0">
              <div class="text-placeholder">{{ t('device.pending.vendor') }}</div>
              <div class="mt-0.5 truncate text-body" :title="row.vendor">{{ row.vendor || '—' }}</div>
            </div>
            <div class="min-w-0">
              <div class="text-placeholder">{{ t('device.pending.model') }}</div>
              <div class="mt-0.5 truncate text-body" :title="row.model">{{ row.model || '—' }}</div>
            </div>
          </div>

          <div class="mt-1 flex items-center gap-2">
            <UiButton variant="primary" size="sm" class="flex-1 justify-center" @click="askConfirm(row)">
              <Icon name="check" :size="13" />{{ t('device.pending.confirm') }}
            </UiButton>
            <UiButton variant="dangerText" size="sm" class="flex-1 justify-center" @click="doReject(row)">
              <Icon name="x" :size="13" />{{ t('device.pending.reject') }}
            </UiButton>
          </div>
        </div>
      </div>
    </UiLoading>
  </UiCard>

  <!-- 确认入组弹窗 -->
  <UiDialog v-model:open="cfmDlg.show" :title="t('device.pending.dlgTitle')" width="max-w-md">
    <div class="grid grid-cols-[80px_1fr] items-center gap-x-3 gap-y-3">
      <label class="text-right text-sm text-muted">{{ t('device.pending.gbId') }}</label>
      <span class="font-mono text-sm text-ink">{{ cfmDlg.row?.gbId }}</span>
      <label class="text-right text-sm text-body"><span class="text-danger">*</span> {{ t('device.add.name') }}</label>
      <UiInput v-model="cfmDlg.name" :placeholder="t('device.pending.namePlaceholder')" />
      <label class="text-right text-sm text-body"><span class="text-danger">*</span> {{ t('common.group') }}</label>
      <UiSelect v-model="cfmDlg.groupId" :options="groupOptions" :placeholder="t('device.pending.selectGroup')" />
    </div>
    <template #footer>
      <UiButton @click="cfmDlg.show = false">{{ t('common.cancel') }}</UiButton>
      <UiButton variant="primary" @click="doConfirm">{{ t('common.confirm') }}</UiButton>
    </template>
  </UiDialog>
</template>
