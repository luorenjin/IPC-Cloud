<script setup lang="ts">
// 设备详情（MGR-03）：概览 / 通道 / 配置 / 诊断 / 日志（PRD 五 Tab）
const route = useRoute()
const api = useApi()
const toast = useToast()
const { t } = useI18n()
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
    { label: t('device.detail.deviceId'), value: d.id },
    { label: t('device.detail.model'), value: d.model },
    { label: t('device.detail.vendor'), value: d.vendor },
    { label: t('device.detail.firmware'), value: d.firmware || d.firmwareVersion },
    { label: 'IP', value: d.ip },
    { label: 'MAC', value: d.mac },
    { label: t('device.detail.location'), value: d.location },
    { label: t('common.remark'), value: d.remark },
    { label: t('device.detail.lastOnline'), value: d.lastOnline || d.lastSeen ? ago(d.lastOnline || d.lastSeen, t) : '' }
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
    toastApiError(e, t('device.msg.diagFailed'))
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
  return { color: st.color, value: ts ? ago(ts, t) : '—', sub: t('device.detail.currentStatus', { status: t(st.labelKey) }) }
})
const streamSeg = computed(() => {
  const chs = channels.value
  const total = chs.length
  const live = chs.filter((ch: any) => streamInfo(ch).color === 'success').length
  const color = !total ? 'info' : live === total ? 'success' : live > 0 ? 'warning' : 'info'
  return {
    color,
    value: total ? t('device.detail.streamCount', { live, total }) : t('device.detail.noChannel'),
    sub: total ? t('device.detail.streamingCount') : t('device.detail.noChannelReported')
  }
})
const diagSeg = computed(() => {
  if (diag.loading) return { color: 'warning', value: t('device.diag.running'), sub: t('device.diag.probing'), pulse: true }
  if (!diag.ran) return { color: 'info', value: t('device.diag.notRun'), sub: t('device.diag.notRunSub') }
  if (!diag.items.length) return { color: 'info', value: t('device.diag.noResult'), sub: t('device.diag.noResultSub') }
  const failCount = diag.items.filter((it: any) => it.ok === false).length
  return failCount
    ? { color: 'danger', value: t('device.diag.failCount', { n: failCount }), sub: t('device.diag.totalCount', { n: diag.items.length }) }
    : { color: 'success', value: t('device.diag.allOk'), sub: t('device.diag.allOkSub', { n: diag.items.length }) }
})
const healthSegs = computed(() => [
  { key: 'uptime', icon: 'clock', title: t('device.detail.uptime'), ...uptimeSeg.value },
  { key: 'stream', icon: 'video', title: t('device.detail.streamState'), ...streamSeg.value },
  { key: 'diag', icon: 'activity', title: t('device.detail.lastDiag'), ...diagSeg.value }
])

// ================= 通道操作 =================
async function toggleCh(ch: any, val: any) {
  try {
    await api.put(`/channels/${ch.id}`, { enabled: !!val })
    ch.enabled = !!val
    toast.success(t('device.msg.updatedOk'))
  } catch (e: any) {
    toastApiError(e, t('device.msg.setFailed'))
  }
}
async function refreshCover(ch: any) {
  try {
    await api.post(`/channels/${ch.id}/cover`)
    toast.success(t('device.msg.coverRefreshSubmitted'))
  } catch (e: any) {
    toastApiError(e, t('device.msg.opFailed'))
  }
}
const snapDlg = reactive({ show: false, src: '' })
async function takeSnap(ch: any) {
  try {
    const res: any = await api.post(`/channels/${ch.id}/snapshot`)
    snapDlg.src = res.url || res.snapshot || res.dataUrl || (typeof res === 'string' ? res : '')
    snapDlg.show = true
  } catch (e: any) {
    toastApiError(e, t('device.msg.snapFailed'))
  }
}

// ================= 远程配置（MGR-09，按能力集渲染） =================
const cfg = reactive({ loading: false, saving: false, data: {} as any, denied: [] as string[], supported: [] as string[] })
// 配置键 → 中文标签（与后端 cfgKeys 对齐）
const CFG_LABELS: Record<string, string> = {
  'video.main.resolution': 'device.config.resolution', 'video.main.fps': 'device.config.fps', 'video.main.bitrate': 'device.config.bitrate',
  'video.main.gop': 'device.config.gop', 'video.main.encode': 'device.config.encode',
  'image.brightness': 'device.config.brightness', 'image.contrast': 'device.config.contrast', 'image.saturation': 'device.config.saturation',
  'image.sharpness': 'device.config.sharpness', 'image.mirror': 'device.config.mirror', 'image.wdr': 'device.config.wdr',
  'osd.enable': 'device.config.osdEnable', 'osd.text': 'device.config.osdText', 'record.mode': 'device.config.recordMode',
  'alarm.motion.sensitivity': 'device.config.motionSens', 'time.ntp': 'device.config.ntp'
}
const cfgFields = computed(() =>
  (cfg.supported.length ? cfg.supported : Object.keys(CFG_LABELS)).map((k) => ({ key: k, label: CFG_LABELS[k] ? t(CFG_LABELS[k]) : k })))
async function loadCfg() {
  cfg.loading = true
  cfg.denied = []
  try {
    const res: any = await api.get(`/devices/${devId}/config`)
    cfg.data = res?.config || {}
    cfg.supported = res?.supported || []
  } catch (e: any) {
    toastApiError(e, t('device.msg.configLoadFailed'))
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
    toast.success(cfg.denied.length ? t('device.msg.configPartlyRejected') : t('device.msg.configSent'))
    setTimeout(loadCfg, 5000)
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
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
  if (!name) return toast.warning(t('device.msg.nameRequired'))
  editDlg.saving = true
  try {
    await api.put(`/devices/${devId}`, { name, location: editDlg.location.trim(), remark: editDlg.remark.trim() })
    toast.success(t('common.savedOk'))
    editDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    editDlg.saving = false
  }
}

// ================= 重启 / 转移 / 升级 / 安全删除（MGR-08/10/11/12） =================
const confirmBox = useConfirm()
const router = useRouter()

async function rebootDevice() {
  const ok = await confirmBox.ask({
    title: t('device.msg.rebootTitle'),
    message: t('device.msg.rebootMsg', { name: dev.value?.name || devId }),
    detail: t('device.msg.rebootDetail'),
    confirmText: t('device.confirm.rebootNow')
  })
  if (!ok) return
  try {
    await api.post(`/devices/${devId}/reboot`)
    toast.success(t('device.msg.rebootSent'))
  } catch (e: any) {
    toastApiError(e, t('device.msg.rebootFailed'))
  }
}

const moveDlg = reactive({ show: false, groupId: '', saving: false })
const groups = ref<any[]>([])
async function openMoveDlg() {
  try {
    const res: any = await api.get('/groups')
    groups.value = res?.items || []
  } catch (e: any) {
    toastApiError(e, t('device.msg.groupListFailed'))
  }
  moveDlg.groupId = dev.value?.groupId || ''
  moveDlg.show = true
}
async function saveMove() {
  if (!moveDlg.groupId) return toast.warning(t('device.msg.selectGroup'))
  moveDlg.saving = true
  try {
    await api.put(`/devices/${devId}`, { groupId: moveDlg.groupId })
    toast.success(t('device.msg.groupMoveOk'))
    moveDlg.show = false
    load()
  } catch (e: any) {
    toastApiError(e, t('device.msg.moveFailed'))
  } finally {
    moveDlg.saving = false
  }
}

async function askDeleteDevice() {
  const devName = dev.value?.name || devId
  const ok = await confirmBox.ask({
    title: t('device.msg.deleteDetailTitle'),
    message: t('device.msg.deleteDetailMsg', { name: devName }),
    detail: t('device.msg.deleteDetailHint'),
    danger: true,
    confirmText: t('device.confirm.deleteOk'),
    inputConfirm: devName,
    inputPlaceholder: devName
  })
  if (!ok) return
  try {
    await api.request(`/devices/${devId}`, { method: 'DELETE', body: { confirmName: devName } })
    toast.success(t('device.msg.deleteOk'))
    router.push('/devices')
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
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
    toastApiError(e, t('common.loadFailed'))
  } finally {
    loading.value = false
  }
}

/* 实时状态（E8）：只关心当前这台设备的事件 */
useWs((ev: any) => {
  if (!dev.value || ev.deviceId !== dev.value.id) return
  if (ev.type === 'device.online' || ev.type === 'device.offline') {
    dev.value.status = ev.type === 'device.online' ? 'online' : 'offline'
    dev.value.lastSeenAt = ev.ts || Date.now()
  }
})

onMounted(load)
</script>

<template>
  <UiLoading :loading="loading">
    <UiCard flat>
      <!-- 头部 -->
      <div class="mb-1 flex items-center gap-2.5">
        <button class="flex items-center gap-1 rounded-chrome border border-line px-2 py-1 text-xs text-muted transition-colors hover:border-primary hover:text-primary" @click="navigateTo('/devices')">
          <Icon name="arrow-left" :size="12" />{{ t('device.detail.back') }}
        </button>
        <span class="text-base font-bold text-ink">{{ dev?.name || t('device.detail.title') }}</span>
        <UiTag v-if="dev" :color="statusInfo(dev.status).color as any" dot>{{ t(statusInfo(dev.status).labelKey) }}</UiTag>
        <UiTag :color="sourceInfo(dev?.source).color as any" plain>{{ t(sourceInfo(dev?.source).labelKey) }}</UiTag>
        <div class="ml-auto flex flex-wrap items-center gap-2">
          <UiButton variant="primary" @click="openEdit"><Icon name="edit" :size="13" />{{ t('common.edit') }}</UiButton>
          <UiButton @click="rebootDevice"><Icon name="refresh-cw" :size="13" />{{ t('device.detail.reboot') }}</UiButton>
          <UiButton @click="openMoveDlg"><Icon name="folder" :size="13" />{{ t('device.detail.moveGroup') }}</UiButton>
          <UiTooltip :label="t('device.detail.otaDisabled')">
            <span><UiButton disabled><Icon name="upload" :size="13" />{{ t('device.detail.ota') }}</UiButton></span>
          </UiTooltip>
          <UiButton variant="dangerText" @click="askDeleteDevice"><Icon name="trash" :size="13" />{{ t('device.toolbar.delete') }}</UiButton>
        </div>
      </div>

      <UiTabs v-model="tab" :items="[
        { label: t('device.detail.tabOverview'), value: 'overview' },
        { label: t('device.detail.tabChannels'), value: 'channels' },
        { label: t('device.detail.tabConfig'), value: 'config' },
        { label: t('device.detail.tabDiag'), value: 'diag' },
        { label: t('device.detail.tabLogs'), value: 'logs' }
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
            <p class="mb-2 text-xs text-placeholder">{{ t('device.detail.caps') }}</p>
            <div class="flex flex-wrap gap-2">
              <UiTag v-for="(c, i) in caps" :key="i" color="primary" plain>{{ c }}</UiTag>
              <span v-if="!caps.length" class="text-sm text-placeholder">{{ t('device.detail.capsNone') }}</span>
            </div>
          </div>

          <!-- status.metrics（IDP 运行指标，MGR-03） -->
          <div v-if="metrics">
            <p class="mb-2 text-xs text-placeholder">{{ t('device.detail.metrics') }}</p>
            <div class="grid grid-cols-2 gap-3 md:grid-cols-5">
              <div v-for="m in [
                { label: 'CPU', value: pct(metrics.cpu) },
                { label: t('device.detail.memory'), value: pct(metrics.memory ?? metrics.mem) },
                { label: t('device.detail.temperature'), value: temp(metrics.temperature ?? metrics.temp) },
                { label: t('device.detail.tfCard'), value: metrics.tfHealth || (metrics.tfTotal ? (metrics.tfUsed || 0) + '/' + metrics.tfTotal + 'GB' : '') },
                { label: t('device.detail.bitrate'), value: metrics.bitrate ? metrics.bitrate + 'kbps' : '' }
              ]" :key="m.label" v-show="m.value && m.value !== '—'"
                class="rounded-signal border border-line py-3 text-center">
                <p class="text-xs text-placeholder">{{ m.label }}</p>
                <p class="mt-1 text-lg font-bold text-ink">{{ m.value }}</p>
              </div>
            </div>
          </div>

          <!-- 最近事件 -->
          <div v-if="opRows.length">
            <p class="mb-2 text-xs text-placeholder">{{ t('device.detail.recentEvents') }}</p>
            <div class="space-y-1">
              <div v-for="(o, i) in opRows.slice(0, 5)" :key="i" class="flex items-center gap-2 text-[13px]">
                <span class="text-placeholder">{{ ago(o.ts, t) }}</span>
                <span class="text-body">{{ o.action }}</span>
                <UiTag :color="o.ok ? 'success' : 'danger'" plain>{{ o.ok ? t('common.success') : t('common.failed') }}</UiTag>
              </div>
            </div>
          </div>
        </div>

        <!-- b) 通道 -->
        <div v-if="tab === 'channels'" class="pt-4">
          <UiTable
            :columns="[
              { key: 'name', label: t('common.name') },
              { key: 'no', label: t('device.channel.no'), width: '90px', align: 'center' },
              { key: 'enabled', label: t('device.channel.enabled'), width: '90px', align: 'center' },
              { key: 'stream', label: t('device.channel.streamStatus'), width: '100px' },
              { key: 'cover', label: t('device.channel.cover'), width: '120px' },
              { key: 'ops', label: t('common.action'), width: '180px', ellipsis: false }
            ]"
            :rows="channels" :empty="t('device.channel.empty')"
          >
            <template #no="{ row }">{{ row.channelNo ?? row.num ?? row.channel ?? '—' }}</template>
            <template #enabled="{ row }">
              <UiSwitch :model-value="!!row.enabled" size="sm" :aria-label="t('device.channel.enableAria', { name: row.name || row.id })" @update:model-value="toggleCh(row, $event)" />
            </template>
            <template #stream="{ row }"><UiTag :color="streamInfo(row).color as any">{{ t(streamInfo(row).labelKey) }}</UiTag></template>
            <template #cover="{ row }">
              <img v-if="row.coverUrl" :src="row.coverUrl" class="h-8 w-14 rounded-signal object-cover" alt="">
              <span v-else class="text-xs text-placeholder">—</span>
            </template>
            <template #ops="{ row }">
              <UiButton variant="text" size="sm" @click="refreshCover(row)">{{ t('device.channel.refreshCover') }}</UiButton>
              <UiButton variant="text" size="sm" @click="takeSnap(row)">{{ t('device.channel.snapshot') }}</UiButton>
            </template>
          </UiTable>
        </div>

        <!-- c) 配置（MGR-09，能力灰显） -->
        <div v-if="tab === 'config'" class="pt-4">
          <UiLoading :loading="cfg.loading">
            <div class="min-h-40 space-y-4">
              <div v-if="dev?.source !== 'idp'" class="flex items-center gap-2 rounded-signal border border-line bg-zone px-3 py-2.5 text-sm text-muted">
                <Icon name="info" :size="15" class="text-placeholder" />
                {{ t('device.config.unsupported', { source: t(sourceInfo(dev?.source).labelKey) }) }}
              </div>
              <template v-else>
                <div class="grid grid-cols-1 gap-x-8 gap-y-3 md:grid-cols-2">
                  <div v-for="f in cfgFields" :key="f.key" class="flex items-center gap-3">
                    <label class="w-28 shrink-0 text-right text-sm text-muted">{{ f.label }}</label>
                    <UiInput
                      :model-value="String(cfg.data?.[f.key] ?? '')" size="sm" width="w-40"
                      @update:model-value="cfg.data[f.key] = $event"
                    />
                    <span v-if="cfg.denied.includes(f.key)" class="text-xs text-danger">{{ t('device.config.rejected') }}</span>
                  </div>
                </div>
                <div class="flex gap-2">
                  <UiButton variant="primary" :disabled="cfg.saving" @click="saveCfg">{{ t('device.config.submit') }}</UiButton>
                  <UiButton @click="loadCfg">{{ t('device.config.reload') }}</UiButton>
                </div>
              </template>
            </div>
          </UiLoading>
        </div>

        <!-- d) 诊断：示波器读数式列表——状态灯 + 探测项 + 耗时（等宽右对齐），扫读友好 -->
        <div v-if="tab === 'diag'" class="space-y-3 pt-4">
          <div class="flex items-center gap-3">
            <UiButton variant="primary" :disabled="diag.loading" @click="runDiag">
              <Icon name="activity" :size="14" :class="diag.loading ? 'ipc-spin' : ''" />{{ t('device.diag.run') }}
            </UiButton>
            <span v-if="diag.items.length && !diag.loading" class="text-xs text-placeholder">
              {{ t('device.diag.summary', { total: diag.items.length, pass: diag.items.filter((it) => it.ok).length }) }}
            </span>
          </div>

          <div v-if="diag.items.length" class="divide-y divide-line-soft rounded-signal border border-line">
            <div v-for="(it, i) in diag.items" :key="i" class="flex items-center gap-3 px-3 py-2.5 text-sm">
              <span class="h-2 w-2 shrink-0 rounded-full" :class="diagDotClass(it)" />
              <span class="w-32 shrink-0 truncate text-ink">{{ it.name || it.item || t('device.diag.item') }}</span>
              <span class="min-w-0 flex-1 truncate text-[13px]" :class="it.ok === false ? 'text-danger' : 'text-muted'">
                {{ it.msg || (it.ok === false ? t('device.diag.itemFail') : it.ok === true ? t('device.diag.itemOk') : t('device.diag.itemWaiting')) }}
              </span>
              <span v-if="it.cost != null" class="ml-auto shrink-0 font-mono text-xs tabular-nums text-placeholder">{{ it.cost }}ms</span>
            </div>
          </div>
          <div v-else-if="diag.loading" class="rounded-signal border border-line-soft px-3 py-6 text-center text-sm text-placeholder">
            <Icon name="activity" :size="18" class="ipc-spin mx-auto mb-2 text-primary" />{{ t('device.diag.probing') }}
          </div>
          <p v-else-if="diag.ran" class="text-sm text-placeholder">{{ t('device.diag.noResult') }}</p>
          <p v-else class="text-sm text-placeholder">{{ t('device.diag.idle') }}</p>
        </div>

        <!-- e) 日志 -->
        <div v-if="tab === 'logs'" class="pt-4">
          <UiTable
            :columns="[
              { key: 'ts', label: t('common.time'), width: '120px' },
              { key: 'action', label: t('common.action'), width: '150px' },
              { key: 'operator', label: t('device.log.operator'), width: '130px' },
              { key: 'ok', label: t('device.log.result'), width: '90px' },
              { key: 'msg', label: t('common.detail') }
            ]"
            :rows="opRows" :empty="t('device.log.empty')"
          >
            <template #ts="{ row }">{{ ago(row.ts, t) }}</template>
            <template #ok="{ row }"><UiTag :color="row.ok ? 'success' : 'danger'">{{ row.ok ? t('common.success') : t('common.failed') }}</UiTag></template>
          </UiTable>
        </div>
      </UiTabs>
    </UiCard>

    <!-- 快照弹窗 -->
    <UiDialog v-model:open="snapDlg.show" :title="t('device.channel.snapTitle')" width="max-w-xl">
      <img v-if="snapDlg.src" :src="snapDlg.src" class="block w-full rounded-signal" :alt="t('device.channel.snapAlt')">
      <UiEmptyState v-else :text="t('device.channel.snapEmpty')" />
    </UiDialog>

    <!-- 编辑设备 -->
    <UiDialog v-model:open="editDlg.show" :title="t('device.edit.title')" width="max-w-md">
      <div class="grid grid-cols-[80px_1fr] items-center gap-x-3 gap-y-3">
        <label class="text-right text-sm text-muted">{{ t('device.detail.editName') }}</label>
        <UiInput v-model="editDlg.name" />
        <label class="text-right text-sm text-muted">{{ t('device.detail.location') }}</label>
        <UiInput v-model="editDlg.location" />
        <label class="text-right text-sm text-muted">{{ t('common.remark') }}</label>
        <UiInput v-model="editDlg.remark" />
      </div>
      <template #footer>
        <UiButton @click="editDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="editDlg.saving" @click="saveEdit">{{ t('common.save') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 转移分组弹窗 -->
    <UiDialog v-model:open="moveDlg.show" :title="t('device.detail.moveTitle')" width="max-w-sm">
      <div class="space-y-3">
        <label class="block text-xs text-muted">{{ t('device.move.targetGroup') }}</label>
        <UiSelect v-model="moveDlg.groupId" :options="groups.map(g => ({ label: g.name, value: g.id }))" class="w-full" />
      </div>
      <template #footer>
        <UiButton @click="moveDlg.show = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :loading="moveDlg.saving" @click="saveMove">{{ t('device.transfer.submit') }}</UiButton>
      </template>
    </UiDialog>
  </UiLoading>
</template>
