<script setup lang="ts">
// 消息中心（ALM-06/07）：告警列表 / 筛选 / 导出 / 详情抽屉（快照+回放此刻）/ 实时提醒 / 全部已读
const api = useApi()
const route = useRoute()
const toast = useToast()
const confirmBox = useConfirm()
const { t } = useI18n()

// 类型中文映射见 utils/enums.ts（全站唯一来源）

const fmt = (ts: any) => new Date(Number(ts)).toLocaleString('zh-CN', { hour12: false })
// 类型名与级别汉化（A16：不显示英文 warn/info）统一取自 utils/enums.ts
// 走词条键，随语言切换；alarmKindName 只是中文兜底，不直接渲染
const kindName = (k: string) => t(alarmKindKey(k))
const levelInfo = (l: string) => alarmLevelInfo(l)
// 行首导轨的色/光晕统一由 utils/enums.ts 的 alarmRailStyle() 给出。
// 此前这里就地再写了一份级别→色映射（bg-danger/bg-warning/bg-info），与首页告警流各自维护，
// 已收敛：改级别配色只需动 --color-level-* 一处。

// ---------- 列表状态 ----------
const tab = ref<'all' | 'device' | 'platform'>('all')
const page = ref(1)
const pageSize = ref(20)
const total = ref(0)
// 未读计数与顶栏共享（layout 用同名 state）
const unread = useState('unreadAlarms', () => 0)
const items = ref<any[]>([])
const loading = ref(false)
// 勾选的行主键（仅当前页）：用于「删除所选」，筛选/翻页时清空
const selection = ref<string[]>([])
// 当前筛选下的已读条数（跨全部页，服务端统计）：决定「删除已读」是否可用与待删条数
const readTotal = ref(0)

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
    const params: any = { page: page.value, pageSize: pageSize.value, ...serverFilterParams() }
    if (onlyUnread.value) params.read = 'false' // 服务端: read=false → 未读
    if (pendingFocus.value) params.focus = pendingFocus.value
    const res: any = await api.get('/alarms', params)
    items.value = res.items || []
    total.value = res.total || 0
    unread.value = res.unread || 0
    // 已读条数（当前筛选、跨全部页）：去掉它前端就只能拿当前页瞎猜，
    // 「删除已读」会变成盲盒按钮。
    readTotal.value = res.readTotal || 0
    // 深链定位：跳到目标所在页后再拉一次；pendingFocus 只用于定位，highlightId 负责高亮
    if (pendingFocus.value && res.focusPage && res.focusPage !== page.value) {
      page.value = res.focusPage
      pendingFocus.value = ''
      await load()
      return
    }
    pendingFocus.value = ''
  } catch (e: any) {
    toastApiError(e, t('alarm.msg.loadListFailed'))
  } finally {
    loading.value = false
  }
}

// 服务端筛选参数：列表、已读计数、删除三处必须用同一份。
// tab（scope）与时间范围（fromTs）都下沉到服务端——只裁当前页会让 total/分页仍按全量算
// （页码虚高、翻到后面全是空页），而且「删除已读」的范围无法与屏幕上的口径对齐。
function serverFilterParams() {
  const params: any = {}
  if (tab.value === 'device' || tab.value === 'platform') params.scope = tab.value
  if (levelFilter.value) params.level = levelFilter.value
  if (rangeHours.value && rangeHours.value !== '0') {
    params.fromTs = Date.now() - Number(rangeHours.value) * 3600e3
  }
  return params
}

// 深链定位：/alarms?focus=<id>（总览页「最近告警」点击）——跳到该条所在页并高亮
const highlightId = ref('')
const pendingFocus = ref('')

async function scrollToFocus() {
  if (!highlightId.value) return
  await nextTick()
  const el = document.querySelector(`[data-row-key="${CSS.escape(highlightId.value)}"]`)
  el?.scrollIntoView({ block: 'center', behavior: 'smooth' })
}

// 顶栏铃铛在"已处于消息中心"时点击会递增该信号：同路由导航不会重新挂载页面，
// 若不响应，用户点铃铛只会看到"毫无反应"。这里回到最新（第 1 页 + 滚到顶）。
const messageCenterTick = useState('messageCenterTick', () => 0)
watch(messageCenterTick, async () => {
  page.value = 1
  highlightId.value = ''
  await load()
  await nextTick()
  document.getElementById('ipc-main')?.scrollTo({ top: 0, behavior: 'smooth' })
})

// 搜索关键字：对当前页数据做前端过滤（只做"找一条"的辅助，不参与总数/删除范围）
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
  return list
})

// 筛选变化一律回到第 1 页并清空勾选：勾选是"对着当前这屏做的选择"，
// 翻页/换筛选后行已不可见，留着会让「删除所选」删掉用户看不见的记录。
function onFilterChange() { page.value = 1; selection.value = []; load() }
function onPageChange() { selection.value = []; load() }
watch(tab, onFilterChange)
watch(levelFilter, onFilterChange)
watch(onlyUnread, onFilterChange)
watch(rangeHours, onFilterChange)

function resetFilters() {
  keyword.value = ''
  levelFilter.value = ''
  onlyUnread.value = false
  rangeHours.value = '0'
  page.value = 1
  selection.value = []
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
    // 重拉一次：全部已读会改变 readTotal（否则「删除已读」的可用性停留在旧值，
    // 刚标完已读反而删不了）
    await load()
  } catch (e: any) {
    toastApiError(e, t('alarm.msg.actionFailed'))
  }
}

// ---------- 删除消息（ALM-06 清理已读） ----------
// 两个入口共用一个端点 DELETE /alarms：ids = 勾选项（上限 200 条）；
// allRead = 当前筛选下的全部已读（跨页）。删未读也是允许的：勾选是显式意图，
// 确认框会先把"其中含 N 条未读"摆出来，而不是背地里跳过。

/** 勾选框的可访问名称：告警行没有 name 字段，默认会回落到主键（al_xxx）读屏毫无意义 */
const alarmRowLabel = (r: any) => `${kindName(r.kind)} ${fmt(r.ts)}`

/** 当前筛选的中文描述，用于确认框——写清楚会删掉什么，别让"一键清理"变成盲盒 */
function deleteScopeText() {
  const tabText = { all: t('alarm.list.tabAll'), device: t('alarm.list.tabDevice'), platform: t('alarm.list.tabPlatform') }[tab.value]
  const rangeText = { '0': t('alarm.list.rangeAll'), '1': t('alarm.list.range1h'), '24': t('alarm.list.range24h'), '168': t('alarm.list.range7d') }[rangeHours.value]
  const bits = [tabText, rangeText || t('alarm.list.rangeAll')]
  if (levelFilter.value) bits.push(t(levelInfo(levelFilter.value).labelKey))
  return bits.join(t('alarm.list.sep'))
}

async function doDelete(body: Record<string, any>) {
  try {
    const res: any = await api.request('/alarms', {
      method: 'DELETE',
      body: { ...body, ...serverFilterParams() }
    })
    selection.value = []
    const removed = res?.filesRemoved ?? 0
    toast.success(removed > 0
      ? t('alarm.msg.deleteOkWithFiles', { n: res?.deleted ?? 0, m: removed })
      : t('alarm.msg.deleteOk', { n: res?.deleted ?? 0 }))
    await load()
    // 删完当前页可能空了（页码越界）：退回上一页，避免用户盯着空列表以为操作失败
    if (!items.value.length && page.value > 1) {
      page.value -= 1
      await load()
    }
  } catch (e: any) {
    toastApiError(e, t('alarm.msg.deleteFailed'))
  }
}

async function deleteSelected() {
  if (!selection.value.length) return
  const unreadSel = items.value.filter((r) => selection.value.includes(r.id) && !r.read).length
  const ok = await confirmBox.ask({
    title: t('alarm.confirm.deleteSelectedTitle'),
    message: t('alarm.confirm.deleteSelectedMsg', { n: selection.value.length }),
    detail: (unreadSel
      ? t('alarm.confirm.deleteSelectedUnread', { n: unreadSel })
      : t('alarm.confirm.deleteSelectedDetail')) + t('alarm.confirm.deleteSnapshotNote'),
    danger: true,
    confirmText: t('common.delete')
  })
  if (!ok) return
  await doDelete({ ids: [...selection.value] })
}

async function deleteAllRead() {
  if (!readTotal.value) return
  const kw = keyword.value.trim()
  const ok = await confirmBox.ask({
    title: t('alarm.confirm.deleteReadTitle'),
    message: t('alarm.confirm.deleteReadMsg', { n: readTotal.value }),
    detail: t('alarm.confirm.deleteReadDetail', { scope: deleteScopeText() }) +
      (kw ? t('alarm.confirm.deleteReadKeywordNote') : '') +
      t('alarm.confirm.deleteSnapshotNote'),
    danger: true,
    confirmText: t('common.delete')
  })
  if (!ok) return
  await doDelete({ allRead: true })
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
  // ALM-08 抓拍是落库后异步补的：alarm.new 到达时快照还不存在（设备抓图要一次往返），
  // 抓图完成后后端再推 snapshot.ready。这里就地给对应行补图，不重新拉列表——
  // 重新拉取会让正在浏览的页码/滚动位置跳动。
  if (ev.type === 'snapshot.ready') {
    const id = ev.data?.alarmId
    const url = ev.data?.snapshotUrl
    if (!id || !url) return
    const row = items.value.find((i: any) => i.id === id)
    if (row) row.snapshotUrl = url
    if (detail.value?.id === id) detail.value.snapshotUrl = url // 抽屉正好开着这条
    return
  }
  if (ev.type !== 'alarm.new') return
  const a = ev.alarm || ev.data || {}
  toast.warning({
    title: t('alarm.msg.newAlarmTitle', { kind: kindName(a.kind || ev.kind || '') }),
    description: t('alarm.msg.newAlarmDesc', { content: a.error?.msg || a.name || '' })
  })
  load() // 未读计数由 layout 的共享 state 统一递增，这里仅刷新列表
})

onMounted(async () => {
  // 深链定位：先把目标 id 落下来，再由 load() 解析出所在页码并跳转 + 高亮
  const f = route.query.focus
  if (typeof f === 'string' && f) {
    highlightId.value = f
    pendingFocus.value = f
  }
  loadBase()
  await load()
  await scrollToFocus()
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
              <!-- 勾选后出现的批量动作：按钮自带条数，不再另占一行提示空间 -->
              <UiButton v-if="selection.length" size="sm" variant="dangerText" @click="deleteSelected">
                <UiIcon name="trash" :size="13" />{{ t('alarm.list.deleteSelected', { n: selection.length }) }}
              </UiButton>
              <UiButton v-if="selection.length" size="sm" variant="text" @click="selection = []">
                {{ t('alarm.list.clearSelection') }}
              </UiButton>
              <UiButton size="sm" variant="dangerText" :disabled="!readTotal" @click="deleteAllRead">
                <UiIcon name="trash" :size="13" />{{ t('alarm.list.deleteRead', { n: readTotal }) }}
              </UiButton>
            </div>
          </div>

          <!-- 告警列表 -->
          <UiTable
            :columns="[
              { key: 'snapshot', label: t('alarm.list.colSnapshot'), width: '70px', align: 'center' },
              { key: 'ts', label: t('alarm.list.colTime'), width: '160px' },
              // kind/level 的显示值是汉化后的文案，与原始字段（motion/warn）不同，
              // 故关闭 ellipsis：否则 UiTable 会把原始英文值当成 title 悬浮提示（违反 A16）。
              { key: 'kind', label: t('alarm.list.colKind'), width: '110px', ellipsis: false },
              { key: 'level', label: t('alarm.list.colLevel'), width: '92px', ellipsis: false },
              { key: 'device', label: t('alarm.list.colDevice'), width: '140px' },
              { key: 'channel', label: t('alarm.list.colChannel'), width: '130px' },
              { key: 'content', label: t('alarm.list.colContent'), width: '200px' },
              { key: 'status', label: t('alarm.list.colStatus'), width: '80px' },
              { key: 'ops', label: t('common.action'), width: '80px', align: 'center', ellipsis: false }
            ]"
            :rows="shownItems" :loading="loading" :row-key="'id'" :empty="t('alarm.list.empty')"
            :highlight="highlightId"
            selectable v-model:selection="selection" :row-label="alarmRowLabel"
          >
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
            <!-- 级别 = 导轨 + 文字标签，两者编码同一件事（红=严重 / 琥珀=警告 / 中性灰=提示）。
                 导轨是给长列表快速扫视用的，故 aria-hidden；语义由文字标签承载，不单独依赖颜色。
                 此前色条独占一列且表头为空、无 title，既看不出含义又与级别重复。 -->
            <template #level="{ row }">
              <span class="flex items-center gap-1.5">
                <span class="h-5 w-1 shrink-0 rounded-full" :style="alarmRailStyle(row.level)" aria-hidden="true" />
                <UiTag :color="levelInfo(row.level).color as any">{{ t(levelInfo(row.level).labelKey) }}</UiTag>
              </span>
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
            @update:page="onPageChange" @update:page-size="onPageChange"
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
