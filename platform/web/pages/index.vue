<script setup lang="ts">
// 仪表盘（DASH-01）：统计概览 / 来源分布 / 节点健康 / 最近告警
const api = useApi()
const dash = ref<any>(null)

// 来源设备数分布
const srcMeta: Record<string, { label: string; color: string }> = {
  idp: { label: '自有设备', color: '#409eff' },
  gb28181: { label: '国标', color: '#67c23a' },
  onvif: { label: 'ONVIF', color: '#e6a23c' },
  rtsp: { label: 'RTSP', color: '#909399' }
}
const srcRows = computed(() =>
  Object.keys(srcMeta).map((k) => ({
    key: k, label: srcMeta[k].label, color: srcMeta[k].color,
    count: dash.value?.bySource?.[k] ?? 0
  })))

// 在线率
const onlineRate = computed(() => {
  const t = dash.value?.deviceTotal || 0
  const o = dash.value?.deviceOnline || 0
  return t ? ((o / t) * 100).toFixed(1) + '%' : '-'
})

// 节点健康行（字段容错）
const nodeRows = computed(() => (dash.value?.nodes || []).map((n: any) => ({
  id: n.id, name: n.name, status: n.status || 'offline',
  streams: n.streams ?? n.streamCount ?? 0,
  max: n.maxStreams ?? n.max ?? '-'
})))
function nodeTag(s: string) {
  if (s === 'online' || s === 'ok') return { label: '在线', type: 'success' as const }
  if (s === 'error') return { label: '异常', type: 'danger' as const }
  if (s === 'offline') return { label: '离线', type: 'info' as const }
  return { label: '未知', type: 'warning' as const }
}

// 最近告警（字段容错，最多 10 条）
const alarmRows = computed(() => (dash.value?.recentAlarms || []).slice(0, 10).map((a: any) => ({
  id: a.id,
  level: a.level || a.severity || 'info',
  msg: a.msg || a.content || a.message || '告警事件',
  src: a.sourceName || a.deviceName || a.source || '',
  ts: a.ts || a.time || a.createdAt || 0
})))
function levelTag(l: string) {
  if (l === 'critical' || l === 'high' || l === 'error') return { label: '严重', type: 'danger' as const }
  if (l === 'warning' || l === 'medium') return { label: '警告', type: 'warning' as const }
  return { label: '提示', type: 'info' as const }
}

// 相对时间
function ago(ts: number) {
  const s = Math.floor((Date.now() - ts) / 1000)
  if (s < 60) return '刚刚'
  if (s < 3600) return Math.floor(s / 60) + ' 分钟前'
  if (s < 86400) return Math.floor(s / 3600) + ' 小时前'
  return Math.floor(s / 86400) + ' 天前'
}

async function load() {
  try { dash.value = await api.get('/dashboard') } catch (e: any) { ElMessage.error(e.msg || '加载失败') }
}
onMounted(load)

// 实时事件：新告警 / 设备上下线 → 重新拉取
useWs((ev: any) => {
  if (['alarm.new', 'device.online', 'device.offline'].includes(ev.type)) load()
})
</script>

<template>
  <div class="dash">
    <!-- 统计卡片行 -->
    <el-row :gutter="12" class="mb12">
      <el-col :span="5">
        <el-card shadow="never" class="stat">
          <div class="stat-label">设备总数</div>
          <div class="stat-val">{{ dash?.deviceTotal ?? '-' }}</div>
        </el-card>
      </el-col>
      <el-col :span="5">
        <el-card shadow="never" class="stat">
          <div class="stat-label">在线设备</div>
          <div class="stat-val green">{{ dash?.deviceOnline ?? '-' }} <span class="sub">{{ onlineRate }}</span></div>
        </el-card>
      </el-col>
      <el-col :span="5">
        <el-card shadow="never" class="stat">
          <div class="stat-label">通道数</div>
          <div class="stat-val">{{ dash?.channelTotal ?? '-' }}</div>
        </el-card>
      </el-col>
      <el-col :span="5">
        <el-card shadow="never" class="stat">
          <div class="stat-label">当前播放</div>
          <div class="stat-val">{{ dash?.playing ?? '-' }} <span class="sub">路</span></div>
        </el-card>
      </el-col>
      <el-col :span="4">
        <el-card shadow="never" class="stat">
          <div class="stat-label">今日告警</div>
          <div class="stat-val red">{{ dash?.alarmToday ?? '-' }}</div>
        </el-card>
      </el-col>
    </el-row>

    <!-- 按来源设备数 -->
    <el-row :gutter="12" class="mb12">
      <el-col v-for="s in srcRows" :key="s.key" :span="6">
        <el-card shadow="never" class="src-card">
          <span class="dot" :style="{ background: s.color }" />
          <span class="src-label">{{ s.label }}</span>
          <span class="src-count">{{ s.count }}</span>
        </el-card>
      </el-col>
    </el-row>

    <el-row :gutter="12">
      <!-- 节点健康 -->
      <el-col :span="14">
        <el-card shadow="never">
          <template #header>媒体节点健康</template>
          <el-table :data="nodeRows" size="small">
            <el-table-column prop="name" label="名称" min-width="120" show-overflow-tooltip />
            <el-table-column label="状态" width="90">
              <template #default="{ row }">
                <el-tag :type="nodeTag(row.status).type" size="small">{{ nodeTag(row.status).label }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="流数" width="80">
              <template #default="{ row }">{{ row.streams }}</template>
            </el-table-column>
            <el-table-column label="上限" width="80">
              <template #default="{ row }">{{ row.max }}</template>
            </el-table-column>
          </el-table>
        </el-card>
      </el-col>
      <!-- 最近告警 -->
      <el-col :span="10">
        <el-card shadow="never">
          <template #header>最近告警</template>
          <div v-if="!alarmRows.length" class="empty">暂无告警</div>
          <div
            v-for="a in alarmRows" :key="a.id" class="alarm-item"
            @click="navigateTo('/alarms')"
          >
            <el-tag :type="levelTag(a.level).type" size="small" effect="plain">{{ levelTag(a.level).label }}</el-tag>
            <span class="alarm-msg">{{ a.msg }}</span>
            <span class="alarm-time">{{ a.src ? a.src + ' · ' : '' }}{{ ago(a.ts) }}</span>
          </div>
        </el-card>
      </el-col>
    </el-row>
  </div>
</template>

<style scoped>
.mb12 { margin-bottom: 12px; }
.stat-label { font-size: 13px; color: #909399; }
.stat-val { font-size: 26px; font-weight: 700; color: #303133; margin-top: 4px; }
.stat-val .sub { font-size: 13px; font-weight: 400; color: #909399; margin-left: 4px; }
.green { color: #67c23a; }
.red { color: #f56c6c; }
.src-card { display: flex; }
.dot { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 8px; }
.src-label { color: #606266; }
.src-count { float: right; font-weight: 700; color: #303133; }
.alarm-item {
  display: flex; align-items: center; gap: 8px; padding: 8px 4px;
  border-bottom: 1px solid #f0f0f0; cursor: pointer;
}
.alarm-item:hover { background: #f5f7fa; }
.alarm-msg { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 13px; color: #303133; }
.alarm-time { font-size: 12px; color: #909399; white-space: nowrap; }
.empty { color: #909399; font-size: 13px; text-align: center; padding: 24px 0; }
</style>