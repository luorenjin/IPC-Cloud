<script setup lang="ts">
// 媒体节点管理（SYS-01/02）：节点增删改查、禁用、自检、流列表抽屉与踢流，WebSocket 实时刷新状态
const api = useApi()
const toast = useToast()
const { t } = useI18n()
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
    toastApiError(e, t('system.msg.nodesLoadFailed'))
  } finally {
    loading.value = false
  }
}

/* 状态标签：映射见 utils/enums.ts 的 NODE_STATUS_MAP——节点在线用信号青（primary），
   区别于设备业务在线态的 success 绿（两者刻意不同，勿合并） */
function statusTag(s: string) {
  const info = nodeStatusInfo(s)
  return { text: info.label, color: info.color }
}

/* 状态灯点样式（纯展示，复用 statusTag 的语义色） */
const LAMP_STYLE: Record<string, { dot: string; text: string }> = {
  primary: { dot: 'bg-primary shadow-[0_0_6px_var(--color-primary)]', text: 'text-primary' },
  danger: { dot: 'bg-danger shadow-[0_0_6px_var(--color-danger)]', text: 'text-danger' },
  warning: { dot: 'bg-warning', text: 'text-warning' },
  info: { dot: 'bg-info', text: 'text-info' }
}
function nodeLamp(row: any) {
  if (row.disabled) return { ...LAMP_STYLE.info, label: t('common.disabled') }
  const t = statusTag(row.status)
  return { ...(LAMP_STYLE[t.color] || LAMP_STYLE.info), label: t.text }
}

/* 负载：以流数/上限占用率表示（后端未提供 CPU/负载指标，取现有字段派生，纯展示） */
function loadPct(row: any): number {
  const max = Number(row?.maxStreams || 0)
  return max > 0 ? Math.min(100, Math.round((Number(row?.streams || 0) / max) * 100)) : 0
}

/* ---------------- 卡片迷你实时曲线（sparkline，纯展示） ----------------
 * 后端未下发历史序列，这里新增一份仅供本页渲染使用的本地滑动窗口缓存：
 * 每次 loadNodes() 成功刷新节点列表（初始加载 / 手动刷新 / node.status 推送触发的刷新）
 * 就为每个节点追加一个采样点，超出窗口长度后丢弃最旧的点；不引入定时轮询，
 * 不影响原有节点数据获取与自检/禁用等业务逻辑。
 */
const HISTORY_LEN = 24
const nodeHistory = reactive<Record<string, { load: number[]; bwIn: number[]; bwOut: number[]; playing: number[] }>>({})

function pushHistoryPoint(arr: number[], v: number) {
  arr.push(v)
  if (arr.length > HISTORY_LEN) arr.shift()
}
function recordNodeHistory(list: any[]) {
  const ids = new Set(list.map((n) => n.id))
  Object.keys(nodeHistory).forEach((id) => { if (!ids.has(id)) delete nodeHistory[id] }) // 节点被删除时清理，避免无限增长
  list.forEach((row) => {
    const h = nodeHistory[row.id] || (nodeHistory[row.id] = { load: [], bwIn: [], bwOut: [], playing: [] })
    pushHistoryPoint(h.load, loadPct(row))
    pushHistoryPoint(h.bwIn, Number(row.bytesIn ?? row.bwIn ?? 0))
    pushHistoryPoint(h.bwOut, Number(row.bytesOut ?? row.bwOut ?? 0))
    pushHistoryPoint(h.playing, Number(row.playing ?? row.viewers ?? 0))
  })
}
watch(nodes, (list) => recordNodeHistory(list))

/* 把数值窗口归一化为 SVG polyline 的 points 坐标串（纯展示，不引入图表库） */
function sparkPoints(values: number[], w = 64, h = 22): string {
  if (!values || !values.length) return ''
  const vals = values.length === 1 ? [values[0], values[0]] : values
  const min = Math.min(...vals)
  const max = Math.max(...vals)
  const span = max - min
  const step = w / (vals.length - 1)
  return vals.map((v, i) => {
    const y = span === 0 ? h / 2 : h - ((v - min) / span) * h
    return (i * step).toFixed(1) + ',' + y.toFixed(1)
  }).join(' ')
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

/* 端口（1-65535 整数）；非法时提示并阻止提交，不再静默改写为默认值 */
function isValidPort(v: any): boolean {
  const n = Number(v)
  return Number.isFinite(n) && Number.isInteger(n) && n >= 1 && n <= 65535
}

/* 最大流数（≥1 整数） */
function isValidMaxStreams(v: any): boolean {
  const n = Number(v)
  return Number.isFinite(n) && Number.isInteger(n) && n >= 1
}

/* 权重（1-100 整数） */
function isValidWeight(v: any): boolean {
  const n = Number(v)
  return Number.isFinite(n) && Number.isInteger(n) && n >= 1 && n <= 100
}

async function saveNode() {
  const f = nodeDlg.form
  if (!f.name.trim() || !f.apiUrl.trim()) return toast.warning(t('system.msg.nodeNeedNameApi'))
  if (nodeDlg.mode === 'create' && !f.secret.trim()) return toast.warning(t('system.msg.nodeNeedSecret'))
  if (nodeDlg.mode === 'create' && !f.publicHost.trim()) return toast.warning(t('system.msg.nodeNeedPublicHost'))
  if (!isValidPort(f.httpPort) || !isValidPort(f.httpsPort) || !isValidPort(f.rtmpPort) || !isValidPort(f.rtspPort)) {
    return toast.warning(t('system.msg.nodePortRange'))
  }
  if (!isValidMaxStreams(f.maxStreams)) return toast.warning(t('system.msg.nodeMaxStreamsRange'))
  if (!isValidWeight(f.weight)) return toast.warning(t('system.msg.nodeWeightRange'))
  nodeDlg.saving = true
  try {
    const body: any = {
      name: f.name.trim(),
      apiUrl: f.apiUrl.trim(),
      publicHost: f.publicHost.trim(),
      rtmpPort: Math.floor(Number(f.rtmpPort)),
      httpPort: Math.floor(Number(f.httpPort)),
      httpsPort: Math.floor(Number(f.httpsPort)),
      rtspPort: Math.floor(Number(f.rtspPort)),
      rtpRange: f.rtpRange.trim(),
      maxStreams: Math.floor(Number(f.maxStreams)),
      weight: Math.floor(Number(f.weight))
    }
    if (f.secret.trim()) body.secret = f.secret.trim()
    if (nodeDlg.mode === 'create') {
      await api.post('/media-nodes', body)
      toast.success(t('system.msg.nodeCreated'))
    } else {
      await api.put('/media-nodes/' + nodeDlg.id, body)
      toast.success(t('system.msg.nodeUpdated'))
    }
    nodeDlg.visible = false
    await loadNodes()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
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
      if (fresh.status === 'online') { toast.success(t('system.msg.selfcheckOk', { name: fresh.name })); return }
      // ACC-02：后端自检失败会带回具体原因（statusReason），优先呈现它而非泛化猜测
      toast.error({
        title: t('system.msg.selfcheckFailed', { name: fresh.name }),
        suggest: fresh.statusReason || t('system.msg.selfcheckFailedSuggest')
      })
    }, 1500)
  } catch (e: any) {
    checkingId.value = null
    // 自检失败呈现具体原因：不可达 / secret 错误 / 版本过低
    const msg = String(e?.msg || '')
    let suggest = e?.suggest || t('system.msg.selfcheckUnreachable')
    if (/secret|密钥|auth|401/i.test(msg)) suggest = t('system.msg.selfcheckBadSecret')
    else if (/version|版本/i.test(msg)) suggest = t('system.msg.selfcheckOldVersion')
    else if (msg) suggest = msg
    toast.error({ title: (e?.code ? e.code + '：' : '') + (msg || t('system.msg.selfcheckTitle')), suggest })
  }
}

/* 禁用/启用节点（后端 disabled 字段；禁用后不参与调度，已有流不受影响） */
async function toggleDisable(row: any) {
  const disabling = !row.disabled
  const ok = await confirm.ask({
    title: disabling ? t('system.nodes.disableTitle') : t('system.nodes.enableTitle'),
    message: disabling
      ? t('system.nodes.disableMsg', { name: row.name })
      : t('system.nodes.enableMsg', { name: row.name }),
    detail: disabling ? t('system.nodes.disableDetail') : t('system.nodes.enableDetail'),
    danger: disabling,
    confirmText: disabling ? t('common.disable') : t('common.enable')
  })
  if (!ok) return
  try {
    await api.put('/media-nodes/' + row.id, { disabled: disabling })
    toast.success(disabling ? t('system.msg.nodeDisabled') : t('system.msg.nodeEnabled'))
    await loadNodes()
  } catch (e: any) {
    toastApiError(e, t('system.msg.opFailed'))
  }
}

/* 删除节点（有活跃流时禁用按钮并 tooltip 说明） */
async function removeNode(row: any) {
  const ok = await confirm.ask({
    title: t('system.nodes.deleteTitle'),
    message: t('system.nodes.deleteMsg', { name: row.name }),
    detail: t('system.nodes.deleteDetail'),
    danger: true,
    confirmText: t('common.delete')
  })
  if (!ok) return
  try {
    await api.del('/media-nodes/' + row.id)
    toast.success(t('common.deletedOk'))
    await loadNodes()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
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
    toastApiError(e, t('system.msg.streamsLoadFailed'))
  } finally {
    detail.loading = false
  }
}

/* 踢流 */
async function kickStream(ss: any) {
  const ok = await confirm.ask({
    title: t('system.nodes.kickTitle'),
    message: t('system.nodes.kickMsg', { stream: ss.stream }),
    detail: t('system.nodes.kickDetail'),
    danger: true,
    confirmText: t('system.nodes.kick')
  })
  if (!ok) return
  try {
    await api.post('/media-nodes/' + detail.node.id + '/streams/' + ss.id + '/kick')
    toast.success(t('system.msg.kicked'))
    await showDetail(detail.node)
    await loadNodes()
  } catch (e: any) {
    toastApiError(e, t('system.msg.kickFailed'))
  }
}

const streamCols = computed(() => [
  { key: 'app', label: t('system.nodes.colApp'), width: '90px' },
  { key: 'stream', label: t('system.nodes.colStream'), width: '180px' },
  { key: 'channel', label: t('system.nodes.colChannel'), width: '130px' },
  { key: 'viewers', label: t('system.nodes.colViewers'), width: '80px', align: 'center' as const },
  { key: 'bitrate', label: t('system.nodes.colBitrate'), width: '90px', align: 'right' as const },
  { key: 'ops', label: t('common.action'), width: '70px', align: 'center' as const }
])


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
    <UiCard :title="t('system.nodes.title')" flat>
      <template #extra>
        <div class="flex items-center gap-2">
          <UiButton size="sm" :disabled="loading" @click="loadNodes">
            <UiIcon name="refresh" :size="14" />{{ t('common.refresh') }}
          </UiButton>
          <UiButton variant="primary" size="sm" @click="openNodeDlg('create')">
            <UiIcon name="plus" :size="14" />{{ t('system.nodes.create') }}
          </UiButton>
        </div>
      </template>

      <UiLoading :loading="loading" class="min-h-[168px]">
        <div v-if="nodes.length" class="grid grid-cols-1 gap-3 sm:grid-cols-2 xl:grid-cols-3">
          <div
            v-for="row in nodes" :key="row.id"
            class="flex flex-col overflow-hidden rounded-signal border border-line bg-surface transition-colors hover:border-placeholder"
          >
            <!-- 状态灯 + 名称 -->
            <div class="flex items-center justify-between gap-2 border-b border-line-soft px-3.5 py-2.5">
              <div class="flex min-w-0 items-center gap-2">
                <span class="h-2 w-2 shrink-0 rounded-full" :class="nodeLamp(row).dot" />
                <span class="truncate text-sm font-semibold text-ink" :title="row.name">{{ row.name }}</span>
              </div>
              <span class="shrink-0 text-xs" :class="nodeLamp(row).text">{{ nodeLamp(row).label }}</span>
            </div>

            <div class="space-y-2.5 px-3.5 py-3">
              <!-- 基本信息 -->
              <div class="flex flex-wrap items-center gap-x-3 gap-y-1 font-mono text-[11px] text-placeholder">
                <span class="max-w-full truncate" :title="row.apiUrl">{{ row.apiUrl }}</span>
                <span>v{{ row.version || '—' }}</span>
                <span :class="row.weight === 0 ? 'text-placeholder' : ''">{{ t('system.nodes.weightLabel', { n: row.weight ?? 100 }) }}</span>
                <span>{{ t('system.nodes.keepalive', { t: ago(row.lastKeepalive, t) }) }}</span>
              </div>

              <!-- 迷你实时曲线：负载 / 带宽 / 在线通道数（最近若干次轮询的采样点连线） -->
              <div class="grid grid-cols-3 gap-2">
                <div class="rounded-signal bg-zone px-2 py-1.5">
                  <div class="flex items-baseline justify-between gap-1">
                    <span class="text-[11px] text-muted">{{ t('system.nodes.streamUsage') }}</span>
                    <span class="font-mono text-xs" :class="row.maxStreams && row.streams >= row.maxStreams ? 'text-danger' : 'text-body'">{{ loadPct(row) }}%</span>
                  </div>
                  <svg v-if="(nodeHistory[row.id]?.load || []).length >= 2" viewBox="0 0 64 22" class="mt-1 h-5 w-full" preserveAspectRatio="none">
                    <polyline
                      :points="sparkPoints(nodeHistory[row.id]?.load || [])" fill="none" stroke="var(--color-primary)"
                      stroke-width="1.5" stroke-linejoin="round" stroke-linecap="round"
                    />
                  </svg>
                  <div v-else class="mt-1 flex h-5 items-center text-[10px] text-placeholder">{{ t('system.nodes.sampling') }}</div>
                </div>
                <div class="rounded-signal bg-zone px-2 py-1.5">
                  <div class="flex items-baseline justify-between gap-1">
                    <span class="text-[11px] text-muted">{{ t('system.nodes.bandwidth') }}</span>
                  </div>
                  <svg v-if="(nodeHistory[row.id]?.bwIn || []).length >= 2" viewBox="0 0 64 22" class="mt-1 h-5 w-full" preserveAspectRatio="none">
                    <polyline
                      :points="sparkPoints(nodeHistory[row.id]?.bwIn || [])" fill="none" stroke="var(--color-primary)"
                      stroke-width="1.5" stroke-linejoin="round" stroke-linecap="round"
                    />
                    <polyline
                      :points="sparkPoints(nodeHistory[row.id]?.bwOut || [])" fill="none" stroke="var(--color-muted)"
                      stroke-width="1.5" stroke-linejoin="round" stroke-linecap="round"
                    />
                  </svg>
                  <div v-else class="mt-1 flex h-5 items-center text-[10px] text-placeholder">{{ t('system.nodes.sampling') }}</div>
                  <div class="mt-0.5 truncate font-mono text-[10px] text-placeholder">
                    {{ fmtRate(row.bytesIn ?? row.bwIn) }} / {{ fmtRate(row.bytesOut ?? row.bwOut) }}
                  </div>
                </div>
                <div class="rounded-signal bg-zone px-2 py-1.5">
                  <div class="flex items-baseline justify-between gap-1">
                    <span class="text-[11px] text-muted">{{ t('system.nodes.onlineChannels') }}</span>
                    <span class="font-mono text-xs text-body">{{ row.playing ?? row.viewers ?? 0 }}</span>
                  </div>
                  <svg v-if="(nodeHistory[row.id]?.playing || []).length >= 2" viewBox="0 0 64 22" class="mt-1 h-5 w-full" preserveAspectRatio="none">
                    <polyline
                      :points="sparkPoints(nodeHistory[row.id]?.playing || [])" fill="none" stroke="var(--color-success)"
                      stroke-width="1.5" stroke-linejoin="round" stroke-linecap="round"
                    />
                  </svg>
                  <div v-else class="mt-1 flex h-5 items-center text-[10px] text-placeholder">{{ t('system.nodes.sampling') }}</div>
                </div>
              </div>

              <div class="text-xs text-placeholder">
                {{ t('system.nodes.streamsOfMax') }}
                <span class="font-mono" :class="row.maxStreams && row.streams >= row.maxStreams ? 'text-danger' : 'text-body'">{{ row.streams ?? 0 }} / {{ row.maxStreams ?? 0 }}</span>
              </div>
            </div>

            <!-- 操作 -->
            <div class="mt-auto flex flex-wrap items-center gap-1 border-t border-line-soft px-2.5 py-2">
              <UiButton variant="text" size="sm" @click="openNodeDlg('edit', row)">{{ t('common.edit') }}</UiButton>
              <UiButton variant="text" size="sm" :disabled="checkingId === row.id" @click="selfcheck(row)">
                {{ checkingId === row.id ? t('system.nodes.selfchecking') : t('system.nodes.selfcheck') }}
              </UiButton>
              <UiButton variant="text" size="sm" @click="showDetail(row)">{{ t('common.detail') }}</UiButton>
              <UiButton variant="text" size="sm" @click="toggleDisable(row)">{{ row.disabled ? t('common.enable') : t('common.disable') }}</UiButton>
              <UiTooltip v-if="row.streams > 0" :label="t('system.nodes.activeStreamTip')">
                <span class="ml-auto"><UiButton variant="dangerText" size="sm" disabled>{{ t('common.delete') }}</UiButton></span>
              </UiTooltip>
              <UiButton v-else variant="dangerText" size="sm" class="ml-auto" @click="removeNode(row)">{{ t('common.delete') }}</UiButton>
            </div>
          </div>
        </div>
        <UiEmptyState v-else-if="!loading" :text="t('system.nodes.empty')">
          <template #action>
            <UiButton variant="primary" @click="openNodeDlg('create')">{{ t('system.nodes.create') }}</UiButton>
          </template>
        </UiEmptyState>
      </UiLoading>
    </UiCard>

    <!-- 新建/编辑节点对话框 -->
    <UiDialog
      v-model:open="nodeDlg.visible"
      :title="nodeDlg.mode === 'create' ? t('system.nodes.createDlgTitle') : t('system.nodes.editDlgTitle')"
      width="max-w-lg"
    >
      <div class="grid grid-cols-[110px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('system.nodes.name') }}</span>
        <UiInput v-model="nodeDlg.form.name" :placeholder="t('system.nodes.name')" :maxlength="50" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('system.nodes.apiUrl') }}</span>
        <UiInput v-model="nodeDlg.form.apiUrl" placeholder="http://host:port" :maxlength="200" />
        <span class="text-right text-sm text-body">
          <span v-if="nodeDlg.mode === 'create'" class="text-danger">*</span>{{ t('system.nodes.secret') }}
        </span>
        <UiInput
          v-model="nodeDlg.form.secret" type="password"
          :placeholder="nodeDlg.mode === 'create' ? t('system.nodes.secretPlaceholder') : t('system.nodes.secretKeepPlaceholder')" :maxlength="128"
        />
        <span class="text-right text-sm text-body">
          <span v-if="nodeDlg.mode === 'create'" class="text-danger">*</span>{{ t('system.nodes.publicHost') }}
        </span>
        <UiInput v-model="nodeDlg.form.publicHost" :placeholder="t('system.nodes.publicHostPlaceholder')" :maxlength="200" />
        <span class="text-right text-sm text-body">{{ t('system.nodes.httpPorts') }}</span>
        <div class="grid grid-cols-2 gap-3">
          <UiInput v-model="nodeDlg.form.httpPort" placeholder="80" :maxlength="5" />
          <UiInput v-model="nodeDlg.form.httpsPort" placeholder="443" :maxlength="5" />
        </div>
        <span class="text-right text-sm text-body">{{ t('system.nodes.mediaPorts') }}</span>
        <div class="grid grid-cols-2 gap-3">
          <UiInput v-model="nodeDlg.form.rtmpPort" placeholder="1936" :maxlength="5" />
          <UiInput v-model="nodeDlg.form.rtspPort" placeholder="554" :maxlength="5" />
        </div>
        <span class="text-right text-sm text-body">{{ t('system.nodes.rtpRange') }}</span>
        <UiInput v-model="nodeDlg.form.rtpRange" placeholder="30000-30100" :maxlength="30" width="w-45" />
        <span class="text-right text-sm text-body">{{ t('system.nodes.maxStreams') }}</span>
        <UiInput v-model="nodeDlg.form.maxStreams" placeholder="200" :maxlength="5" width="w-45" />
        <span class="text-right text-body text-sm">{{ t('system.nodes.weight') }}</span>
        <UiInput v-model="nodeDlg.form.weight" placeholder="100" :maxlength="4" width="w-45" />
      </div>
      <template #footer>
        <UiButton @click="nodeDlg.visible = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="nodeDlg.saving" @click="saveNode">{{ nodeDlg.saving ? t('common.saving') : t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 节点详情抽屉（SYS-02） -->
    <UiDrawer v-model:open="detail.open" :title="t('system.nodes.detailTitle', { name: detail.node?.name || '' })" width="max-w-xl">
      <div class="space-y-4 p-5">
        <!-- 配置摘要 -->
        <div>
          <p class="mb-2 text-sm font-semibold text-ink">{{ t('system.nodes.configSummary') }}</p>
          <div class="grid grid-cols-2 gap-x-4 gap-y-2 rounded border border-line bg-canvas p-3 text-[13px]">
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">{{ t('system.nodes.apiUrl') }}</span><span class="truncate text-body" :title="detail.node?.apiUrl">{{ detail.node?.apiUrl || '—' }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">{{ t('system.nodes.publicHost') }}</span><span class="truncate text-body">{{ detail.node?.publicHost || '—' }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">{{ t('system.nodes.ports') }}</span><span class="text-body">HTTP {{ detail.node?.httpPort ?? '—' }} · HTTPS {{ detail.node?.httpsPort ?? '—' }} · RTMP {{ detail.node?.rtmpPort ?? '—' }} · RTSP {{ detail.node?.rtspPort ?? '—' }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">{{ t('system.nodes.rtpSeg') }}</span><span class="text-body">{{ detail.node?.rtpRange || '—' }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">{{ t('system.nodes.streamsOfMax') }}</span><span class="text-body">{{ detail.node?.streams ?? 0 }} / {{ detail.node?.maxStreams ?? 0 }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">{{ t('system.nodes.weight') }}</span><span class="text-body">{{ detail.node?.weight ?? 100 }}</span></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">{{ t('common.status') }}</span><UiTag :color="statusTag(detail.node?.status).color" dot>{{ statusTag(detail.node?.status).text }}</UiTag></div>
            <div class="flex gap-2"><span class="shrink-0 text-placeholder">{{ t('system.nodes.lastKeepalive') }}</span><span class="text-body">{{ ago(detail.node?.lastKeepalive, t) }}</span></div>
            <!-- ACC-02：仅离线且后端带回原因时呈现，占整行避免长文本挤压相邻字段 -->
            <div v-if="detail.node?.status !== 'online' && detail.node?.statusReason" class="col-span-2 flex gap-2">
              <span class="shrink-0 text-placeholder">{{ t('system.nodes.offlineReason') }}</span>
              <span class="text-danger">{{ detail.node.statusReason }}</span>
            </div>
          </div>
        </div>

        <!-- 当前流列表 -->
        <div>
          <p class="mb-2 text-sm font-semibold text-ink">{{ t('system.nodes.streamList') }}</p>
          <UiTable :columns="streamCols" :rows="detail.items" :loading="detail.loading" dense :empty="t('system.nodes.streamEmpty')">
            <template #channel="{ row }">
              <span class="block truncate" :title="row.channel">{{ row.channel || '—' }}</span>
            </template>
            <template #viewers="{ row }">{{ row.viewers ?? 0 }}</template>
            <template #bitrate="{ row }">
              <span class="text-xs">{{ row.bitrate ? (row.bitrate / 1000).toFixed(0) + ' kbps' : '—' }}</span>
            </template>
            <template #ops="{ row }">
              <UiButton variant="dangerText" size="sm" @click="kickStream(row)">{{ t('system.nodes.kick') }}</UiButton>
            </template>
          </UiTable>
          <p v-if="detail.items.length" class="mt-2 text-xs text-placeholder">{{ t('system.nodes.streamStartedAt', { time: fmtTime(detail.items[0].startedAt), n: detail.items.length }) }}</p>
        </div>
      </div>
      <template #footer>
        <div class="flex justify-end gap-2">
          <UiButton @click="detail.open = false">{{ t('common.close') }}</UiButton>
          <UiButton variant="primary" :disabled="!detail.node" @click="showDetail(detail.node)">
            <UiIcon name="refresh" :size="14" />{{ t('system.nodes.refreshStreams') }}
          </UiButton>
        </div>
      </template>
    </UiDrawer>
  </div>
</template>
