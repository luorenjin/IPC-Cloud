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

// 来源展示名/颜色见 utils/enums.ts SOURCE_MAP（全站唯一来源，PRD §9.1）
// 设备/推流状态映射见 utils/enums.ts
const statusInfo = (s?: string) => deviceStatusInfo(s)
const streamInfo = (ch: any) => streamStatusInfo(ch.streamStatus ?? ch.status ?? '')
// 相对时间/百分比/温度格式化见 utils/format.ts
const pct = (v: any) => fmtPercent(v)
const temp = (v: any) => fmtTemp(v)

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

// ================= 诊断（MGR-07：读取 /devices/:id/diag 的 { results, at }） =================
const diag = reactive({ loading: false, ran: false, items: [] as any[] })
async function runDiag() {
  diag.loading = true
  diag.items = []
  try {
    const res: any = await api.post(`/devices/${devId}/diag`)
    diag.items = res?.results || []
    diag.ran = true
  } catch (e: any) {
    toastApiError(e, '诊断失败')
  } finally {
    diag.loading = false
  }
}
// 诊断项状态灯颜色映射：成功=绿点/失败=红点/结果未知（理论上不会出现，容错兜底）=脉冲琥珀
function diagDotClass(it: any) {
  if (it.ok === true) return 'bg-success'
  if (it.ok === false) return 'bg-danger'
  return 'bg-warning animate-pulse'
}

// ================= 健康仪表条（概览 Tab 顶部三段式，纯展示计算属性） =================
const barDot: Record<string, string> = { success: 'bg-success', warning: 'bg-warning', danger: 'bg-danger', info: 'bg-info' }
const barText: Record<string, string> = { success: 'text-success', warning: 'text-warning', danger: 'text-danger', info: 'text-info' }
const uptimeSeg = computed(() => {
  const d = dev.value || {}
  const st = statusInfo(d.status)
  const ts = d.lastOnline || d.lastSeen
  return { color: st.color, value: ts ? ago(ts) : '—', sub: `当前状态：${st.label}` }
})
const streamSeg = computed(() => {
  const chs = channels.value
  const total = chs.length
  const live = chs.filter((ch: any) => streamInfo(ch).color === 'success').length
  const color = !total ? 'info' : live === total ? 'success' : live > 0 ? 'warning' : 'info'
  return { color, value: total ? `${live}/${total} 路` : '无通道', sub: total ? '推流中通道数' : '该设备暂未上报通道' }
})
const diagSeg = computed(() => {
  if (diag.loading) return { color: 'warning', value: '诊断中', sub: '正在探测设备连通性…', pulse: true }
  if (!diag.ran) return { color: 'info', value: '尚未诊断', sub: '前往"诊断"页运行一键检测' }
  if (!diag.items.length) return { color: 'info', value: '未返回诊断项', sub: '设备本次未返回任何探测结果' }
  const failCount = diag.items.filter((it: any) => it.ok === false).length
  return failCount
    ? { color: 'danger', value: `${failCount} 项异常`, sub: `共 ${diag.items.length} 项检测` }
    : { color: 'success', value: '全部正常', sub: `共 ${diag.items.length} 项检测均通过` }
})
const healthSegs = computed(() => [
  { key: 'uptime', icon: 'clock', title: '在线时长', ...uptimeSeg.value },
  { key: 'stream', icon: 'video', title: '码流状态', ...streamSeg.value },
  { key: 'diag', icon: 'activity', title: '最近诊断结果', ...diagSeg.value }
])

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
const editDlg = reactive({ show: false, name: '', location: '', remark: '', saving: false })
function openEdit() {
  editDlg.name = dev.value?.name || ''
  editDlg.location = dev.value?.location || ''
  editDlg.remark = dev.value?.remark || ''
  editDlg.show = true
}
async function saveEdit() {
  const name = editDlg.name.trim()
  if (!name) return toast.warning('设备名称不能为空')
  editDlg.saving = true
  try {
    await api.put(`/devices/${devId}`, { name, location: editDlg.location.trim(), remark: editDlg.remark.trim() })
    toast.success('已保存')
    editDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    editDlg.saving = false
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
        <button class="flex items-center gap-1 rounded-chrome border border-line px-2 py-1 text-xs text-muted transition-colors hover:border-primary hover:text-primary" @click="navigateTo('/devices')">
          <Icon name="arrow-left" :size="12" />返回
        </button>
        <span class="text-base font-bold text-ink">{{ dev?.name || '设备详情' }}</span>
        <UiTag v-if="dev" :color="statusInfo(dev.status).color as any" dot>{{ statusInfo(dev.status).label }}</UiTag>
        <UiTag :color="sourceInfo(dev?.source).color as any" plain>{{ sourceInfo(dev?.source).label }}</UiTag>
        <div class="ml-auto flex flex-wrap items-center gap-2">
          <UiButton variant="primary" @click="openEdit"><Icon name="edit" :size="13" />编辑</UiButton>
          <UiButton @click="rebootDevice"><Icon name="refresh-cw" :size="13" />重启</UiButton>
          <UiButton @click="openMoveDlg"><Icon name="folder" :size="13" />转移分组</UiButton>
          <UiTooltip label="固件升级功能尚未开放">
            <span><UiButton disabled><Icon name="upload" :size="13" />固件升级</UiButton></span>
          </UiTooltip>
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
          <!-- 健康仪表条：在线时长 / 码流状态 / 最近诊断结果，三段式横向排列 -->
          <div class="grid grid-cols-1 divide-y divide-line-soft rounded-signal border border-line bg-zone md:grid-cols-3 md:divide-x md:divide-y-0">
            <div v-for="seg in healthSegs" :key="seg.key" class="flex items-center gap-3 px-4 py-3">
              <span class="h-2 w-2 shrink-0 rounded-full" :class="[barDot[seg.color], seg.pulse ? 'animate-pulse' : '']" />
              <Icon :name="seg.icon" :size="15" class="shrink-0 text-placeholder" />
              <div class="min-w-0">
                <p class="text-xs text-placeholder">{{ seg.title }}</p>
                <p class="truncate text-sm font-semibold" :class="barText[seg.color]">{{ seg.value }}</p>
                <p class="truncate text-[11px] text-muted">{{ seg.sub }}</p>
              </div>
            </div>
          </div>

          <div class="grid grid-cols-1 gap-x-8 gap-y-2 rounded-signal border border-line md:grid-cols-3">
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
                class="rounded-signal border border-line py-3 text-center">
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
              <UiSwitch :model-value="!!row.enabled" size="sm" :aria-label="`启用通道 ${row.name || row.id}`" @update:model-value="toggleCh(row, $event)" />
            </template>
            <template #stream="{ row }"><UiTag :color="streamInfo(row).color as any">{{ streamInfo(row).label }}</UiTag></template>
            <template #cover="{ row }">
              <img v-if="row.coverUrl" :src="row.coverUrl" class="h-8 w-14 rounded-signal object-cover" alt="">
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
              <div v-if="dev?.source !== 'idp'" class="flex items-center gap-2 rounded-signal border border-line bg-zone px-3 py-2.5 text-sm text-muted">
                <Icon name="info" :size="15" class="text-placeholder" />
                该设备来源（{{ sourceInfo(dev?.source).label }}）不支持远程配置，功能已禁用。
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

        <!-- d) 诊断：示波器读数式列表——状态灯 + 探测项 + 耗时（等宽右对齐），扫读友好 -->
        <div v-if="tab === 'diag'" class="space-y-3 pt-4">
          <div class="flex items-center gap-3">
            <UiButton variant="primary" :disabled="diag.loading" @click="runDiag">
              <Icon name="activity" :size="14" :class="diag.loading ? 'ipc-spin' : ''" />开始诊断
            </UiButton>
            <span v-if="diag.items.length && !diag.loading" class="text-xs text-placeholder">
              共 {{ diag.items.length }} 项 · {{ diag.items.filter((it) => it.ok).length }} 项通过
            </span>
          </div>

          <div v-if="diag.items.length" class="divide-y divide-line-soft rounded-signal border border-line">
            <div v-for="(it, i) in diag.items" :key="i" class="flex items-center gap-3 px-3 py-2.5 text-sm">
              <span class="h-2 w-2 shrink-0 rounded-full" :class="diagDotClass(it)" />
              <span class="w-32 shrink-0 truncate text-ink">{{ it.name || it.item || '检查项' }}</span>
              <span class="min-w-0 flex-1 truncate text-[13px]" :class="it.ok === false ? 'text-danger' : 'text-muted'">
                {{ it.msg || (it.ok === false ? '未通过' : it.ok === true ? '正常' : '等待结果') }}
              </span>
              <span v-if="it.cost != null" class="ml-auto shrink-0 font-mono text-xs tabular-nums text-placeholder">{{ it.cost }}ms</span>
            </div>
          </div>
          <div v-else-if="diag.loading" class="rounded-signal border border-line-soft px-3 py-6 text-center text-sm text-placeholder">
            <Icon name="activity" :size="18" class="ipc-spin mx-auto mb-2 text-primary" />正在探测设备连通性…
          </div>
          <p v-else-if="diag.ran" class="text-sm text-placeholder">未返回诊断项</p>
          <p v-else class="text-sm text-placeholder">点击"开始诊断"检查设备连通性与配置</p>
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
      <img v-if="snapDlg.src" :src="snapDlg.src" class="block w-full rounded-signal" alt="快照">
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
        <UiButton variant="primary" :disabled="editDlg.saving" @click="saveEdit">保存</UiButton>
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
  </UiLoading>
</template>
