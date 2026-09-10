<script setup lang="ts">
// 消息中心（ALM-06/07）：告警列表 / 筛选 / 导出 / 详情抽屉（快照+回放此刻）/ 实时提醒 / 全部已读
const api = useApi()
const toast = useToast()

// 类型中文映射见 utils/enums.ts（全站唯一来源）
const KIND_MAP = ALARM_KIND_MAP
const DEVICE_KINDS = DEVICE_ALARM_KINDS
const PLATFORM_KINDS = ['device_offline', 'node_offline', 'stream_lost', 'disk_full']
// 级别汉化（A16：不显示英文 warn/info）：error 严重 / warn 警告 / info 提示
const LEVEL_MAP: Record<string, { label: string; color: string }> = {
  error: { label: '严重', color: 'danger' },
  warn: { label: '警告', color: 'warning' },
  info: { label: '提示', color: 'info' }
}

const fmt = (ts: any) => new Date(Number(ts)).toLocaleString('zh-CN', { hour12: false })
const kindName = (k: string) => KIND_MAP[k] || k
const levelInfo = (l: string) => LEVEL_MAP[l] || { label: l || '-', color: 'default' }
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
  } catch {}
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
    toastApiError(e, '加载告警列表失败')
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
  const head = ['时间', '类型', '级别', '设备', '通道', '内容', '状态']
  const rows = shownItems.value.map((r) => [
    fmt(r.ts), kindName(r.kind), levelInfo(r.level).label,
    deviceMap.value[r.deviceId] || r.deviceId || '-',
    channelMap.value[r.channelId] || r.channelId || '-',
    rowContent(r), r.read ? '已读' : '未读'
  ])
  const csv = '\uFEFF' + [head, ...rows]
    .map((rs) => rs.map((c) => `"${String(c).replace(/"/g, '""')}"`).join(','))
    .join('\r\n')
  const a = document.createElement('a')
  a.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' }))
  a.download = '告警消息_' + new Date().toISOString().slice(0, 10) + '.csv'
  a.click()
  URL.revokeObjectURL(a.href)
}

// 全部已读
async function readAll() {
  try {
    await api.post('/alarms/read-all')
    unread.value = 0
    items.value.forEach((i) => (i.read = true))
    toast.success('已全部标记为已读')
  } catch (e: any) {
    toastApiError(e, '操作失败')
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
  return (r.kinds || []).map((k: string) => kindName(k)).join('、') || '-'
}

// 实时告警提醒（ALM-07，WebSocket）
useWs((ev: any) => {
  if (ev.type !== 'alarm.new') return
  const a = ev.alarm || ev.data || {}
  toast.warning({
    title: '新告警：' + kindName(a.kind || ev.kind || ''),
    description: (a.error?.msg || a.name || '') + '，请到消息中心查看详情'
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
        { label: '全部', value: 'all', badge: unread || undefined },
        { label: '设备事件', value: 'device' },
        { label: '平台事件', value: 'platform' }
      ]">
        <div class="pt-3">
          <!-- 工具栏 -->
          <div class="mb-3 flex flex-wrap items-center gap-2">
            <UiInput v-model="keyword" placeholder="搜索设备 / 类型 / 内容" clearable width="w-64" size="md" @clear="page = 1">
              <template #prefix><UiIcon name="search" :size="14" class="text-placeholder" /></template>
            </UiInput>
            <UiPopover>
              <template #trigger>
                <UiButton><UiIcon name="filter" :size="14" />筛选<UiIcon name="chevron-down" :size="12" /></UiButton>
              </template>
              <div class="w-52 space-y-2">
                <p class="text-xs text-placeholder">时间范围（当前页）</p>
                <UiSelect v-model="rangeHours" :options="[
                  { label: '全部', value: '0' },
                  { label: '近 1 小时', value: '1' },
                  { label: '近 24 小时', value: '24' },
                  { label: '近 7 天', value: '168' }
                ]" />
                <p class="text-xs text-placeholder">级别</p>
                <UiSelect v-model="levelFilter" :options="[
                  { label: '全部', value: '' },
                  { label: '严重', value: 'error' },
                  { label: '警告', value: 'warn' },
                  { label: '提示', value: 'info' }
                ]" />
                <div class="pt-1">
                  <UiCheckbox v-model="onlyUnread" label="仅看未读" />
                </div>
                <UiButton size="sm" block class="mt-1" @click="resetFilters">重置</UiButton>
              </div>
            </UiPopover>
            <span class="text-xs text-muted">未读 {{ unread }} 条</span>
            <div class="ml-auto flex items-center gap-2">
              <UiButton size="sm" @click="exportCsv"><UiIcon name="download" :size="13" />导出</UiButton>
              <UiButton size="sm" variant="primary" :disabled="!unread" @click="readAll">
                <UiIcon name="check" :size="13" />全部已读
              </UiButton>
            </div>
          </div>

          <!-- 告警列表 -->
          <UiTable
            :columns="[
              { key: 'bar', label: '', width: '28px', align: 'center', ellipsis: false },
              { key: 'snapshot', label: '抓拍', width: '70px', align: 'center' },
              { key: 'ts', label: '时间', width: '160px' },
              { key: 'kind', label: '类型', width: '110px' },
              { key: 'level', label: '级别', width: '80px' },
              { key: 'device', label: '设备', width: '140px' },
              { key: 'channel', label: '通道', width: '130px' },
              { key: 'content', label: '内容', width: '200px' },
              { key: 'status', label: '状态', width: '80px' },
              { key: 'ops', label: '操作', width: '80px', align: 'center', ellipsis: false }
            ]"
            :rows="shownItems" :loading="loading" :row-key="'id'" empty="暂无告警消息"
          >
            <template #bar="{ row }">
              <span class="mx-auto block h-5 w-1 rounded-full" :class="levelBarClass(row.level)" />
            </template>
            <template #snapshot="{ row }">
              <img v-if="row.snapshotUrl" :src="row.snapshotUrl" class="h-6 w-9 rounded-signal object-cover cursor-pointer hover:opacity-80 mx-auto" @click.stop="openDetail(row)" alt="抓拍" />
              <span v-else class="text-xs text-placeholder">—</span>
            </template>
            <template #ts="{ row }">{{ fmt(row.ts) }}</template>
            <template #kind="{ row }">{{ kindName(row.kind) }}</template>
            <template #level="{ row }">
              <UiTag :color="levelInfo(row.level).color as any">{{ levelInfo(row.level).label }}</UiTag>
            </template>
            <template #device="{ row }">{{ deviceMap[row.deviceId] || row.deviceId || '—' }}</template>
            <template #channel="{ row }">{{ channelMap[row.channelId] || row.channelId || '—' }}</template>
            <template #content="{ row }">{{ rowContent(row) }}</template>
            <template #status="{ row }">
              <span v-if="!row.read" class="inline-flex items-center gap-1.5 font-semibold text-ink">
                <span class="h-1.5 w-1.5 rounded-full bg-danger" />未读
              </span>
              <span v-else class="text-xs text-muted">已读</span>
            </template>
            <template #ops="{ row }">
              <UiButton variant="text" size="sm" @click="openDetail(row)">详情</UiButton>
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
    <UiDrawer v-model:open="drawer" title="告警详情" width="max-w-md">
      <UiLoading :loading="detailLoading" class="min-h-full">
        <div v-if="detail" class="space-y-4 p-5 text-sm">
          <!-- 快照 -->
          <div v-if="detail.snapshotUrl">
            <p class="mb-1.5 text-xs font-medium text-muted">现场快照</p>
            <img :src="detail.snapshotUrl" alt="告警快照" class="w-full rounded-signal border border-line" />
          </div>
          <div v-else class="flex h-32 items-center justify-center rounded-signal border border-line bg-zone text-xs text-placeholder">
            <UiIcon name="image" :size="20" class="mr-1.5" />无快照
          </div>

          <!-- 基本信息 -->
          <div class="grid grid-cols-[72px_1fr] gap-x-3 gap-y-2.5">
            <span class="text-muted">类型</span><span class="text-ink">{{ kindName(detail.kind) }}</span>
            <span class="text-muted">级别</span>
            <span><UiTag :color="levelInfo(detail.level).color as any">{{ levelInfo(detail.level).label }}</UiTag></span>
            <span class="text-muted">时间</span><span class="text-body">{{ fmt(detail.ts) }}</span>
            <span class="text-muted">内容</span><span class="text-body">{{ rowContent(detail) }}</span>
            <span class="text-muted">状态</span>
            <span>
              <UiTag v-if="!detail.read" color="danger" dot>未读</UiTag>
              <UiTag v-else color="default">已读</UiTag>
            </span>
          </div>

          <!-- 设备信息 -->
          <div>
            <p class="mb-1.5 text-xs font-medium text-muted">设备信息</p>
            <div class="rounded-signal border border-line bg-zone/50 p-3">
              <div class="grid grid-cols-[72px_1fr] gap-x-3 gap-y-2 text-sm">
                <span class="text-muted">设备</span>
                <span class="text-ink">{{ detail.device?.name || deviceMap[detail.deviceId] || detail.deviceId || '—' }}</span>
                <span class="text-muted">型号</span><span class="text-body">{{ detail.device?.model || '—' }}</span>
                <span class="text-muted">通道</span>
                <span class="text-body">{{ channelMap[detail.channelId] || detail.channelId || '—' }}</span>
              </div>
            </div>
          </div>

          <!-- 关联规则 -->
          <div>
            <p class="mb-1.5 text-xs font-medium text-muted">关联规则</p>
            <div v-if="detail.rules?.length" class="space-y-1.5">
              <div
                v-for="r in detail.rules" :key="r.id"
                class="flex items-center justify-between gap-2 rounded-signal border border-line px-3 py-2 text-xs"
              >
                <span class="truncate text-body">{{ ruleKindsText(r) }}</span>
                <UiTag :color="r.enabled ? 'success' : 'default'" plain>{{ r.enabled ? '启用' : '停用' }}</UiTag>
              </div>
            </div>
            <p v-else class="text-xs text-placeholder">该通道暂无关联告警规则</p>
          </div>

          <!-- 原始数据 -->
          <div>
            <p class="mb-1.5 text-xs font-medium text-muted">详细数据</p>
            <pre class="max-h-52 overflow-auto rounded-signal bg-zone p-2.5 font-mono text-xs text-body">{{ JSON.stringify(detail.data, null, 2) }}</pre>
          </div>
        </div>
      </UiLoading>
      <template #footer>
        <UiButton variant="primary" block :disabled="!detail?.channelId" @click="goPlayback">
          <UiIcon name="history" :size="14" />回放此刻
        </UiButton>
      </template>
    </UiDrawer>
  </div>
</template>
