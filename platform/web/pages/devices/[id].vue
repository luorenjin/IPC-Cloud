<script setup lang="ts">
// 设备详情（MGR-03）：概览 / 通道 / 诊断 / 日志
const route = useRoute()
const api = useApi()
const devId = String(route.params.id)

const dev = ref<any>(null)
const loading = ref(false)

const channels = computed(() => dev.value?.channels || [])
const metrics = computed(() => dev.value?.metrics || null)
// 能力集（字符串或对象均兼容）
const caps = computed<string[]>(() =>
  (dev.value?.capabilities || dev.value?.caps || [])
    .map((c: any) => (typeof c === 'string' ? c : c.name || c.key || ''))
    .filter(Boolean))

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
function streamInfo(ch: any) {
  const s = ch.streamStatus ?? ch.status ?? ''
  if (s === 'online' || s === 'live') return { label: '推流中', type: 'success' }
  if (s === 'offline' || s === '') return { label: '未推流', type: 'info' }
  return { label: String(s), type: 'warning' }
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
// 指标格式化（百分比 / 温度）
function pct(v: any) {
  if (v == null) return '-'
  const n = typeof v === 'number' ? v : parseFloat(v)
  return isNaN(n) ? String(v) : n + '%'
}
function temp(v: any) {
  if (v == null) return '-'
  const n = typeof v === 'number' ? v : parseFloat(v)
  return isNaN(n) ? String(v) : n + '℃'
}

// ================= 诊断 =================
const diag = reactive({ loading: false, items: [] as any[] })
async function runDiag() {
  diag.loading = true
  diag.items = []
  try {
    const res: any = await api.post(`/devices/${devId}/diag`)
    diag.items = res.items || res.checks || (Array.isArray(res) ? res : [])
  } catch (e: any) {
    ElMessage.error(e.msg || '诊断失败')
  } finally {
    diag.loading = false
  }
}

// ================= 通道操作 =================
async function toggleCh(ch: any, val: any) {
  try {
    await api.put(`/channels/${ch.id}`, { enabled: !!val })
    ch.enabled = !!val
    ElMessage.success('已更新')
  } catch (e: any) {
    ElMessage.error(e.msg || '设置失败')
  }
}
async function refreshCover(ch: any) {
  try {
    await api.post(`/channels/${ch.id}/cover`)
    ElMessage.success('封面刷新任务已提交')
  } catch (e: any) {
    ElMessage.error(e.msg || '操作失败')
  }
}
const snapDlg = reactive({ show: false, src: '' })
async function takeSnap(ch: any) {
  try {
    const res: any = await api.post(`/channels/${ch.id}/snapshot`)
    snapDlg.src = res.url || res.snapshot || res.dataUrl || (typeof res === 'string' ? res : '')
    snapDlg.show = true
  } catch (e: any) {
    ElMessage.error(e.msg || '抓图失败')
  }
}

// ================= 编辑 =================
const editDlg = reactive({ show: false, name: '', location: '', remark: '' })
function openEdit() {
  editDlg.name = dev.value?.name || ''
  editDlg.location = dev.value?.location || ''
  editDlg.remark = dev.value?.remark || ''
  editDlg.show = true
}
async function saveEdit() {
  try {
    await api.put(`/devices/${devId}`, { name: editDlg.name, location: editDlg.location, remark: editDlg.remark })
    ElMessage.success('已保存')
    editDlg.show = false
    load()
  } catch (e: any) {
    ElMessage.error(e.msg || '保存失败')
  }
}

// ================= 操作日志行（字段容错） =================
const opRows = computed(() => (dev.value?.recentOps || []).map((o: any) => ({
  ts: o.ts || o.time || o.createdAt || 0,
  action: o.action || o.type || '-',
  operator: o.operator || o.user || o.username || '-',
  ok: o.ok ?? !(o.result === '失败' || o.result === 'fail' || o.result === 'error'),
  msg: o.msg || o.detail || ''
})))

async function load() {
  loading.value = true
  try {
    dev.value = await api.get(`/devices/${devId}`)
  } catch (e: any) {
    ElMessage.error(e.msg || '加载失败')
  } finally {
    loading.value = false
  }
}
onMounted(load)
</script>

<template>
  <div class="page">
    <el-card shadow="never" v-loading="loading">
      <div class="head">
        <el-button size="small" @click="navigateTo('/devices')">返回</el-button>
        <span class="name">{{ dev?.name || '设备详情' }}</span>
        <el-tag v-if="dev" :type="statusInfo(dev.status).type" size="small">
          {{ statusInfo(dev.status).label }}
        </el-tag>
        <div class="grow" />
        <el-button type="primary" plain @click="openEdit">编辑</el-button>
      </div>

      <el-tabs>
        <!-- a) 概览 -->
        <el-tab-pane label="概览">
          <el-descriptions :column="3" border class="mb12">
            <el-descriptions-item label="名称">{{ dev?.name || '-' }}</el-descriptions-item>
            <el-descriptions-item label="来源">
              <el-tag :type="srcInfo(dev?.source).type" size="small" effect="plain">
                {{ srcInfo(dev?.source).label }}
              </el-tag>
            </el-descriptions-item>
            <el-descriptions-item label="状态">
              <el-tag :type="statusInfo(dev?.status).type" size="small">
                {{ statusInfo(dev?.status).label }}
              </el-tag>
            </el-descriptions-item>
            <el-descriptions-item label="型号">{{ dev?.model || '-' }}</el-descriptions-item>
            <el-descriptions-item label="厂商">{{ dev?.vendor || '-' }}</el-descriptions-item>
            <el-descriptions-item label="固件">{{ dev?.firmware || dev?.firmwareVersion || '-' }}</el-descriptions-item>
            <el-descriptions-item label="IP">{{ dev?.ip || '-' }}</el-descriptions-item>
            <el-descriptions-item label="MAC">{{ dev?.mac || '-' }}</el-descriptions-item>
            <el-descriptions-item label="安装位置">{{ dev?.location || '-' }}</el-descriptions-item>
          </el-descriptions>

          <div class="sec-title">能力集</div>
          <div class="caps">
            <el-tag v-for="(c, i) in caps" :key="i" size="small" effect="plain" class="cap">{{ c }}</el-tag>
            <span v-if="!caps.length" class="empty">无</span>
          </div>

          <template v-if="metrics">
            <div class="sec-title">运行指标</div>
            <el-row :gutter="12">
              <el-col :span="8">
                <el-card shadow="never" class="metric">
                  <div class="ml">CPU</div>
                  <div class="mv">{{ pct(metrics.cpu) }}</div>
                </el-card>
              </el-col>
              <el-col :span="8">
                <el-card shadow="never" class="metric">
                  <div class="ml">内存</div>
                  <div class="mv">{{ pct(metrics.memory ?? metrics.mem) }}</div>
                </el-card>
              </el-col>
              <el-col :span="8">
                <el-card shadow="never" class="metric">
                  <div class="ml">温度</div>
                  <div class="mv">{{ temp(metrics.temperature ?? metrics.temp) }}</div>
                </el-card>
              </el-col>
            </el-row>
          </template>
        </el-tab-pane>

        <!-- b) 通道 -->
        <el-tab-pane label="通道">
          <el-table :data="channels">
            <el-table-column prop="name" label="名称" min-width="140" show-overflow-tooltip />
            <el-table-column label="通道号" width="90">
              <template #default="{ row }">{{ row.channelNo ?? row.num ?? row.channel ?? '-' }}</template>
            </el-table-column>
            <el-table-column label="启用" width="90">
              <template #default="{ row }">
                <el-switch :model-value="!!row.enabled" @change="toggleCh(row, $event)" />
              </template>
            </el-table-column>
            <el-table-column label="流状态" width="100">
              <template #default="{ row }">
                <el-tag :type="streamInfo(row).type" size="small">{{ streamInfo(row).label }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="180">
              <template #default="{ row }">
                <el-button link type="primary" size="small" @click="refreshCover(row)">刷新封面</el-button>
                <el-button link type="primary" size="small" @click="takeSnap(row)">快照</el-button>
              </template>
            </el-table-column>
          </el-table>
          <div v-if="!channels.length" class="empty">暂无通道</div>
        </el-tab-pane>

        <!-- c) 诊断 -->
        <el-tab-pane label="诊断">
          <el-button type="primary" :loading="diag.loading" @click="runDiag">开始诊断</el-button>
          <div v-if="diag.items.length" class="diag-list">
            <div v-for="(it, i) in diag.items" :key="i" class="diag-item">
              <span class="mark" :class="it.ok ? 'ok' : 'err'">{{ it.ok ? '✓' : '✕' }}</span>
              <span class="diag-name">{{ it.name || it.item || '检查项' }}</span>
              <span class="diag-msg" :class="{ err: !it.ok }">{{ it.msg || '' }}</span>
            </div>
          </div>
          <div v-else-if="!diag.loading" class="empty">点击"开始诊断"检查设备连通性与配置</div>
        </el-tab-pane>

        <!-- d) 日志 -->
        <el-tab-pane label="日志">
          <el-table :data="opRows" size="small">
            <el-table-column label="时间" width="120">
              <template #default="{ row }">{{ ago(row.ts) }}</template>
            </el-table-column>
            <el-table-column prop="action" label="操作" width="150" />
            <el-table-column prop="operator" label="操作人" width="130" />
            <el-table-column label="结果" width="90">
              <template #default="{ row }">
                <el-tag :type="row.ok ? 'success' : 'danger'" size="small">{{ row.ok ? '成功' : '失败' }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="msg" label="详情" show-overflow-tooltip />
          </el-table>
          <div v-if="!opRows.length" class="empty">暂无操作记录</div>
        </el-tab-pane>
      </el-tabs>
    </el-card>

    <!-- 快照弹窗 -->
    <el-dialog v-model="snapDlg.show" title="通道快照" width="580px">
      <img v-if="snapDlg.src" :src="snapDlg.src" class="snap-img" alt="快照">
      <el-empty v-else description="暂无快照数据" />
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
  </div>
</template>

<style scoped>
.head { display: flex; align-items: center; gap: 10px; margin-bottom: 8px; }
.name { font-size: 16px; font-weight: 700; color: #303133; }
.grow { flex: 1; }
.mb12 { margin-bottom: 16px; }
.sec-title { font-size: 13px; color: #909399; margin: 12px 0 8px; }
.caps { display: flex; flex-wrap: wrap; gap: 8px; }
.metric { text-align: center; }
.metric .ml { font-size: 13px; color: #909399; }
.metric .mv { font-size: 22px; font-weight: 700; color: #303133; margin-top: 4px; }
.diag-list { margin-top: 12px; }
.diag-item { display: flex; align-items: baseline; gap: 8px; padding: 6px 0; border-bottom: 1px dashed #eee; }
.mark { font-weight: 700; }
.mark.ok { color: #67c23a; }
.mark.err { color: #f56c6c; }
.diag-name { width: 110px; color: #303133; flex-shrink: 0; }
.diag-msg { color: #909399; font-size: 13px; }
.diag-msg.err { color: #f56c6c; }
.snap-img { width: 100%; display: block; }
.empty { color: #909399; font-size: 13px; text-align: center; padding: 24px 0; }
</style>