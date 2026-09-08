<script setup lang="ts">
// 设备详情（MGR-03）：概览 / 通道 / 配置 / 诊断 / 日志（PRD 五 Tab）
const route = useRoute()
const api = useApi()
const toast = useToast()
const devId = String(route.params.id)

const dev = ref<any>(null)
const loading = ref(false)
const tab = ref('overview')

const channels = computed(() => dev.value?.channels || [])
const metrics = computed(() => dev.value?.metrics || null)
const caps = computed<string[]>(() =>
  (dev.value?.capabilities || dev.value?.caps || [])
    .map((c: any) => (typeof c === 'string' ? c : c.name || c.key || ''))
    .filter(Boolean))
function hasCap(prefix: string) {
  return caps.value.some((c) => c.startsWith(prefix))
}

const srcMap: Record<string, { label: string; color: string }> = {
  idp: { label: '自有', color: 'idp' },
  gb28181: { label: '国标', color: 'gb' },
  onvif: { label: 'ONVIF', color: 'onvif' },
  rtsp: { label: 'RTSP', color: 'rtsp' }
}
function srcInfo(s?: string) { return srcMap[s || ''] || { label: s || '—', color: 'default' } }
function statusInfo(s?: string) {
  if (s === 'online') return { label: '在线', color: 'success' }
  if (s === 'pending') return { label: '待确认', color: 'warning' }
  if (s === 'error') return { label: '错误', color: 'danger' }
  return { label: '离线', color: 'info' }
}
function streamInfo(ch: any) {
  const s = ch.streamStatus ?? ch.status ?? ''
  if (s === 'online' || s === 'live') return { label: '推流中', color: 'success' }
  if (s === 'offline' || s === '') return { label: '未推流', color: 'info' }
  return { label: String(s), color: 'warning' }
}
function ago(ts?: number) {
  if (!ts) return '—'
  const s = Math.floor((Date.now() - ts) / 1000)
  if (s < 60) return '刚刚'
  if (s < 3600) return Math.floor(s / 60) + ' 分钟前'
  if (s < 86400) return Math.floor(s / 3600) + ' 小时前'
  return Math.floor(s / 86400) + ' 天前'
}
function pct(v: any) {
  if (v == null) return '—'
  const n = typeof v === 'number' ? v : parseFloat(v)
  return isNaN(n) ? String(v) : n + '%'
}
function temp(v: any) {
  if (v == null) return '—'
  const n = typeof v === 'number' ? v : parseFloat(v)
  return isNaN(n) ? String(v) : n + '℃'
}

// 概览字段（空字段隐藏，修复审计 B7）
const infoRows = computed(() => {
  const d = dev.value || {}
  return [
    { label: '设备 ID', value: d.id },
    { label: '型号', value: d.model },
    { label: '厂商', value: d.vendor },
    { label: '固件', value: d.firmware || d.firmwareVersion },
    { label: 'IP', value: d.ip },
    { label: 'MAC', value: d.mac },
    { label: '安装位置', value: d.location },
    { label: '备注', value: d.remark },
    { label: '最后在线', value: d.lastOnline || d.lastSeen ? ago(d.lastOnline || d.lastSeen) : '' }
  ].filter((r) => r.value !== undefined && r.value !== null && r.value !== '')
})

// ================= 诊断 =================
const diag = reactive({ loading: false, items: [] as any[] })
async function runDiag() {
  diag.loading = true
  diag.items = []
  try {
    const res: any = await api.post(`/devices/${devId}/diag`)
    diag.items = res.items || res.checks || (Array.isArray(res) ? res : [])
  } catch (e: any) {
    toastApiError(e, '诊断失败')
  } finally {
    diag.loading = false
  }
}

// ================= 通道操作 =================
async function toggleCh(ch: any, val: any) {
  try {
    await api.put(`/channels/${ch.id}`, { enabled: !!val })
    ch.enabled = !!val
    toast.success('已更新')
  } catch (e: any) {
    toastApiError(e, '设置失败')
  }
}
async function refreshCover(ch: any) {
  try {
    await api.post(`/channels/${ch.id}/cover`)
    toast.success('封面刷新任务已提交')
  } catch (e: any) {
    toastApiError(e, '操作失败')
  }
}
const snapDlg = reactive({ show: false, src: '' })
async function takeSnap(ch: any) {
  try {
    const res: any = await api.post(`/channels/${ch.id}/snapshot`)
    snapDlg.src = res.url || res.snapshot || res.dataUrl || (typeof res === 'string' ? res : '')
    snapDlg.show = true
  } catch (e: any) {
    toastApiError(e, '抓图失败')
  }
}

// ================= 远程配置（MGR-09，按能力集渲染） =================
const cfg = reactive({ loading: false, saving: false, data: {} as any, denied: [] as string[], supported: [] as string[] })
// 配置键 → 中文标签（与后端 cfgKeys 对齐）
const CFG_LABELS: Record<string, string> = {
  'video.main.resolution': '分辨率', 'video.main.fps': '帧率', 'video.main.bitrate': '码率(kbps)',
  'video.main.gop': 'GOP', 'video.main.encode': '编码格式',
  'image.brightness': '亮度', 'image.contrast': '对比度', 'image.saturation': '饱和度',
  'image.sharpness': '锐度', 'image.mirror': '镜像', 'image.wdr': 'WDR',
  'osd.enable': 'OSD 开关', 'osd.text': 'OSD 文字', 'record.mode': '录像模式',
  'alarm.motion.sensitivity': '移动侦测灵敏度', 'time.ntp': 'NTP 服务器'
}
const cfgFields = computed(() =>
  (cfg.supported.length ? cfg.supported : Object.keys(CFG_LABELS)).map((k) => ({ key: k, label: CFG_LABELS[k] || k })))
async function loadCfg() {
  cfg.loading = true
  cfg.denied = []
  try {
    const res: any = await api.get(`/devices/${devId}/config`)
    cfg.data = res?.config || {}
    cfg.supported = res?.supported || []
  } catch (e: any) {
    toastApiError(e, '配置读取失败')
    cfg.data = {}
  } finally {
    cfg.loading = false
  }
}
watch(tab, (t) => { if (t === 'config' && !Object.keys(cfg.data).length) loadCfg() })
async function saveCfg() {
  cfg.saving = true
  try {
    const res: any = await api.put(`/devices/${devId}/config`, { values: cfg.data })
    cfg.denied = res?.rejected || []
    toast.success(cfg.denied.length ? '部分配置项被设备拒绝' : '配置已下发，5 秒内回读确认')
    setTimeout(loadCfg, 5000)
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    cfg.saving = false
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
    toast.success('已保存')
    editDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  }
}

// ================= 重启 / 转移 / 升级 / 安全删除（MGR-08/10/11/12） =================
const confirmBox = useConfirm()
const router = useRouter()

async function rebootDevice() {
  const ok = await confirmBox.ask({
    title: '远程重启设备',
    message: `确定重启设备「${dev.value?.name || devId}」？`,
    detail: '重启过程通常持续 30-60 秒，期间视频预览将短暂中断。',
    confirmText: '立即重启'
  })
  if (!ok) return
  try {
    await api.post(`/devices/${devId}/reboot`)
    toast.success('重启指令已下发')
  } catch (e: any) {
    toastApiError(e, '重启失败')
  }
}

const moveDlg = reactive({ show: false, groupId: '', saving: false })
const groups = ref<any[]>([])
async function openMoveDlg() {
  try {
    const res: any = await api.get('/groups')
    groups.value = res?.items || []
  } catch {}
  moveDlg.groupId = dev.value?.groupId || ''
  moveDlg.show = true
}
async function saveMove() {
  if (!moveDlg.groupId) return toast.warning('请选择目标分组')
  moveDlg.saving = true
  try {
    await api.put(`/devices/${devId}`, { groupId: moveDlg.groupId })
    toast.success('分组转移成功')
    moveDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, '转移失败')
  } finally {
    moveDlg.saving = false
  }
}

const upgradeDlg = reactive({ show: false, version: '', file: '', saving: false })
function openUpgrade() {
  upgradeDlg.version = ''
  upgradeDlg.file = ''
  upgradeDlg.show = true
}
async function submitUpgrade() {
  if (!upgradeDlg.version.trim()) return toast.warning('请输入目标固件版本号')
  upgradeDlg.saving = true
  try {
    await api.post(`/devices/${devId}/upgrade`, { version: upgradeDlg.version.trim(), file: upgradeDlg.file })
    toast.success('固件升级任务已下发至任务中心')
    upgradeDlg.show = false
  } catch (e: any) {
    toastApiError(e, '固件升级触发失败')
  } finally {
    upgradeDlg.saving = false
  }
}

async function askDeleteDevice() {
  const devName = dev.value?.name || devId
  const ok = await confirmBox.ask({
    title: '删除设备二次确认',
    message: `将永久删除设备「${devName}」及其全部通道与录像配置！`,
    detail: '为防止误操作，请在下方输入框中完整输入设备名称以确认删除。',
    danger: true,
    confirmText: '确认删除',
    inputConfirm: devName,
    inputPlaceholder: devName
  })
  if (!ok) return
  try {
    await api.request(`/devices/${devId}`, { method: 'DELETE', body: { confirmName: devName } })
    toast.success('设备已成功删除')
    router.push('/devices')
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

// ================= 操作日志行 =================
const opRows = computed(() => (dev.value?.recentOps || []).map((o: any) => ({
  ts: o.ts || o.time || o.createdAt || 0,
  action: o.action || o.type || '—',
  operator: o.operator || o.user || o.username || '—',
  ok: o.ok ?? !(o.result === '失败' || o.result === 'fail' || o.result === 'error'),
  msg: o.msg || o.detail || ''
})))

async function load() {
  loading.value = true
  try {
    dev.value = await api.get(`/devices/${devId}`)
  } catch (e: any) {
    toastApiError(e, '加载失败')
  } finally {
    loading.value = false
  }
}
onMounted(load)
</script>

<template>
  <UiLoading :loading="loading">
    <UiCard flat>
      <!-- 头部 -->
      <div class="mb-1 flex items-center gap-2.5">
        <button class="flex items-center gap-1 rounded border border-line px-2 py-1 text-xs text-muted transition-colors hover:border-primary hover:text-primary" @click="navigateTo('/devices')">
          <Icon name="arrow-left" :size="12" />返回
        </button>
        <span class="text-base font-bold text-ink">{{ dev?.name || '设备详情' }}</span>
        <UiTag v-if="dev" :color="statusInfo(dev.status).color as any" dot>{{ statusInfo(dev.status).label }}</UiTag>
        <UiTag :color="srcInfo(dev?.source).color as any" plain>{{ srcInfo(dev?.source).label }}</UiTag>
        <div class="ml-auto flex flex-wrap items-center gap-2">
          <UiButton variant="primary" @click="openEdit"><Icon name="edit" :size="13" />编辑</UiButton>
          <UiButton @click="rebootDevice"><Icon name="refresh-cw" :size="13" />重启</UiButton>
          <UiButton @click="openMoveDlg"><Icon name="folder" :size="13" />转移分组</UiButton>
          <UiButton @click="openUpgrade"><Icon name="upload" :size="13" />固件升级</UiButton>
          <UiButton variant="dangerText" @click="askDeleteDevice"><Icon name="trash" :size="13" />删除设备</UiButton>
        </div>
      </div>

      <UiTabs v-model="tab" :items="[
        { label: '概览', value: 'overview' },
        { label: '通道', value: 'channels' },
        { label: '配置', value: 'config' },
        { label: '诊断', value: 'diag' },
        { label: '日志', value: 'logs' }
      ]">
        <!-- a) 概览 -->
        <div v-if="tab === 'overview'" class="space-y-4 pt-4">
          <div class="grid grid-cols-1 gap-x-8 gap-y-2 rounded border border-line md:grid-cols-3">
            <div v-for="r in infoRows" :key="r.label" class="flex border-b border-line-soft px-3 py-2 text-sm last:border-0">
              <span class="w-24 shrink-0 text-placeholder">{{ r.label }}</span>
              <span class="min-w-0 truncate text-ink" :title="String(r.value)">{{ r.value }}</span>
            </div>
          </div>

          <div>
            <p class="mb-2 text-xs text-placeholder">能力集</p>
            <div class="flex flex-wrap gap-2">
              <UiTag v-for="(c, i) in caps" :key="i" color="primary" plain>{{ c }}</UiTag>
              <span v-if="!caps.length" class="text-sm text-placeholder">无</span>
            </div>
          </div>

          <!-- status.metrics（IDP 运行指标，MGR-03） -->
          <div v-if="metrics">
            <p class="mb-2 text-xs text-placeholder">运行指标</p>
            <div class="grid grid-cols-2 gap-3 md:grid-cols-5">
              <div v-for="m in [
                { label: 'CPU', value: pct(metrics.cpu) },
                { label: '内存', value: pct(metrics.memory ?? metrics.mem) },
                { label: '温度', value: temp(metrics.temperature ?? metrics.temp) },
                { label: 'TF 卡', value: metrics.tfHealth || (metrics.tfTotal ? (metrics.tfUsed || 0) + '/' + metrics.tfTotal + 'GB' : '') },
                { label: '码率', value: metrics.bitrate ? metrics.bitrate + 'kbps' : '' }
              ]" :key="m.label" v-show="m.value && m.value !== '—'"
                class="rounded border border-line py-3 text-center">
                <p class="text-xs text-placeholder">{{ m.label }}</p>
                <p class="mt-1 text-lg font-bold text-ink">{{ m.value }}</p>
              </div>
            </div>
          </div>

          <!-- 最近事件 -->
          <div v-if="opRows.length">
            <p class="mb-2 text-xs text-placeholder">最近事件</p>
            <div class="space-y-1">
              <div v-for="(o, i) in opRows.slice(0, 5)" :key="i" class="flex items-center gap-2 text-[13px]">
                <span class="text-placeholder">{{ ago(o.ts) }}</span>
                <span class="text-body">{{ o.action }}</span>
                <UiTag :color="o.ok ? 'success' : 'danger'" plain>{{ o.ok ? '成功' : '失败' }}</UiTag>
              </div>
            </div>
          </div>
        </div>

        <!-- b) 通道 -->
        <div v-if="tab === 'channels'" class="pt-4">
          <UiTable
            :columns="[
              { key: 'name', label: '名称' },
              { key: 'no', label: '通道号', width: '90px', align: 'center' },
              { key: 'enabled', label: '启用', width: '90px', align: 'center' },
              { key: 'stream', label: '流状态', width: '100px' },
              { key: 'cover', label: '封面', width: '120px' },
              { key: 'ops', label: '操作', width: '180px', ellipsis: false }
            ]"
            :rows="channels" empty="暂无通道"
          >
            <template #no="{ row }">{{ row.channelNo ?? row.num ?? row.channel ?? '—' }}</template>
            <template #enabled="{ row }">
              <UiSwitch :model-value="!!row.enabled" size="sm" @update:model-value="toggleCh(row, $event)" />
            </template>
            <template #stream="{ row }"><UiTag :color="streamInfo(row).color as any">{{ streamInfo(row).label }}</UiTag></template>
            <template #cover="{ row }">
              <img v-if="row.coverUrl" :src="row.coverUrl" class="h-8 w-14 rounded object-cover" alt="">
              <span v-else class="text-xs text-placeholder">—</span>
            </template>
            <template #ops="{ row }">
              <UiButton variant="text" size="sm" @click="refreshCover(row)">刷新封面</UiButton>
              <UiButton variant="text" size="sm" @click="takeSnap(row)">快照</UiButton>
            </template>
          </UiTable>
        </div>

        <!-- c) 配置（MGR-09，能力灰显） -->
        <div v-if="tab === 'config'" class="pt-4">
          <UiLoading :loading="cfg.loading">
            <div class="min-h-40 space-y-4">
              <div v-if="dev?.source !== 'idp'" class="flex items-center gap-2 rounded border border-line bg-zone px-3 py-2.5 text-sm text-muted">
                <Icon name="info" :size="15" class="text-placeholder" />
                该设备来源（{{ srcInfo(dev?.source).label }}）不支持远程配置，功能已禁用。
              </div>
              <template v-else>
                <div class="grid grid-cols-1 gap-x-8 gap-y-3 md:grid-cols-2">
                  <div v-for="f in cfgFields" :key="f.key" class="flex items-center gap-3">
                    <label class="w-28 shrink-0 text-right text-sm text-muted">{{ f.label }}</label>
                    <UiInput
                      :model-value="String(cfg.data?.[f.key] ?? '')" size="sm" width="w-40"
                      @update:model-value="cfg.data[f.key] = $event"
                    />
                    <span v-if="cfg.denied.includes(f.key)" class="text-xs text-danger">被设备拒绝</span>
                  </div>
                </div>
                <div class="flex gap-2">
                  <UiButton variant="primary" :disabled="cfg.saving" @click="saveCfg">保存并下发</UiButton>
                  <UiButton @click="loadCfg">重新回读</UiButton>
                </div>
              </template>
            </div>
          </UiLoading>
        </div>

        <!-- d) 诊断 -->
        <div v-if="tab === 'diag'" class="space-y-3 pt-4">
          <UiButton variant="primary" :disabled="diag.loading" @click="runDiag">
            <Icon name="activity" :size="14" :class="diag.loading ? 'ipc-spin' : ''" />开始诊断
          </UiButton>
          <div v-if="diag.items.length" class="rounded border border-line">
            <div v-for="(it, i) in diag.items" :key="i" class="flex items-baseline gap-2 border-b border-line-soft px-3 py-2.5 text-sm last:border-0">
              <span class="font-bold" :class="it.ok ? 'text-success' : 'text-danger'">{{ it.ok ? '✓' : '✕' }}</span>
              <span class="w-32 shrink-0 text-ink">{{ it.name || it.item || '检查项' }}</span>
              <span class="text-[13px]" :class="it.ok ? 'text-muted' : 'text-danger'">{{ it.msg || '' }}</span>
              <span v-if="it.cost" class="ml-auto text-xs text-placeholder">{{ it.cost }}ms</span>
            </div>
          </div>
          <p v-else-if="!diag.loading" class="text-sm text-placeholder">点击"开始诊断"检查设备连通性与配置（诊断记录保留 7 天）</p>
        </div>

        <!-- e) 日志 -->
        <div v-if="tab === 'logs'" class="pt-4">
          <UiTable
            :columns="[
              { key: 'ts', label: '时间', width: '120px' },
              { key: 'action', label: '操作', width: '150px' },
              { key: 'operator', label: '操作人', width: '130px' },
              { key: 'ok', label: '结果', width: '90px' },
              { key: 'msg', label: '详情' }
            ]"
            :rows="opRows" empty="暂无操作记录"
          >
            <template #ts="{ row }">{{ ago(row.ts) }}</template>
            <template #ok="{ row }"><UiTag :color="row.ok ? 'success' : 'danger'">{{ row.ok ? '成功' : '失败' }}</UiTag></template>
          </UiTable>
        </div>
      </UiTabs>
    </UiCard>

    <!-- 快照弹窗 -->
    <UiDialog v-model:open="snapDlg.show" title="通道快照" width="max-w-xl">
      <img v-if="snapDlg.src" :src="snapDlg.src" class="block w-full rounded" alt="快照">
      <UiEmptyState v-else text="暂无快照数据" />
    </UiDialog>

    <!-- 编辑设备 -->
    <UiDialog v-model:open="editDlg.show" title="编辑设备" width="max-w-md">
      <div class="grid grid-cols-[80px_1fr] items-center gap-x-3 gap-y-3">
        <label class="text-right text-sm text-muted">名称</label>
        <UiInput v-model="editDlg.name" />
        <label class="text-right text-sm text-muted">安装位置</label>
        <UiInput v-model="editDlg.location" />
        <label class="text-right text-sm text-muted">备注</label>
        <UiInput v-model="editDlg.remark" />
      </div>
      <template #footer>
        <UiButton @click="editDlg.show = false">取消</UiButton>
        <UiButton variant="primary" @click="saveEdit">保存</UiButton>
      </template>
    </UiDialog>

    <!-- 转移分组弹窗 -->
    <UiDialog v-model:open="moveDlg.show" title="转移设备分组" width="max-w-sm">
      <div class="space-y-3">
        <label class="block text-xs text-muted">选择目标分组</label>
        <UiSelect v-model="moveDlg.groupId" :options="groups.map(g => ({ label: g.name, value: g.id }))" class="w-full" />
      </div>
      <template #footer>
        <UiButton @click="moveDlg.show = false">取消</UiButton>
        <UiButton variant="primary" :loading="moveDlg.saving" @click="saveMove">确定转移</UiButton>
      </template>
    </UiDialog>

    <!-- 固件升级弹窗 -->
    <UiDialog v-model:open="upgradeDlg.show" title="设备固件升级" width="max-w-md">
      <div class="space-y-3">
        <div>
          <label class="mb-1 block text-xs text-muted">目标固件版本 *</label>
          <UiInput v-model="upgradeDlg.version" placeholder="如：v1.2.0-rc1" />
        </div>
        <div>
          <label class="mb-1 block text-xs text-muted">升级包文件名 / URL (选填)</label>
          <UiInput v-model="upgradeDlg.file" placeholder="固件升级包路径或包名" />
        </div>
        <p class="text-xs text-placeholder">升级任务提交后将由后台统一分发，进度可在全局任务中心中实时查看。</p>
      </div>
      <template #footer>
        <UiButton @click="upgradeDlg.show = false">取消</UiButton>
        <UiButton variant="primary" :loading="upgradeDlg.saving" @click="submitUpgrade">开始升级</UiButton>
      </template>
    </UiDialog>
  </UiLoading>
</template>
