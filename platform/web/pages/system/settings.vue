<script setup lang="ts">
// 系统设置（SET-01/02）：全局参数表单 + IDP 服务状态与 CRL 吊销列表管理
const api = useApi()
const toast = useToast()
const { currentProject, loadMe } = useAuth()

const loading = ref(false)
const saving = ref(false)

/* 全局参数表单（缺省时使用默认值） */
const form = reactive({
  streamIdleSec: 30, // 停流等待秒数
  playTokenTtlMin: 10, // 播放 token 有效期（分钟）
  gbPassword: '', // 国标全局 SIP 密码（存储 key: gb.password）
  autoInit: false, // 新设备自动应用默认策略
  tz: 'Asia/Shanghai', // 时区
  captchaRate: 5 // 验证码限速（次/分钟）
})

// 兼容两种存储形态：{ value: xxx } 包装 或 裸值
function pick(v: any): any {
  if (v && typeof v === 'object' && 'value' in v) return v.value
  return v
}

async function loadSettings() {
  loading.value = true
  try {
    const res: any = await api.get('/settings', { scope: 'global' })
    // 返回可能是 JSONB 键值对象（可能为空 {}），也可能是 { items: [...] }
    let map: Record<string, any> = {}
    if (Array.isArray(res?.items)) {
      res.items.forEach((it: any) => {
        if (it?.key !== undefined) map[it.key] = it.value
      })
    } else if (res && typeof res === 'object') {
      map = res
    }
    const idle = pick(map.streamIdleSec)
    form.streamIdleSec = idle === undefined || idle === null || idle === '' ? 30 : Number(idle)
    const ttl = pick(map.playTokenTtlMin)
    form.playTokenTtlMin = ttl === undefined || ttl === null || ttl === '' ? 10 : Number(ttl)
    const gb = pick(map['gb.password'])
    form.gbPassword = gb === undefined || gb === null ? '' : String(gb)
    form.autoInit = !!pick(map.autoInit)
    const tz = pick(map.tz)
    form.tz = tz ? String(tz) : 'Asia/Shanghai'
    const cap = pick(map.captchaRate)
    form.captchaRate = cap === undefined || cap === null || cap === '' ? 5 : Number(cap)
  } catch (e: any) {
    toastApiError(e, '加载设置失败')
  } finally {
    loading.value = false
  }
}

/* 保存：每个字段单独 PUT /settings { key, value: { value }, scope: 'global' } */
async function saveSettings() {
  if (form.streamIdleSec < 5 || form.streamIdleSec > 600) return toast.warning('停流等待秒数应在 5-600 之间')
  if (form.playTokenTtlMin < 1 || form.playTokenTtlMin > 1440) return toast.warning('token 有效期应在 1-1440 分钟之间')
  saving.value = true
  try {
    await api.put('/settings', { key: 'streamIdleSec', value: { value: Number(form.streamIdleSec) }, scope: 'global' })
    await api.put('/settings', { key: 'playTokenTtlMin', value: { value: Number(form.playTokenTtlMin) }, scope: 'global' })
    await api.put('/settings', { key: 'gb.password', value: { value: String(form.gbPassword || '') }, scope: 'global' })
    await api.put('/settings', { key: 'autoInit', value: { value: !!form.autoInit }, scope: 'global' })
    await api.put('/settings', { key: 'tz', value: { value: String(form.tz) }, scope: 'global' })
    await api.put('/settings', { key: 'captchaRate', value: { value: Number(form.captchaRate) }, scope: 'global' })
    toast.success('设置已保存')
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    saving.value = false
  }
}

function resetForm() {
  loadSettings()
  toast.info('已重新加载当前设置')
}

/* ---------------- 国标 SIP 参数（只读，来自 /projects/:id/gb28181/params） ---------------- */
const gbParams = ref<any>(null)
async function loadGbParams() {
  if (!currentProject.value?.id) return
  try {
    gbParams.value = await api.get('/projects/' + currentProject.value.id + '/gb28181/params')
  } catch {
    gbParams.value = null
  }
}

/* ---------------- SET-02 IDP 服务 ---------------- */
const idp = ref<any>(null)
const idpLoading = ref(false)
const brokerOnline = ref<boolean | null>(null)

async function loadIdp() {
  idpLoading.value = true
  try {
    idp.value = await api.get('/idp/config')
    // Broker 在线状态：有 broker 地址且 caStatus active 视为在线（后端未提供显式探活字段时按配置推断）
    brokerOnline.value = !!(idp.value?.broker) && idp.value?.caStatus !== 'revoked'
  } catch {
    idp.value = null // IDP 未配置时静默展示占位
    brokerOnline.value = null
  } finally {
    idpLoading.value = false
  }
}

/* ---------------- CRL 吊销列表管理 ---------------- */
const crlList = ref<any[]>([])
const crlLoading = ref(false)
const crlQuery = ref('')
const crlUploading = ref(false)
const crlFileInput = ref<HTMLInputElement | null>(null)

async function loadCRL() {
  crlLoading.value = true
  try {
    const res: any = await api.get('/idp/crl')
    crlList.value = res?.items || []
  } catch (e: any) {
    toastApiError(e, '查询 CRL 列表失败')
  } finally {
    crlLoading.value = false
  }
}

async function queryCRL() {
  const id = crlQuery.value.trim()
  if (!id) return loadCRL()
  crlLoading.value = true
  try {
    const res: any = await api.get('/idp/crl', { deviceId: id })
    crlList.value = res?.items || []
  } catch (e: any) {
    toastApiError(e, '查询失败')
  } finally {
    crlLoading.value = false
  }
}

/* 上传 CRL：读取 PEM 内容，逐条按设备序列号提交（后端接口 POST /idp/crl { deviceId }） */
async function onCrlFile(e: Event) {
  const input = e.target as HTMLInputElement
  const file = input.files?.[0]
  input.value = ''
  if (!file) return
  const text = await file.text()
  const ids = [...text.matchAll(/-----BEGIN CERTIFICATE REVOCATION LIST-----(?:\s*)([A-Za-z0-9_:\-]+)/g)]
    .map((m) => m[1].trim())
  if (!ids.length) {
    toast.warning('未解析到吊销条目：请在 PEM 文件中以"-----BEGIN CERTIFICATE REVOCATION LIST-----"后跟设备 ID 的格式登记')
    return
  }
  crlUploading.value = true
  let done = 0
  let failed = 0
  for (const id of ids) {
    try {
      await api.post('/idp/crl', { deviceId: id })
      done++
    } catch {
      failed++
    }
  }
  crlUploading.value = false
  if (done) toast.success('已上传 ' + done + ' 条吊销记录' + (failed ? '，' + failed + ' 条失败' : ''))
  else toast.error({ title: 'CRL 上传失败', suggest: '共 ' + failed + ' 条提交失败，请检查设备 ID 与网络后重试。' })
  await loadCRL()
}

const crlCols = [
  { key: 'deviceId', label: '设备 ID', width: '200px' },
  { key: 'revokedAt', label: '吊销时间', width: '170px' },
  { key: 'reason', label: '原因', width: '140px' }
]

const fmtTime = (ts: any) =>
  ts ? new Date(typeof ts === 'string' ? Date.parse(ts) : ts).toLocaleString() : '-'

watch(currentProject, () => loadGbParams())

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await Promise.all([loadSettings(), loadIdp(), loadCRL(), loadGbParams()])
})
</script>

<template>
  <div class="mx-auto max-w-3xl space-y-3">
    <!-- SET-01 停流与播放策略 -->
    <UiCard title="停流与播放策略" flat>
      <UiLoading :loading="loading">
        <div class="grid grid-cols-[160px_1fr] items-center gap-x-3 gap-y-3">
          <span class="text-right text-sm text-body">停流等待时长</span>
          <div class="flex items-center gap-2">
            <UiInput v-model="form.streamIdleSec" width="w-28" :maxlength="4" />
            <span class="text-xs text-placeholder">秒（5-600）：流空闲超过该时长后自动停流</span>
          </div>
          <span class="text-right text-sm text-body">播放 token 有效期</span>
          <div class="flex items-center gap-2">
            <UiInput v-model="form.playTokenTtlMin" width="w-28" :maxlength="5" />
            <span class="text-xs text-placeholder">分钟（1-1440）：播放令牌的有效时长</span>
          </div>
          <span class="text-right text-sm text-body">默认策略</span>
          <div class="flex items-center gap-2">
            <UiSwitch v-model="form.autoInit" />
            <span class="text-xs text-placeholder">新接入设备自动套用默认录像 / 告警策略</span>
          </div>
          <span class="text-right text-sm text-body">时区</span>
          <UiInput v-model="form.tz" placeholder="Asia/Shanghai" :maxlength="64" width="w-60" />
        </div>
      </UiLoading>
    </UiCard>

    <!-- SET-01 信令与接入参数 -->
    <UiCard title="信令与接入参数" flat>
      <div class="grid grid-cols-[160px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body">国标 SIP 密码</span>
        <div class="flex items-center gap-2">
          <UiInput v-model="form.gbPassword" type="password" placeholder="国标设备统一 SIP 认证密码" :maxlength="64" width="w-72" />
          <span class="text-xs text-placeholder">用于 GB28181 设备注册鉴权</span>
        </div>
        <span class="text-right text-sm text-body">SIP 参数生成规则</span>
        <div class="flex flex-wrap items-center gap-2 text-xs text-muted">
          <template v-if="gbParams">
            <UiTag color="default">服务器 ID：{{ gbParams.serverId || '—' }}</UiTag>
            <UiTag color="default">域：{{ gbParams.domain || '—' }}</UiTag>
            <UiTag color="default">端口：{{ gbParams.port || '—' }}</UiTag>
            <UiTag color="default">{{ gbParams.transport || 'UDP' }} · 注册 {{ gbParams.expires || 3600 }}s · 心跳 {{ gbParams.keepalive || 60 }}s</UiTag>
            <span class="text-placeholder">（由部署配置生成，只读）</span>
          </template>
          <span v-else class="text-placeholder">当前项目暂无 SIP 参数</span>
        </div>
        <span class="text-right text-sm text-body">验证码限速</span>
        <div class="flex items-center gap-2">
          <UiInput v-model="form.captchaRate" width="w-28" :maxlength="3" />
          <span class="text-xs text-placeholder">次/分钟：设备接入验证码请求限速</span>
        </div>
      </div>
    </UiCard>

    <!-- SET-02 IDP 服务 -->
    <UiCard title="IDP 服务" flat>
      <template #extra>
        <UiButton size="sm" :disabled="idpLoading" @click="loadIdp">
          <UiIcon name="refresh" :size="13" />刷新
        </UiButton>
      </template>
      <UiLoading :loading="idpLoading">
        <div v-if="idp" class="space-y-3">
          <div class="grid grid-cols-[160px_1fr] items-center gap-x-3 gap-y-3">
            <span class="text-right text-sm text-body">Broker 连接状态</span>
            <div class="flex items-center gap-2">
              <UiTag v-if="brokerOnline === true" color="success" dot>在线</UiTag>
              <UiTag v-else-if="brokerOnline === false" color="danger" dot>离线</UiTag>
              <UiTag v-else color="info" dot>未知</UiTag>
              <span class="text-xs text-placeholder">{{ idp.broker || '未配置 Broker 地址' }}</span>
            </div>
            <span class="text-right text-sm text-body">TLS</span>
            <UiTag :color="idp.tls ? 'success' : 'info'">{{ idp.tls ? '已启用' : '未启用' }}</UiTag>
            <span class="text-right text-sm text-body">设备 CA 信息</span>
            <div class="flex flex-wrap items-center gap-2 text-sm text-body">
              <span>状态：<UiTag :color="idp.caStatus === 'active' ? 'success' : 'warning'">{{ idp.caStatus || '—' }}</UiTag></span>
              <span v-if="idp.idpCount !== undefined" class="text-xs text-placeholder">已接入 IDP 设备 {{ idp.idpCount }} 台</span>
            </div>
          </div>
        </div>
        <UiEmptyState v-else text="IDP 服务未配置" />
      </UiLoading>

      <!-- CRL 吊销列表管理 -->
      <div class="mt-4 border-t border-line-soft pt-3">
        <div class="mb-3 flex flex-wrap items-center gap-2">
          <span class="text-sm font-semibold text-ink">CRL 吊销列表</span>
          <UiInput v-model="crlQuery" placeholder="按设备 ID 查询" clearable width="w-52" size="sm" @enter="queryCRL" @clear="loadCRL" />
          <UiButton size="sm" :disabled="crlLoading" @click="queryCRL">
            <UiIcon name="search" :size="13" />查询
          </UiButton>
          <div class="ml-auto flex items-center gap-2">
            <input ref="crlFileInput" type="file" accept=".pem,.txt,.crl,.list" class="hidden" @change="onCrlFile" />
            <UiButton variant="primary" size="sm" :disabled="crlUploading" @click="crlFileInput?.click()">
              <UiIcon name="upload" :size="13" />{{ crlUploading ? '上传中…' : '上传 CRL' }}
            </UiButton>
          </div>
        </div>
        <UiTable :columns="crlCols" :rows="crlList" :loading="crlLoading" dense empty="暂无吊销记录">
          <template #revokedAt="{ row }">{{ fmtTime(row.revokedAt || row.ts || row.createdAt) }}</template>
          <template #reason="{ row }">{{ row.reason || '手动吊销' }}</template>
        </UiTable>
        <p class="mt-2 text-xs text-placeholder">
          上传格式：PEM 文本，每条以 "-----BEGIN CERTIFICATE REVOCATION LIST-----" 开头、下一行为设备 ID。吊销后的设备将无法再接入。
        </p>
      </div>
    </UiCard>

    <!-- 底部保存/重置 -->
    <div class="flex items-center justify-end gap-2 pb-2">
      <UiButton @click="resetForm">重置</UiButton>
      <UiButton variant="primary" :disabled="saving || loading" @click="saveSettings">
        {{ saving ? '保存中…' : '保存设置' }}
      </UiButton>
    </div>
  </div>
</template>
