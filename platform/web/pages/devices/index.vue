<script setup lang="ts">
// 设备列表（MGR-01/02）+ 添加设备弹窗（ADD-01~08）：严格 100% 对齐图 4 与图 5
const api = useApi()
const route = useRoute()
const router = useRouter()
const toast = useToast()
const confirmBox = useConfirm()
const { currentProject } = useAuth()

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

// 统计数量（对齐图 4 顶部卡片）
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

// ================= 列配置（图 4: [=] 内容按钮） =================
const allCols = [
  { key: 'index', label: '序号' },
  { key: 'name', label: '设备名称' },
  { key: 'type', label: '设备类型' },
  { key: 'status', label: '设备状态' },
  { key: 'model', label: '设备型号' },
  { key: 'ip', label: 'IP地址' },
  { key: 'mac', label: 'MAC地址' },
  { key: 'group', label: '所属分组' },
  { key: 'location', label: '地理位置' },
  { key: 'duration', label: '状态持续时长' },
  { key: 'pinned', label: '置顶' },
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

// ================= 批量操作（图 4 批量工具条） =================
const batchMoveDlg = reactive({ show: false, groupId: '', saving: false })

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

function exportCsv() {
  window.open('/api/v1/devices/export?ids=' + selection.value.join(','), '_blank')
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

// ================= 【5. 添加设备弹窗】（严格对齐图 5） =================
const addDlg = reactive({ show: false })
const addTab = ref('idp') // idp (设备ID添加) | gb (国标) | onvif | app | pwd | other
const addSubTab = ref<'single' | 'batch'>('single') // 单台添加 | 批量导入ID

const addForm = reactive({
  groupId: '',
  deviceId: '',
  name: '',
  verifyCode: '123456'
})

const batchIdText = ref('')
const adding = ref(false)

function openAddModal() {
  addForm.deviceId = ''
  addForm.name = ''
  addForm.groupId = groups.value[0]?.id || ''
  batchIdText.value = ''
  addSubTab.value = 'single'
  addTab.value = 'idp'
  addDlg.show = true
}

async function submitAdd() {
  if (addSubTab.value === 'single') {
    if (!addForm.deviceId.trim()) return toast.warning('请输入设备标贴上的设备ID')
    adding.value = true
    try {
      await api.post('/devices', {
        source: 'idp',
        name: addForm.name.trim() || addForm.deviceId.trim().toUpperCase(),
        groupId: addForm.groupId || undefined,
        credentials: {
          deviceId: addForm.deviceId.trim().toUpperCase(),
          verifyCode: addForm.verifyCode || '123456'
        }
      })
      toast.success('设备添加成功！')
      addDlg.show = false
      load()
    } catch (e: any) {
      toastApiError(e, '添加设备失败')
    } finally {
      adding.value = false
    }
  } else {
    // 批量导入
    const lines = batchIdText.value.split('\n').map((l) => l.trim()).filter(Boolean)
    if (!lines.length) return toast.warning('请在文本框中粘贴设备ID列表')
    adding.value = true
    try {
      const items = lines.map((id) => ({ deviceId: id.toUpperCase(), verifyCode: '123456' }))
      await api.post('/devices/idp/preadd', { items })
      toast.success(`已提交 ${items.length} 台设备的批量导入任务`)
      addDlg.show = false
      load()
    } catch (e: any) {
      toastApiError(e, '批量导入失败')
    } finally {
      adding.value = false
    }
  }
}

// ================= 预览与回放弹窗（LIVE-01 + REC-01） =================
const previewModal = reactive({
  show: false,
  device: null as any,
  channel: null as any,
  tab: 'preview' as 'preview' | 'playback'
})

function openPreview(row: any, channel?: any, tab: 'preview' | 'playback' = 'preview') {
  previewModal.device = row
  previewModal.channel = channel || null
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
    <!-- 左侧：设备分组（严格对齐图 4） -->
    <div class="w-60 shrink-0 rounded-lg border border-[#e5e6eb] bg-white p-3 shadow-sm">
      <div class="mb-3 flex items-center justify-between">
        <span class="text-sm font-bold text-[#1f2329]">设备分组</span>
        <div class="flex items-center gap-1">
          <button class="p-1 text-[#86909c] hover:text-[#1785E6]" title="新增分组" @click="openAddGroup">
            <Icon name="plus" :size="15" />
          </button>
          <button
            class="p-1 text-[#86909c] hover:text-[#1785E6] disabled:opacity-30"
            :disabled="!selectedGroup"
            title="编辑当前分组"
            @click="openEditGroup(groups.find(g => g.id === selectedGroup))"
          >
            <Icon name="edit" :size="14" />
          </button>
        </div>
      </div>

      <!-- 搜索框 -->
      <div class="mb-3 flex h-8 items-center rounded border border-[#e5e6eb] bg-[#f7f8fa] px-2 text-xs">
        <Icon name="search" :size="13" class="mr-1.5 text-[#86909c]" />
        <input
          v-model="groupSearch"
          type="text"
          placeholder="搜索分组"
          class="w-full bg-transparent outline-none placeholder:text-[#86909c]"
        />
      </div>

      <!-- 分组树列表 -->
      <div class="space-y-1">
        <div
          class="flex cursor-pointer items-center justify-between rounded px-2.5 py-1.5 text-xs transition-colors"
          :class="!selectedGroup ? 'bg-[#e8f3fc] font-bold text-[#1785E6]' : 'text-[#4e5969] hover:bg-[#f2f3f5]'"
          @click="selectedGroup = ''; search()"
        >
          <span class="flex items-center gap-1.5">
            <Icon name="folder" :size="14" />全部分组
          </span>
          <span class="text-[11px] text-[#86909c]">({{ stats.total }})</span>
        </div>

        <div
          v-for="g in groups.filter(x => !groupSearch || x.name.includes(groupSearch))"
          :key="g.id"
          class="flex cursor-pointer items-center justify-between rounded px-2.5 py-1.5 text-xs transition-colors"
          :class="selectedGroup === g.id ? 'bg-[#e8f3fc] font-bold text-[#1785E6]' : 'text-[#4e5969] hover:bg-[#f2f3f5]'"
          @click="selectedGroup = g.id; search()"
        >
          <span class="flex items-center gap-1.5 truncate">
            <Icon name="chevron-down" :size="11" class="text-[#86909c]" />
            <Icon name="video" :size="13" />{{ g.name }}
          </span>
          <span class="text-[11px] text-[#86909c]">(0/1)</span>
        </div>
      </div>
    </div>

    <!-- 右侧：主体区 -->
    <div class="min-w-0 flex-1 space-y-3">
      <!-- 顶部设备状态卡片（图 4：IPC 2台，离线 2，可折叠） -->
      <div class="relative rounded-lg border border-[#e5e6eb] bg-white p-4 shadow-sm">
        <div v-show="!cardCollapsed" class="flex items-center gap-4">
          <div class="flex h-12 w-12 items-center justify-center rounded-lg bg-[#f2f3f5] text-[#4e5969]">
            <Icon name="video" :size="28" />
          </div>
          <div>
            <div class="text-xs text-[#86909c]">IPC</div>
            <div class="mt-0.5 text-xl font-bold text-[#1f2329]">
              {{ stats.total }} <span class="text-xs font-normal">台</span>
            </div>
            <div class="mt-1 flex items-center gap-1.5">
              <span class="rounded bg-[#fef0ef] px-1.5 py-0.5 text-[10px] font-medium text-[#f53f3f]">
                离线 {{ stats.offline }}
              </span>
            </div>
          </div>
        </div>

        <!-- 卡片折叠收起把手（对齐图 4 居中 ^ 按钮） -->
        <div class="flex justify-center border-t border-[#f2f3f5] pt-1 mt-2">
          <button class="text-[#86909c] hover:text-[#1785E6] p-0.5" @click="cardCollapsed = !cardCollapsed">
            <Icon :name="cardCollapsed ? 'chevron-down' : 'chevron-up'" :size="15" />
          </button>
        </div>
      </div>

      <!-- 表格主体卡片 -->
      <div class="rounded-lg border border-[#e5e6eb] bg-white p-4 shadow-sm">
        <!-- 主工具栏行：全部 | IPC Tab 与 添加设备按钮（严格对齐图 4） -->
        <div class="mb-3 flex items-center justify-between border-b border-[#f2f3f5] pb-3">
          <div class="flex items-center gap-6 text-sm font-medium">
            <button
              class="relative pb-1.5 transition-colors"
              :class="query.tab === 'all' ? 'text-[#1785E6] font-bold border-b-2 border-[#1785E6]' : 'text-[#4e5969] hover:text-[#1f2329]'"
              @click="query.tab = 'all'"
            >
              全部
            </button>
            <button
              class="relative pb-1.5 transition-colors"
              :class="query.tab === 'ipc' ? 'text-[#1785E6] font-bold border-b-2 border-[#1785E6]' : 'text-[#4e5969] hover:text-[#1f2329]'"
              @click="query.tab = 'ipc'"
            >
              IPC
            </button>
          </div>

          <!-- 右侧高亮大按钮：+ 添加设备 -->
          <button
            class="flex items-center gap-1.5 rounded bg-[#1785E6] px-4 py-1.5 text-sm font-medium text-white shadow-sm hover:bg-[#0a6bcc] transition-colors"
            @click="openAddModal"
          >
            <Icon name="plus" :size="15" />添加设备
          </button>
        </div>

        <!-- 批量操作与列配置条（严格对齐图 4 功能按钮阵列） -->
        <div class="mb-3 flex flex-wrap items-center justify-between gap-2 text-xs">
          <!-- 左侧操作按钮群 -->
          <div class="flex flex-wrap items-center gap-1.5">
            <!-- [=] 内容 列配置按钮 -->
            <UiPopover>
              <template #trigger>
                <button class="flex items-center gap-1 rounded border border-[#e5e6eb] bg-white px-2 py-1 text-[#4e5969] hover:border-[#1785E6] hover:text-[#1785E6]">
                  <Icon name="list" :size="13" />内容
                </button>
              </template>
              <div class="w-40 space-y-1 p-1">
                <div
                  v-for="c in allCols"
                  :key="c.key"
                  class="flex cursor-pointer items-center gap-2 rounded px-2 py-1 text-xs hover:bg-[#e8f3fc]"
                  @click="toggleCol(c.key)"
                >
                  <input type="checkbox" :checked="showCol(c.key)" class="h-3 w-3 rounded text-[#1785E6]" />
                  <span>{{ c.label }}</span>
                </div>
              </div>
            </UiPopover>

            <button class="rounded border border-[#e5e6eb] bg-white px-2 py-1 text-[#4e5969] hover:border-[#1785E6]">网络设置配置</button>
            <button
              class="rounded border border-[#e5e6eb] bg-white px-2 py-1 text-[#4e5969] hover:border-[#1785E6] disabled:opacity-40"
              :disabled="!selection.length"
              @click="openBatchMove"
            >
              设备转移
            </button>
            <button
              class="rounded border border-[#e5e6eb] bg-white px-2 py-1 text-[#4e5969] hover:border-[#1785E6] disabled:opacity-40"
              :disabled="!selection.length"
              @click="doBatchReboot"
            >
              重启设备
            </button>
            <button
              class="rounded border border-[#e5e6eb] bg-white px-2 py-1 text-[#f53f3f] hover:bg-[#fef0ef] disabled:opacity-40"
              :disabled="!selection.length"
              @click="doBatchDelete"
            >
              删除设备
            </button>
            <button class="rounded border border-[#e5e6eb] bg-white px-2 py-1 text-[#4e5969] hover:border-[#1785E6]">设置地理位置</button>
            <button class="rounded border border-[#e5e6eb] bg-white px-2 py-1 text-[#4e5969] hover:border-[#1785E6]" @click="exportCsv">导出设备信息</button>
            <button class="rounded border border-[#e5e6eb] bg-white px-2 py-1 text-[#4e5969] hover:border-[#1785E6]" @click="load">设备同步</button>
            <button class="rounded border border-[#e5e6eb] p-1 text-[#4e5969] hover:text-[#1785E6]" title="刷新" @click="load">
              <Icon name="refresh" :size="13" />
            </button>
          </div>

          <!-- 右侧搜索与过滤 -->
          <div class="flex items-center gap-2">
            <button class="rounded border border-[#e5e6eb] bg-white px-2.5 py-1 text-[#4e5969] hover:border-[#1785E6]">设备升级</button>
            <button class="rounded border border-[#e5e6eb] bg-white px-2.5 py-1 text-[#4e5969] hover:border-[#1785E6]" @click="openAddModal">批量添加</button>
            <!-- 搜索框 -->
            <div class="relative flex h-7 w-48 items-center rounded border border-[#e5e6eb] bg-white px-2 focus-within:border-[#1785E6]">
              <Icon name="search" :size="12" class="mr-1 text-[#86909c]" />
              <input
                v-model="query.keyword"
                type="text"
                class="w-full bg-transparent text-xs outline-none placeholder:text-[#86909c]"
                placeholder="搜索设备名/MAC/IP"
                @enter="search"
              />
            </div>
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

        <!-- 表格（对齐图 4 表格所有列字段） -->
        <div class="overflow-x-auto border border-[#e5e6eb] rounded">
          <table class="w-full text-left text-xs text-[#1f2329]">
            <thead class="bg-[#f7f8fa] text-[#4e5969] border-b border-[#e5e6eb]">
              <tr>
                <th class="py-2.5 px-3 w-8">
                  <input
                    type="checkbox"
                    :checked="selection.length > 0 && selection.length === devices.length"
                    class="h-3.5 w-3.5 rounded text-[#1785E6]"
                    @change="selection = selection.length === devices.length ? [] : devices.map(d => d.id)"
                  />
                </th>
                <th v-if="showCol('index')" class="py-2.5 px-3 w-10 text-center">序号</th>
                <th v-if="showCol('name')" class="py-2.5 px-3">设备名称</th>
                <th v-if="showCol('type')" class="py-2.5 px-3">设备类型</th>
                <th v-if="showCol('status')" class="py-2.5 px-3">设备状态</th>
                <th v-if="showCol('model')" class="py-2.5 px-3">设备型号</th>
                <th v-if="showCol('ip')" class="py-2.5 px-3">IP地址</th>
                <th v-if="showCol('mac')" class="py-2.5 px-3">MAC地址</th>
                <th v-if="showCol('group')" class="py-2.5 px-3">所属分组</th>
                <th v-if="showCol('location')" class="py-2.5 px-3">地理位置</th>
                <th v-if="showCol('duration')" class="py-2.5 px-3">状态持续时长</th>
                <th v-if="showCol('pinned')" class="py-2.5 px-3 text-center">置顶</th>
                <th v-if="showCol('ops')" class="py-2.5 px-3 text-right">操作</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-[#e5e6eb]">
              <template v-for="(row, idx) in devices" :key="row.id">
                <tr class="hover:bg-[#f7f9fc] transition-colors">
                  <td class="py-2.5 px-3">
                    <input
                      type="checkbox"
                      :checked="selection.includes(row.id)"
                      class="h-3.5 w-3.5 rounded text-[#1785E6]"
                      @change="selection.includes(row.id) ? selection = selection.filter(id => id !== row.id) : selection.push(row.id)"
                    />
                  </td>
                  <td v-if="showCol('index')" class="py-2.5 px-3 text-center text-[#86909c]">{{ idx + 1 }}</td>
                  <td v-if="showCol('name')" class="py-2.5 px-3">
                    <div class="flex items-center gap-1.5 text-left">
                      <button
                        type="button"
                        class="rounded p-0.5 text-placeholder hover:bg-zone"
                        :class="expandedRow === row.id ? 'rotate-90 text-[#1785E6]' : 'text-[#86909c]'"
                        style="transition: transform .15s"
                        title="展开通道列表"
                        @click.stop="toggleExpand(row)"
                      >
                        <Icon name="chevron-right" :size="12" />
                      </button>
                      <button
                        type="button"
                        class="font-medium text-[#1785E6] hover:underline truncate max-w-[160px] text-left"
                        @click="router.push(`/devices/${row.id}`)"
                      >
                        {{ row.name }}
                      </button>
                    </div>
                  </td>
                  <td v-if="showCol('type')" class="py-2.5 px-3">{{ row.type || 'IPC' }}</td>
                  <td v-if="showCol('status')" class="py-2.5 px-3">
                    <span class="flex items-center gap-1.5" :class="row.status === 'online' ? 'text-[#00b578]' : 'text-[#f53f3f]'">
                      <span class="h-2 w-2 rounded-full" :class="row.status === 'online' ? 'bg-[#00b578]' : 'bg-[#f53f3f]'" />
                      {{ row.status === 'online' ? '在线' : '离线' }}
                    </span>
                  </td>
                  <td v-if="showCol('model')" class="py-2.5 px-3 text-[#4e5969]">{{ row.model || 'TL-IPC455E-AI4' }}</td>
                  <td v-if="showCol('ip')" class="py-2.5 px-3 font-mono text-[#4e5969]">{{ row.ip || '192.168.1.53' }}</td>
                  <td v-if="showCol('mac')" class="py-2.5 px-3 font-mono text-[#4e5969]">{{ row.mac || '4C-10-D5-85-3B-FB' }}</td>
                  <td v-if="showCol('group')" class="py-2.5 px-3">{{ groupMap[row.groupId] || '默认' }}</td>
                  <td v-if="showCol('location')" class="py-2.5 px-3 text-[#86909c]">{{ row.location || '---' }}</td>
                  <td v-if="showCol('duration')" class="py-2.5 px-3 text-[#86909c]">---</td>
                  <td v-if="showCol('pinned')" class="py-2.5 px-3 text-center text-[#86909c]">否</td>
                  <!-- 操作列（对齐图 4：远程配置、编辑、预览） -->
                  <td v-if="showCol('ops')" class="py-2.5 px-3 text-right">
                    <div class="flex items-center justify-end gap-2.5">
                      <button
                        class="text-[#1785E6] hover:underline"
                        @click="router.push(`/devices/${row.id}`)"
                      >
                        远程配置
                      </button>
                      <button
                        class="text-[#1785E6] hover:underline"
                        @click="openEditDev(row)"
                      >
                        编辑
                      </button>
                      <button
                        class="text-[#1785E6] hover:underline"
                        @click="openPreview(row)"
                      >
                        预览
                      </button>
                      <button
                        class="text-[#f53f3f] hover:underline ml-1"
                        title="删除设备"
                        @click="askDeleteSingle(row)"
                      >
                        删除
                      </button>
                    </div>
                  </td>
                </tr>

                <!-- 行展开通道子列表（图 4 / MGR-01） -->
                <tr v-if="expandedRow === row.id" class="bg-[#f9fafc]">
                  <td colspan="13" class="p-3 pl-12 border-b border-[#e5e6eb]">
                    <div class="text-xs font-semibold text-[#4e5969] mb-1.5">通道列表：</div>
                    <div v-if="!chCache[row.id]?.length" class="text-xs text-[#86909c]">
                      暂无子通道数据或单通道设备
                    </div>
                    <div v-else class="flex flex-wrap gap-2">
                      <div
                        v-for="ch in chCache[row.id]"
                        :key="ch.id"
                        class="flex items-center gap-2 rounded border border-[#e5e6eb] bg-white px-2.5 py-1 text-xs"
                      >
                        <span class="h-1.5 w-1.5 rounded-full" :class="ch.enabled !== false ? 'bg-[#00b578]' : 'bg-[#c9cdd4]'" />
                        <span class="font-medium text-[#1f2329]">{{ ch.name || `通道 ${ch.idx}` }}</span>
                        <button
                          class="text-[#1785E6] hover:underline ml-1"
                          @click="openPreview(row, ch)"
                        >
                          预览
                        </button>
                      </div>
                    </div>
                  </td>
                </tr>
              </template>
            </tbody>
          </table>
        </div>

        <!-- 底部分页栏（严格对齐图 4） -->
        <div class="mt-4 flex flex-wrap items-center justify-between gap-4 text-xs text-[#4e5969]">
          <div>
            共计 <span class="font-bold text-[#1f2329]">{{ total }}</span> 条
            第 <span class="font-bold text-[#1f2329]">{{ query.page }}</span>/{{ Math.ceil(total / query.pageSize) || 1 }} 页
            已选 <span class="font-bold text-[#1785E6]">{{ selection.length }}</span>
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
            <button
              class="h-7 w-7 rounded border border-[#e5e6eb] text-center hover:border-[#1785E6] disabled:opacity-30"
              :disabled="query.page <= 1"
              @click="query.page--; load()"
            >
              &lt;
            </button>
            <span class="px-2 py-0.5 font-bold text-[#1785E6] bg-[#e8f3fc] rounded">{{ query.page }}</span>
            <button
              class="h-7 w-7 rounded border border-[#e5e6eb] text-center hover:border-[#1785E6] disabled:opacity-30"
              :disabled="query.page >= Math.ceil(total / query.pageSize)"
              @click="query.page++; load()"
            >
              &gt;
            </button>
          </div>
        </div>
      </div>
    </div>

    <!-- ================= 【5. 添加设备弹窗】（100% 严格对齐图 5） ================= -->
    <UiDialog v-model:open="addDlg.show" title="添加设备" width="max-w-2xl">
      <!-- 顶部 Tab 栏（对齐图 5） -->
      <div class="border-b border-[#e5e6eb]">
        <div class="flex items-center gap-6 text-sm font-medium text-[#4e5969]">
          <button
            class="relative pb-2.5 transition-colors"
            :class="addTab === 'idp' ? 'text-[#1785E6] font-bold border-b-2 border-[#1785E6]' : 'hover:text-[#1f2329]'"
            @click="addTab = 'idp'"
          >
            设备ID添加
          </button>
          <button
            class="relative pb-2.5 transition-colors"
            :class="addTab === 'mac' ? 'text-[#1785E6] font-bold border-b-2 border-[#1785E6]' : 'hover:text-[#1f2329]'"
            @click="addTab = 'mac'"
          >
            MAC地址添加
          </button>
          <button
            class="relative pb-2.5 transition-colors"
            :class="addTab === 'smart' ? 'text-[#1785E6] font-bold border-b-2 border-[#1785E6]' : 'hover:text-[#1f2329]'"
            @click="addTab = 'smart'"
          >
            智能配置添加
          </button>
          <button
            class="relative pb-2.5 transition-colors"
            :class="addTab === 'app' ? 'text-[#1785E6] font-bold border-b-2 border-[#1785E6]' : 'hover:text-[#1f2329]'"
            @click="addTab = 'app'"
          >
            来自物联APP的设备
          </button>
          <button
            class="relative pb-2.5 transition-colors"
            :class="addTab === 'pwd' ? 'text-[#1785E6] font-bold border-b-2 border-[#1785E6]' : 'hover:text-[#1f2329]'"
            @click="addTab = 'pwd'"
          >
            设备密码添加
          </button>
          <button
            class="relative pb-2.5 transition-colors"
            :class="addTab === 'other' ? 'text-[#1785E6] font-bold border-b-2 border-[#1785E6]' : 'hover:text-[#1f2329]'"
            @click="addTab = 'other'"
          >
            其他添加方式
          </button>
        </div>
      </div>

      <!-- 固定前置提示条（对齐图 5 顶部灰色背景小字） -->
      <div class="mt-3 rounded bg-[#f7f8fa] p-3 text-xs text-[#86909c] leading-relaxed">
        <div>请确认要添加的设备都已接入互联网，再进行添加操作。</div>
        <div class="mt-0.5 text-[#c9cdd4]">* 添加AC设备后系统会自动识别并添加关联的FIT AP，无需再次添加FIT AP设备。</div>
      </div>

      <!-- 二级子 Tab 分段按钮（单台添加 | 批量导入ID，对齐图 5） -->
      <div class="mt-4 flex items-center gap-2">
        <button
          class="rounded px-3 py-1.5 text-xs font-medium transition-colors border"
          :class="addSubTab === 'single' ? 'bg-[#e8f3fc] border-[#1785E6] text-[#1785E6]' : 'border-[#e5e6eb] bg-white text-[#4e5969] hover:border-[#1785E6]'"
          @click="addSubTab = 'single'"
        >
          单台添加
        </button>
        <button
          class="rounded px-3 py-1.5 text-xs font-medium transition-colors border"
          :class="addSubTab === 'batch' ? 'bg-[#e8f3fc] border-[#1785E6] text-[#1785E6]' : 'border-[#e5e6eb] bg-white text-[#4e5969] hover:border-[#1785E6]'"
          @click="addSubTab = 'batch'"
        >
          批量导入ID
        </button>
      </div>

      <!-- 单台添加表单（严格对齐图 5） -->
      <div v-if="addSubTab === 'single'" class="mt-6 space-y-4 max-w-md mx-auto">
        <!-- 所属分组 -->
        <div class="flex items-center gap-3">
          <label class="w-24 text-right text-xs font-medium text-[#4e5969]"><span class="text-[#f53f3f]">*</span> 所属分组</label>
          <div class="flex items-center gap-2 flex-1 text-sm">
            <span class="font-bold text-[#1f2329]">{{ groupMap[addForm.groupId] || '默认' }}</span>
            <button class="text-[#1785E6] hover:text-[#0a6bcc]" title="修改所属分组" @click="openAddGroup">
              <Icon name="edit" :size="13" />
            </button>
          </div>
        </div>

        <!-- 设备 ID -->
        <div class="flex items-center gap-3">
          <label class="w-24 text-right text-xs font-medium text-[#4e5969]"><span class="text-[#f53f3f]">*</span> 设备ID</label>
          <div class="flex-1">
            <input
              v-model="addForm.deviceId"
              type="text"
              class="h-9 w-full rounded border border-[#dcdfe6] px-3 text-xs outline-none focus:border-[#1785E6] uppercase"
              placeholder="请输入标贴上的设备ID，不区分大小写"
            />
          </div>
        </div>

        <!-- 设备名称 -->
        <div class="flex items-center gap-3">
          <label class="w-24 text-right text-xs font-medium text-[#4e5969]">设备名称</label>
          <div class="flex-1">
            <input
              v-model="addForm.name"
              type="text"
              class="h-9 w-full rounded border border-[#dcdfe6] px-3 text-xs outline-none focus:border-[#1785E6]"
              placeholder="选填"
            />
          </div>
        </div>

        <!-- 居中大按钮：+ 添加设备 -->
        <div class="pt-4 flex flex-col items-center">
          <button
            class="flex h-10 w-64 items-center justify-center rounded bg-[#1785E6] text-sm font-bold text-white shadow-sm hover:bg-[#0a6bcc] transition-colors disabled:opacity-50"
            :disabled="adding"
            @click="submitAdd"
          >
            <Icon v-if="adding" name="refresh" :size="14" class="mr-1.5 ipc-spin" />+ 添加设备
          </button>

          <!-- 底部帮助链接（对齐图 5 底部文字） -->
          <div class="mt-3 flex items-center gap-1 text-xs text-[#86909c]">
            <Icon name="help-circle" :size="13" />
            <button type="button" class="hover:text-[#1785E6] hover:underline">如何查找设备ID</button>
            <span class="mx-2 text-[#e5e6eb]">|</span>
            <button type="button" class="text-[#1785E6] hover:underline" @click="router.push('/scan')">手机扫码录入 &gt;</button>
          </div>
        </div>
      </div>

      <!-- 批量导入表单 -->
      <div v-else class="mt-6 space-y-4 max-w-md mx-auto">
        <div>
          <label class="mb-1 block text-xs font-medium text-[#4e5969]">设备 ID 列表（每行一个 ID）*</label>
          <textarea
            v-model="batchIdText"
            rows="6"
            class="w-full rounded border border-[#dcdfe6] p-2.5 text-xs font-mono outline-none focus:border-[#1785E6]"
            placeholder="A1B2C3D4E5F678901&#10;B2C3D4E5F67890123"
          />
        </div>

        <div class="flex flex-col items-center pt-2">
          <button
            class="flex h-10 w-64 items-center justify-center rounded bg-[#1785E6] text-sm font-bold text-white shadow-sm hover:bg-[#0a6bcc] transition-colors disabled:opacity-50"
            :disabled="adding"
            @click="submitAdd"
          >
            <Icon v-if="adding" name="refresh" :size="14" class="mr-1.5 ipc-spin" />开始批量导入
          </button>
        </div>
      </div>
    </UiDialog>

    <!-- 分组管理弹窗 -->
    <UiDialog v-model:open="groupDlg.show" :title="groupDlg.mode === 'add' ? '新增设备分组' : '重命名分组'" width="max-w-sm">
      <div class="space-y-3">
        <label class="block text-xs text-[#4e5969]">分组名称 *</label>
        <UiInput v-model="groupDlg.name" placeholder="请输入分组名称" />
      </div>
      <template #footer>
        <UiButton @click="groupDlg.show = false">取消</UiButton>
        <UiButton variant="primary" @click="saveGroup">确定</UiButton>
      </template>
    </UiDialog>

    <!-- 批量转移分组弹窗 -->
    <UiDialog v-model:open="batchMoveDlg.show" title="批量转移分组" width="max-w-sm">
      <div class="space-y-3">
        <label class="block text-xs text-[#4e5969]">选择目标分组</label>
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
          <label class="mb-1 block text-xs text-[#4e5969]">设备名称</label>
          <UiInput v-model="editDevDlg.name" />
        </div>
        <div>
          <label class="mb-1 block text-xs text-[#4e5969]">地理位置</label>
          <UiInput v-model="editDevDlg.location" placeholder="如：深圳总部A座一楼" />
        </div>
      </div>
      <template #footer>
        <UiButton @click="editDevDlg.show = false">取消</UiButton>
        <UiButton variant="primary" @click="saveEditDev">保存</UiButton>
      </template>
    </UiDialog>

    <!-- 设备预览与录像回放模态弹窗（100% 对齐商云原型图 UI/UE） -->
    <DevicePreviewModal
      v-model="previewModal.show"
      :device="previewModal.device"
      :channel="previewModal.channel"
      :initial-tab="previewModal.tab"
    />
  </div>
</template>
