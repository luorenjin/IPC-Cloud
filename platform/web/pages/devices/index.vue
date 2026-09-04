<script setup lang="ts">
// 设备列表（MGR-01/02）+ 添加设备弹窗（ADD-01/05/06/07）
const api = useApi()
const route = useRoute()

// ================= 分组 =================
const groups = ref<any[]>([])
const selectedNode = ref<any>(null)

// 分组 id → 名称 映射
const groupMap = computed(() => {
  const m: Record<string, string> = {}
  for (const g of groups.value) m[g.id] = g.name
  return m
})
// 扁平化分组（供下拉选择）
const groupOptions = computed(() => groups.value.map((g: any) => ({ value: g.id, label: g.name })))

// 由 parentId 构建树，根节点为"全部分组"
const treeData = computed(() => {
  function build(pid: string): any[] {
    return groups.value
      .filter((g: any) => (g.parentId || '') === pid)
      .map((g: any) => ({ ...g, children: build(g.id) }))
  }
  return [{ id: '', name: '全部分组', children: build('') }]
})

async function loadGroups() {
  try {
    const res: any = await api.get('/groups')
    groups.value = res.items || []
  } catch (e: any) {
    ElMessage.error(e.msg || '分组加载失败')
  }
}

function onNodeClick(data: any) {
  selectedNode.value = data
  query.groupId = data.id || ''
  query.page = 1
  load()
}

// 树上右键菜单
const ctx = reactive({ show: false, x: 0, y: 0, node: null as any })
function onCtx(e: MouseEvent, data: any) {
  e.preventDefault()
  ctx.show = true
  ctx.x = e.clientX
  ctx.y = e.clientY
  ctx.node = data
}
function hideCtx() { ctx.show = false }
onMounted(() => document.addEventListener('click', hideCtx))
onBeforeUnmount(() => document.removeEventListener('click', hideCtx))

async function addGroup(parentId = '') {
  let name = ''
  try {
    const r: any = await ElMessageBox.prompt('请输入分组名称', '新增分组', {
      inputPattern: /\S+/, inputErrorMessage: '名称不能为空'
    })
    name = r.value
  } catch { return }
  try {
    await api.post('/groups', { name, parentId: parentId || '' })
    ElMessage.success('分组已创建')
    loadGroups()
  } catch (e: any) {
    ElMessage.error(e.msg || '创建失败')
  }
}

async function delGroup(node: any) {
  if (!node?.id) return
  try { await ElMessageBox.confirm(`确定删除分组「${node.name}」？`, '删除分组', { type: 'warning' }) } catch { return }
  try {
    await api.del(`/groups/${node.id}`)
    ElMessage.success('已删除')
    if (query.groupId === node.id) { query.groupId = ''; selectedNode.value = null }
    loadGroups()
    load()
  } catch (e: any) {
    ElMessage.error(e.msg || '删除失败')
  }
}

// ================= 设备列表 =================
const query = reactive({ groupId: '', keyword: '', page: 1, pageSize: 20 })
const total = ref(0)
const devices = ref<any[]>([])
const stats = ref<any>({ total: 0, online: 0, offline: 0 })
const loading = ref(false)

async function load() {
  loading.value = true
  try {
    const params: any = { page: query.page, pageSize: query.pageSize }
    if (query.groupId) params.groupId = query.groupId
    if (query.keyword) params.keyword = query.keyword
    const res: any = await api.get('/devices', params)
    devices.value = res.items || []
    total.value = res.total || 0
    if (res.stats) stats.value = res.stats
  } catch (e: any) {
    ElMessage.error(e.msg || '加载失败')
  } finally {
    loading.value = false
  }
}

function search() { query.page = 1; load() }

// ================= 映射 =================
const srcMap: Record<string, { label: string; type: any }> = {
  idp: { label: '自有', type: 'primary' },
  gb28181: { label: '国标', type: 'success' },
  onvif: { label: 'ONVIF', type: 'warning' },
  rtsp: { label: 'RTSP', type: 'info' }
}
function srcInfo(s?: string) { return srcMap[s || ''] || { label: s || '-', type: 'info' } }
function statusInfo(s?: string) {
  if (s === 'online') return { label: '在线', type: 'success' }
  if (s === 'pending') return { label: '待确认', type: 'warning' }
  if (s === 'error') return { label: '错误', type: 'danger' }
  return { label: '离线', type: 'info' }
}
// 相对时间
function ago(ts?: number) {
  if (!ts) return '-'
  const s = Math.floor((Date.now() - ts) / 1000)
  if (s < 60) return '刚刚'
  if (s < 3600) return Math.floor(s / 60) + ' 分钟前'
  if (s < 86400) return Math.floor(s / 3600) + ' 小时前'
  return Math.floor(s / 86400) + ' 天前'
}

function openRow(row: any) { navigateTo('/devices/' + row.id) }

// ================= 编辑 / 删除 / 诊断 / 重启 =================
const editDlg = reactive({ show: false, row: null as any, name: '', location: '', remark: '' })
function openEdit(row: any) {
  editDlg.row = row
  editDlg.name = row.name || ''
  editDlg.location = row.location || ''
  editDlg.remark = row.remark || ''
  editDlg.show = true
}
async function saveEdit() {
  try {
    await api.put(`/devices/${editDlg.row.id}`, { name: editDlg.name, location: editDlg.location, remark: editDlg.remark })
    ElMessage.success('已保存')
    editDlg.show = false
    load()
  } catch (e: any) {
    ElMessage.error(e.msg || '保存失败')
  }
}

const delDlg = reactive({ show: false, row: null as any, name: '' })
function askDelete(row: any) {
  delDlg.row = row
  delDlg.name = ''
  delDlg.show = true
}
async function doDelete() {
  if (!delDlg.row) return
  if (delDlg.name !== delDlg.row.name) { ElMessage.error('输入的设备名不一致'); return }
  try {
    await api.request(`/devices/${delDlg.row.id}`, { method: 'DELETE', body: { confirmName: delDlg.name } })
    ElMessage.success('设备已删除')
    delDlg.show = false
    load()
  } catch (e: any) {
    ElMessage.error(e.msg || '删除失败')
  }
}

const diagDlg = reactive({ show: false, loading: false, target: '', items: [] as any[] })
async function runDiag(row: any) {
  diagDlg.show = true
  diagDlg.loading = true
  diagDlg.target = row.name || row.id
  diagDlg.items = []
  try {
    const res: any = await api.post(`/devices/${row.id}/diag`)
    diagDlg.items = res.items || res.checks || (Array.isArray(res) ? res : [])
  } catch (e: any) {
    ElMessage.error(e.msg || '诊断失败')
    diagDlg.show = false
  } finally {
    diagDlg.loading = false
  }
}

async function reboot(row: any) {
  try {
    await api.post(`/devices/${row.id}/reboot`)
    ElMessage.success('重启指令已下发')
  } catch (e: any) {
    ElMessage.error(e.msg || '操作失败')
  }
}

function onMore(cmd: any) {
  if (!cmd) return
  if (cmd.action === 'diag') runDiag(cmd.row)
  else if (cmd.action === 'reboot') reboot(cmd.row)
  else if (cmd.action === 'delete') askDelete(cmd.row)
}

// ================= 添加设备 =================
const addDlg = reactive({ show: false })
const addTab = ref('idp')
function openAdd() { addDlg.show = true }

// a) 自有设备（IDP）绑定
const idpErr = ref<any>(null)
const idpLoading = ref(false)
const idpForm = reactive({ deviceId: '', verifyCode: '', name: '', groupId: '' })
async function idpBind() {
  idpErr.value = null
  if (!idpForm.deviceId || !idpForm.verifyCode) {
    idpErr.value = { msg: '请填写 DeviceID 和验证码' }
    return
  }
  idpLoading.value = true
  try {
    await api.post('/devices/idp/bind', {
      deviceId: idpForm.deviceId.toUpperCase(),
      verifyCode: idpForm.verifyCode,
      groupId: idpForm.groupId,
      name: idpForm.name
    })
    ElMessage.success('绑定成功')
    Object.assign(idpForm, { deviceId: '', verifyCode: '', name: '', groupId: '' })
    addDlg.show = false
    load()
  } catch (e: any) {
    idpErr.value = e
  } finally {
    idpLoading.value = false
  }
}

// b) 国标接入
const gbParams = ref<any>(null)
const gbWhitelist = ref<any[]>([])
const gbForm = reactive({ gbId: '', pwd: '', groupId: '', name: '' })
watch(addTab, (t) => { if (t === 'gb') loadGb() })
async function loadGb() {
  try {
    const pid = api.project().value
    gbParams.value = await api.get(`/projects/${pid}/gb28181/params`)
    const wl: any = await api.get('/devices/gb28181/whitelist')
    gbWhitelist.value = wl.items || wl || []
  } catch (e: any) {
    ElMessage.error(e.msg || '国标参数加载失败')
  }
}
// SIP 参数转行展示（键值对）
const gbParamRows = computed(() =>
  Object.entries(gbParams.value || {}).map(([k, v]) => ({ k, v: String(v) })))
async function copyParams() {
  const text = gbParamRows.value.map((r) => `${r.k}: ${r.v}`).join('\n')
  try {
    await navigator.clipboard.writeText(text)
    ElMessage.success('已复制到剪贴板')
  } catch {
    ElMessage.error('复制失败，请手动复制')
  }
}
async function addWhitelist() {
  if (!gbForm.gbId) { ElMessage.warning('请填写国标 ID'); return }
  try {
    await api.post('/devices/gb28181/whitelist', { ...gbForm })
    ElMessage.success('已加入白名单')
    Object.assign(gbForm, { gbId: '', pwd: '', groupId: '', name: '' })
    loadGb()
  } catch (e: any) {
    ElMessage.error(e.msg || '添加失败')
  }
}

// c) ONVIF 自动发现
const onvifLoading = ref(false)
const onvifResults = ref<any[]>([])
async function discover() {
  onvifLoading.value = true
  try {
    const res: any = await api.post('/devices/onvif/discover')
    onvifResults.value = res.items || res.devices || (Array.isArray(res) ? res : [])
  } catch (e: any) {
    ElMessage.error(e.msg || '发现失败')
  } finally {
    onvifLoading.value = false
  }
}

// d) 手动添加
const manualProto = ref('onvif')
const manualErr = ref<any>(null)
const manualLoading = ref(false)
const onvifForm = reactive({ ip: '', port: 80, user: '', pass: '' })
const rtspMode = ref('url')
const rtspForm = reactive({ url: '', brand: 'hikvision', ip: '', user: '', pass: '' })
const brands = [
  { value: 'hikvision', label: '海康威视' },
  { value: 'dahua', label: '大华' },
  { value: 'uniview', label: '宇视' },
  { value: 'ezviz', label: '萤石' },
  { value: 'tplink', label: 'TP-LINK' }
]
async function submitManual() {
  manualErr.value = null
  manualLoading.value = true
  try {
    if (manualProto.value === 'onvif') {
      await api.post('/devices/onvif', { ip: onvifForm.ip, port: onvifForm.port, user: onvifForm.user, pass: onvifForm.pass })
    } else if (rtspMode.value === 'url') {
      await api.post('/devices/rtsp', { url: rtspForm.url })
    } else {
      await api.post('/devices/rtsp', { brand: rtspForm.brand, ip: rtspForm.ip, user: rtspForm.user, pass: rtspForm.pass })
    }
    ElMessage.success('添加成功')
    addDlg.show = false
    load()
  } catch (e: any) {
    manualErr.value = e
  } finally {
    manualLoading.value = false
  }
}

// e) 批量导入（预添加）
const preaddText = ref('')
const preaddResults = ref<any[]>([])
const preaddLoading = ref(false)
async function preadd() {
  const items = preaddText.value
    .split('\n').map((l) => l.trim()).filter(Boolean)
    .map((l) => {
      const [deviceId, verifyCode] = l.split(/[,，\s]+/)
      return { deviceId: (deviceId || '').toUpperCase(), verifyCode: verifyCode || '' }
    })
    .filter((i) => i.deviceId)
  if (!items.length) { ElMessage.warning('请按每行"DeviceID,验证码"的格式粘贴'); return }
  preaddLoading.value = true
  try {
    const res: any = await api.post('/devices/idp/preadd', { items })
    preaddResults.value = res.items || res.results || []
    ElMessage.success('提交完成')
  } catch (e: any) {
    ElMessage.error(e.msg || '提交失败')
  } finally {
    preaddLoading.value = false
  }
}

onMounted(async () => {
  await loadGroups()
  load()
  // 侧栏"添加设备"入口：/devices?add=1 直接打开弹窗
  if (route.query.add) addDlg.show = true
})
</script>

<template>
  <div class="page">
    <el-row :gutter="12">
      <!-- 左：分组树 -->
      <el-col :span="5">
        <el-card shadow="never" class="group-card">
          <template #header>
            <div class="card-head">
              <span>设备分组</span>
              <span>
                <el-button size="small" type="primary" link @click="addGroup(selectedNode?.id || '')">新增</el-button>
                <el-button size="small" type="danger" link :disabled="!selectedNode?.id" @click="delGroup(selectedNode)">
                  删除
                </el-button>
              </span>
            </div>
          </template>
          <el-tree
            :data="treeData" node-key="id" :props="{ label: 'name', children: 'children' }"
            highlight-current default-expand-all
            @node-click="onNodeClick" @node-contextmenu="onCtx"
          />
        </el-card>
      </el-col>

      <!-- 右：设备表格 -->
      <el-col :span="19">
        <el-card shadow="never">
          <div class="stat-line">
            <span>总数 <b>{{ stats.total ?? 0 }}</b></span>
            <span>在线 <b class="green">{{ stats.online ?? 0 }}</b></span>
            <span>离线 <b class="gray">{{ stats.offline ?? 0 }}</b></span>
          </div>
          <div class="toolbar">
            <el-button type="primary" @click="openAdd">添加设备</el-button>
            <el-input
              v-model="query.keyword" placeholder="按名称 / IP 搜索" clearable style="width: 220px"
              @keyup.enter="search" @clear="search"
            />
            <el-button @click="search">搜索</el-button>
            <el-button @click="load">刷新</el-button>
            <el-link type="primary" href="/api/v1/devices/export" target="_blank" class="export">导出</el-link>
          </div>
          <el-table :data="devices" v-loading="loading" @row-click="openRow">
            <el-table-column prop="name" label="名称" min-width="140" show-overflow-tooltip />
            <el-table-column label="来源" width="90">
              <template #default="{ row }">
                <el-tag :type="srcInfo(row.source).type" size="small" effect="plain">
                  {{ srcInfo(row.source).label }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="状态" width="90">
              <template #default="{ row }">
                <el-tag :type="statusInfo(row.status).type" size="small">{{ statusInfo(row.status).label }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="model" label="型号" width="110" show-overflow-tooltip />
            <el-table-column prop="vendor" label="厂商" width="100" show-overflow-tooltip />
            <el-table-column prop="ip" label="IP" width="130" />
            <el-table-column label="分组" width="110">
              <template #default="{ row }">{{ groupMap[row.groupId] || '-' }}</template>
            </el-table-column>
            <el-table-column label="最后在线" width="110">
              <template #default="{ row }">{{ ago(row.lastOnline || row.lastSeen) }}</template>
            </el-table-column>
            <el-table-column label="操作" width="200" fixed="right">
              <template #default="{ row }">
                <el-button link type="primary" size="small" @click.stop="openRow(row)">详情</el-button>
                <el-button link type="primary" size="small" @click.stop="openEdit(row)">编辑</el-button>
                <el-dropdown trigger="click" class="more-dd" @command="onMore">
                  <el-button link type="primary" size="small" @click.stop>更多</el-button>
                  <template #dropdown>
                    <el-dropdown-menu>
                      <el-dropdown-item :command="{ action: 'diag', row }">诊断</el-dropdown-item>
                      <el-dropdown-item :command="{ action: 'reboot', row }">重启</el-dropdown-item>
                      <el-dropdown-item :command="{ action: 'delete', row }" divided>删除</el-dropdown-item>
                    </el-dropdown-menu>
                  </template>
                </el-dropdown>
              </template>
            </el-table-column>
          </el-table>
          <el-pagination
            v-model:current-page="query.page" v-model:page-size="query.pageSize"
            :total="total" :page-sizes="[10, 20, 50, 100]" layout="total, sizes, prev, pager, next"
            class="pager" @current-change="load" @size-change="search"
          />
        </el-card>
      </el-col>
    </el-row>

    <!-- 分组右键菜单 -->
    <div v-if="ctx.show" class="ctx-menu" :style="{ left: ctx.x + 'px', top: ctx.y + 'px' }">
      <div class="ctx-item" @click="addGroup(ctx.node?.id || ''); ctx.show = false">新增分组</div>
      <div v-if="ctx.node?.id" class="ctx-item danger" @click="delGroup(ctx.node); ctx.show = false">删除分组</div>
    </div>

    <!-- 添加设备弹窗（五个 tab） -->
    <el-dialog v-model="addDlg.show" title="添加设备" width="660px" destroy-on-close>
      <el-tabs v-model="addTab">
        <!-- a) 自有设备（IDP）绑定 -->
        <el-tab-pane label="自有设备" name="idp">
          <el-alert
            v-if="idpErr" :title="idpErr.msg" type="error" :closable="false" show-icon class="mb12"
            :description="idpErr.suggest"
          />
          <el-form label-width="90px">
            <el-form-item label="DeviceID" required>
              <el-input v-model="idpForm.deviceId" maxlength="17" placeholder="17 位大写字母/数字" />
            </el-form-item>
            <el-form-item label="验证码" required>
              <el-input v-model="idpForm.verifyCode" maxlength="6" placeholder="设备机身 6 位验证码" />
            </el-form-item>
            <el-form-item label="名称">
              <el-input v-model="idpForm.name" placeholder="设备显示名称" />
            </el-form-item>
            <el-form-item label="分组">
              <el-select v-model="idpForm.groupId" placeholder="选择分组" clearable style="width: 100%">
                <el-option v-for="g in groupOptions" :key="g.value" :label="g.label" :value="g.value" />
              </el-select>
            </el-form-item>
          </el-form>
          <div class="tab-btns">
            <el-button type="primary" :loading="idpLoading" @click="idpBind">绑定</el-button>
          </div>
        </el-tab-pane>

        <!-- b) 国标接入 -->
        <el-tab-pane label="国标接入" name="gb">
          <div class="gb-head">
            <span>SIP 接入参数（在设备/平台端配置）</span>
            <el-button size="small" @click="copyParams">复制</el-button>
          </div>
          <el-table :data="gbParamRows" size="small" max-height="170">
            <el-table-column prop="k" label="参数" width="170" />
            <el-table-column prop="v" label="值" />
          </el-table>
          <el-divider content-position="left">白名单（预授权国标设备）</el-divider>
          <el-form inline>
            <el-form-item><el-input v-model="gbForm.gbId" placeholder="国标 ID" style="width: 180px" /></el-form-item>
            <el-form-item><el-input v-model="gbForm.pwd" placeholder="接入密码" style="width: 120px" /></el-form-item>
            <el-form-item><el-input v-model="gbForm.name" placeholder="名称" style="width: 120px" /></el-form-item>
            <el-form-item>
              <el-select v-model="gbForm.groupId" placeholder="分组" clearable style="width: 130px">
                <el-option v-for="g in groupOptions" :key="g.value" :label="g.label" :value="g.value" />
              </el-select>
            </el-form-item>
            <el-form-item>
              <el-button type="primary" @click="addWhitelist">添加白名单</el-button>
            </el-form-item>
          </el-form>
          <el-table :data="gbWhitelist" size="small" max-height="150">
            <el-table-column prop="gbId" label="国标 ID" min-width="160" />
            <el-table-column prop="name" label="名称" min-width="100" />
            <el-table-column label="分组">
              <template #default="{ row }">{{ groupMap[row.groupId] || '-' }}</template>
            </el-table-column>
          </el-table>
        </el-tab-pane>

        <!-- c) ONVIF 自动发现 -->
        <el-tab-pane label="自动发现" name="onvif">
          <div class="mb12">
            <el-button type="primary" :loading="onvifLoading" @click="discover">开始发现</el-button>
          </div>
          <el-table :data="onvifResults" size="small" max-height="320">
            <el-table-column label="IP" width="150">
              <template #default="{ row }">{{ row.ip || row.addr || '-' }}</template>
            </el-table-column>
            <el-table-column label="XAddr" min-width="220" show-overflow-tooltip>
              <template #default="{ row }">{{ row.xaddr || row.XAddrs || row.xAddr || row.url || '-' }}</template>
            </el-table-column>
            <el-table-column label="已添加" width="90">
              <template #default="{ row }">
                <el-tag :type="row.added ? 'success' : 'info'" size="small">{{ row.added ? '是' : '否' }}</el-tag>
              </template>
            </el-table-column>
          </el-table>
          <div v-if="!onvifResults.length && !onvifLoading" class="empty">尚未发现设备，请点击"开始发现"</div>
        </el-tab-pane>

        <!-- d) 手动添加 -->
        <el-tab-pane label="手动添加" name="manual">
          <el-radio-group v-model="manualProto" class="mb12">
            <el-radio-button value="onvif">ONVIF</el-radio-button>
            <el-radio-button value="rtsp">RTSP</el-radio-button>
          </el-radio-group>
          <el-alert
            v-if="manualErr" :title="manualErr.msg" type="error" :closable="false" show-icon class="mb12"
            :description="manualErr.suggest"
          />
          <el-form v-if="manualProto === 'onvif'" label-width="70px">
            <el-form-item label="IP" required>
              <el-input v-model="onvifForm.ip" placeholder="设备 IP" />
            </el-form-item>
            <el-form-item label="端口">
              <el-input-number v-model="onvifForm.port" :min="1" :max="65535" />
            </el-form-item>
            <el-form-item label="用户名">
              <el-input v-model="onvifForm.user" placeholder="ONVIF 用户名" />
            </el-form-item>
            <el-form-item label="密码">
              <el-input v-model="onvifForm.pass" type="password" show-password placeholder="ONVIF 密码" />
            </el-form-item>
          </el-form>
          <template v-else>
            <el-radio-group v-model="rtspMode" size="small" class="mb12">
              <el-radio-button value="url">完整 RTSP 地址</el-radio-button>
              <el-radio-button value="brand">按品牌拼装</el-radio-button>
            </el-radio-group>
            <el-form v-if="rtspMode === 'url'" label-width="70px">
              <el-form-item label="RTSP" required>
                <el-input v-model="rtspForm.url" placeholder="rtsp://user:pass@ip:554/..." />
              </el-form-item>
            </el-form>
            <el-form v-else label-width="70px">
              <el-form-item label="品牌" required>
                <el-select v-model="rtspForm.brand" style="width: 160px">
                  <el-option v-for="b in brands" :key="b.value" :label="b.label" :value="b.value" />
                </el-select>
              </el-form-item>
              <el-form-item label="IP" required>
                <el-input v-model="rtspForm.ip" placeholder="设备 IP" />
              </el-form-item>
              <el-form-item label="用户名">
                <el-input v-model="rtspForm.user" />
              </el-form-item>
              <el-form-item label="密码">
                <el-input v-model="rtspForm.pass" type="password" show-password />
              </el-form-item>
            </el-form>
          </template>
          <div class="tab-btns">
            <el-button type="primary" :loading="manualLoading" @click="submitManual">添加</el-button>
          </div>
        </el-tab-pane>

        <!-- e) 批量导入（预添加） -->
        <el-tab-pane label="批量导入" name="batch">
          <el-input
            v-model="preaddText" type="textarea" :rows="6"
            placeholder="每行一条：DeviceID,验证码&#10;例如：34020000001320000001,123456"
          />
          <div class="tab-btns">
            <el-button type="primary" :loading="preaddLoading" @click="preadd">提交预添加</el-button>
          </div>
          <el-table v-if="preaddResults.length" :data="preaddResults" size="small" class="mt12" max-height="240">
            <el-table-column prop="deviceId" label="DeviceID" min-width="180" />
            <el-table-column label="结果" width="90">
              <template #default="{ row }">
                <el-tag :type="row.ok === false || row.success === false ? 'danger' : 'success'" size="small">
                  {{ row.ok === false || row.success === false ? '失败' : '成功' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="说明">
              <template #default="{ row }">{{ row.msg || row.reason || '-' }}</template>
            </el-table-column>
          </el-table>
        </el-tab-pane>
      </el-tabs>
    </el-dialog>

    <!-- 编辑设备 -->
    <el-dialog v-model="editDlg.show" title="编辑设备" width="420px">
      <el-form label-width="80px">
        <el-form-item label="名称"><el-input v-model="editDlg.name" /></el-form-item>
        <el-form-item label="安装位置"><el-input v-model="editDlg.location" /></el-form-item>
        <el-form-item label="备注"><el-input v-model="editDlg.remark" type="textarea" :rows="2" /></el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="editDlg.show = false">取消</el-button>
        <el-button type="primary" @click="saveEdit">保存</el-button>
      </template>
    </el-dialog>

    <!-- 删除确认（需输入设备名） -->
    <el-dialog v-model="delDlg.show" title="删除设备" width="420px">
      <p class="del-tip">
        将删除设备 <b>{{ delDlg.row?.name }}</b> 及其通道、录像计划等关联数据，操作不可恢复。
      </p>
      <el-input
        v-model="delDlg.name"
        :placeholder="'请输入设备名「' + (delDlg.row?.name || '') + '」以确认'"
      />
      <template #footer>
        <el-button @click="delDlg.show = false">取消</el-button>
        <el-button type="danger" :disabled="delDlg.name !== delDlg.row?.name" @click="doDelete">确认删除</el-button>
      </template>
    </el-dialog>

    <!-- 诊断结果 -->
    <el-dialog v-model="diagDlg.show" :title="'诊断结果 · ' + diagDlg.target" width="500px">
      <div v-loading="diagDlg.loading" class="diag-list">
        <div v-for="(it, i) in diagDlg.items" :key="i" class="diag-item">
          <span class="mark" :class="it.ok ? 'ok' : 'err'">{{ it.ok ? '✓' : '✕' }}</span>
          <span class="diag-name">{{ it.name || it.item || '检查项' }}</span>
          <span class="diag-msg" :class="{ err: !it.ok }">{{ it.msg || '' }}</span>
        </div>
        <div v-if="!diagDlg.loading && !diagDlg.items.length" class="empty">无诊断数据</div>
      </div>
    </el-dialog>
  </div>
</template>

<style scoped>
.group-card :deep(.el-card__body) { padding: 8px; }
.card-head { display: flex; justify-content: space-between; align-items: center; }
.stat-line { display: flex; gap: 24px; color: #606266; font-size: 13px; margin-bottom: 12px; }
.stat-line b { color: #303133; }
.stat-line .green { color: #67c23a; }
.stat-line .gray { color: #909399; }
.toolbar { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; }
.export { margin-left: auto; }
.more-dd { margin-left: 12px; }
.pager { margin-top: 12px; justify-content: flex-end; }
.ctx-menu {
  position: fixed; z-index: 3000; background: #fff; border: 1px solid #e4e7ed;
  border-radius: 4px; box-shadow: 0 2px 12px rgba(0, 0, 0, 0.1); min-width: 110px; padding: 4px 0;
}
.ctx-item { padding: 7px 16px; font-size: 13px; cursor: pointer; color: #606266; }
.ctx-item:hover { background: #f5f7fa; color: #409eff; }
.ctx-item.danger:hover { color: #f56c6c; }
.tab-btns { text-align: center; margin-top: 8px; }
.gb-head { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; color: #606266; font-size: 13px; }
.mt12 { margin-top: 12px; }
.mb12 { margin-bottom: 12px; }
.del-tip { color: #606266; font-size: 13px; margin-top: 0; }
.diag-list { min-height: 60px; }
.diag-item { display: flex; align-items: baseline; gap: 8px; padding: 6px 0; border-bottom: 1px dashed #eee; }
.mark { font-weight: 700; }
.mark.ok { color: #67c23a; }
.mark.err { color: #f56c6c; }
.diag-name { width: 110px; color: #303133; flex-shrink: 0; }
.diag-msg { color: #909399; font-size: 13px; }
.diag-msg.err { color: #f56c6c; }
.empty { color: #909399; font-size: 13px; text-align: center; padding: 24px 0; }
</style>