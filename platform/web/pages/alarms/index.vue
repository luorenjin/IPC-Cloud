<script setup lang="ts">
// 消息中心（ALM-06）：告警列表 / 详情抽屉 / 实时提醒 / 全部已读
const api = useApi()

// 类型中文映射（设备事件 + 平台事件）
const KIND_MAP: Record<string, string> = {
  motion: '移动侦测',
  humanoid: '人形',
  tamper: '视频遮挡',
  io: 'IO报警',
  device_offline: '设备离线',
  node_offline: '节点离线',
  stream_lost: '流中断',
  disk_full: '存储不足',
  tf_error: 'TF卡异常'
}
const DEVICE_KINDS = ['motion', 'humanoid', 'tamper', 'io', 'tf_error']
const PLATFORM_KINDS = ['device_offline', 'node_offline', 'stream_lost', 'disk_full']
// 级别标签色：warn 橙 / info 灰 / error 红
const LEVEL_TAG: Record<string, string> = { warn: 'warning', info: 'info', error: 'danger' }

const fmt = (ts: any) => new Date(Number(ts)).toLocaleString('zh-CN', { hour12: false })
const kindName = (k: string) => KIND_MAP[k] || k

// ---------- 列表状态 ----------
const tab = ref<'all' | 'device' | 'platform'>('all')
const kindFilter = ref('')
const page = ref(1)
const pageSize = 20
const total = ref(0)
const unread = ref(0)
const items = ref<any[]>([])
const loading = ref(false)

async function load() {
  loading.value = true
  try {
    const params: any = { page: page.value, pageSize }
    if (kindFilter.value) params.kind = kindFilter.value
    const res: any = await api.get('/alarms', params)
    let list: any[] = res.items || []
    // 前端按 tab 分类过滤
    if (tab.value === 'device') list = list.filter((i) => DEVICE_KINDS.includes(i.kind))
    else if (tab.value === 'platform') list = list.filter((i) => PLATFORM_KINDS.includes(i.kind))
    items.value = list
    total.value = res.total || 0
    unread.value = res.unread || 0
  } catch (e: any) {
    ElMessage.error(`${e.code} ${e.msg} ${e.suggest || ''}`)
  } finally {
    loading.value = false
  }
}

watch(tab, () => { page.value = 1; load() })
watch(kindFilter, () => { page.value = 1; load() })

// 全部已读
async function readAll() {
  try {
    await api.post('/alarms/read-all')
    unread.value = 0
    items.value.forEach((i) => (i.read = true))
    ElMessage.success('已全部标记为已读')
  } catch (e: any) {
    ElMessage.error(`${e.code} ${e.msg} ${e.suggest || ''}`)
  }
}

// ---------- 详情抽屉 ----------
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
    unread.value = Math.max(0, unread.value - 1)
  }
}

// 回放此刻：跳转回放页并定位到告警时间
function goPlayback() {
  if (!detail.value?.channelId) return
  navigateTo(`/playback?channelId=${detail.value.channelId}&ts=${detail.value.ts}`)
}

// 内容列：data.error?.msg || data.name || kind
function rowContent(r: any) {
  const d = r.data || {}
  return d.error?.msg || d.name || kindName(r.kind)
}

// 实时告警提醒（WebSocket）
useWs((ev: any) => {
  if (ev.type !== 'alarm.new') return
  const a = ev.alarm || ev.data || {}
  ElNotification({
    title: '新告警：' + kindName(a.kind || ev.kind || ''),
    message: a.error?.msg || a.name || '请到消息中心查看详情',
    type: 'warning'
  })
  unread.value++
  load()
})

onMounted(load)
</script>

<template>
  <div class="alarm-page">
    <!-- 分类 Tabs -->
    <el-tabs v-model="tab">
      <el-tab-pane label="全部" name="all" />
      <el-tab-pane label="设备事件" name="device" />
      <el-tab-pane label="平台事件" name="platform" />
    </el-tabs>

    <!-- 工具栏 -->
    <div class="toolbar">
      <el-select v-model="kindFilter" placeholder="全部类型" clearable style="width: 160px">
        <el-option v-for="(label, k) in KIND_MAP" :key="k" :label="label" :value="k" />
      </el-select>
      <el-badge :value="unread" :hidden="!unread">
        <el-button size="small" type="primary" plain @click="readAll">全部已读</el-button>
      </el-badge>
      <span class="unread-tip">未读 {{ unread }} 条</span>
    </div>

    <!-- 告警列表 -->
    <el-table v-loading="loading" :data="items" class="tbl" @row-click="openDetail">
      <el-table-column label="时间" width="175">
        <template #default="{ row }">{{ fmt(row.ts) }}</template>
      </el-table-column>
      <el-table-column label="类型" width="120">
        <template #default="{ row }">{{ kindName(row.kind) }}</template>
      </el-table-column>
      <el-table-column label="级别" width="90">
        <template #default="{ row }">
          <el-tag size="small" :type="LEVEL_TAG[row.level] || 'info'">{{ row.level }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="内容" min-width="220">
        <template #default="{ row }">{{ rowContent(row) }}</template>
      </el-table-column>
      <el-table-column label="已读" width="80">
        <template #default="{ row }">
          <el-tag v-if="!row.read" size="small" type="danger">未读</el-tag>
          <span v-else class="read-txt">已读</span>
        </template>
      </el-table-column>
    </el-table>

    <div class="pager">
      <el-pagination
        v-model:current-page="page" layout="total, prev, pager, next"
        :total="total" :page-size="pageSize" @current-change="load"
      />
    </div>

    <!-- 详情抽屉 -->
    <el-drawer v-model="drawer" title="告警详情" size="420px">
      <div v-if="detail" class="detail">
        <div class="d-row"><span class="d-k">类型</span>{{ kindName(detail.kind) }}</div>
        <div class="d-row"><span class="d-k">级别</span>{{ detail.level }}</div>
        <div class="d-row"><span class="d-k">时间</span>{{ fmt(detail.ts) }}</div>
        <div class="d-row"><span class="d-k">设备</span>{{ detail.deviceId || '-' }}</div>
        <div class="d-row"><span class="d-k">通道</span>{{ detail.channelId || '-' }}</div>
        <div class="d-k json-title">数据</div>
        <pre class="json">{{ JSON.stringify(detail.data, null, 2) }}</pre>
        <template v-if="detail.snapshotUrl">
          <div class="d-k json-title">快照</div>
          <img :src="detail.snapshotUrl" class="snapshot" />
        </template>
        <el-button
          type="primary" size="small" style="margin-top: 12px"
          :disabled="!detail.channelId" @click="goPlayback"
        >回放此刻</el-button>
      </div>
      <div v-else-if="detailLoading" class="empty">加载中…</div>
    </el-drawer>
  </div>
</template>

<style scoped>
.alarm-page { background: #fff; border: 1px solid #e4e7ed; border-radius: 6px; padding: 4px 16px 16px; }
.toolbar { display: flex; align-items: center; gap: 16px; margin-bottom: 12px; }
.unread-tip { color: #909399; font-size: 12px; }
.tbl { cursor: pointer; }
.read-txt { color: #909399; font-size: 12px; }
.pager { display: flex; justify-content: flex-end; margin-top: 12px; }

.detail { font-size: 13px; }
.d-row { margin-bottom: 10px; color: #303133; }
.d-k { color: #909399; display: inline-block; width: 44px; }
.json-title { display: block; margin: 6px 0 4px; }
.json {
  background: #f5f7fa; border-radius: 4px; padding: 8px;
  font-size: 12px; max-height: 200px; overflow: auto; margin: 0;
}
.snapshot { max-width: 100%; border-radius: 4px; display: block; margin-top: 4px; }
.empty { color: #909399; text-align: center; padding: 40px 0; }
</style>