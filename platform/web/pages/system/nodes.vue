<script setup lang="ts">
// 媒体节点管理（SYS-01/02）：节点增删改查、自检、流列表，WebSocket 实时刷新状态
const api = useApi()
const { currentProject, loadMe } = useAuth()

const nodes = ref<any[]>([])
const loading = ref(false)

async function loadNodes() {
  loading.value = true
  try {
    const res: any = await api.get('/media-nodes')
    nodes.value = res?.items || []
  } catch (e: any) {
    ElMessage.error(e?.msg || '加载媒体节点失败')
  } finally {
    loading.value = false
  }
}

/* 状态标签：online 绿 / offline 红 */
function statusTag(s: string) {
  if (s === 'online') return { text: '在线', type: 'success' as const }
  if (s === 'offline') return { text: '离线', type: 'danger' as const }
  return { text: s || '未知', type: 'info' as const }
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
        rtpRange: '30000-30100',
        maxStreams: 200,
        weight: 100
      }
  nodeDlg.visible = true
}

async function saveNode() {
  const f = nodeDlg.form
  if (!f.name.trim() || !f.apiUrl.trim()) return ElMessage.warning('请填写节点名称与 API 地址')
  if (nodeDlg.mode === 'create' && !f.secret.trim()) return ElMessage.warning('请填写接入密钥')
  nodeDlg.saving = true
  try {
    const body: any = {
      name: f.name.trim(),
      apiUrl: f.apiUrl.trim(),
      publicHost: f.publicHost.trim(),
      rtmpPort: f.rtmpPort,
      httpPort: f.httpPort,
      httpsPort: f.httpsPort,
      rtpRange: f.rtpRange.trim(),
      maxStreams: f.maxStreams,
      weight: f.weight
    }
    if (f.secret.trim()) body.secret = f.secret.trim()
    if (nodeDlg.mode === 'create') {
      await api.post('/media-nodes', body)
      ElMessage.success('节点已创建')
    } else {
      await api.put('/media-nodes/' + nodeDlg.id, body)
      ElMessage.success('节点已更新')
    }
    nodeDlg.visible = false
    await loadNodes()
  } catch (e: any) {
    ElMessage.error(e?.msg || '保存失败')
  } finally {
    nodeDlg.saving = false
  }
}

/* 自检 */
const checkingId = ref<any>(null)
async function selfcheck(row: any) {
  checkingId.value = row.id
  try {
    const res: any = await api.post('/media-nodes/' + row.id + '/selfcheck')
    ElMessage.success(typeof res?.message === 'string' && res.message ? res.message : '自检完成')
    await loadNodes()
  } catch (e: any) {
    ElMessage.error(e?.msg || '自检失败')
  } finally {
    checkingId.value = null
  }
}

/* 删除节点 */
async function removeNode(row: any) {
  try {
    await ElMessageBox.confirm('确定删除节点「' + row.name + '」？', '删除节点', {
      type: 'warning',
      confirmButtonText: '删除',
      cancelButtonText: '取消'
    })
  } catch {
    return
  }
  try {
    await api.del('/media-nodes/' + row.id)
    ElMessage.success('已删除')
    await loadNodes()
  } catch (e: any) {
    ElMessage.error(e?.msg || '删除失败')
  }
}

/* 查看节点上的流列表 */
const streamsDlg = reactive({ visible: false, node: null as any, items: [] as any[], loading: false })

async function showStreams(row: any) {
  streamsDlg.node = row
  streamsDlg.items = []
  streamsDlg.visible = true
  streamsDlg.loading = true
  try {
    const res: any = await api.get('/media-nodes/' + row.id + '/streams')
    streamsDlg.items = res?.items || []
  } catch (e: any) {
    ElMessage.error(e?.msg || '获取流列表失败')
  } finally {
    streamsDlg.loading = false
  }
}

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
  <div class="page">
    <el-card shadow="never">
      <template #header>
        <div class="head">
          <span>媒体节点</span>
          <div class="head-ops">
            <el-button @click="loadNodes" :loading="loading">刷新</el-button>
            <el-button type="primary" @click="openNodeDlg('create')">新建节点</el-button>
          </div>
        </div>
      </template>

      <el-table v-loading="loading" :data="nodes" border>
        <el-table-column prop="name" label="名称" min-width="120" />
        <el-table-column prop="apiUrl" label="API 地址" min-width="180" show-overflow-tooltip />
        <el-table-column label="公网地址" min-width="150" show-overflow-tooltip>
          <template #default="{ row }">{{ row.publicHost || '-' }}</template>
        </el-table-column>
        <el-table-column label="状态" width="80" align="center">
          <template #default="{ row }">
            <el-tag :type="statusTag(row.status).type" size="small">{{ statusTag(row.status).text }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="流数" width="100" align="center">
          <template #default="{ row }">{{ row.streams ?? 0 }} / {{ row.maxStreams ?? 0 }}</template>
        </el-table-column>
        <el-table-column prop="weight" label="权重" width="70" align="center" />
        <el-table-column label="最近心跳" width="110" align="center">
          <template #default="{ row }">{{ ago(row.lastKeepalive) }}</template>
        </el-table-column>
        <el-table-column label="操作" width="230" align="center" fixed="right">
          <template #default="{ row }">
            <el-button link type="primary" @click="openNodeDlg('edit', row)">编辑</el-button>
            <el-button link type="primary" :loading="checkingId === row.id" @click="selfcheck(row)">自检</el-button>
            <el-button link type="primary" @click="showStreams(row)">流列表</el-button>
            <el-button link type="danger" @click="removeNode(row)">删除</el-button>
          </template>
        </el-table-column>
      </el-table>
      <el-empty v-if="!loading && !nodes.length" description="暂无媒体节点" :image-size="80" />
    </el-card>

    <!-- 新建/编辑节点对话框 -->
    <el-dialog
      v-model="nodeDlg.visible"
      :title="nodeDlg.mode === 'create' ? '新建媒体节点' : '编辑媒体节点'"
      width="560px"
    >
      <el-form label-width="120px" @submit.prevent>
        <el-form-item label="节点名称" required>
          <el-input v-model="nodeDlg.form.name" placeholder="节点名称" maxlength="50" />
        </el-form-item>
        <el-form-item label="API 地址" required>
          <el-input v-model="nodeDlg.form.apiUrl" placeholder="http://host:port" maxlength="200" />
        </el-form-item>
        <el-form-item :label="nodeDlg.mode === 'create' ? '接入密钥' : '接入密钥（可选）'" :required="nodeDlg.mode === 'create'">
          <el-input
            v-model="nodeDlg.form.secret"
            show-password
            :placeholder="nodeDlg.mode === 'create' ? '节点接入密钥' : '留空表示不修改'"
            maxlength="128"
          />
        </el-form-item>
        <el-form-item label="公网地址">
          <el-input v-model="nodeDlg.form.publicHost" placeholder="公网访问域名或 IP" maxlength="200" />
        </el-form-item>
        <el-form-item label="RTMP 端口">
          <el-input-number v-model="nodeDlg.form.rtmpPort" :min="1" :max="65535" controls-position="right" />
        </el-form-item>
        <el-form-item label="HTTP 端口">
          <el-input-number v-model="nodeDlg.form.httpPort" :min="1" :max="65535" controls-position="right" />
        </el-form-item>
        <el-form-item label="HTTPS 端口">
          <el-input-number v-model="nodeDlg.form.httpsPort" :min="1" :max="65535" controls-position="right" />
        </el-form-item>
        <el-form-item label="RTP 端口范围">
          <el-input v-model="nodeDlg.form.rtpRange" placeholder="30000-30100" maxlength="30" style="width: 180px" />
        </el-form-item>
        <el-form-item label="最大流数">
          <el-input-number v-model="nodeDlg.form.maxStreams" :min="1" :max="10000" controls-position="right" />
        </el-form-item>
        <el-form-item label="权重">
          <el-input-number v-model="nodeDlg.form.weight" :min="1" :max="1000" controls-position="right" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="nodeDlg.visible = false">取消</el-button>
        <el-button type="primary" :loading="nodeDlg.saving" @click="saveNode">确定</el-button>
      </template>
    </el-dialog>

    <!-- 流列表对话框 -->
    <el-dialog
      v-model="streamsDlg.visible"
      :title="'流列表 - ' + (streamsDlg.node?.name || '')"
      width="720px"
    >
      <el-table v-loading="streamsDlg.loading" :data="streamsDlg.items" border max-height="420">
        <el-table-column prop="app" label="应用" min-width="100" />
        <el-table-column prop="stream" label="流名称" min-width="180" show-overflow-tooltip />
        <el-table-column prop="kind" label="类型" width="90" align="center" />
        <el-table-column label="开始时间" width="170">
          <template #default="{ row }">{{ fmtTime(row.startedAt) }}</template>
        </el-table-column>
      </el-table>
      <el-empty
        v-if="!streamsDlg.loading && !streamsDlg.items.length"
        description="该节点暂无推流"
        :image-size="60"
      />
    </el-dialog>
  </div>
</template>

<style scoped>
.head { display: flex; justify-content: space-between; align-items: center; }
.head-ops { display: flex; gap: 8px; }
</style>