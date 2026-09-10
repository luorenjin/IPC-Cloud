<script setup lang="ts">
// 消息中心（ALM-06/07）：告警列表 / 筛选 / 导出 / 详情抽屉（快照+回放此刻）/ 实时提醒 / 全部已读
const api = useApi()
const toast = useToast()
const { t } = useI18n()

// 类型中文映射见 utils/enums.ts（全站唯一来源）
const DEVICE_KINDS = DEVICE_ALARM_KINDS
const PLATFORM_KINDS = ['device_offline', 'node_offline', 'stream_lost', 'disk_full']

const fmt = (ts: any) => new Date(Number(ts)).toLocaleString('zh-CN', { hour12: false })
// 类型名与级别汉化（A16：不显示英文 warn/info）统一取自 utils/enums.ts
// 走词条键，随语言切换；alarmKindName 只是中文兜底，不直接渲染
const kindName = (k: string) => t(alarmKindKey(k))
const levelInfo = (l: string) => alarmLevelInfo(l)
// 行首色条：级别→背景色（延续告警红=高危 / 警告黄=中危 / 信息灰=低危三级语义）
const LEVEL_BAR: Record<string, string> = { error: 'bg-danger', warn: 'bg-warning', info: 'bg-info' }
const levelBarClass = (l: string) => LEVEL_BAR[l] || 'bg-placeholder'

// ---------- 列表状态 ----------
const tab = ref<'all' | 'device' | 'platform'>('all')
const page = ref(1)
const pageSize = ref(20)
const total = ref(0)
// 未读计数与顶栏共享（layout 用同名 state）
const unread = useState('unreadAlarms', () => 0)
const items = ref<any[]>([])
const loading = ref(false)

// 工具栏筛选：搜索（设备/类型，前端过滤）+ Popover（时间范围/级别/未读，级别与未读走服务端）
const keyword = ref('')
const levelFilter = ref('')
const onlyUnread = ref(false)
const rangeHours = ref('0') // 0=全部（前端按 ts 过滤当前页）

// 设备/通道名称映射（列表展示设备、通道列）
const devices = ref<any[]>([])
const channels = ref<any[]>([])
const deviceMap = computed(() => Object.fromEntries(devices.value.map((d: any) => [d.id, d.name])))
const channelMap = computed(() => Object.fromEntries(channels.value.map((c: any) => [c.id, c.name])))

async function loadBase() {
  try {
    const [dRes, cRes]: any[] = await Promise.all([api.get('/devices'), api.get('/channels')])
    devices.value = dRes.items || dRes || []
    channels.value = cRes.items || cRes || []
  } catch {
    // 仅用于把 deviceId/channelId 显示成名称；取不到就回落显示 ID，
    // 不影响告警列表本身，故不打扰用户。
  }
}

async function load() {
  loading.value = true
  try {
    const params: any = { page: page.value, pageSize: pageSize.value }
    if (levelFilter.value) params.level = levelFilter.value
    if (onlyUnread.value) params.read = 'false' // 服务端: read=false → 未读
    const res: any = await api.get('/alarms', params)
    let list: any[] = res.items || []
    // 前端按 tab 分类过滤
    if (tab.value === 'device') list = list.filter((i) => DEVICE_KINDS.includes(i.kind))
    else if (tab.value === 'platform') list = list.filter((i) => PLATFORM_KINDS.includes(i.kind))
    items.value = list
    total.value = res.total || 0
    unread.value = res.unread || 0
  } catch (e: any) {
    toastApiError(e, t('alarm.msg.loadListFailed'))
  } finally {
    loading.value = false
  }
}

// 搜索关键字 + 时间范围：对当前页数据做前端过滤
const shownItems = computed(() => {
  let list = items.value
  const q = keyword.value.trim().toLowerCase()
  if (q) {
    list = list.filter((r) => {
      const hay = [
        deviceMap.value[r.deviceId] || '', r.deviceId || '',
        channelMap.value[r.channelId] || '', r.channelId || '',
        kindName(r.kind), rowContent(r)
      ].join(' ').toLowerCase()
      return hay.includes(q)
    })
  }
  if (rangeHours.value && rangeHours.value !== '0') {
    const from = Date.now() - Number(rangeHours.value) * 3600e3
    list = list.filter((r) => Number(r.ts) >= from)
  }
  return list
})

watch(tab, () => { page.value = 1; load() })
watch(levelFilter, () => { page.value = 1; load() })
watch(onlyUnread, () => { page.value = 1; load() })

function resetFilters() {
  keyword.value = ''
  levelFilter.value = ''
  onlyUnread.value = false
  rangeHours.value = '0'
  page.value = 1
  load()
}

// 导出：当前页数据导出 CSV（服务端暂无导出接口，前端生成）
function exportCsv() {
  const head = [t('alarm.list.csvTime'), t('alarm.list.csvKind'), t('alarm.list.csvLevel'), t('alarm.list.csvDevice'), t('alarm.list.csvChannel'), t('alarm.list.csvContent'), t('alarm.list.csvStatus')]
  const rows = shownItems.value.map((r) => [
    fmt(r.ts), kindName(r.kind), t(levelInfo(r.level).labelKey),
    deviceMap.value[r.deviceId] || r.deviceId || '-',
    channelMap.value[r.channelId] || r.channelId || '-',
    rowContent(r), r.read ? t('alarm.list.read') : t('alarm.list.unread')
  ])
  const csv = '\uFEFF' + [head, ...rows]
    .map((rs) => rs.map((c) => `"${String(c).replace(/"/g, '""')}"`).join(','))
    .join('\r\n')
  const a = document.createElement('a')
  a.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' }))
  a.download = t('alarm.list.csvFilePrefix') + new Date().toISOString().slice(0, 10) + '.csv'
  a.click()
  URL.revokeObjectURL(a.href)
}

// 全部已读
async function readAll() {
  try {
    await api.post('/alarms/read-all')
    unread.value = 0
    items.value.forEach((i) => (i.read = true))
    toast.success(t('alarm.msg.readAllOk'))
  } catch (e: any) {
    toastApiError(e, t('alarm.msg.actionFailed'))
  }
}

// ---------- 详情抽屉（A16） ----------
const drawer = ref(false)
const detail = ref<any>(null)
const detailLoading = ref(false)

async function openDetail(row: any) {
  drawer.value = true
  detailLoading.value = true
  try {
    detail.value = await api.get(`/alarms/${row.id}`)
  } catch {
    detail.value = row // 拉取失败兜底显示行数据
  } finally {
    detailLoading.value = false
  }
  if (!row.read) {
    row.read = true
    if (detail.value) detail.value.read = true
    unread.value = Math.max(0, unread.value - 1)
    api.post(`/alarms/${row.id}/read`).catch(() => {}) // 同步服务端已读
  }
}

// 回放此刻：跳转回放页并定位到告警时间（playback 页读取 channelId/ts 参数）
function goPlayback() {
  if (!detail.value?.channelId) return
  navigateTo(`/playback?channelId=${detail.value.channelId}&ts=${detail.value.ts}`)
}

// 内容列：data.error?.msg || data.name || kind
function rowContent(r: any) {
  const d = r.data || {}
  return d.error?.msg || d.name || kindName(r.kind)
}

// 关联规则展示
function ruleKindsText(r: any) {
  return (r.kinds || []).map((k: string) => kindName(k)).join(t('alarm.list.sep')) || '-'
}

// 实时告警提醒（ALM-07，WebSocket）
useWs((ev: any) => {
  if (ev.type !== 'alarm.new') return
  const a = ev.alarm || ev.data || {}
  toast.warning({
    title: t('alarm.msg.newAlarmTitle', { kind: kindName(a.kind || ev.kind || '') }),
    description: t('alarm.msg.newAlarmDesc', { content: a.error?.msg || a.name || '' })
  })
  load() // 未读计数由 layout 的共享 state 统一递增，这里仅刷新列表
})

onMounted(() => {
  loadBase()
  load()
})
</script>

<template>
  <div class="space-y-3">
    <UiCard flat>
      <UiTabs v-model="tab" :items="[
        { label: t('alarm.list.tabAll'), value: 'all', badge: unread || undefined },
        { label: t('alarm.list.tabDevice'), value: 'device' },
        { label: t('alarm.list.tabPlatform'), value: 'platform' }
      ]">
        <div class="pt-3">
          <!-- 工具栏 -->
          <div class="mb-3 flex flex-wrap items-center gap-2">
            <UiInput v-model="keyword" :placeholder="t('alarm.list.searchPlaceholder')" clearable width="w-64" size="md" @clear="page = 1">
              <template #prefix><UiIcon name="search" :size="14" class="text-placeholder" /></template>
            </UiInput>
            <UiPopover>
              <template #trigger>
                <UiButton><UiIcon name="filter" :size="14" />{{ t('common.filter') }}<UiIcon name="chevron-down" :size="12" /></UiButton>
              </template>
              <div class="w-52 space-y-2">
                <p class="text-xs text-placeholder">{{ t('alarm.list.filterTimeRange') }}</p>
                <UiSelect v-model="rangeHours" :options="[
                  { label: t('alarm.list.rangeAll'), value: '0' },
                  { label: t('alarm.list.range1h'), value: '1' },
                  { label: t('alarm.list.range24h'), value: '24' },
                  { label: t('alarm.list.range7d'), value: '168' }
                ]" />
                <p class="text-xs text-placeholder">{{ t('alarm.list.filterLevel') }}</p>
                <UiSelect v-model="levelFilter" :options="[
                  { label: t('alarm.list.levelAll'), value: '' },
                  { label: t('alarm.list.levelError'), value: 'error' },
                  { label: t('alarm.list.levelWarn'), value: 'warn' },
                  { label: t('alarm.list.levelInfo'), value: 'info' }
                ]" />
                <div class="pt-1">
                  <UiCheckbox v-model="onlyUnread" :label="t('alarm.list.onlyUnread')" />
                </div>
                <UiButton size="sm" block class="mt-1" @click="resetFilters">{{ t('common.reset') }}</UiButton>
              </div>
            </UiPopover>
            <span class="text-xs text-muted">{{ t('alarm.list.unreadCount', { n: unread }) }}</span>
            <div class="ml-auto flex items-center gap-2">
              <UiButton size="sm" @click="exportCsv"><UiIcon name="download" :size="13" />{{ t('common.export') }}</UiButton>
              <UiButton size="sm" variant="primary" :disabled="!unread" @click="readAll">
                <UiIcon name="check" :size="13" />{{ t('alarm.list.readAll') }}
              </UiButton>
            </div>
          </div>

          <!-- 告警列表 -->
          <UiTable
            :columns="[
              { key: 'bar', label: '', width: '28px', align: 'center', ellipsis: false },
              { key: 'snapshot', label: t('alarm.list.colSnapshot'), width: '70px', align: 'center' },
              { key: 'ts', label: t('alarm.list.colTime'), width: '160px' },
              { key: 'kind', label: t('alarm.list.colKind'), width: '110px' },
              { key: 'level', label: t('alarm.list.colLevel'), width: '80px' },
              { key: 'device', label: t('alarm.list.colDevice'), width: '140px' },
              { key: 'channel', label: t('alarm.list.colChannel'), width: '130px' },
              { key: 'content', label: t('alarm.list.colContent'), width: '200px' },
              { key: 'status', label: t('alarm.list.colStatus'), width: '80px' },
              { key: 'ops', label: t('common.action'), width: '80px', align: 'center', ellipsis: false }
            ]"
            :rows="shownItems" :loading="loading" :row-key="'id'" :empty="t('alarm.list.empty')"
          >
            <template #bar="{ row }">
              <span class="mx-auto block h-5 w-1 rounded-full" :class="levelBarClass(row.level)" />
            </template>
            <template #snapshot="{ row }">
              <button
                v-if="row.snapshotUrl" type="button" class="mx-auto block rounded-signal"
                :aria-label="t('alarm.list.viewDetail')" @click.stop="openDetail(row)"
              >
                <img :src="row.snapshotUrl" class="h-6 w-9 rounded-signal object-cover hover:opacity-80" :alt="t('alarm.list.snapshotAlt')" />
              </button>
              <span v-else class="text-xs text-placeholder">—</span>
            </template>
            <template #ts="{ row }">{{ fmt(row.ts) }}</template>
            <template #kind="{ row }">{{ kindName(row.kind) }}</template>
            <template #level="{ row }">
              <UiTag :color="levelInfo(row.level).color as any">{{ t(levelInfo(row.level).labelKey) }}</UiTag>
            </template>
            <template #device="{ row }">{{ deviceMap[row.deviceId] || row.deviceId || '—' }}</template>
            <template #channel="{ row }">{{ channelMap[row.channelId] || row.channelId || '—' }}</template>
            <template #content="{ row }">{{ rowContent(row) }}</template>
            <template #status="{ row }">
              <span v-if="!row.read" class="inline-flex items-center gap-1.5 font-semibold text-ink">
                <span class="h-1.5 w-1.5 rounded-full bg-danger" />{{ t('alarm.list.unread') }}
              </span>
              <span v-else class="text-xs text-muted">{{ t('alarm.list.read') }}</span>
            </template>
            <template #ops="{ row }">
              <UiButton variant="text" size="sm" @click="openDetail(row)">{{ t('common.detail') }}</UiButton>
            </template>
          </UiTable>

          <UiPagination
            v-model:page="page" v-model:page-size="pageSize" :total="total" :page-sizes="[10, 20, 50]"
            @update:page="load" @update:page-size="load"
          />
        </div>
      </UiTabs>
    </UiCard>

    <!-- 详情抽屉（A16：快照 / 回放此刻 / 设备信息 / 关联规则） -->
    <UiDrawer v-model:open="drawer" :title="t('alarm.detail.title')" width="max-w-md">
      <UiLoading :loading="detailLoading" class="min-h-full">
        <div v-if="detail" class="space-y-4 p-5 text-sm">
          <!-- 快照 -->
          <div v-if="detail.snapshotUrl">
            <p class="mb-1.5 text-xs font-medium text-muted">{{ t('alarm.detail.snapshot') }}</p>
            <img :src="detail.snapshotUrl" :alt="t('alarm.detail.snapshotAlt')" class="w-full rounded-signal border border-line" />
          </div>
          <div v-else class="flex h-32 items-center justify-center rounded-signal border border-line bg-zone text-xs text-placeholder">
            <UiIcon name="image" :size="20" class="mr-1.5" />{{ t('alarm.detail.noSnapshot') }}
          </div>

          <!-- 基本信息 -->
          <div class="grid grid-cols-[72px_1fr] gap-x-3 gap-y-2.5">
            <span class="text-muted">{{ t('alarm.detail.kind') }}</span><span class="text-ink">{{ kindName(detail.kind) }}</span>
            <span class="text-muted">{{ t('alarm.detail.level') }}</span>
            <span><UiTag :color="levelInfo(detail.level).color as any">{{ t(levelInfo(detail.level).labelKey) }}</UiTag></span>
            <span class="text-muted">{{ t('alarm.detail.time') }}</span><span class="text-body">{{ fmt(detail.ts) }}</span>
            <span class="text-muted">{{ t('alarm.detail.content') }}</span><span class="text-body">{{ rowContent(detail) }}</span>
            <span class="text-muted">{{ t('alarm.detail.status') }}</span>
            <span>
              <UiTag v-if="!detail.read" color="danger" dot>{{ t('alarm.list.unread') }}</UiTag>
              <UiTag v-else color="default">{{ t('alarm.list.read') }}</UiTag>
            </span>
          </div>

          <!-- 设备信息 -->
          <div>
            <p class="mb-1.5 text-xs font-medium text-muted">{{ t('alarm.detail.deviceInfo') }}</p>
            <div class="rounded-signal border border-line bg-zone/50 p-3">
              <div class="grid grid-cols-[72px_1fr] gap-x-3 gap-y-2 text-sm">
                <span class="text-muted">{{ t('alarm.detail.device') }}</span>
                <span class="text-ink">{{ detail.device?.name || deviceMap[detail.deviceId] || detail.deviceId || '—' }}</span>
                <span class="text-muted">{{ t('alarm.detail.model') }}</span><span class="text-body">{{ detail.device?.model || '—' }}</span>
                <span class="text-muted">{{ t('alarm.detail.channel') }}</span>
                <span class="text-body">{{ channelMap[detail.channelId] || detail.channelId || '—' }}</span>
              </div>
            </div>
          </div>

          <!-- 关联规则 -->
          <div>
            <p class="mb-1.5 text-xs font-medium text-muted">{{ t('alarm.detail.relatedRules') }}</p>
            <div v-if="detail.rules?.length" class="space-y-1.5">
              <div
                v-for="r in detail.rules" :key="r.id"
                class="flex items-center justify-between gap-2 rounded-signal border border-line px-3 py-2 text-xs"
              >
                <span class="truncate text-body">{{ ruleKindsText(r) }}</span>
                <UiTag :color="r.enabled ? 'success' : 'default'" plain>{{ r.enabled ? t('alarm.detail.ruleEnabled') : t('alarm.detail.ruleDisabled') }}</UiTag>
              </div>
            </div>
            <p v-else class="text-xs text-placeholder">{{ t('alarm.detail.noRules') }}</p>
          </div>

          <!-- 原始数据 -->
          <div>
            <p class="mb-1.5 text-xs font-medium text-muted">{{ t('alarm.detail.rawData') }}</p>
            <pre class="max-h-52 overflow-auto rounded-signal bg-zone p-2.5 font-mono text-xs text-body">{{ JSON.stringify(detail.data, null, 2) }}</pre>
          </div>
        </div>
      </UiLoading>
      <template #footer>
        <UiButton variant="primary" block :disabled="!detail?.channelId" @click="goPlayback">
          <UiIcon name="history" :size="14" />{{ t('alarm.detail.playbackNow') }}
        </UiButton>
      </template>
    </UiDrawer>
  </div>
</template>
