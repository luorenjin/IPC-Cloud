<script setup lang="ts">
// 媒体节点管理（SYS-01/02）：节点增删改查、禁用、自检、流列表抽屉与踢流，WebSocket 实时刷新状态
const api = useApi()
const toast = useToast()
const confirm = useConfirm()
const { currentProject, loadMe } = useAuth()

const nodes = ref<any[]>([])
const loading = ref(false)

async function loadNodes() {
  loading.value = true
  try {
    const res: any = await api.get('/media-nodes')
    nodes.value = res?.items || []
  } catch (e: any) {
    toastApiError(e, '加载媒体节点失败')
  } finally {
    loading.value = false
  }
}

/* 状态标签：online 绿 / offline 红 */
function statusTag(s: string) {
  if (s === 'online') return { text: '在线', color: 'success' as const }
  if (s === 'offline') return { text: '离线', color: 'danger' as const }
  if (s === 'disabled') return { text: '已禁用', color: 'info' as const }
  return { text: s || '未知', color: 'warning' as const }
}

/* 相对时间 */
const ago = (ts: any) => {
  if (!ts) return '—'
  const t = typeof ts === 'string' ? Date.parse(ts) : ts
  const s = Math.floor((Date.now() - t) / 1000)
  if (!isFinite(s) || s < 0) return '—'
  return s < 60
    ? '刚刚'
    : s < 3600
      ? Math.floor(s / 60) + '分钟前'
      : s < 86400
        ? Math.floor(s / 3600) + '小时前'
        : Math.floor(s / 86400) + '天前'
}

/* 带宽格式化（bytes） */
const fmtBytes = (b: any) => {
  const n = Number(b || 0)
  if (!n) return '0 B/s'
  const units = ['B/s', 'KB/s', 'MB/s', 'GB/s']
  let i = 0
  let v = n
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++ }
  return v.toFixed(i === 0 ? 0 : 1) + ' ' + units[i]
}

/* ---------------- 新建/编辑节点 ---------------- */
const nodeDlg = reactive({
  visible: false,
  mode: 'create' as 'create' | 'edit',
  id: null as any,
  form: {
    name: '',
    apiUrl: '',
    secret: '',
    publicHost: '',
    rtmpPort: 1936,
    httpPort: 80,
    httpsPort: 443,
    rtspPort: 554,
    rtpRange: '30000-30100',
    maxStreams: 200,
    weight: 100
  },
  saving: false
})

function openNodeDlg(mode: 'create' | 'edit', row?: any) {
  nodeDlg.mode = mode
  nodeDlg.id = row?.id ?? null
  nodeDlg.form = row
    ? {
        name: row.name || '',
        apiUrl: row.apiUrl || '',
        secret: '', // 编辑时留空表示不修改密钥
        publicHost: row.publicHost || '',
        rtmpPort: row.rtmpPort ?? 1936,
        httpPort: row.httpPort ?? 80,
        httpsPort: row.httpsPort ?? 443,
        rtspPort: row.rtspPort ?? 554,
        rtpRange: row.rtpRange || '30000-30100',
        maxStreams: row.maxStreams ?? 200,
        weight: row.weight ?? 100
      }
    : {
        name: '',
        apiUrl: '',
        secret: '',
        publicHost: '',
        rtmpPort: 1936,
        httpPort: 80,
        httpsPort: 443,
        rtspPort: 554,
        rtpRange: '30000-30100',
        maxStreams: 200,
        weight: 100
      }
  nodeDlg.visible = true
}

function clampPort(v: any, def: number) {
  const n = Number(v)
  return isFinite(n) && n >= 1 && n <= 65535 ? Math.floor(n) : def
}

async function saveNode() {
  const f = nodeDlg.form
  if (!f.name.trim() || !f.apiUrl.trim()) return toast.warning('请填写节点名称与 API 地址')
  if (nodeDlg.mode === 'create' && !f.secret.trim()) return toast.warning('请填写接入密钥')
  if (nodeDlg.mode === 'create' && !f.publicHost.trim()) return toast.warning('请填写公网地址')
  nodeDlg.saving = true
  try {
    const body: any = {
      name: f.name.trim(),
      apiUrl: f.apiUrl.trim(),
      publicHost: f.publicHost.trim(),
      rtmpPort: clampPort(f.rtmpPort, 1936),
      httpPort: clampPort(f.httpPort, 80),
      httpsPort: clampPort(f.httpsPort, 443),
      rtspPort: clampPort(f.rtspPort, 554),
      rtpRange: f.rtpRange.trim(),
      maxStreams: clampPort(f.maxStreams, 200),
      weight: clampPort(f.weight, 100)
    }
    if (f.secret.trim()) body.secret = f.secret.trim()
    if (nodeDlg.mode === 'create') {
      await api.post('/media-nodes', body)
      toast.success('节点已创建')
    } else {
      await api.put('/media-nodes/' + nodeDlg.id, body)
      toast.success('节点已更新')
    }
    nodeDlg.visible = false
    await loadNodes()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    nodeDlg.saving = false
  }
}

/* 自检：后端异步执行，等待 node.status 事件或轮询刷新；失败呈现具体原因 */
const checkingId = ref<any>(null)
async function selfcheck(row: any) {
  checkingId.value = row.id
  try {
    await api.post('/media-nodes/' + row.id + '/selfcheck')
    // 自检为异步任务，稍后刷新拿最新状态
    setTimeout(async () => {
      await loadNodes()
      checkingId.value = null
      const fresh = nodes.value.find((n) => n.id === row.id)
      if (!fresh) return
      if (fresh.status === 'online') toast.success('自检通过：节点「' + fresh.name + '」在线')
      else toast.error({ title: '自检失败：节点「' + fresh.name + '」不可达', suggest: '请检查 API 地址与端口是否正确、节点服务是否启动、网络是否连通。' })
    }, 1500)
  } catch (e: any) {
    checkingId.value = null
    // 自检失败呈现具体原因：不可达 / secret 错误 / 版本过低
    const msg = String(e?.msg || '')
    let suggest = e?.suggest || '节点服务不可达：请检查 API 地址与端口是否正确、服务是否启动、网络是否连通。'
    if (/secret|密钥|auth|401/i.test(msg)) suggest = '接入密钥（secret）错误：请核对节点配置中的 secret 后重试。'
    else if (/version|版本/i.test(msg)) suggest = '节点版本过低：请升级流媒体服务后重试。'
    else if (msg) suggest = msg
    toast.error({ title: (e?.code ? e.code + '：' : '') + (msg || '自检失败'), suggest })
  }
}

/* 禁用/启用节点（后端 disabled 字段；禁用后不参与调度，已有流不受影响） */
async function toggleDisable(row: any) {
  const disabling = !row.disabled
  const ok = await confirm.ask({
    title: disabling ? '禁用节点' : '启用节点',
    message: disabling
      ? '确定禁用节点「' + row.name + '」？'
      : '确定启用节点「' + row.name + '」？',
    detail: disabling ? '禁用后新流不再调度到该节点，已有流不受影响。' : '启用后恢复参与调度。',
    danger: disabling,
    confirmText: disabling ? '禁用' : '启用'
  })
  if (!ok) return
  try {
    await api.put('/media-nodes/' + row.id, { disabled: disabling })
    toast.success(disabling ? '已禁用' : '已启用')
    await loadNodes()
  } catch (e: any) {
    toastApiError(e, '操作失败')
  }
}

/* 删除节点（有活跃流时禁用按钮并 tooltip 说明） */
async function removeNode(row: any) {
  const ok = await confirm.ask({
    title: '删除节点',
    message: '确定删除节点「' + row.name + '」？',
    detail: '节点存在活跃流时无法删除，请先踢流。',
    danger: true,
    confirmText: '删除'
  })
  if (!ok) return
  try {
    await api.del('/media-nodes/' + row.id)
    toast.success('已删除')
    await loadNodes()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

/* ---------------- 节点详情抽屉（SYS-02） ---------------- */
const detail = reactive({
  open: false,
  node: null as any,
  items: [] as any[],
  loading: false
})

async function showDetail(row: any) {
  detail.node = row
  detail.items = []
  detail.open = true
  detail.loading = true
  try {
    const res: any = await api.get('/media-nodes/' + row.id + '/streams')
    detail.items = res?.items || []
  } catch (e: any) {
    toastApiError(e, '获取流列表失败')
  } finally {
    detail.loading = false
  }
}

/* 踢流 */
async function kickStream(ss: any) {
  const ok = await confirm.ask({
    title: '踢流',
    message: '确定断开流「' + ss.stream + '」？',
    detail: '踢流后对应观看端将停止播放。',
    danger: true,
    confirmText: '踢流'
  })
  if (!ok) return
  try {
    await api.post('/media-nodes/' + detail.node.id + '/streams/' + ss.id + '/kick')
    toast.success('已踢流')
    await showDetail(detail.node)
    await loadNodes()
  } catch (e: any) {
    toastApiError(e, '踢流失败')
  }
}

const cols = [
  { key: 'name', label: '名称', width: '130px' },
  { key: 'apiUrl', label: 'API 地址', width: '180px' },
  { key: 'status', label: '状态', width: '86px', align: 'center' as const },
  { key: 'version', label: '版本', width: '90px' },
  { key: 'streams', label: '流数/上限', width: '100px', align: 'center' as const },
  { key: 'playing', label: '播放数', width: '80px', align: 'center' as const },
  { key: 'bw', label: '带宽 in/out', width: '140px' },
  { key: 'lastKeepalive', label: '最近心跳', width: '105px', align: 'center' as const },
  { key: 'weight', label: '权重', width: '70px', align: 'center' as const },
  { key: 'ops', label: '操作', width: '250px', align: 'center' as const, fixed: true }
]
const streamCols = [
  { key: 'app', label: '应用', width: '90px' },
  { key: 'stream', label: '流名称', width: '180px' },
  { key: 'channel', label: '来源通道', width: '130px' },
  { key: 'viewers', label: '观看数', width: '80px', align: 'center' as const },
  { key: 'bitrate', label: '码率', width: '90px', align: 'right' as const },
  { key: 'ops', label: '操作', width: '70px', align: 'center' as const }
]

const fmtTime = (ts: any) =>
  ts ? new Date(typeof ts === 'string' ? Date.parse(ts) : ts).toLocaleString() : '-'

/* WebSocket 实时刷新：节点状态变化事件 */
useWs((ev: any) => {
  if (ev.type === 'node.status') loadNodes()
})

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await loadNodes()
})
</script>

<template>
  <div class="space-y-3">
    <UiCard title="媒体节点" flat>
      <template #extra>
        <div class="flex items-center gap-2">
          <UiButton size="sm" :disabled="loading" @click="loadNodes">
            <UiIcon name="refresh" :size="14" />刷新
          </UiButton>
          <UiButton variant="primary" size="sm" @click="openNodeDlg('create')">
            <UiIcon name="plus" :size="14" />新建节点
          </UiButton>
        </div>
      </template>

      <UiTable :columns="cols" :rows="nodes" :loading="loading" empty="暂无媒体节点">
        <template #apiUrl="{ row }">
          <span class="block truncate" :title="row.apiUrl">{{ row.apiUrl }}</span>
        </template>
        <template #status="{ row }">
          <UiTag v-if="row.disabled" color="info" dot>已禁用</UiTag>
          <UiTag v-else :color="statusTag(row.status).color" dot>{{ statusTag(row.status).text }}</UiTag>
        </template>
        <template #version="{ row }">{{ row.version || '—' }}</template>
        <template #streams="{ row }">
          <span :class="row.streams >= row.maxStreams ? 'text-danger' : ''">{{ row.streams ?? 0 }} / {{ row.maxStreams ?? 0 }}</span>
        </template>
        <template #playing="{ row }">{{ row.playing ?? row.viewers ?? 0 }}</template>
        <template #bw="{ row }">
          <span class="text-xs">{{ fmtBytes(row.bytesIn ?? row.bwIn) }} / {{ fmtBytes(row.bytesOut ?? row.bwOut) }}</span>
        </template>
        <template #lastKeepalive="{ row }">{{ ago(row.lastKeepalive) }}</template>
        <template #weight="{ row }">
          <span :class="row.weight === 0 ? 'text-placeholder' : ''">{{ row.weight ?? 100 }}</span>
        </template>
        <template #ops="{ row }">
          <span class="inline-flex items-center justify-center gap-1">
            <UiButton variant="text" size="sm" @click="openNodeDlg('edit', row)">编辑</UiButton>
            <UiButton variant="text" size="sm" :disabled="checkingId === row.id" @click="selfcheck(row)">
              {{ checkingId === row.id ? '自检中…' : '自检' }}
            </UiButton>
            <UiButton variant="text" size="sm" @click="showDetail(row)">详情</UiButton>
            <UiButton variant="text" size="sm" @click="toggleDisable(row)">{{ row.weight === 0 ? '启用' : '禁用' }}</UiButton>
            <UiTooltip v-if="row.streams > 0" label="节点存在活跃流，请先在详情中踢流">
              <span><UiButton variant="dangerText" size="sm" disabled>删除</UiButton></span>
            </UiTooltip>
            <UiButton v-else variant="dangerText" size="sm" @click="removeNode(row)">删除</UiButton>
          </span>
        </template>
        <template #empty-action>
          <UiButton variant="primary" @click="openNodeDlg('create')">新建节点</UiButton>
        </template>
      </UiTable>
    </UiCard>

    <!-- 新建/编辑节点对话框 -->
    <UiDialog
      v-model:open="nodeDlg.visible"
      :title="nodeDlg.mode === 'create' ? '新建媒体节点' : '编辑媒体节点'"
      width="max-w-lg"
    >
      <div class="grid grid-cols-[110px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>节点名称</span>
        <UiInput v-model="nodeDlg.form.name" placeholder="节点名称" :maxlength="50" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>API 地址</span>
        <UiInput v-model="nodeDlg.form.apiUrl" placeholder="http://host:port" :maxlength="200" />
        <span class="text-right text-sm text-body">
          <span v-if="nodeDlg.mode === 'create'" class="text-danger">*</span>接入密钥
        </span>
        <UiInput
          v-model="nodeDlg.form.secret" type="password"
          :placeholder="nodeDlg.mode === 'create' ? '节点接入密钥' : '留空表示不修改'" :maxlength="128"
        />
        <span class="text-right text-sm text-body">
          <span v-if="nodeDlg.mode === 'create'" class="text-danger">*</span>公网地址
        </span>
        <UiInput v-model="nodeDlg.form.publicHost" placeholder="公网访问域名或 IP" :maxlength="200" />
        <span class="text-right text-sm text-body">HTTP 端口</span>
        <div class="grid grid-cols-2 gap-3">
          <UiInput v-model="nodeDlg.form.httpPort" placeholder="80" :maxlength="5" />
          <UiInput v-model="nodeDlg.form.httpsPort" placeholder="443" :maxlength="5" />
        </div>
        <span class="text-right text-sm text-body">RTMP / RTSP 端口</span>
        <div class="grid grid-cols-2 gap-3">
          <UiInput v-model="nodeDlg.form.rtmpPort" placeholder="1936" :maxlength="5" />
          <UiInput v-model="nodeDlg.form.rtspPort" placeholder="554" :maxlength="5" />
        </div>
        <span class="text-right text-sm text-body">RTP 端口段</span>
        <UiInput v-model="nodeDlg.form.rtpRange" placeholder="30000-30100" :maxlength="30" width="w-45" />
        <span class="text-right text-sm text-body">最大流数</span>
        <UiInput v-model="nodeDlg.form.maxStreams" placeholder="200" :maxlength="5" width="w-45" />
        <span class="text-right text-body text-sm">权重</span>
        <UiInput v-model="nodeDlg.form.weight" placeholder="100" :maxlength="4" width="w-45" />
      </div>
      <template #footer>
        <UiButton @click="nodeDlg.visible = false">取消</UiButton>
        <UiButton variant="primary" :disabled="nodeDlg.saving" @click="saveNode">{{ nodeDlg.saving ? '保存中…' : '确定' }}</UiButton>
      </template>
    </UiDialog>

    <!-- 节点详情抽屉（SYS-02） -->
    <UiDrawer v-model:open="detail.open" :title="'节点详情 - ' + (detail.node?.name || '')" width="max-w-xl">
      <div class="space-y-4 p-5">
        <!-- 配置摘要 -->
        <div>
          <p class="mb-2 text-sm font-semibold text-ink">配置摘要</p>
          <div class="grid grid-cols-2 gap-x-4 gap-y-2 rounded border border-line bg-canvas p-3 text-[13px]">
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">API 地址</span><span class="truncate text-body" :title="detail.node?.apiUrl">{{ detail.node?.apiUrl || '—' }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">公网地址</span><span class="truncate text-body">{{ detail.node?.publicHost || '—' }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">端口</span><span class="text-body">HTTP {{ detail.node?.httpPort ?? '—' }} · HTTPS {{ detail.node?.httpsPort ?? '—' }} · RTMP {{ detail.node?.rtmpPort ?? '—' }} · RTSP {{ detail.node?.rtspPort ?? '—' }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">RTP 段</span><span class="text-body">{{ detail.node?.rtpRange || '—' }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">流数/上限</span><span class="text-body">{{ detail.node?.streams ?? 0 }} / {{ detail.node?.maxStreams ?? 0 }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">权重</span><span class="text-body">{{ detail.node?.weight ?? 100 }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">状态</span><UiTag :color="statusTag(detail.node?.status).color" dot>{{ statusTag(detail.node?.status).text }}</UiTag></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">最近心跳</span><span class="text-body">{{ ago(detail.node?.lastKeepalive) }}</span></div>
          </div>
        </div>

        <!-- 当前流列表 -->
        <div>
          <p class="mb-2 text-sm font-semibold text-ink">当前流列表</p>
          <UiTable :columns="streamCols" :rows="detail.items" :loading="detail.loading" dense empty="该节点暂无推流">
            <template #channel="{ row }">
              <span class="block truncate" :title="row.channelName || row.channelId">{{ row.channelName || row.channelId || '—' }}</span>
            </template>
            <template #viewers="{ row }">{{ row.viewers ?? 0 }}</template>
            <template #bitrate="{ row }">
              <span class="text-xs">{{ row.bitrate ? (row.bitrate / 1000).toFixed(0) + ' kbps' : '—' }}</span>
            </template>
            <template #ops="{ row }">
              <UiButton variant="dangerText" size="sm" @click="kickStream(row)">踢流</UiButton>
            </template>
          </UiTable>
          <p v-if="detail.items.length" class="mt-2 text-xs text-placeholder">开始时间：{{ fmtTime(detail.items[0].startedAt) }} 起，共 {{ detail.items.length }} 路</p>
        </div>
      </div>
      <template #footer>
        <div class="flex justify-end gap-2">
          <UiButton @click="detail.open = false">关闭</UiButton>
          <UiButton variant="primary" :disabled="!detail.node" @click="showDetail(detail.node)">
            <UiIcon name="refresh" :size="14" />刷新流列表
          </UiButton>
        </div>
      </template>
    </UiDrawer>
  </div>
</template>
