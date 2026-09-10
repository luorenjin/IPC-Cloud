<script setup lang="ts">
// 设备列表（MGR-01/02）+ 添加设备弹窗（ADD-01~08）
const api = useApi()
const route = useRoute()
const router = useRouter()
const toast = useToast()
const confirmBox = useConfirm()
const { currentProject } = useAuth()

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
    toastApiError(e, '分组加载失败')
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
  if (!groupDlg.name.trim()) return toast.warning('分组名称不能为空')
  try {
    if (groupDlg.mode === 'add') {
      await api.post('/groups', { name: groupDlg.name.trim() })
      toast.success('分组已创建')
    } else {
      await api.put(`/groups/${groupDlg.id}`, { name: groupDlg.name.trim() })
      toast.success('分组已重命名')
    }
    groupDlg.show = false
    await loadGroups()
  } catch (e: any) {
    toastApiError(e, '保存分组失败')
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
    toastApiError(e, '加载设备列表失败')
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
  { key: 'index', label: '序号' },
  { key: 'name', label: '设备名称' },
  { key: 'type', label: '设备类型' },
  { key: 'source', label: '来源' },
  { key: 'status', label: '设备状态' },
  { key: 'model', label: '设备型号' },
  { key: 'ip', label: 'IP地址' },
  { key: 'mac', label: 'MAC地址' },
  { key: 'group', label: '所属分组' },
  { key: 'location', label: '地理位置' },
  { key: 'ops', label: '操作' }
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
  } catch {}
}

const showCol = (k: string) => !hiddenCols.value.includes(k)

// ================= 批量操作（批量工具条） =================
const batchMoveDlg = reactive({ show: false, groupId: '', saving: false })

// 跨项目转移（MGR-12）：与「移动到分组」不同——后者只改 group_id，
// 这里会把设备连同其通道一起划到另一个项目下，并通知适配器（cmd.transfer）。
const xferDlg = reactive({ show: false, projectId: '', saving: false })
const xferProjects = ref<any[]>([])

async function openTransfer() {
  if (!selection.value.length) return toast.warning('请先勾选需要转移的设备')
  xferDlg.projectId = ''
  xferDlg.show = true
  if (!xferProjects.value.length) {
    try {
      const res: any = await api.get('/projects')
      // 排除当前项目：转到自己没有意义
      xferProjects.value = (res?.items || res || []).filter((p: any) => p.id !== currentProject.value?.id)
    } catch (e: any) {
      toastApiError(e, '加载项目列表失败')
    }
  }
}

async function doTransfer() {
  if (!xferDlg.projectId) return toast.warning('请选择目标项目')
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
      toast.success(`已将 ${ids.length} 台设备转移到目标项目`)
    } else if (failed.length === ids.length) {
      toast.error({ title: '转移失败', suggest: '请确认对目标项目有配置权限后重试。' })
    } else {
      toast.warning(`${ids.length - failed.length} 台转移成功，${failed.length} 台失败`)
    }
    xferDlg.show = false
    selection.value = []
    load()
  } finally {
    xferDlg.saving = false
  }
}

function openBatchMove() {
  if (!selection.value.length) return toast.warning('请先勾选需要转移的设备')
  batchMoveDlg.groupId = ''
  batchMoveDlg.show = true
}

async function doBatchMove() {
  if (!batchMoveDlg.groupId) return toast.warning('请选择目标分组')
  batchMoveDlg.saving = true
  try {
    await api.post('/devices/batch', { action: 'move', ids: selection.value, groupId: batchMoveDlg.groupId })
    toast.success('已成功转移所选设备')
    batchMoveDlg.show = false
    selection.value = []
    load()
  } catch (e: any) {
    toastApiError(e, '转移失败')
  } finally {
    batchMoveDlg.saving = false
  }
}

async function doBatchReboot() {
  if (!selection.value.length) return toast.warning('请先勾选需要重启的设备')
  const ok = await confirmBox.ask({
    title: '批量重启设备',
    message: `确定对选中的 ${selection.value.length} 台设备下发重启指令？`,
    detail: '重启期间设备视频流将短暂中断。',
    confirmText: '立即重启'
  })
  if (!ok) return
  try {
    await api.post('/devices/batch', { action: 'reboot', ids: selection.value })
    toast.success('重启指令已下发')
  } catch (e: any) {
    toastApiError(e, '重启失败')
  }
}

async function doBatchDelete() {
  if (!selection.value.length) return toast.warning('请先勾选需要删除的设备')
  const ok = await confirmBox.ask({
    title: '批量删除设备',
    message: `确定彻底删除选中的 ${selection.value.length} 台设备及其配置？`,
    detail: '该操作不可逆，请谨慎操作。',
    danger: true,
    confirmText: '确认删除'
  })
  if (!ok) return
  try {
    await api.post('/devices/batch', { action: 'delete', ids: selection.value })
    toast.success('所选设备已删除')
    selection.value = []
    load()
  } catch (e: any) {
    toastApiError(e, '删除失败')
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
    toastApiError(e, '导出失败')
  }
}

const syncing = ref(false)
async function doBatchSync() {
  if (!selection.value.length) return toast.warning('请先勾选需要同步的设备')
  syncing.value = true
  try {
    let ok = 0
    for (const id of selection.value) {
      try {
        await api.post(`/devices/${id}/sync`)
        ok++
      } catch (e: any) {
        toastApiError(e, `设备 ${id} 同步失败`)
      }
    }
    if (ok) toast.success(`已同步 ${ok} 台设备`)
    load()
  } finally {
    syncing.value = false
  }
}

// 单台设备危险删除（MGR-11：输入设备名称二次确认）
async function askDeleteSingle(row: any) {
  const ok = await confirmBox.ask({
    title: '删除设备二次确认',
    message: `将删除设备「${row.name}」及其通道、录像配置！`,
    detail: '为防止误操作，请在下方完整输入该设备名称以确认删除：',
    danger: true,
    confirmText: '确认删除',
    inputConfirm: row.name,
    inputPlaceholder: row.name
  })
  if (!ok) return
  try {
    await api.request(`/devices/${row.id}`, { method: 'DELETE', body: { confirmName: row.name } })
    toast.success('设备已成功删除')
    load()
  } catch (e: any) {
    toastApiError(e, '删除失败')
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
    toast.success('修改已保存')
    editDevDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, '保存失败')
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
    toastApiError(e, '加载国标白名单失败')
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
    if (!onvifDiscover.items.length) toast.info('未发现同网段的 ONVIF 设备')
  } catch (e: any) {
    toastApiError(e, 'ONVIF 发现失败')
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
    if (!addForm.deviceId.trim()) return toast.warning('请输入设备标贴上的设备ID')
    if (addForm.verifyCode.trim().length < 6) return toast.warning('请输入 6 位及以上验证码')
    adding.value = true
    try {
      await api.post('/devices/idp/bind', {
        deviceId: addForm.deviceId.trim().toUpperCase(),
        verifyCode: addForm.verifyCode.trim(),
        groupId: addForm.groupId || undefined,
        name: addForm.name.trim() || undefined
      })
      toast.success('设备添加成功')
      addDlg.show = false
      load()
    } catch (e: any) {
      toastApiError(e, '添加设备失败')
    } finally {
      adding.value = false
    }
  } else {
    // 批量导入：每行「设备ID,验证码」，分隔符逗号或空格
    const lines = batchIdText.value.split('\n').map((l) => l.trim()).filter(Boolean)
    if (!lines.length) return toast.warning('请在文本框中粘贴设备ID列表')
    const items: { deviceId: string; verifyCode: string }[] = []
    for (let i = 0; i < lines.length; i++) {
      const parts = lines[i].split(/[,\s]+/).filter(Boolean)
      if (parts.length < 2) return toast.warning(`第 ${i + 1} 行缺少验证码：${lines[i]}`)
      items.push({ deviceId: parts[0].toUpperCase(), verifyCode: parts[1] })
    }
    adding.value = true
    try {
      await api.post('/devices/idp/preadd', { items })
      toast.success(`已提交 ${items.length} 台设备的预添加登记`)
      addDlg.show = false
      load()
    } catch (e: any) {
      toastApiError(e, '批量导入失败')
    } finally {
      adding.value = false
    }
  }
}

async function submitOnvif() {
  if (!onvifForm.ip.trim()) return toast.warning('请输入设备 IP')
  if (!onvifForm.user.trim() || !onvifForm.pass) return toast.warning('请输入 ONVIF 账号与密码')
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
    toast.success('ONVIF 设备添加成功')
    addDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, 'ONVIF 设备添加失败')
  } finally {
    adding.value = false
  }
}

async function submitRtsp() {
  if (!rtspForm.url.trim()) return toast.warning('请输入 RTSP 地址')
  adding.value = true
  try {
    await api.post('/devices/rtsp', {
      url: rtspForm.url.trim(),
      subUrl: rtspForm.subUrl.trim() || undefined,
      groupId: addForm.groupId || undefined,
      name: rtspForm.name.trim() || undefined
    })
    toast.success('RTSP 设备添加成功')
    addDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, 'RTSP 设备添加失败')
  } finally {
    adding.value = false
  }
}

async function submitGb() {
  if (!gbForm.gbId.trim()) return toast.warning('请输入国标设备编号')
  if (!gbForm.pwd) return toast.warning('请输入接入密码')
  adding.value = true
  try {
    await api.post('/devices/gb28181/whitelist', {
      gbId: gbForm.gbId.trim(),
      pwd: gbForm.pwd,
      groupId: addForm.groupId || undefined,
      name: gbForm.name.trim() || undefined
    })
    toast.success('已登记到白名单，设备注册后将自动接入')
    Object.assign(gbForm, { gbId: '', pwd: '', name: '' })
    loadGbWhitelist()
  } catch (e: any) {
    toastApiError(e, '登记白名单失败')
  } finally {
    adding.value = false
  }
}

async function delGbWhitelist(row: any) {
  const ok = await confirm.ask({
    title: '移除白名单',
    message: `确定移除国标编号 ${row.gbId}？移除后该设备再注册将被拒绝。`,
    confirmText: '移除', danger: true
  })
  if (!ok) return
  try {
    await api.del('/devices/gb28181/whitelist/' + row.id)
    toast.success('已移除')
    loadGbWhitelist()
  } catch (e: any) {
    toastApiError(e, '移除失败')
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
        toastApiError(e, '通道列表加载失败')
        return
      }
    }
    ch = chCache[row.id]?.find((c: any) => c.enabled !== false) || null
  }
  if (!ch) {
    toast.warning('该设备暂无可预览的通道')
    return
  }
  previewModal.device = row
  previewModal.channel = ch
  previewModal.tab = tab
  previewModal.show = true
}

onMounted(async () => {
  try {
    const savedCols = localStorage.getItem('ipc_dev_hidden_cols')
    if (savedCols) hiddenCols.value = JSON.parse(savedCols)
  } catch {}
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
        <span class="text-sm font-bold text-ink">设备分组</span>
        <div class="flex items-center gap-1">
          <button type="button" class="rounded-chrome p-1 text-muted transition-colors hover:bg-zone hover:text-primary" aria-label="新增分组" title="新增分组" @click="openAddGroup">
            <Icon name="plus" :size="15" />
          </button>
          <button
            class="rounded-chrome p-1 text-muted transition-colors hover:bg-zone hover:text-primary disabled:opacity-30"
            :disabled="!selectedGroup"
            title="编辑当前分组"
            @click="openEditGroup(groups.find(g => g.id === selectedGroup))"
          >
            <Icon name="edit" :size="14" />
          </button>
        </div>
      </div>

      <!-- 搜索框 -->
      <UiInput v-model="groupSearch" placeholder="搜索分组" size="sm" class="mb-3">
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
            <Icon name="folder" :size="14" :class="!selectedGroup ? 'text-primary' : ''" />全部分组
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
              {{ stats.total }} <span class="text-xs font-normal">台</span>
            </div>
            <div class="mt-1 flex items-center gap-1.5">
              <span class="rounded-signal bg-danger-soft px-1.5 py-0.5 text-[10px] font-medium text-danger">
                离线 {{ stats.offline }}
              </span>
            </div>
          </div>
        </div>

        <!-- 卡片折叠收起把手 -->
        <div class="flex justify-center border-t border-line-soft pt-1 mt-2">
          <button type="button" class="rounded-chrome p-0.5 text-muted transition-colors hover:text-primary" :aria-label="cardCollapsed ? '展开统计卡片' : '收起统计卡片'" :aria-expanded="!cardCollapsed" @click="cardCollapsed = !cardCollapsed">
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
            :items="[{ label: '全部', value: 'all' }, { label: 'IPC', value: 'ipc' }]"
          >
            <template #extra>
              <!-- 国标待确认入口：与侧栏徽标同源（useState('gbPending')），仅有待确认时出现 -->
              <UiButton v-if="pendingCount > 0" class="ml-auto" @click="navigateTo('/devices/pending')">
                待确认(国标)
                <span class="ml-1 rounded-full bg-danger px-1.5 text-[10px] leading-4 text-white">
                  {{ pendingCount > 99 ? '99+' : pendingCount }}
                </span>
              </UiButton>
              <UiButton variant="primary" :class="pendingCount > 0 ? '' : 'ml-auto'" @click="openAddModal">
                <Icon name="plus" :size="15" />添加设备
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
                <UiButton size="sm"><Icon name="list" :size="13" />内容</UiButton>
              </template>
              <div class="w-40 space-y-0.5 p-1">
                <div v-for="c in allCols" :key="c.key" class="rounded px-1.5 py-1 hover:bg-zone">
                  <UiCheckbox :model-value="showCol(c.key)" :label="c.label" @update:model-value="toggleCol(c.key)" />
                </div>
              </div>
            </UiPopover>

            <UiButton size="sm" :disabled="!selection.length" @click="openBatchMove">移动到分组</UiButton>
            <UiButton size="sm" :disabled="!selection.length" @click="openTransfer">转移到项目</UiButton>
            <UiButton size="sm" :disabled="!selection.length" @click="doBatchReboot">重启设备</UiButton>
            <UiButton variant="dangerText" size="sm" :disabled="!selection.length" @click="doBatchDelete">删除设备</UiButton>
            <UiButton size="sm" @click="exportCsv">导出设备信息</UiButton>
            <UiButton size="sm" :disabled="!selection.length || syncing" @click="doBatchSync">设备同步</UiButton>
            <UiButton size="sm" title="刷新" @click="load"><Icon name="refresh" :size="13" /></UiButton>
          </div>

          <!-- 右侧搜索与过滤 -->
          <div class="flex items-center gap-2">
            <UiButton size="sm" @click="openAddModal">批量添加</UiButton>
            <!-- 搜索框 -->
            <UiInput v-model="query.keyword" placeholder="搜索设备名/MAC/IP" size="sm" width="w-48" @enter="search">
              <template #prefix><Icon name="search" :size="12" class="mr-1 text-placeholder" /></template>
            </UiInput>
            <!-- 筛选下拉 -->
            <UiSelect
              v-model="query.status"
              :options="[
                { label: '全部状态', value: '' },
                { label: '在线', value: 'online' },
                { label: '离线', value: 'offline' }
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
                    :aria-label="selection.length === devices.length ? '取消全选' : '全选本页设备'"
                    @update:model-value="selection = selection.length === devices.length ? [] : devices.map(d => d.id)"
                  />
                </th>
                <th v-if="showCol('index')" class="py-2.5 px-3 w-10 text-center">序号</th>
                <th v-if="showCol('name')" class="py-2.5 px-3">设备名称</th>
                <th v-if="showCol('type')" class="py-2.5 px-3">设备类型</th>
                <th v-if="showCol('source')" class="py-2.5 px-3">来源</th>
                <th v-if="showCol('status')" class="py-2.5 px-3">设备状态</th>
                <th v-if="showCol('model')" class="py-2.5 px-3">设备型号</th>
                <th v-if="showCol('ip')" class="py-2.5 px-3 text-right">IP地址</th>
                <th v-if="showCol('mac')" class="py-2.5 px-3 text-right">MAC地址</th>
                <th v-if="showCol('group')" class="py-2.5 px-3">所属分组</th>
                <th v-if="showCol('location')" class="py-2.5 px-3">地理位置</th>
                <th v-if="showCol('ops')" class="py-2.5 px-3 text-right">操作</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-line-soft">
              <template v-for="(row, idx) in devices" :key="row.id">
                <tr class="transition-colors hover:bg-primary-softer">
                  <td class="py-2.5 px-3">
                    <UiCheckbox
                      :model-value="selection.includes(row.id)"
                      :aria-label="`选择设备 ${row.name || row.id}`"
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
                        :aria-label="`${expandedRow === row.id ? '收起' : '展开'} ${row.name} 的通道列表`"
                        :aria-expanded="expandedRow === row.id"
                        title="展开通道列表"
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
                  <td v-if="showCol('source')" class="py-2.5 px-3"><UiTag :color="sourceInfo(row.source).color">{{ sourceInfo(row.source).label }}</UiTag></td>
                  <td v-if="showCol('status')" class="py-2.5 px-3">
                    <span class="flex items-center gap-1.5" :class="row.status === 'online' ? 'text-primary' : 'text-muted'">
                      <span class="h-2 w-2 rounded-full transition-colors" :class="row.status === 'online' ? 'bg-primary' : 'bg-muted'" />
                      {{ row.status === 'online' ? '在线' : '离线' }}
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
                      <UiButton variant="text" size="sm" @click="router.push(`/devices/${row.id}`)">远程配置</UiButton>
                      <UiButton variant="text" size="sm" @click="openEditDev(row)">编辑</UiButton>
                      <UiButton variant="text" size="sm" @click="openPreview(row)">预览</UiButton>
                      <UiButton variant="dangerText" size="sm" title="删除设备" @click="askDeleteSingle(row)">删除</UiButton>
                    </div>
                  </td>
                </tr>

                <!-- 行展开通道子列表（MGR-01） -->
                <tr v-if="expandedRow === row.id">
                  <td colspan="12" class="border-b border-line-soft bg-zone p-3 pl-12">
                    <div class="mb-1.5 text-xs font-semibold text-muted">通道列表：</div>
                    <div v-if="!chCache[row.id]?.length" class="text-xs text-placeholder">
                      暂无子通道数据或单通道设备
                    </div>
                    <div v-else class="flex flex-wrap gap-2">
                      <div
                        v-for="ch in chCache[row.id]"
                        :key="ch.id"
                        class="flex items-center gap-2 rounded-signal border border-line bg-surface px-2.5 py-1 text-xs"
                      >
                        <span class="h-1.5 w-1.5 rounded-full" :class="ch.enabled !== false ? 'bg-primary' : 'bg-line'" />
                        <span class="font-medium text-ink">{{ ch.name || `通道 ${ch.idx}` }}</span>
                        <UiButton variant="text" size="sm" class="ml-1" @click="openPreview(row, ch)">预览</UiButton>
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
            共计 <span class="font-bold text-ink">{{ total }}</span> 条
            第 <span class="font-bold text-ink">{{ query.page }}</span>/{{ Math.ceil(total / query.pageSize) || 1 }} 页
            已选 <span class="font-bold text-primary">{{ selection.length }}</span>
          </div>

          <div class="flex items-center gap-2">
            <UiSelect
              v-model="query.pageSize"
              :options="[
                { label: '20条/页', value: 20 },
                { label: '50条/页', value: 50 },
                { label: '100条/页', value: 100 }
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
    <UiDialog v-model:open="addDlg.show" title="添加设备" width="max-w-2xl">
      <!-- 四种接入方式，与 device.source 的四个判别值一一对应 -->
      <UiTabs
        v-model="addTab"
        :items="[
          { label: '自有设备', value: 'idp' },
          { label: 'ONVIF', value: 'onvif' },
          { label: 'RTSP', value: 'rtsp' },
          { label: '国标 GB/T 28181', value: 'gb28181' }
        ]"
      />

      <!-- 所属分组：四种方式共用 -->
      <div class="mt-4 flex items-center gap-3">
        <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">所属分组</label>
        <div class="flex flex-1 items-center gap-2 text-sm">
          <span class="font-medium text-ink">{{ groupMap[addForm.groupId] || '未分组' }}</span>
          <button type="button" class="rounded-chrome text-primary hover:text-primary-deep" aria-label="修改所属分组" title="修改所属分组" @click="openAddGroup">
            <Icon name="edit" :size="13" />
          </button>
        </div>
      </div>

      <!-- ---------- 自有设备（IDP）---------- -->
      <template v-if="addTab === 'idp'">
        <UiSegmented
          v-model="addSubTab"
          class="mt-4"
          :items="[{ label: '单台添加', value: 'single' }, { label: '批量登记', value: 'batch' }]"
        />

        <div v-if="addSubTab === 'single'" class="mx-auto mt-5 max-w-md space-y-4">
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> 设备ID</label>
            <UiInput v-model="addForm.deviceId" class="flex-1 uppercase" placeholder="设备标贴上的设备ID，不区分大小写" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> 验证码</label>
            <UiInput v-model="addForm.verifyCode" class="flex-1" placeholder="设备标贴上的 6 位及以上验证码" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">设备名称</label>
            <UiInput v-model="addForm.name" class="flex-1" placeholder="选填，留空则用设备型号自动命名" />
          </div>
          <div class="flex flex-col items-center pt-2">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">添加设备</UiButton>
            <button type="button" class="mt-3 text-xs text-primary hover:underline" @click="router.push('/scan')">
              用手机扫码录入
            </button>
          </div>
        </div>

        <div v-else class="mx-auto mt-5 max-w-md space-y-4">
          <div>
            <label class="mb-1 block text-xs font-medium text-muted">
              <span class="text-danger">*</span> 设备列表（每行一台，格式「设备ID,验证码」）
            </label>
            <textarea
              v-model="batchIdText"
              rows="6"
              class="w-full rounded-chrome border border-line bg-surface p-2.5 font-mono text-xs text-body outline-none focus:border-primary"
              placeholder="A1B2C3D4E5F678901,123456&#10;B2C3D4E5F67890123,654321"
            />
            <p class="mt-1 text-xs text-placeholder">
              登记后设备上电联网即自动接入，无需逐台操作。
            </p>
          </div>
          <div class="flex flex-col items-center pt-1">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">提交登记</UiButton>
          </div>
        </div>
      </template>

      <!-- ---------- ONVIF ---------- -->
      <template v-else-if="addTab === 'onvif'">
        <div class="mt-4 flex items-center justify-between gap-3 rounded-signal border border-line bg-canvas px-3 py-2">
          <p class="text-xs text-muted">扫描本网段内的 ONVIF 设备（约 10 秒）</p>
          <UiButton size="sm" :loading="onvifDiscover.loading" @click="doOnvifDiscover">搜索设备</UiButton>
        </div>

        <div v-if="onvifDiscover.items.length" class="mt-2 max-h-40 overflow-auto rounded-signal border border-line">
          <table class="w-full text-xs">
            <tbody>
              <tr v-for="it in onvifDiscover.items" :key="it.xaddr" class="border-b border-line-soft last:border-0">
                <td class="px-3 py-2 font-mono text-body">{{ it.ip }}</td>
                <td class="px-3 py-2 text-placeholder">{{ (it.scopes || []).join(' ') || '—' }}</td>
                <td class="px-3 py-2 text-right">
                  <UiTag v-if="it.added" color="info">已添加</UiTag>
                  <UiButton v-else variant="text" size="sm" @click="pickDiscovered(it)">选择</UiButton>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
        <p v-else-if="onvifDiscover.done" class="mt-2 text-xs text-placeholder">
          未发现设备。设备与平台需在同一网段，且已开启 ONVIF；也可在下方直接填写 IP 添加。
        </p>

        <div class="mx-auto mt-5 max-w-md space-y-4">
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> 设备 IP</label>
            <UiInput v-model="onvifForm.ip" class="flex-1" placeholder="192.168.1.64" />
            <UiInput v-model="onvifForm.port" width="w-20" placeholder="80" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> 账号</label>
            <UiInput v-model="onvifForm.user" class="flex-1" placeholder="ONVIF 账号（通常与 Web 登录一致）" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> 密码</label>
            <UiInput v-model="onvifForm.pass" type="password" class="flex-1" placeholder="ONVIF 密码" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">设备名称</label>
            <UiInput v-model="onvifForm.name" class="flex-1" placeholder="选填" />
          </div>
          <div class="flex flex-col items-center pt-2">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">添加设备</UiButton>
          </div>
        </div>
      </template>

      <!-- ---------- RTSP ---------- -->
      <template v-else-if="addTab === 'rtsp'">
        <div class="mx-auto mt-5 max-w-md space-y-4">
          <div class="flex items-start gap-3">
            <label class="w-24 shrink-0 pt-1.5 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> 主码流地址</label>
            <div class="flex-1">
              <UiInput v-model="rtspForm.url" placeholder="rtsp://user:pass@192.168.1.64:554/Streaming/Channels/101" />
              <p class="mt-1 text-xs text-placeholder">账号密码写在地址中；添加前平台会先探测该地址是否可达。</p>
            </div>
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">子码流地址</label>
            <UiInput v-model="rtspForm.subUrl" class="flex-1" placeholder="选填，用于多画面预览省带宽" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">设备名称</label>
            <UiInput v-model="rtspForm.name" class="flex-1" placeholder="选填" />
          </div>
          <div class="flex flex-col items-center pt-2">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">添加设备</UiButton>
          </div>
        </div>
      </template>

      <!-- ---------- 国标白名单 ---------- -->
      <template v-else>
        <p class="mt-4 rounded-signal bg-zone p-3 text-xs leading-relaxed text-muted">
          国标设备由设备侧主动向平台注册。请先在此登记设备编号与接入密码，
          并在设备上填写平台的 SIP 服务器信息（见 系统设置 → 国标参数）。
        </p>

        <div class="mx-auto mt-4 max-w-md space-y-4">
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> 设备编号</label>
            <UiInput v-model="gbForm.gbId" class="flex-1 font-mono" placeholder="20 位国标编号，如 34020000001320000001" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted"><span class="text-danger">*</span> 接入密码</label>
            <UiInput v-model="gbForm.pwd" type="password" class="flex-1" placeholder="需与设备端 SIP 注册密码一致" />
          </div>
          <div class="flex items-center gap-3">
            <label class="w-24 shrink-0 text-right text-xs font-medium text-muted">设备名称</label>
            <UiInput v-model="gbForm.name" class="flex-1" placeholder="选填" />
          </div>
          <div class="flex flex-col items-center pt-1">
            <UiButton variant="primary" size="lg" class="w-64" :loading="adding" @click="submitAdd">登记到白名单</UiButton>
          </div>
        </div>

        <div class="mt-5">
          <p class="mb-2 text-xs font-medium text-muted">已登记的编号</p>
          <div v-if="gbWhitelist.loading" class="py-4 text-center"><Icon name="refresh" :size="16" class="ipc-spin text-primary" /></div>
          <p v-else-if="!gbWhitelist.items.length" class="py-3 text-center text-xs text-placeholder">暂无登记，登记后设备注册才会被接受。</p>
          <div v-else class="max-h-40 overflow-auto rounded-signal border border-line">
            <table class="w-full text-xs">
              <tbody>
                <tr v-for="w in gbWhitelist.items" :key="w.id" class="border-b border-line-soft last:border-0">
                  <td class="px-3 py-2 font-mono text-body">{{ w.gbId }}</td>
                  <td class="px-3 py-2 text-placeholder">{{ dash(w.name) }}</td>
                  <td class="px-3 py-2 text-right">
                    <UiButton variant="dangerText" size="sm" @click="delGbWhitelist(w)">移除</UiButton>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </template>
    </UiDialog>

    <!-- 分组管理弹窗 -->
    <UiDialog v-model:open="groupDlg.show" :title="groupDlg.mode === 'add' ? '新增设备分组' : '重命名分组'" width="max-w-sm">
      <div class="space-y-3">
        <label class="block text-xs text-muted">分组名称 *</label>
        <UiInput v-model="groupDlg.name" placeholder="请输入分组名称" />
      </div>
      <template #footer>
        <UiButton @click="groupDlg.show = false">取消</UiButton>
        <UiButton variant="primary" @click="saveGroup">确定</UiButton>
      </template>
    </UiDialog>

    <!-- 批量转移分组弹窗 -->
    <!-- 跨项目转移（MGR-12） -->
    <UiDialog v-model:open="xferDlg.show" title="转移到其他项目" width="max-w-sm">
      <div class="space-y-3 p-1">
        <p class="text-xs text-placeholder">
          将选中的 {{ selection.length }} 台设备连同其通道一并划归目标项目。
          转移后本项目将不再看到这些设备，其录像计划与告警规则需在新项目中重新配置。
        </p>
        <div>
          <label class="mb-1 block text-xs font-medium text-muted">目标项目</label>
          <UiSelect v-model="xferDlg.projectId" :options="xferProjects.map((p: any) => ({ label: p.name, value: p.id }))" placeholder="选择目标项目" class="w-full" />
          <p v-if="!xferProjects.length" class="mt-1 text-xs text-placeholder">没有其他可选项目。</p>
        </div>
      </div>
      <template #footer>
        <UiButton size="sm" @click="xferDlg.show = false">取消</UiButton>
        <UiButton variant="primary" size="sm" :loading="xferDlg.saving" :disabled="!xferDlg.projectId" @click="doTransfer">确定转移</UiButton>
      </template>
    </UiDialog>

    <UiDialog v-model:open="batchMoveDlg.show" title="移动到分组" width="max-w-sm">
      <div class="space-y-3">
        <label class="block text-xs text-muted">选择目标分组</label>
        <UiSelect v-model="batchMoveDlg.groupId" :options="groupOptions" class="w-full" />
      </div>
      <template #footer>
        <UiButton @click="batchMoveDlg.show = false">取消</UiButton>
        <UiButton variant="primary" :loading="batchMoveDlg.saving" @click="doBatchMove">确定转移</UiButton>
      </template>
    </UiDialog>

    <!-- 编辑设备基本信息弹窗 -->
    <UiDialog v-model:open="editDevDlg.show" title="编辑设备" width="max-w-md">
      <div class="space-y-3">
        <div>
          <label class="mb-1 block text-xs text-muted">设备名称</label>
          <UiInput v-model="editDevDlg.name" />
        </div>
        <div>
          <label class="mb-1 block text-xs text-muted">地理位置</label>
          <UiInput v-model="editDevDlg.location" placeholder="如：深圳总部A座一楼" />
        </div>
      </div>
      <template #footer>
        <UiButton @click="editDevDlg.show = false">取消</UiButton>
        <UiButton variant="primary" @click="saveEditDev">保存</UiButton>
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
