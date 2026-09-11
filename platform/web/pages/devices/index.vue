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
  bulk: ''
})

const total = ref(0)
const devices = ref<any[]>([])
const loading = ref(false)
const selection = ref<string[]>([])
const cardCollapsed = ref(false)

// 左侧分组面板折叠：分组列表只承担筛选，列多时（11 列 + 操作）不该常驻 240px。
// 折叠后留一条窄轨道，选择记忆写入 localStorage，刷新后保持。
const groupPanelCollapsed = ref(false)
try { groupPanelCollapsed.value = localStorage.getItem('ipc_dev_group_collapsed') === '1' } catch {}
function toggleGroupPanel() {
  groupPanelCollapsed.value = !groupPanelCollapsed.value
  try { localStorage.setItem('ipc_dev_group_collapsed', groupPanelCollapsed.value ? '1' : '0') } catch {}
}

/** 折叠轨道上的提示语：明确当前是否正在按某个分组筛选，避免"看不见分组名却还在被过滤" */
const groupPanelTip = computed(() =>
  selectedGroup.value
    ? t('device.list.filteredByGroup', { name: groupMap[selectedGroup.value] || selectedGroup.value })
    : t('device.list.expandGroupPanel')
)

// 项目全量统计（顶部卡片与「全部分组」计数）。来自后端 res.stats，
// 不随筛选、不随分页变化——按当前页 devices 自行统计会在翻页/筛选后漂移。
const stats = ref({
  total: 0,
  online: 0,
  offline: 0
})

async function load() {
  loading.value = true
  try {
    const params: any = { page: query.page, pageSize: query.pageSize }
    if (selectedGroup.value) params.groupId = selectedGroup.value
    if (query.keyword) params.keyword = query.keyword.trim()
    if (query.status) params.status = query.status
    if (query.source) params.source = query.source
    // 批量搜索：换行/逗号分隔的多行粘贴，后端按 deviceId / gbId / IP 精确匹配
    if (query.bulk) params.bulk = query.bulk

    const res: any = await api.get('/devices', params)
    devices.value = res?.items || []
    total.value = res?.total || devices.value.length

    // 统计取后端项目全量口径；后端未返回时退回当前页结果，保证卡片不至于空着
    const s = res?.stats
    stats.value = s
      ? { total: s.all ?? 0, online: s.online ?? 0, offline: s.offline ?? 0 }
      : {
          total: total.value,
          online: devices.value.filter((d: any) => d.status === 'online').length,
          offline: devices.value.filter((d: any) => d.status === 'offline').length
        }
  } catch (e: any) {
    toastApiError(e, t('device.msg.listLoadFailed'))
  } finally {
    loading.value = false
  }
}

// ================= 批量搜索（MGR-02） =================
// 后端 deviceFilters 早已支持 ?bulk=（换行/逗号切分后精确匹配 deviceId / gbId / IP），
// 但前端一直没有入口，能力一直空转。
const bulkDlg = reactive({ show: false, text: '' })

function search() {
  // 关键词与批量搜索互斥：两者一起发给后端会被 AND 成 0 条，
  // 而用户从列表上看不出是哪个条件把结果滤空的。后输入者胜。
  if (query.keyword.trim()) {
    query.bulk = ''
    bulkDlg.text = ''
  }
  query.page = 1
  load()
}

function openBulkSearch() {
  bulkDlg.text = query.bulk
  bulkDlg.show = true
}
function applyBulkSearch() {
  query.bulk = bulkDlg.text.trim()
  query.keyword = ''
  bulkDlg.show = false
  search()
}
function clearBulkSearch() {
  query.bulk = ''
  bulkDlg.text = ''
  search()
}

// ================= 列配置（内容按钮：控制表格显示的列） =================
// 列清单对齐 PRD MGR-01，`default:false` 的列默认收起。取舍按「每列信息量」而非重要性排序：
//   序号/设备类型/通道数 —— 当前设备集里恒为常量（类型恒 IPC、通道数恒 1）；
//   地理位置/厂商/固件 —— 多数设备为空；最后在线 —— 只有离线时才看得出价值；
//   流状态 —— 会变（推流中/总），保留。
// 实测默认 9 列 ≈ 968px，展开左侧分组面板时容器 1024px，不再横向滚动。
interface ColDef {
  key: string
  labelKey: string
  default?: boolean
}
const allCols: ColDef[] = [
  { key: 'index', labelKey: 'device.col.index', default: false },
  { key: 'name', labelKey: 'device.col.name' },
  { key: 'type', labelKey: 'device.col.type', default: false },
  { key: 'source', labelKey: 'device.col.source' },
  { key: 'status', labelKey: 'device.col.status' },
  { key: 'stream', labelKey: 'device.col.stream' },
  { key: 'model', labelKey: 'device.col.model' },
  { key: 'vendor', labelKey: 'device.col.vendor', default: false },
  { key: 'ip', labelKey: 'device.col.ip' },
  { key: 'mac', labelKey: 'device.col.mac' },
  { key: 'group', labelKey: 'device.col.group' },
  { key: 'channels', labelKey: 'device.col.channels', default: false },
  { key: 'location', labelKey: 'device.col.location', default: false },
  { key: 'fw', labelKey: 'device.col.fw', default: false },
  { key: 'lastSeen', labelKey: 'device.col.lastSeen', default: false },
  { key: 'ops', labelKey: 'device.col.ops' }
]

const defaultHiddenCols = allCols.filter((c) => c.default === false).map((c) => c.key)
const hiddenCols = ref<string[]>([...defaultHiddenCols])

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

/**
 * 设备级「流状态」：按通道聚合，口径与详情页「码流状态 x/y 路」一致。
 * 0 通道设备没有可推进的流，显示 —；否则显示 推流中/总路数，配色按覆盖度：
 * 全部推流 = primary、部分推流 = warning、全部未推流 = muted。
 */
function streamCell(row: any) {
  const total = Number(row.channelCount || 0)
  const live = Number(row.streamingCount || 0)
  if (!total) return { text: '—', dot: 'bg-muted', cls: 'text-muted' }
  if (live === total) return { text: `${live}/${total}`, dot: 'bg-primary', cls: 'text-primary' }
  if (live > 0) return { text: `${live}/${total}`, dot: 'bg-warning', cls: 'text-warning' }
  return { text: `${live}/${total}`, dot: 'bg-muted', cls: 'text-muted' }
}

/** 恢复默认列：回到 defaultHiddenCols 定义的默认视图，而不是「全开」 */
function resetCols() {
  hiddenCols.value = [...defaultHiddenCols]
  try {
    localStorage.removeItem('ipc_dev_hidden_cols')
  } catch {
    // 隐私模式下 localStorage 不可用：仅本次生效
  }
}

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

// 通道缓存：只服务「设备级预览」——openPreview(row) 必须拿到真实 channelId 才能调
// /channels/:id/play。原「行展开→通道列表」已移除：它为一行多打一次
// GET /devices/:id/channels，而单通道设备（当前 4 台全部）只回显一个与设备名相同的通道，
// 零新增信息；多通道也只是个预览挑路器，详情页（MGR-03）已覆盖且更完整。
// 详情页拿通道不花额外请求：GET /devices/:id 的响应里 channels 与设备信息同包返回。
const chCache = reactive<Record<string, any[]>>({})

// 编辑设备弹窗。字段对齐 MGR-04「名称、分组、安装位置、备注」——后端 PUT /devices/:id
// 四个字段都支持，之前只发 name/location，改分组得绕去详情页或批量工具条。
const editDevDlg = reactive({ show: false, row: null as any, name: '', location: '', remark: '', groupId: '' })
function openEditDev(row: any) {
  editDevDlg.row = row
  editDevDlg.name = row.name || ''
  editDevDlg.location = row.location || ''
  editDevDlg.remark = row.remark || ''
  editDevDlg.groupId = row.groupId || ''
  editDevDlg.show = true
}
async function saveEditDev() {
  const name = editDevDlg.name.trim()
  if (!name) return toast.warning(t('device.msg.nameRequired'))
  try {
    await api.put(`/devices/${editDevDlg.row.id}`, {
      name,
      location: editDevDlg.location.trim(),
      remark: editDevDlg.remark.trim(),
      groupId: editDevDlg.groupId
    })
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

/** 工具栏「批量添加」：直接落在 idp 的批量子页，而不是和「添加设备」开同一个单台表单 */
function openAddBatch() {
  openAddModal()
  addSubTab.value = 'batch'
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
  // 深链参数：/devices?status=online|offline&source=..&keyword=..&groupId=..
  // 总览页「在线/离线」指标条等入口全靠这些参数落位；不消费的话点进来仍是全量列表。
  const q = route.query
  if (typeof q.status === 'string') query.status = q.status
  if (typeof q.source === 'string') query.source = q.source
  if (typeof q.keyword === 'string') query.keyword = q.keyword
  if (typeof q.groupId === 'string') selectedGroup.value = q.groupId
  await loadGroups()
  load()
  if (route.query.add === '1') openAddModal()
})
</script>

<template>
  <div class="flex gap-4 items-start">
    <!-- 左侧：设备分组（信号灯式激活态，与全局侧栏呼应：左侧细竖线 + 图标变色，而非整块高亮胶囊）。
         分组列表只承担筛选，表格已有 11 列；折叠后只留一条窄轨道，把宽度让给表格。 -->
    <div v-if="groupPanelCollapsed" class="shrink-0 rounded-signal border border-line bg-surface p-1 shadow-card">
      <button
        type="button"
        class="relative flex h-7 w-6 items-center justify-center rounded-chrome transition-colors hover:bg-zone hover:text-primary"
        :class="selectedGroup ? 'text-primary' : 'text-muted'"
        :aria-label="groupPanelTip"
        :title="groupPanelTip"
        :aria-expanded="false"
        @click="toggleGroupPanel"
      >
        <Icon name="chevron-right" :size="15" />
        <!-- 仍有分组筛选生效时给一个点，避免"看不见分组名却还在被过滤" -->
        <span v-if="selectedGroup" class="absolute right-0 top-0.5 h-1.5 w-1.5 rounded-full bg-primary" aria-hidden="true" />
      </button>
    </div>
    <div v-else class="w-60 shrink-0 rounded-signal border border-line bg-surface p-3 shadow-card">
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
          <button
            type="button"
            class="rounded-chrome p-1 text-muted transition-colors hover:bg-zone hover:text-primary"
            :aria-label="t('device.list.collapseGroupPanel')"
            :title="t('device.list.collapseGroupPanel')"
            :aria-expanded="true"
            @click="toggleGroupPanel"
          >
            <Icon name="chevron-left" :size="15" />
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
          <span class="flex min-w-0 items-center gap-1.5 truncate">
            <Icon name="chevron-down" :size="11" class="text-placeholder" />
            <Icon name="video" :size="13" :class="selectedGroup === g.id ? 'text-primary' : ''" />{{ g.name }}
          </span>
          <span class="shrink-0 pl-1 text-[11px] text-placeholder">({{ g.deviceCount ?? 0 }})</span>
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
        <!-- 主工具栏行。
             原「全部 | IPC」标签页已移除：设备模型没有 type 维度，全站设备类型恒为 IPC，
             该标签不产生任何请求、切换前后结果完全一致（点击零请求的死交互）。待真正引入
             第二类设备（模型加 type 字段 + 后端 type 过滤）时，再随该功能一并加回。 -->
        <div class="mb-3 flex flex-wrap items-center gap-2">
          <!-- 国标待确认入口：与侧栏徽标同源（useState('gbPending')），仅有待确认时出现 -->
          <UiButton v-if="pendingCount > 0" @click="navigateTo('/devices/pending')">
            {{ t('device.list.pendingEntry') }}
            <span class="ml-1 rounded-full bg-danger px-1.5 text-[10px] leading-4 text-white">
              {{ pendingCount > 99 ? '99+' : pendingCount }}
            </span>
          </UiButton>
          <UiButton variant="primary" class="ml-auto" @click="openAddModal">
            <Icon name="plus" :size="15" />{{ t('device.toolbar.addDevice') }}
          </UiButton>
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
              <div class="w-44">
                <p class="mb-1.5 px-1.5 text-xs text-placeholder">{{ t('device.toolbar.columnsHint') }}</p>
                <!-- 列会越加越多，给列表一个固定上限并内部滚动：
                     标题与底部「重置」始终可见，弹窗不会长到顶出视口。 -->
                <div class="max-h-60 space-y-0.5 overflow-y-auto pr-0.5">
                  <div v-for="c in allCols" :key="c.key" class="rounded px-1.5 py-1 hover:bg-zone">
                    <UiCheckbox :model-value="showCol(c.key)" :label="t(c.labelKey)" @update:model-value="toggleCol(c.key)" />
                  </div>
                </div>
                <UiButton size="sm" block class="mt-1.5" @click="resetCols">{{ t('common.reset') }}</UiButton>
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
            <UiButton size="sm" @click="openAddBatch">{{ t('device.toolbar.batchAdd') }}</UiButton>
            <!-- 批量搜索：多行粘贴 deviceId / 国标 ID / IP -->
            <UiButton
              size="sm"
              :variant="query.bulk ? 'primary' : 'default'"
              :title="query.bulk ? t('device.bulkSearch.activeTip') : t('device.bulkSearch.title')"
              @click="openBulkSearch"
            >{{ t('device.toolbar.bulkSearch') }}</UiButton>
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
            <!-- 表头一律 whitespace-nowrap：中文标签最短 2 字、最长 4 字（设备名称/设备类型/设备状态/
                 所属分组/地理位置），列宽不够时若允许折行，「序号」会被拆成「序/号」、「设备状态」
                 会被拆成两行，表头高度参差且与内容对不齐。禁用折行后列宽自然撑到标签宽度，
                 整表不足时由外层 overflow-x-auto 横向滚动；收起左侧分组面板可再多出 240px。 -->
            <thead class="border-b border-line bg-zone text-muted">
              <tr>
                <th class="w-8 whitespace-nowrap px-3 py-2.5">
                  <UiCheckbox
                    :model-value="selection.length > 0 && selection.length === devices.length"
                    :aria-label="selection.length === devices.length ? t('device.list.clearSelection') : t('device.list.selectAllPage')"
                    @update:model-value="selection = selection.length === devices.length ? [] : devices.map(d => d.id)"
                  />
                </th>
                <th v-if="showCol('index')" class="w-12 whitespace-nowrap px-3 py-2.5 text-center">{{ t('device.col.index') }}</th>
                <th v-if="showCol('name')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.name') }}</th>
                <th v-if="showCol('type')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.type') }}</th>
                <th v-if="showCol('source')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.source') }}</th>
                <th v-if="showCol('status')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.status') }}</th>
                <th v-if="showCol('stream')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.stream') }}</th>
                <th v-if="showCol('model')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.model') }}</th>
                <th v-if="showCol('vendor')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.vendor') }}</th>
                <th v-if="showCol('ip')" class="whitespace-nowrap px-3 py-2.5 text-right">{{ t('device.col.ip') }}</th>
                <th v-if="showCol('mac')" class="whitespace-nowrap px-3 py-2.5 text-right">{{ t('device.col.mac') }}</th>
                <th v-if="showCol('group')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.group') }}</th>
                <th v-if="showCol('channels')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.channels') }}</th>
                <th v-if="showCol('location')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.location') }}</th>
                <th v-if="showCol('fw')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.fw') }}</th>
                <th v-if="showCol('lastSeen')" class="whitespace-nowrap px-3 py-2.5">{{ t('device.col.lastSeen') }}</th>
                <th v-if="showCol('ops')" class="whitespace-nowrap px-3 py-2.5 text-right">{{ t('device.col.ops') }}</th>
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
                    <!-- 真链接而非 button：能悬停看 URL、能 Ctrl/中键新开标签页，读屏也念成“链接”。
                         设备名称是列表进详情（MGR-03，含通道列表）的唯一入口。 -->
                    <NuxtLink
                      :to="`/devices/${row.id}`"
                      class="inline-block h-7 max-w-[120px] truncate rounded-chrome px-1 text-xs font-medium leading-7 text-primary transition-colors hover:bg-primary-soft"
                    >{{ row.name }}</NuxtLink>
                  </td>
                  <td v-if="showCol('type')" class="py-2.5 px-3">{{ row.type || 'IPC' }}</td>
                  <td v-if="showCol('source')" class="py-2.5 px-3"><UiTag :color="sourceInfo(row.source).color">{{ t(sourceInfo(row.source).labelKey) }}</UiTag></td>
                  <td v-if="showCol('status')" class="py-2.5 px-3">
                    <span class="flex items-center gap-1.5" :class="row.status === 'online' ? 'text-primary' : 'text-muted'">
                      <span class="h-2 w-2 rounded-full transition-colors" :class="row.status === 'online' ? 'bg-primary' : 'bg-muted'" />
                      {{ row.status === 'online' ? t('common.online') : t('common.offline') }}
                    </span>
                  </td>
                  <!-- 设备级流状态：按通道聚合，非单通道实时值（看单通道请进详情页） -->
                  <td v-if="showCol('stream')" class="whitespace-nowrap px-3 py-2.5">
                    <span class="flex items-center gap-1.5" :class="streamCell(row).cls">
                      <span class="h-2 w-2 rounded-full" :class="streamCell(row).dot" />
                      {{ streamCell(row).text }}
                    </span>
                  </td>
                  <!-- 型号是唯一可能较长的自由文本（SIM-GB28181 / SIM-IPC-100）：不折行，宁可让本列变宽 -->
                  <td v-if="showCol('model')" class="whitespace-nowrap px-3 py-2.5 text-muted">{{ row.model || '—' }}</td>
                  <td v-if="showCol('vendor')" class="whitespace-nowrap px-3 py-2.5 text-muted">{{ row.vendor || '—' }}</td>
                  <td v-if="showCol('ip')" class="py-2.5 px-3 text-right font-mono text-muted">{{ row.ip || '—' }}</td>
                  <td v-if="showCol('mac')" class="py-2.5 px-3 text-right font-mono text-muted">{{ row.mac || '—' }}</td>
                  <td v-if="showCol('group')" class="py-2.5 px-3">{{ groupMap[row.groupId] || '—' }}</td>
                  <td v-if="showCol('channels')" class="py-2.5 px-3 text-muted">{{ row.channelCount ?? '—' }}</td>
                  <td v-if="showCol('location')" class="py-2.5 px-3 text-placeholder">{{ row.location || '—' }}</td>
                  <td v-if="showCol('fw')" class="whitespace-nowrap px-3 py-2.5 font-mono text-muted">{{ row.fw || '—' }}</td>
                  <td v-if="showCol('lastSeen')" class="whitespace-nowrap px-3 py-2.5 text-placeholder">{{ row.lastSeenAt ? ago(row.lastSeenAt, t) : '—' }}</td>
                  <!-- 操作列：远程配置、编辑、预览、删除 -->
                  <td v-if="showCol('ops')" class="py-2.5 px-3 text-right">
                    <div class="flex items-center justify-end gap-1">
                      <UiButton variant="text" size="sm" @click="router.push(`/devices/${row.id}`)">{{ t('device.toolbar.remoteConfig') }}</UiButton>
                      <UiButton variant="text" size="sm" @click="openEditDev(row)">{{ t('common.edit') }}</UiButton>
                      <UiButton variant="text" size="sm" :title="t('device.list.previewTip')" @click="openPreview(row)">{{ t('device.toolbar.preview') }}</UiButton>
                      <UiButton variant="dangerText" size="sm" :title="t('device.toolbar.delete')" @click="askDeleteSingle(row)">{{ t('common.delete') }}</UiButton>
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

    <!-- 编辑设备基本信息弹窗（MGR-04：名称/分组/安装位置/备注） -->
    <UiDialog v-model:open="editDevDlg.show" :title="t('device.edit.title')" width="max-w-md">
      <div class="space-y-3">
        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('device.add.name') }}</label>
          <UiInput v-model="editDevDlg.name" />
        </div>
        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('device.col.group') }}</label>
          <UiSelect v-model="editDevDlg.groupId" :options="groupOptions" class="w-full" />
        </div>
        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('device.edit.location') }}</label>
          <UiInput v-model="editDevDlg.location" :placeholder="t('device.edit.locationPlaceholder')" />
        </div>
        <div>
          <label class="mb-1 block text-xs text-muted">{{ t('common.remark') }}</label>
          <UiInput v-model="editDevDlg.remark" />
        </div>
      </div>
      <template #footer>
        <UiButton @click="editDevDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" @click="saveEditDev">{{ t('common.save') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 批量搜索（MGR-02）：多行粘贴设备ID / 国标 ID / IP -->
    <UiDialog v-model:open="bulkDlg.show" :title="t('device.bulkSearch.title')" width="max-w-md">
      <p class="mb-2 text-xs text-placeholder">{{ t('device.bulkSearch.hint') }}</p>
      <textarea
        v-model="bulkDlg.text"
        rows="6"
        class="w-full rounded-chrome border border-line bg-surface p-2.5 font-mono text-xs text-body outline-none focus:border-primary"
        :placeholder="t('device.bulkSearch.placeholder')"
      />
      <template #footer>
        <UiButton v-if="query.bulk" @click="clearBulkSearch">{{ t('common.clear') }}</UiButton>
        <UiButton @click="bulkDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" @click="applyBulkSearch">{{ t('common.search') }}</UiButton>
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
