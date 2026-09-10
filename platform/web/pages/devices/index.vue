<script setup lang="ts">
// 设备列表（MGR-01/02）+ 添加设备弹窗（ADD-01~08）
const api = useApi()
const route = useRoute()
const router = useRouter()
const toast = useToast()
const confirmBox = useConfirm()
const { currentProject } = useAuth()
const { t } = useI18n()

// 国标待确认计数：与侧栏徽标共用同一份状态（由 layouts/default.vue 拉取并刷新）
const pendingCount = useState('gbPending', () => 0)

// ================= 分组管理 =================
const groups = ref<any[]>([])
const selectedGroup = ref<string>('')
const groupSearch = ref('')

const groupMap = computed(() => {
  const m: Record<string, string> = {}
  for (const g of groups.value) m[g.id] = g.name
  return m
})
const groupOptions = computed(() => groups.value.map((g: any) => ({ value: g.id, label: g.name })))

// 来源展示名/颜色见 utils/enums.ts SOURCE_MAP（全站唯一来源，PRD §9.1）

async function loadGroups() {
  try {
    const res: any = await api.get('/groups')
    groups.value = res.items || []
  } catch (e: any) {
    toastApiError(e, t('device.msg.groupLoadFailed'))
  }
}

// 分组新建与编辑
const groupDlg = reactive({ show: false, mode: 'add', id: '', name: '' })
function openAddGroup() {
  groupDlg.mode = 'add'
  groupDlg.id = ''
  groupDlg.name = ''
  groupDlg.show = true
}
function openEditGroup(g: any) {
  groupDlg.mode = 'edit'
  groupDlg.id = g.id
  groupDlg.name = g.name
  groupDlg.show = true
}
async function saveGroup() {
  if (!groupDlg.name.trim()) return toast.warning(t('device.msg.groupNameRequired'))
  try {
    if (groupDlg.mode === 'add') {
      await api.post('/groups', { name: groupDlg.name.trim() })
      toast.success(t('device.msg.groupCreated'))
    } else {
      await api.put(`/groups/${groupDlg.id}`, { name: groupDlg.name.trim() })
      toast.success(t('device.msg.groupRenamed'))
    }
    groupDlg.show = false
    await loadGroups()
  } catch (e: any) {
    toastApiError(e, t('device.msg.groupSaveFailed'))
  }
}

// ================= 设备列表查询与分页 =================
const query = reactive({
  keyword: '',
  page: 1,
  pageSize: 20,
  status: '',
  source: '',
  tab: 'all' // all | ipc
})

const total = ref(0)
const devices = ref<any[]>([])
const loading = ref(false)
const selection = ref<string[]>([])
const expandedRow = ref<string>('')
const cardCollapsed = ref(false)

// 统计数量（顶部卡片）
const stats = ref({
  total: 2,
  online: 0,
  offline: 2
})

async function load() {
  loading.value = true
  try {
    const params: any = { page: query.page, pageSize: query.pageSize }
    if (selectedGroup.value) params.groupId = selectedGroup.value
    if (query.keyword) params.keyword = query.keyword.trim()
    if (query.status) params.status = query.status
    if (query.source) params.source = query.source

    const res: any = await api.get('/devices', params)
    devices.value = res?.items || []
    total.value = res?.total || devices.value.length

    // 统计更新
    const off = devices.value.filter((d) => d.status === 'offline').length
    const on = devices.value.filter((d) => d.status === 'online').length
    stats.value = {
      total: total.value,
      online: on,
      offline: off
    }
  } catch (e: any) {
    toastApiError(e, t('device.msg.listLoadFailed'))
  } finally {
    loading.value = false
  }
}

function search() {
  query.page = 1
  load()
}

// ================= 列配置（内容按钮：控制表格显示的列） =================
const allCols = [
  { key: 'index', labelKey: 'device.col.index' },
  { key: 'name', labelKey: 'device.col.name' },
  { key: 'type', labelKey: 'device.col.type' },
  { key: 'source', labelKey: 'device.col.source' },
  { key: 'status', labelKey: 'device.col.status' },
  { key: 'model', labelKey: 'device.col.model' },
  { key: 'ip', labelKey: 'device.col.ip' },
  { key: 'mac', labelKey: 'device.col.mac' },
  { key: 'group', labelKey: 'device.col.group' },
  { key: 'location', labelKey: 'device.col.location' },
  { key: 'ops', labelKey: 'device.col.ops' }
]
const hiddenCols = ref<string[]>([])

function toggleCol(k: string) {
  if (hiddenCols.value.includes(k)) {
    hiddenCols.value = hiddenCols.value.filter((x) => x !== k)
  } else {
    hiddenCols.value.push(k)
  }
  try {
    localStorage.setItem('ipc_dev_hidden_cols', JSON.stringify(hiddenCols.value))
  } catch {
    // 隐私模式下 localStorage 不可用：列显隐不被记住，但本次操作已生效
  }
}

const showCol = (k: string) => !hiddenCols.value.includes(k)

// ================= 批量操作（批量工具条） =================
const batchMoveDlg = reactive({ show: false, groupId: '', saving: false })

// 跨项目转移（MGR-12）：与「移动到分组」不同——后者只改 group_id，
// 这里会把设备连同其通道一起划到另一个项目下，并通知适配器（cmd.transfer）。
const xferDlg = reactive({ show: false, projectId: '', saving: false })
const xferProjects = ref<any[]>([])

async function openTransfer() {
  if (!selection.value.length) return toast.warning(t('device.msg.selectDeviceFirst'))
  xferDlg.projectId = ''
  xferDlg.show = true
  if (!xferProjects.value.length) {
    try {
      const res: any = await api.get('/projects')
      // 排除当前项目：转到自己没有意义
      xferProjects.value = (res?.items || res || []).filter((p: any) => p.id !== currentProject.value?.id)
    } catch (e: any) {
      toastApiError(e, t('device.msg.projectLoadFailed'))
    }
  }
}

async function doTransfer() {
  if (!xferDlg.projectId) return toast.warning(t('device.msg.selectProject'))
  xferDlg.saving = true
  const ids = [...selection.value]
  const failed: string[] = []
  try {
    // 后端按单台提供转移端点，这里逐台调用并汇总结果，
    // 不能因为中间一台失败就假装整批成功。
    for (const id of ids) {
      try {
        await api.post(`/devices/${id}/transfer`, { projectId: xferDlg.projectId })
      } catch {
        failed.push(id)
      }
    }
    if (!failed.length) {
      toast.success(t('device.msg.transferOk', { n: ids.length }))
    } else if (failed.length === ids.length) {
      toast.error({ title: t('device.msg.transferFailedTitle'), suggest: t('device.msg.transferFailedSuggest') })
    } else {
      toast.warning(t('device.msg.transferPartial', { ok: ids.length - failed.length, failed: failed.length }))
    }
    xferDlg.show = false
    selection.value = []
    load()
  } finally {
    xferDlg.saving = false
  }
}

function openBatchMove() {
  if (!selection.value.length) return toast.warning(t('device.msg.selectDeviceFirst'))
  batchMoveDlg.groupId = ''
  batchMoveDlg.show = true
}

async function doBatchMove() {
  if (!batchMoveDlg.groupId) return toast.warning(t('device.msg.selectGroup'))
  batchMoveDlg.saving = true
  try {
    await api.post('/devices/batch', { action: 'move', ids: selection.value, groupId: batchMoveDlg.groupId })
    toast.success(t('device.msg.moveOk'))
    batchMoveDlg.show = false
    selection.value = []
    load()
  } catch (e: any) {
    toastApiError(e, t('device.msg.moveFailed'))
  } finally {
    batchMoveDlg.saving = false
  }
}

async function doBatchReboot() {
  if (!selection.value.length) return toast.warning(t('device.msg.selectRebootFirst'))
  const ok = await confirmBox.ask({
    title: t('device.confirm.batchRebootTitle'),
    message: t('device.confirm.batchRebootMsg', { n: selection.value.length }),
    detail: t('device.confirm.batchRebootDetail'),
    confirmText: t('device.confirm.rebootNow')
  })
  if (!ok) return
  try {
    await api.post('/devices/batch', { action: 'reboot', ids: selection.value })
    toast.success(t('device.msg.rebootSent'))
  } catch (e: any) {
    toastApiError(e, t('device.msg.rebootFailed'))
  }
}

async function doBatchDelete() {
  if (!selection.value.length) return toast.warning(t('device.msg.selectDeleteFirst'))
  const ok = await confirmBox.ask({
    title: t('device.confirm.batchDeleteTitle'),
    message: t('device.confirm.batchDeleteMsg', { n: selection.value.length }),
    detail: t('device.confirm.batchDeleteDetail'),
    danger: true,
    confirmText: t('device.confirm.deleteOk')
  })
  if (!ok) return
  try {
    await api.post('/devices/batch', { action: 'delete', ids: selection.value })
    toast.success(t('device.msg.batchDeleteOk'))
    selection.value = []
    load()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
  }
}

async function exportCsv() {
  const params: any = {}
  if (selectedGroup.value) params.groupId = selectedGroup.value
  if (query.keyword) params.keyword = query.keyword.trim()
  if (query.status) params.status = query.status
  if (query.source) params.source = query.source
  if (selection.value.length) params.ids = selection.value.join(',')
  try {
    await api.download('/devices/export', params)
  } catch (e: any) {
    toastApiError(e, t('device.msg.exportFailed'))
  }
}

const syncing = ref(false)
async function doBatchSync() {
  if (!selection.value.length) return toast.warning(t('device.msg.selectSyncFirst'))
  syncing.value = true
  try {
    let ok = 0
    for (const id of selection.value) {
      try {
        await api.post(`/devices/${id}/sync`)
        ok++
      } catch (e: any) {
        toastApiError(e, t('device.msg.syncFailed', { id }))
      }
    }
    if (ok) toast.success(t('device.msg.syncOk', { n: ok }))
    load()
  } finally {
    syncing.value = false
  }
}

// 单台设备危险删除（MGR-11：输入设备名称二次确认）
async function askDeleteSingle(row: any) {
  const ok = await confirmBox.ask({
    title: t('device.confirm.deleteTitle'),
    message: t('device.confirm.deleteMsg', { name: row.name }),
    detail: t('device.confirm.deleteDetail'),
    danger: true,
    confirmText: t('device.confirm.deleteOk'),
    inputConfirm: row.name,
    inputPlaceholder: row.name
  })
  if (!ok) return
  try {
    await api.request(`/devices/${row.id}`, { method: 'DELETE', body: { confirmName: row.name } })
    toast.success(t('device.msg.deleteOk'))
    load()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
  }
}

// 行展开：多通道子列表
const chCache = reactive<Record<string, any[]>>({})
async function toggleExpand(row: any) {
  if (expandedRow.value === row.id) {
    expandedRow.value = ''
    return
  }
  expandedRow.value = row.id
  if (!chCache[row.id]) {
    try {
      const res: any = await api.get(`/devices/${row.id}/channels`)
      chCache[row.id] = res?.items || res?.channels || []
    } catch {
      chCache[row.id] = []
    }
  }
}

// 编辑设备弹窗
const editDevDlg = reactive({ show: false, row: null as any, name: '', location: '' })
function openEditDev(row: any) {
  editDevDlg.row = row
  editDevDlg.name = row.name || ''
  editDevDlg.location = row.location || ''
  editDevDlg.show = true
}
async function saveEditDev() {
  try {
    await api.put(`/devices/${editDevDlg.row.id}`, { name: editDevDlg.name.trim(), location: editDevDlg.location.trim() })
    toast.success(t('device.msg.editSaved'))
    editDevDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  }
}

// ================= 【添加设备弹窗】 =================
// Tab 对应平台真实支持的四种接入方式（ADD-01/03/05/06/07/08），
// 与 device.source 的四个字面判别值一一对应；不再保留竞品原型里的
// 「MAC地址添加/智能配置/物联APP」等本平台并不存在的方式。
const addDlg = reactive({ show: false })
const addTab = ref<'idp' | 'onvif' | 'rtsp' | 'gb28181'>('idp')
const addSubTab = ref<'single' | 'batch'>('single') // 仅 idp 支持批量导入

const addForm = reactive({
  groupId: '',
  deviceId: '',
  name: '',
  verifyCode: ''
})

// ONVIF：发现列表 + 手动填写（后端 /devices/onvif/discover 与 /devices/onvif）
const onvifForm = reactive({ ip: '', port: '80', user: '', pass: '', name: '' })
const onvifDiscover = reactive({ loading: false, items: [] as any[], done: false })

// RTSP：完整 URL 直连（后端 /devices/rtsp）
const rtspForm = reactive({ url: '', subUrl: '', name: '' })

// 国标白名单：设备主动注册前先登记 ID + 密码（ADD-05）
const gbForm = reactive({ gbId: '', pwd: '', name: '' })
const gbWhitelist = reactive({ loading: false, items: [] as any[] })

const batchIdText = ref('')
const adding = ref(false)

function openAddModal() {
  addForm.deviceId = ''
  addForm.name = ''
  addForm.verifyCode = ''
  addForm.groupId = groups.value[0]?.id || ''
  batchIdText.value = ''
  Object.assign(onvifForm, { ip: '', port: '80', user: '', pass: '', name: '' })
  Object.assign(onvifDiscover, { loading: false, items: [], done: false })
  Object.assign(rtspForm, { url: '', subUrl: '', name: '' })
  Object.assign(gbForm, { gbId: '', pwd: '', name: '' })
  addSubTab.value = 'single'
  addTab.value = 'idp'
  addDlg.show = true
}

/** 切到国标 Tab 时拉取现有白名单，让用户看到已登记了哪些 */
watch(addTab, (t) => {
  if (t === 'gb28181' && !gbWhitelist.items.length) loadGbWhitelist()
})

async function loadGbWhitelist() {
  gbWhitelist.loading = true
  try {
    const res: any = await api.get('/devices/gb28181/whitelist')
    gbWhitelist.items = res?.items || []
  } catch (e: any) {
    toastApiError(e, t('device.msg.gbWhitelistLoadFailed'))
  } finally {
    gbWhitelist.loading = false
  }
}

async function doOnvifDiscover() {
  onvifDiscover.loading = true
  onvifDiscover.done = false
  try {
    const res: any = await api.post('/devices/onvif/discover')
    onvifDiscover.items = res?.items || []
    onvifDiscover.done = true
    if (!onvifDiscover.items.length) toast.info(t('device.msg.onvifNotFound'))
  } catch (e: any) {
    toastApiError(e, t('device.msg.onvifDiscoverFailed'))
  } finally {
    onvifDiscover.loading = false
  }
}

/** 从发现结果选一台：回填 IP/端口，用户仍需补账号密码 */
function pickDiscovered(it: any) {
  onvifForm.ip = it.ip || ''
  const m = String(it.xaddr || '').match(/:(\d+)\//)
  onvifForm.port = m ? m[1] : '80'
}

async function submitAdd() {
  if (addTab.value === 'idp') return submitIdp()
  if (addTab.value === 'onvif') return submitOnvif()
  if (addTab.value === 'rtsp') return submitRtsp()
  if (addTab.value === 'gb28181') return submitGb()
}

async function submitIdp() {
  if (addSubTab.value === 'single') {
    if (!addForm.deviceId.trim()) return toast.warning(t('device.msg.deviceIdRequired'))
    if (addForm.verifyCode.trim().length < 6) return toast.warning(t('device.msg.verifyCodeRequired'))
    adding.value = true
    try {
      await api.post('/devices/idp/bind', {
        deviceId: addForm.deviceId.trim().toUpperCase(),
        verifyCode: addForm.verifyCode.trim(),
        groupId: addForm.groupId || undefined,
        name: addForm.name.trim() || undefined
      })
      toast.success(t('device.msg.addOk'))
      addDlg.show = false
      load()
    } catch (e: any) {
      toastApiError(e, t('device.msg.addFailed'))
    } finally {
      adding.value = false
    }
  } else {
    // 批量导入：每行「设备ID,验证码」，分隔符逗号或空格
    const lines = batchIdText.value.split('\n').map((l) => l.trim()).filter(Boolean)
    if (!lines.length) return toast.warning(t('device.msg.batchListRequired'))
    const items: { deviceId: string; verifyCode: string }[] = []
    for (let i = 0; i < lines.length; i++) {
      const parts = lines[i].split(/[,\s]+/).filter(Boolean)
      if (parts.length < 2) return toast.warning(t('device.msg.batchLineMissingCode', { line: i + 1, text: lines[i] }))
      items.push({ deviceId: parts[0].toUpperCase(), verifyCode: parts[1] })
    }
    adding.value = true
    try {
      await api.post('/devices/idp/preadd', { items })
      toast.success(t('device.msg.batchSubmitted', { n: items.length }))
      addDlg.show = false
      load()
    } catch (e: any) {
      toastApiError(e, t('device.msg.batchImportFailed'))
    } finally {
      adding.value = false
    }
  }
}

async function submitOnvif() {
  if (!onvifForm.ip.trim()) return toast.warning(t('device.msg.onvifIpRequired'))
  if (!onvifForm.user.trim() || !onvifForm.pass) return toast.warning(t('device.msg.onvifCredRequired'))
  adding.value = true
  try {
    await api.post('/devices/onvif', {
      ip: onvifForm.ip.trim(),
      port: onvifForm.port.trim() || '80',
      user: onvifForm.user.trim(),
      pass: onvifForm.pass,
      groupId: addForm.groupId || undefined,
      name: onvifForm.name.trim() || undefined
    })
    toast.success(t('device.msg.onvifAddOk'))
    addDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, t('device.msg.onvifAddFailed'))
  } finally {
    adding.value = false
  }
}

async function submitRtsp() {
  if (!rtspForm.url.trim()) return toast.warning(t('device.msg.rtspUrlRequired'))
  adding.value = true
  try {
    await api.post('/devices/rtsp', {
      url: rtspForm.url.trim(),
      subUrl: rtspForm.subUrl.trim() || undefined,
      groupId: addForm.groupId || undefined,
      name: rtspForm.name.trim() || undefined
    })
    toast.success(t('device.msg.rtspAddOk'))
    addDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, t('device.msg.rtspAddFailed'))
  } finally {
    adding.value = false
  }
}

async function submitGb() {
  if (!gbForm.gbId.trim()) return toast.warning(t('device.msg.gbIdRequired'))
  if (!gbForm.pwd) return toast.warning(t('device.msg.gbPwdRequired'))
  adding.value = true
  try {
    await api.post('/devices/gb28181/whitelist', {
      gbId: gbForm.gbId.trim(),
      pwd: gbForm.pwd,
      groupId: addForm.groupId || undefined,
      name: gbForm.name.trim() || undefined
    })
    toast.success(t('device.msg.gbWhitelistOk'))
    Object.assign(gbForm, { gbId: '', pwd: '', name: '' })
    loadGbWhitelist()
  } catch (e: any) {
    toastApiError(e, t('device.msg.gbWhitelistFailed'))
  } finally {
    adding.value = false
  }
}

async function delGbWhitelist(row: any) {
  const ok = await confirm.ask({
    title: t('device.confirm.gbRemoveTitle'),
    message: t('device.confirm.gbRemoveMsg', { id: row.gbId }),
    confirmText: t('device.confirm.gbRemoveOk'), danger: true
  })
  if (!ok) return
  try {
    await api.del('/devices/gb28181/whitelist/' + row.id)
    toast.success(t('device.msg.removedOk'))
    loadGbWhitelist()
  } catch (e: any) {
    toastApiError(e, t('device.msg.removeFailed'))
  }
}

// ================= 预览与回放弹窗（LIVE-01 + REC-01） =================
const previewModal = reactive({
  show: false,
  device: null as any,
  channel: null as any,
  tab: 'preview' as 'preview' | 'playback'
})

// 打开预览弹窗前解析出真实通道对象：显式传入 channel 时直接用；否则取该设备缓存中第一个
// 未禁用的通道，缓存未命中时现拉一次；请求失败与"设备确无通道"是两种不同状态，不可合并：
// 失败不写入缓存（避免把失败结果当"确无通道"缓存下来），只有请求成功且为空数组才提示无通道
async function openPreview(row: any, channel?: any, tab: 'preview' | 'playback' = 'preview') {
  let ch = channel || null
  if (!ch) {
    if (!chCache[row.id]) {
      try {
        const res: any = await api.get(`/devices/${row.id}/channels`)
        chCache[row.id] = res?.items || res?.channels || []
      } catch (e: any) {
        toastApiError(e, t('device.msg.channelLoadFailed'))
        return
      }
    }
    ch = chCache[row.id]?.find((c: any) => c.enabled !== false) || null
  }
  if (!ch) {
    toast.warning(t('device.msg.noPreviewChannel'))
    return
  }
  previewModal.device = row
  previewModal.channel = ch
  previewModal.tab = tab
  previewModal.show = true
}

/* 实时状态（E8）：设备上下线是值班员的核心关注点。
   只就地改这一行的状态字段，不整表重拉——否则会打断勾选与滚动位置。 */
useWs((ev: any) => {
  if (ev.type !== 'device.online' && ev.type !== 'device.offline') return
  const id = ev.deviceId
  if (!id) return
  const row = devices.value.find((d: any) => d.id === id)
  if (row) {
    row.status = ev.type === 'device.online' ? 'online' : 'offline'
    row.lastSeenAt = ev.ts || Date.now()
  } else {
    // 新接入的设备不在当前页，拉一次让它出现
    load()
  }
})

onMounted(async () => {
  try {
    const savedCols = localStorage.getItem('ipc_dev_hidden_cols')
    if (savedCols) hiddenCols.value = JSON.parse(savedCols)
  } catch {
    // 读不到或内容损坏：用默认列配置
  }
  await loadGroups()
  load()
  if (route.query.add === '1') openAddModal()
})
</script>

<template>
  <div class="flex gap-4 items-start">
    <!-- 左侧：设备分组（信号灯式激活态，与全局侧栏呼应：左侧细竖线 + 图标变色，而非整块高亮胶囊） -->
    <div class="w-60 shrink-0 rounded-signal border border-line bg-surface p-3 shadow-card">
      <div class="mb-3 flex items-center justify-between">
        <span class="text-sm font-bold text-ink">{{ t('device.list.groupPanel') }}</span>
        <div class="flex items-center gap-1">
          <button type="button" class="rounded-chrome p-1 text-muted transition-colors hover:bg-zone hover:text-primary" :aria-label="t('device.list.addGroup')" :title="t('device.list.addGroup')" @click="openAddGroup">
            <Icon name="plus" :size="15" />
          </button>
          <button
            class="rounded-chrome p-1 text-muted transition-colors hover:bg-zone hover:text-primary disabled:opacity-30"
            :disabled="!selectedGroup"
            :title="t('device.list.editGroup')"
            @click="openEditGroup(groups.find(g => g.id === selectedGroup))"
          >
            <Icon name="edit" :size="14" />
          </button>
        </div>
      </div>

      <!-- 搜索框 -->
      <UiInput v-model="groupSearch" :placeholder="t('device.list.searchGroup')" size="sm" class="mb-3">
        <template #prefix><Icon name="search" :size="13" class="mr-1.5 text-placeholder" /></template>
      </UiInput>

      <!-- 分组树列表 -->
      <div class="space-y-0.5">
        <div
          class="relative flex cursor-pointer items-center justify-between rounded-signal py-1.5 pl-4 pr-2.5 text-xs transition-colors"
          :class="!selectedGroup ? 'bg-zone font-medium text-ink' : 'text-muted hover:bg-zone hover:text-ink'"
          @click="selectedGroup = ''; search()"
        >
          <span class="absolute left-0.5 top-1/2 h-3.5 w-0.5 -translate-y-1/2 rounded-full bg-primary transition-opacity" :class="!selectedGroup ? 'opacity-100' : 'opacity-0'" />
          <span class="flex items-center gap-1.5">
            <Icon name="folder" :size="14" :class="!selectedGroup ? 'text-primary' : ''" />{{ t('device.list.allGroups') }}
          </span>
          <span class="text-[11px] text-placeholder">({{ stats.total }})</span>
        </div>

        <div
          v-for="g in groups.filter(x => !groupSearch || x.name.includes(groupSearch))"
          :key="g.id"
          class="relative flex cursor-pointer items-center justify-between rounded-signal py-1.5 pl-4 pr-2.5 text-xs transition-colors"
          :class="selectedGroup === g.id ? 'bg-zone font-medium text-ink' : 'text-muted hover:bg-zone hover:text-ink'"
          @click="selectedGroup = g.id; search()"
        >
          <span class="absolute left-0.5 top-1/2 h-3.5 w-0.5 -translate-y-1/2 rounded-full bg-primary transition-opacity" :class="selectedGroup === g.id ? 'opacity-100' : 'opacity-0'" />
          <span class="flex items-center gap-1.5 truncate">
            <Icon name="chevron-down" :size="11" class="text-placeholder" />
            <Icon name="video" :size="13" :class="selectedGroup === g.id ? 'text-primary' : ''" />{{ g.name }}
          </span>
        </div>
      </div>
    </div>

    <!-- 右侧：主体区 -->
    <div class="min-w-0 flex-1 space-y-3">
      <!-- 顶部设备状态卡片：类型统计，可折叠 -->
      <div class="relative rounded-signal border border-line bg-surface p-4 shadow-card">
        <div v-show="!cardCollapsed" class="flex items-center gap-4">
          <div class="flex h-12 w-12 items-center justify-center rounded-signal bg-zone text-muted">
            <Icon name="video" :size="28" />
          </div>
          <div>
            <div class="text-xs text-muted">IPC</div>
            <div class="mt-0.5 text-xl font-bold text-ink">
              {{ stats.total }} <span class="text-xs font-normal">{{ t('device.list.unit') }}</span>
            </div>
            <div class="mt-1 flex items-center gap-1.5">
              <span class="rounded-signal bg-danger-soft px-1.5 py-0.5 text-[10px] font-medium text-danger">
                {{ t('device.list.offlineCount', { n: stats.offline }) }}
              </span>
            </div>
          </div>
        </div>

        <!-- 卡片折叠收起把手 -->
        <div class="flex justify-center border-t border-line-soft pt-1 mt-2">
          <button type="button" class="rounded-chrome p-0.5 text-muted transition-colors hover:text-primary" :aria-label="cardCollapsed ? t('device.list.expandStatCard') : t('device.list.collapseStatCard')" :aria-expanded="!cardCollapsed" @click="cardCollapsed = !cardCollapsed">
            <Icon :name="cardCollapsed ? 'chevron-down' : 'chevron-up'" :size="15" />
          </button>
        </div>
      </div>

      <!-- 表格主体卡片 -->
      <div class="rounded-signal border border-line bg-surface p-4 shadow-card">
        <!-- 主工具栏行：全部 | IPC Tab 与添加设备按钮 -->
        <div class="mb-3">
          <UiTabs
            v-model="query.tab"
            :items="[{ label: t('device.list.tabAll'), value: 'all' }, { label: 'IPC', value: 'ipc' }]"
          >
            <template #extra>
              <!-- 国标待确认入口：与侧栏徽标同源（useState('gbPending')），仅有待确认时出现 -->
              <UiButton v-if="pendingCount > 0" class="ml-auto" @click="navigateTo('/devices/pending')">
                {{ t('device.list.pendingEntry') }}
                <span class="ml-1 rounded-full bg-danger px-1.5 text-[10px] leading-4 text-white">
                  {{ pendingCount > 99 ? '99+' : pendingCount }}
                </span>
              </UiButton>
              <UiButton variant="primary" :class="pendingCount > 0 ? '' : 'ml-auto'" @click="openAddModal">
                <Icon name="plus" :size="15" />{{ t('device.toolbar.addDevice') }}
              </UiButton>
            </template>
          </UiTabs>
        </div>

        <!-- 批量操作与列配置条 -->
        <div class="mb-3 flex flex-wrap items-center justify-between gap-2 text-xs">
          <!-- 左侧操作按钮群 -->
          <div class="flex flex-wrap items-center gap-1.5">
            <!-- [=] 内容 列配置按钮 -->
            <UiPopover>
              <template #trigger>
                <UiButton size="sm"><Icon name="list" :size="13" />{{ t('device.toolbar.columns') }}</UiButton>
              </template>
              <div class="w-40 space-y-0.5 p-1">
                <div v-for="c in allCols" :key="c.key" class="rounded px-1.5 py-1 hover:bg-zone">
                  <UiCheckbox :model-value="showCol(c.key)" :label="t(c.labelKey)" @update:model-value="toggleCol(c.key)" />
                </div>
              </div>
            </UiPopover>

            <UiButton size="sm" :disabled="!selection.length" @click="openBatchMove">{{ t('device.toolbar.moveGroup') }}</UiButton>
            <UiButton size="sm" :disabled="!selection.length" @click="openTransfer">{{ t('device.toolbar.transferProject') }}</UiButton>
            <UiButton size="sm" :disabled="!selection.length" @click="doBatchReboot">{{ t('device.toolbar.reboot') }}</UiButton>
            <UiButton variant="dangerText" size="sm" :disabled="!selection.length" @click="doBatchDelete">{{ t('device.toolbar.delete') }}</UiButton>
            <UiButton size="sm" @click="exportCsv">{{ t('device.toolbar.export') }}</UiButton>
            <UiButton size="sm" :disabled="!selection.length || syncing" @click="doBatchSync">{{ t('device.toolbar.sync') }}</UiButton>
            <UiButton size="sm" :title="t('common.refresh')" @click="load"><Icon name="refresh" :size="13" /></UiButton>
          </div>

          <!-- 右侧搜索与过滤 -->
          <div class="flex items-center gap-2">
            <UiButton size="sm" @click="openAddModal">{{ t('device.toolbar.batchAdd') }}</UiButton>
            <!-- 搜索框 -->
            <UiInput v-model="query.keyword" :placeholder="t('device.toolbar.searchPlaceholder')" size="sm" width="w-48" @enter="search">
              <template #prefix><Icon name="search" :size="12" class="mr-1 text-placeholder" /></template>
            </UiInput>
            <!-- 筛选下拉 -->
            <UiSelect
              v-model="query.status"
              :options="[
                { label: t('device.toolbar.allStatus'), value: '' },
                { label: t('common.online'), value: 'online' },
                { label: t('common.offline'), value: 'offline' }
              ]"
              width="w-24"
              size="sm"
              @update:model-value="search"
            />
          </div>
        </div>

        <!-- 设备表格 -->
        <div class="overflow-x-auto rounded-signal border border-line">
          <table class="w-full text-left text-xs text-body">
            <thead class="border-b border-line bg-zone text-muted">
              <tr>
                <th class="py-2.5 px-3 w-8">
                  <UiCheckbox
                    :model-value="selection.length > 0 && selection.length === devices.length"
                    :aria-label="selection.length === devices.length ? t('device.list.clearSelection') : t('device.list.selectAllPage')"
                    @update:model-value="selection = selection.length === devices.length ? [] : devices.map(d => d.id)"
                  />
                </th>
                <th v-if="showCol('index')" class="py-2.5 px-3 w-10 text-center">{{ t('device.col.index') }}</th>
                <th v-if="showCol('name')" class="py-2.5 px-3">{{ t('device.col.name') }}</th>
                <th v-if="showCol('type')" class="py-2.5 px-3">{{ t('device.col.type') }}</th>
                <th v-if="showCol('source')" class="py-2.5 px-3">{{ t('device.col.source') }}</th>
                <th v-if="showCol('status')" class="py-2.5 px-3">{{ t('device.col.status') }}</th>
                <th v-if="showCol('model')" class="py-2.5 px-3">{{ t('device.col.model') }}</th>
                <th v-if="showCol('ip')" class="py-2.5 px-3 text-right">{{ t('device.col.ip') }}</th>
                <th v-if="showCol('mac')" class="py-2.5 px-3 text-right">{{ t('device.col.mac') }}</th>
                <th v-if="showCol('group')" class="py-2.5 px-3">{{ t('device.col.group') }}</th>
                <th v-if="showCol('location')" class="py-2.5 px-3">{{ t('device.col.location') }}</th>
                <th v-if="showCol('ops')" class="py-2.5 px-3 text-right">{{ t('device.col.ops') }}</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-line-soft">
              <template v-for="(row, idx) in devices" :key="row.id">
                <tr class="transition-colors hover:bg-primary-softer">
                  <td class="py-2.5 px-3">
                    <UiCheckbox
                      :model-value="selection.includes(row.id)"
                      :aria-label="t('device.list.selectDevice', { name: row.name || row.id })"
                      @update:model-value="selection.includes(row.id) ? selection = selection.filter(id => id !== row.id) : selection.push(row.id)"
                    />
                  </td>
                  <td v-if="showCol('index')" class="py-2.5 px-3 text-center text-placeholder">{{ idx + 1 }}</td>
                  <td v-if="showCol('name')" class="py-2.5 px-3">
                    <div class="flex items-center gap-1.5 text-left">
                      <button
                        type="button"
                        class="rounded-chrome p-0.5 text-placeholder transition-transform hover:bg-zone"
                        :class="expandedRow === row.id ? 'rotate-90 text-primary' : ''"
                        :aria-label="expandedRow === row.id ? t('device.list.collapseChannels', { name: row.name }) : t('device.list.expandChannels', { name: row.name })"
                        :aria-expanded="expandedRow === row.id"
                        :title="t('device.list.expandChannelsTip')"
                        @click.stop="toggleExpand(row)"
                      >
                        <Icon name="chevron-right" :size="12" />
                      </button>
                      <UiButton variant="text" size="sm" class="max-w-[160px] truncate" @click="router.push(`/devices/${row.id}`)">
                        {{ row.name }}
                      </UiButton>
                    </div>
                  </td>
                  <td v-if="showCol('type')" class="py-2.5 px-3">{{ row.type || 'IPC' }}</td>
                  <td v-if="showCol('source')" class="py-2.5 px-3"><UiTag :color="sourceInfo(row.source).color">{{ t(sourceInfo(row.source).labelKey) }}</UiTag></td>
                  <td v-if="showCol('status')" class="py-2.5 px-3">
                    <span class="flex items-center gap-1.5" :class="row.status === 'online' ? 'text-primary' : 'text-muted'">
                      <span class="h-2 w-2 rounded-full transition-colors" :class="row.status === 'online' ? 'bg-primary' : 'bg-muted'" />
                      {{ row.status === 'online' ? t('common.online') : t('common.offline') }}
                    </span>
                  </td>
                  <td v-if="showCol('model')" class="py-2.5 px-3 text-muted">{{ row.model || '—' }}</td>
                  <td v-if="showCol('ip')" class="py-2.5 px-3 text-right font-mono text-muted">{{ row.ip || '—' }}</td>
                  <td v-if="showCol('mac')" class="py-2.5 px-3 text-right font-mono text-muted">{{ row.mac || '—' }}</td>
                  <td v-if="showCol('group')" class="py-2.5 px-3">{{ groupMap[row.groupId] || '—' }}</td>
                  <td v-if="showCol('location')" class="py-2.5 px-3 text-placeholder">{{ row.location || '—' }}</td>
                  <!-- 操作列：远程配置、编辑、预览、删除 -->
                  <td v-if="showCol('ops')" class="py-2.5 px-3 text-right">
                    <div class="flex items-center justify-end gap-1">
                      <UiButton variant="text" size="sm" @click="router.push(`/devices/${row.id}`)">{{ t('device.toolbar.remoteConfig') }}</UiButton>
                      <UiButton variant="text" size="sm" @click="openEditDev(row)">{{ t('common.edit') }}</UiButton>
                      <UiButton variant="text" size="sm" @click="openPreview(row)">{{ t('device.toolbar.preview') }}</UiButton>
                      <UiButton variant="dangerText" size="sm" :title="t('device.toolbar.delete')" @click="askDeleteSingle(row)">{{ t('common.delete') }}</UiButton>
                    </div>
                  </td>
                </tr>

                <!-- 行展开通道子列表（MGR-01） -->
                <tr v-if="expandedRow === row.id">
                  <td colspan="12" class="border-b border-line-soft bg-zone p-3 pl-12">
                    <div class="mb-1.5 text-xs font-semibold text-muted">{{ t('device.list.channelList') }}</div>
                    <div v-if="!chCache[row.id]?.length" class="text-xs text-placeholder">
                      {{ t('device.list.noChannel') }}
                    </div>
                    <div v-else class="flex flex-wrap gap-2">
                      <div
                        v-for="ch in chCache[row.id]"
                        :key="ch.id"
                        class="flex items-center gap-2 rounded-signal border border-line bg-surface px-2.5 py-1 text-xs"
                      >
                        <span class="h-1.5 w-1.5 rounded-full" :class="ch.enabled !== false ? 'bg-primary' : 'bg-line'" />
                        <span class="font-medium text-ink">{{ ch.name || t('device.list.channelNo', { n: ch.idx }) }}</span>
                        <UiButton variant="text" size="sm" class="ml-1" @click="openPreview(row, ch)">{{ t('device.toolbar.preview') }}</UiButton>
                      </div>
                    </div>
                  </td>
                </tr>
              </template>
            </tbody>
          </table>
        </div>

        <!-- 底部分页栏 -->
        <div class="mt-4 flex flex-wrap items-center justify-between gap-4 text-xs text-muted">
          <div>
            {{ t('device.list.totalPrefix') }} <span class="font-bold text-ink">{{ total }}</span> {{ t('device.list.totalSuffix') }}
            {{ t('device.list.pagePrefix') }} <span class="font-bold text-ink">{{ query.page }}</span>/{{ Math.ceil(total / query.pageSize) || 1 }} {{ t('device.list.pageSuffix') }}
            {{ t('device.list.selectedPrefix') }} <span class="font-bold text-primary">{{ selection.length }}</span>
          </div>

          <div class="flex items-center gap-2">
            <UiSelect
              v-model="query.pageSize"
              :options="[
                { label: t('page.perPage', { n: 20 }), value: 20 },
                { label: t('page.perPage', { n: 50 }), value: 50 },
                { label: t('page.perPage', { n: 100 }), value: 100 }
              ]"
              width="w-24"
              size="sm"
              @update:model-value="search"
            />
            <UiButton size="sm" class="h-7 w-7 px-0" :disabled="query.page <= 1" @click="query.page--; load()">
              &lt;
            </UiButton>
            <span class="rounded-chrome bg-primary-soft px-2 py-0.5 font-bold text-primary">{{ query.page }}</span>
            <UiButton size="sm" class="h-7 w-7 px-0" :disabled="query.page >= Math.ceil(total / query.pageSize)" @click="query.page++; load()">
              &gt;
            </UiButton>
          </div>
        </div>
      </div>
    </div>

    <!-- ================= 添加设备弹窗 ================= -->
    <UiDialog v-model:open="addDlg.show" :title="t('device.add.title')" width="max-w-2xl">
      <!-- 四种接入方式，与 device.source 的四个判别值一一对应 -->
      <UiTabs
        v-model="addTab"
        :items="[
          { label: t('device.add.tabIdp'), value: 'idp' },
          { label: 'ONVIF', value: 'onvif' },
          { label: 'RTSP', value: 'rtsp' },
          { label: t('device.add.tabGb'), value: 'gb28181' }
        ]"
      />

      <!-- 所属分组：四种方式共用 -->
      <div class="mt-4 flex items-center gap-3">
        <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">{{ t('device.add.group') }}</label>
        <div class="flex flex-1 items-center gap-2 text-sm">
          <span class="font-medium text-ink">{{ groupMap[addForm.groupId] || t('common.ungrouped') }}</span>
          <button type="button" class="rounded-chrome text-primary hover:text-primary-deep" :aria-label="t('device.add.editGroup')" :title="t('device.add.editGroup')" @click="openAddGroup">
            <Icon name="edit" :size="13" />
          </button>
        </div>
      </div>

      <!-- ---------- 自有设备（IDP）---------- -->
      <template v-if="addTab === 'idp'">
        <UiSegmented
          v-model="addSubTab"
          class="mt-4"
          :items="[{ label: t('device.add.single'), value: 'single' }, { label: t('device.add.batch'), value: 'batch' }]"
        />

        <div v-if="addSubTab === 'single'" class="mx-auto mt-5 max-w-md space-y-4">
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> {{ t('device.add.deviceId') }}</label>
            <UiInput v-model="addForm.deviceId" class="flex-1 uppercase" :placeholder="t('device.add.deviceIdPlaceholder')" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> {{ t('device.add.verifyCode') }}</label>
            <UiInput v-model="addForm.verifyCode" class="flex-1" :placeholder="t('device.add.verifyCodePlaceholder')" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">{{ t('device.add.name') }}</label>
            <UiInput v-model="addForm.name" class="flex-1" :placeholder="t('device.add.namePlaceholder')" />
          </div>
          <div class="flex flex-col items-center pt-2">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">{{ t('device.toolbar.addDevice') }}</UiButton>
            <button type="button" class="mt-3 text-xs text-primary hover:underline" @click="router.push('/scan')">
              {{ t('device.add.scanEntry') }}
            </button>
          </div>
        </div>

        <div v-else class="mx-auto mt-5 max-w-md space-y-4">
          <div>
            <label class="mb-1 block text-xs font-medium text-muted">
              <span class="text-danger">*</span> {{ t('device.add.batchLabel') }}
            </label>
            <textarea
              v-model="batchIdText"
              rows="6"
              class="w-full rounded-chrome border border-line bg-surface p-2.5 font-mono text-xs text-body outline-none focus:border-primary"
              placeholder="A1B2C3D4E5F678901,123456&#10;B2C3D4E5F67890123,654321"
            />
            <p class="mt-1 text-xs text-placeholder">
              {{ t('device.add.batchHint') }}
            </p>
          </div>
          <div class="flex flex-col items-center pt-1">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">{{ t('device.add.batchSubmit') }}</UiButton>
          </div>
        </div>
      </template>

      <!-- ---------- ONVIF ---------- -->
      <template v-else-if="addTab === 'onvif'">
        <div class="mt-4 flex items-center justify-between gap-3 rounded-signal border border-line bg-canvas px-3 py-2">
          <p class="text-xs text-muted">{{ t('device.add.onvifScanHint') }}</p>
          <UiButton size="sm" :loading="onvifDiscover.loading" @click="doOnvifDiscover">{{ t('device.add.onvifScan') }}</UiButton>
        </div>

        <div v-if="onvifDiscover.items.length" class="mt-2 max-h-40 overflow-auto rounded-signal border border-line">
          <table class="w-full text-xs">
            <tbody>
              <tr v-for="it in onvifDiscover.items" :key="it.xaddr" class="border-b border-line-soft last:border-0">
                <td class="px-3 py-2 font-mono text-body">{{ it.ip }}</td>
                <td class="px-3 py-2 text-placeholder">{{ (it.scopes || []).join(' ') || '—' }}</td>
                <td class="px-3 py-2 text-right">
                  <UiTag v-if="it.added" color="info">{{ t('device.add.onvifAdded') }}</UiTag>
                  <UiButton v-else variant="text" size="sm" @click="pickDiscovered(it)">{{ t('device.add.onvifPick') }}</UiButton>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
        <p v-else-if="onvifDiscover.done" class="mt-2 text-xs text-placeholder">
          {{ t('device.add.onvifNotFound') }}
        </p>

        <div class="mx-auto mt-5 max-w-md space-y-4">
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> {{ t('device.add.onvifIp') }}</label>
            <UiInput v-model="onvifForm.ip" class="flex-1" placeholder="192.168.1.64" />
            <UiInput v-model="onvifForm.port" width="w-20" placeholder="80" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> {{ t('device.add.onvifUser') }}</label>
            <UiInput v-model="onvifForm.user" class="flex-1" :placeholder="t('device.add.onvifUserPlaceholder')" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> {{ t('device.add.onvifPass') }}</label>
            <UiInput v-model="onvifForm.pass" type="password" class="flex-1" :placeholder="t('device.add.onvifPassPlaceholder')" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">{{ t('device.add.name') }}</label>
            <UiInput v-model="onvifForm.name" class="flex-1" :placeholder="t('device.add.namePlaceholderShort')" />
          </div>
          <div class="flex flex-col items-center pt-2">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">{{ t('device.toolbar.addDevice') }}</UiButton>
          </div>
        </div>
      </template>

      <!-- ---------- RTSP ---------- -->
      <template v-else-if="addTab === 'rtsp'">
        <div class="mx-auto mt-5 max-w-md space-y-4">
          <div class="flex items-start gap-3">
            <label class="w-24 shrink-0 pt-1.5 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> {{ t('device.add.rtspMain') }}</label>
            <div class="flex-1">
              <UiInput v-model="rtspForm.url" placeholder="rtsp://user:pass@192.168.1.64:554/Streaming/Channels/101" />
              <p class="mt-1 text-xs text-placeholder">{{ t('device.add.rtspHint') }}</p>
            </div>
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">{{ t('device.add.rtspSub') }}</label>
            <UiInput v-model="rtspForm.subUrl" class="flex-1" :placeholder="t('device.add.rtspSubPlaceholder')" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">{{ t('device.add.name') }}</label>
            <UiInput v-model="rtspForm.name" class="flex-1" :placeholder="t('device.add.namePlaceholderShort')" />
          </div>
          <div class="flex flex-col items-center pt-2">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">{{ t('device.toolbar.addDevice') }}</UiButton>
          </div>
        </div>
      </template>

      <!-- ---------- 国标白名单 ---------- -->
      <template v-else>
        <p class="mt-4 rounded-signal bg-zone p-3 text-xs leading-relaxed text-muted">
          {{ t('device.add.gbIntro') }}
        </p>

        <div class="mx-auto mt-4 max-w-md space-y-4">
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> {{ t('device.add.gbId') }}</label>
            <UiInput v-model="gbForm.gbId" class="flex-1 font-mono" :placeholder="t('device.add.gbIdPlaceholder')" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> {{ t('device.add.gbPwd') }}</label>
            <UiInput v-model="gbForm.pwd" type="password" class="flex-1" :placeholder="t('device.add.gbPwdPlaceholder')" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">{{ t('device.add.name') }}</label>
            <UiInput v-model="gbForm.name" class="flex-1" :placeholder="t('device.add.namePlaceholderShort')" />
          </div>
          <div class="flex flex-col items-center pt-1">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">{{ t('device.add.gbSubmit') }}</UiButton>
          </div>
        </div>

        <div class="mt-5">
          <p class="mb-2 text-xs font-medium text-muted">{{ t('device.add.gbListed') }}</p>
          <div v-if="gbWhitelist.loading" class="py-4 text-center"><Icon name="refresh" :size="16" class="ipc-spin text-primary" /></div>
          <p v-else-if="!gbWhitelist.items.length" class="py-3 text-center text-xs text-placeholder">{{ t('device.add.gbEmpty') }}</p>
          <div v-else class="max-h-40 overflow-auto rounded-signal border border-line">
            <table class="w-full text-xs">
              <tbody>
                <tr v-for="w in gbWhitelist.items" :key="w.id" class="border-b border-line-soft last:border-0">
                  <td class="px-3 py-2 font-mono text-body">{{ w.gbId }}</td>
                  <td class="px-3 py-2 text-placeholder">{{ dash(w.name) }}</td>
                  <td class="px-3 py-2 text-right">
                    <UiButton variant="dangerText" size="sm" @click="delGbWhitelist(w)">{{ t('common.remove') }}</UiButton>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </template>
    </UiDialog>

    <!-- 分组管理弹窗 -->
    <UiDialog v-model:open="groupDlg.show" :title="groupDlg.mode === 'add' ? t('device.group.addTitle') : t('device.group.renameTitle')" width="max-w-sm">
      <div class="space-y-3">
        <label class="block text-xs text-muted">{{ t('device.group.nameLabel') }}</label>
        <UiInput v-model="groupDlg.name" :placeholder="t('device.group.namePlaceholder')" />
      </div>
      <template #footer>
        <UiButton @click="groupDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" @click="saveGroup">{{ t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 批量转移分组弹窗 -->
    <!-- 跨项目转移（MGR-12） -->
    <UiDialog v-model:open="xferDlg.show" :title="t('device.transfer.title')" width="max-w-sm">
      <div class="space-y-3 p-1">
        <p class="text-xs text-placeholder">
          {{ t('device.transfer.hint', { n: selection.length }) }}
        </p>
        <div>
          <label class="mb-1 block text-xs font-medium text-muted">{{ t('device.transfer.targetProject') }}</label>
          <UiSelect v-model="xferDlg.projectId" :options="xferProjects.map((p: any) => ({ label: p.name, value: p.id }))" :placeholder="t('device.transfer.selectProject')" class="w-full" />
          <p v-if="!xferProjects.length" class="mt-1 text-xs text-placeholder">{{ t('device.transfer.noProject') }}</p>
        </div>
      </div>
      <template #footer>
        <UiButton size="sm" @click="xferDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" size="sm" :loading="xferDlg.saving" :disabled="!xferDlg.projectId" @click="doTransfer">{{ t('device.transfer.submit') }}</UiButton>
      </template>
    </UiDialog>

    <UiDialog v-model:open="batchMoveDlg.show" :title="t('device.move.title')" width="max-w-sm">
      <div class="space-y-3">
        <label class="block text-xs text-muted">{{ t('device.move.targetGroup') }}</label>
        <UiSelect v-model="batchMoveDlg.groupId" :options="groupOptions" class="w-full" />
      </div>
      <template #footer>
        <UiButton @click="batchMoveDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :loading="batchMoveDlg.saving" @click="doBatchMove">{{ t('device.transfer.submit') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 编辑设备基本信息弹窗 -->
    <UiDialog v-model:open="editDevDlg.show" :title="t('device.edit.title')" width="max-w-md">
      <div class="space-y-3">
        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('device.add.name') }}</label>
          <UiInput v-model="editDevDlg.name" />
        </div>
        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('device.edit.location') }}</label>
          <UiInput v-model="editDevDlg.location" :placeholder="t('device.edit.locationPlaceholder')" />
        </div>
      </div>
      <template #footer>
        <UiButton @click="editDevDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" @click="saveEditDev">{{ t('common.save') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 设备预览与录像回放模态弹窗 -->
    <DevicePreviewModal
      v-model="previewModal.show"
      :device="previewModal.device"
      :channel="previewModal.channel"
      :initial-tab="previewModal.tab"
    />
  </div>
</template>
