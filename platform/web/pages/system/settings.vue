<script setup lang="ts">
// 系统设置（SET-01/02）：全局参数表单 + IDP 服务状态与 CRL 吊销列表管理
const api = useApi()
const toast = useToast()
const { currentProject, loadMe } = useAuth()

const loading = ref(false)
const saving = ref(false)

/* 左侧二级导航（纯展示分组，不影响下方任何数据/校验/提交逻辑） */
const sections = [
  { key: 'general', label: '系统参数', icon: 'sliders' },
  { key: 'idp', label: 'IDP 证书 / CRL', icon: 'shield' }
] as const
const activeSection = ref<'general' | 'idp'>('general')

/* 全局参数表单（缺省时使用默认值） */
const form = reactive({
  streamIdleSec: 30, // 停流等待秒数
  playTokenTtlMin: 10, // 播放 token 有效期（分钟）
  gbPassword: '', // 国标全局 SIP 密码（存储 key: gb.password）
  tz: 'Asia/Shanghai', // 时区
  captchaRate: 5, // 验证码限速（次/分钟）
  // 默认录像策略（ADD-09）：项目级设置，scope = 当前项目 ID，与上面的全局项分开读写
  recordDefaults: { enabled: false, templateId: '', profile: 'main' },
  // 默认告警策略（ADD-09）：项目级设置，与 recordDefaults 同 scope
  alarmDefaults: { enabled: false, kinds: [] as string[], templateId: '' }
})

/* 本项目的录像模板（供默认策略下拉选择） */
const recordTemplates = ref<any[]>([])
const recordTemplateOptions = computed(() =>
  recordTemplates.value.map((t: any) => ({ label: t.name, value: t.id }))
)

/* 本项目的布防模板（供默认告警策略下拉选择） */
const alarmTemplates = ref<any[]>([])
const alarmTemplateOptions = computed(() =>
  alarmTemplates.value.map((t: any) => ({ label: t.name, value: t.id }))
)

/* 设备侧告警类型（ALM-03）；平台侧事件由「告警策略」页管理，不在此处 */
const ALARM_KINDS = [
  { value: 'motion', label: '移动侦测' },
  { value: 'humanoid', label: '人形侦测' },
  { value: 'intrusion', label: '区域入侵' },
  { value: 'linecross', label: '越界侦测' },
  { value: 'tamper', label: '视频遮挡' },
  { value: 'io', label: '外接IO' }
]
function toggleAlarmKind(k: string) {
  const i = form.alarmDefaults.kinds.indexOf(k)
  if (i >= 0) form.alarmDefaults.kinds.splice(i, 1)
  else form.alarmDefaults.kinds.push(k)
}

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
    const tz = pick(map.tz)
    form.tz = tz ? String(tz) : 'Asia/Shanghai'
    const cap = pick(map.captchaRate)
    form.captchaRate = cap === undefined || cap === null || cap === '' ? 5 : Number(cap)
  } catch (e: any) {
    toastApiError(e, '加载设置失败')
  } finally {
    loading.value = false
  }
  await loadRecordDefaults()
}

/* 默认策略区块（录像/告警）加载状态：加载失败时禁用该区块保存按钮，直到重新加载成功 */
const defaultsLoadFailed = ref(false)

/* 默认录像策略（ADD-09）：项目级 scope，与上面的 global 设置分开读取 */
async function loadRecordDefaults() {
  const pid = currentProject.value?.id
  if (!pid) return
  try {
    const [tpls, atpls, st]: any[] = await Promise.all([
      api.get('/record-templates'),
      api.get('/alarm-templates'),
      api.get('/settings', { scope: pid })
    ])
    recordTemplates.value = tpls?.items || []
    alarmTemplates.value = atpls?.items || []
    // 服务端以裸对象存储（devsvc 直接读 value.enabled），此处不做 { value: x } 解包
    const rd = st?.recordDefaults || {}
    form.recordDefaults.enabled = !!rd.enabled
    form.recordDefaults.templateId = String(rd.templateId || '')
    form.recordDefaults.profile = rd.profile === 'sub' ? 'sub' : 'main'
    const ad = st?.alarmDefaults || {}
    form.alarmDefaults.enabled = !!ad.enabled
    form.alarmDefaults.templateId = String(ad.templateId || '')
    form.alarmDefaults.kinds = Array.isArray(ad.kinds) ? ad.kinds.map(String) : []
    defaultsLoadFailed.value = false
  } catch (e: any) {
    defaultsLoadFailed.value = true
    toastApiError(e, '默认策略加载失败')
  }
}

/* 保存：每个字段单独 PUT /settings { key, value: { value }, scope: 'global' } */
async function saveSettings() {
  if (form.streamIdleSec < 5 || form.streamIdleSec > 600) return toast.warning('停流等待秒数应在 5-600 之间')
  if (form.playTokenTtlMin < 1 || form.playTokenTtlMin > 1440) return toast.warning('token 有效期应在 1-1440 分钟之间')
  if (form.recordDefaults.enabled && !form.recordDefaults.templateId) {
    return toast.warning('已开启默认录像策略，请选择要套用的录像模板')
  }
  if (form.alarmDefaults.enabled && !form.alarmDefaults.kinds.length) {
    return toast.warning('已开启默认告警策略，请至少选择一种告警类型')
  }
  saving.value = true
  try {
    await api.put('/settings', { key: 'streamIdleSec', value: { value: Number(form.streamIdleSec) }, scope: 'global' })
    await api.put('/settings', { key: 'playTokenTtlMin', value: { value: Number(form.playTokenTtlMin) }, scope: 'global' })
    await api.put('/settings', { key: 'gb.password', value: { value: String(form.gbPassword || '') }, scope: 'global' })
    await api.put('/settings', { key: 'tz', value: { value: String(form.tz) }, scope: 'global' })
    await api.put('/settings', { key: 'captchaRate', value: { value: Number(form.captchaRate) }, scope: 'global' })
    // 默认录像策略：项目级 scope，且以裸对象存储（devsvc.ApplyDefaultRecordPlan 直接读 value.enabled/templateId/profile）
    const pid = currentProject.value?.id
    if (pid) {
      await api.put('/settings', {
        key: 'recordDefaults',
        value: {
          enabled: !!form.recordDefaults.enabled,
          templateId: String(form.recordDefaults.templateId || ''),
          profile: form.recordDefaults.profile === 'sub' ? 'sub' : 'main'
        },
        scope: pid
      })
      // 默认告警策略：项目级 scope，且以裸对象存储（devsvc.ApplyDefaultAlarmRule 直接读 value.enabled/kinds/templateId）
      await api.put('/settings', {
        key: 'alarmDefaults',
        value: {
          enabled: !!form.alarmDefaults.enabled,
          kinds: form.alarmDefaults.kinds.slice(),
          templateId: String(form.alarmDefaults.templateId || '')
        },
        scope: pid
      })
    }
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
const crlAddVisible = ref(false)
const crlAddText = ref('')
const crlAdding = ref(false)

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

/* 新增吊销设备：按行输入设备 ID，逐条提交（后端接口 POST /idp/crl { deviceId }） */
function openCrlAddDlg() {
  crlAddText.value = ''
  crlAddVisible.value = true
}

async function submitCrlAdd() {
  const ids = crlAddText.value
    .split('\n')
    .map((s) => s.trim())
    .filter(Boolean)
  if (!ids.length) return toast.warning('请输入至少一个设备 ID')
  crlAdding.value = true
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
  crlAdding.value = false
  if (done) toast.success('已吊销 ' + done + ' 台设备' + (failed ? '，' + failed + ' 台失败' : ''))
  else toast.error({ title: 'CRL 提交失败', suggest: '共 ' + failed + ' 条提交失败，请检查设备 ID 与网络后重试。' })
  if (done) crlAddVisible.value = false
  await loadCRL()
}

const crlCols = [
  { key: 'deviceId', label: '设备 ID', width: '200px' },
  { key: 'revokedAt', label: '吊销时间', width: '170px' },
  { key: 'reason', label: '原因', width: '140px' }
]


watch(currentProject, () => loadGbParams())

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await Promise.all([loadSettings(), loadIdp(), loadCRL(), loadGbParams()])
})
</script>

<template>
  <div class="mx-auto flex max-w-5xl items-start gap-4">
    <!-- 左侧二级导航 -->
    <div class="w-48 shrink-0 space-y-0.5 rounded-signal border border-line bg-surface p-1.5">
      <button
        v-for="s in sections" :key="s.key" type="button"
        class="flex w-full items-center gap-2 rounded-chrome px-3 py-2 text-left text-sm transition-colors"
        :class="activeSection === s.key ? 'bg-primary-soft font-medium text-primary' : 'text-body hover:bg-zone'"
        @click="activeSection = s.key"
      >
        <UiIcon :name="s.icon" :size="15" />{{ s.label }}
      </button>
    </div>

    <!-- 右侧表单区 -->
    <div class="min-w-0 flex-1 space-y-3">
      <!-- 系统参数：停流/播放策略 + 信令与接入参数 -->
      <template v-if="activeSection === 'general'">
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
              <span class="text-right text-sm text-body">默认录像策略</span>
              <div class="flex flex-wrap items-center gap-2">
                <UiSwitch v-model="form.recordDefaults.enabled" aria-label="新设备默认开启录像" />
                <span class="text-xs text-placeholder">新接入设备的通道自动套用录像计划（ADD-09），仅对本项目生效</span>
                <template v-if="form.recordDefaults.enabled">
                  <div class="flex w-full items-center gap-2 pt-1">
                    <UiSelect
                      v-model="form.recordDefaults.templateId"
                      :options="recordTemplateOptions"
                      placeholder="选择录像模板"
                      width="w-44"
                    />
                    <UiSelect
                      v-model="form.recordDefaults.profile"
                      :options="[{ label: '主码流', value: 'main' }, { label: '子码流', value: 'sub' }]"
                      width="w-28"
                    />
                    <span v-if="!recordTemplateOptions.length" class="text-xs text-warning">
                      本项目暂无录像模板，请先在「计划 → 计划模板」创建
                    </span>
                  </div>
                </template>
              </div>
              <span class="text-right text-sm text-body">默认告警策略</span>
              <div class="flex flex-wrap items-center gap-2">
                <UiSwitch v-model="form.alarmDefaults.enabled" aria-label="新设备默认开启告警" />
                <span class="text-xs text-placeholder">新接入设备的通道自动套用告警规则（ADD-09），仅对本项目生效</span>
                <template v-if="form.alarmDefaults.enabled">
                  <div class="flex w-full flex-wrap items-center gap-2 pt-1">
                    <UiSelect
                      v-model="form.alarmDefaults.templateId"
                      :options="alarmTemplateOptions"
                      placeholder="选择布防模板"
                      width="w-44"
                    />
                    <span v-if="!alarmTemplateOptions.length" class="text-xs text-warning">
                      本项目暂无布防模板，请先在「告警 → 布防模板」创建
                    </span>
                  </div>
                  <div class="flex w-full flex-wrap items-center gap-1.5 pt-1">
                    <button
                      v-for="k in ALARM_KINDS" :key="k.value" type="button"
                      class="rounded-signal border px-2 py-0.5 text-xs transition-colors"
                      :class="form.alarmDefaults.kinds.includes(k.value)
                        ? 'border-primary bg-primary-softer text-primary'
                        : 'border-line text-placeholder hover:border-placeholder'"
                      @click="toggleAlarmKind(k.value)"
                    >{{ k.label }}</button>
                  </div>
                </template>
                <span v-else class="w-full pt-1 text-xs text-warning">
                  关闭后，本项目新接入设备的通道不会自动创建告警规则，其移动侦测、人形侦测等设备侧告警会被永久拦截且不可补发（严格模式下无规则的通道等同于全部丢弃事件）；如需该通道产生告警，请到「告警 → 告警规则」手动创建
                </span>
              </div>
              <span class="text-right text-sm text-body">时区</span>
              <UiInput v-model="form.tz" placeholder="Asia/Shanghai" :maxlength="64" width="w-60" />
            </div>
          </UiLoading>
        </UiCard>

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
                <UiTag color="default">服务器 ID <span class="font-mono">{{ gbParams.serverId || '—' }}</span></UiTag>
                <UiTag color="default">域 <span class="font-mono">{{ gbParams.domain || '—' }}</span></UiTag>
                <UiTag color="default">端口 <span class="font-mono">{{ gbParams.port || '—' }}</span></UiTag>
                <UiTag color="default">
                  {{ gbParams.transport || 'UDP' }} · 注册 <span class="font-mono">{{ gbParams.expires || 3600 }}s</span>
                  · 心跳 <span class="font-mono">{{ gbParams.keepalive || 60 }}s</span>
                </UiTag>
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

        <div class="flex items-center justify-end gap-2 pb-2">
          <UiButton @click="resetForm">重置</UiButton>
          <UiTooltip v-if="defaultsLoadFailed" label="默认策略加载失败，重新加载成功后才能保存">
            <span><UiButton variant="primary" disabled>保存设置</UiButton></span>
          </UiTooltip>
          <UiButton v-else variant="primary" :disabled="saving || loading" @click="saveSettings">
            {{ saving ? '保存中…' : '保存设置' }}
          </UiButton>
        </div>
      </template>

      <!-- IDP 证书 / CRL -->
      <template v-else>
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
                  <span class="text-xs text-placeholder" :class="idp.broker ? 'font-mono' : ''">{{ idp.broker || '未配置 Broker 地址' }}</span>
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
                <UiButton variant="primary" size="sm" @click="openCrlAddDlg">
                  <UiIcon name="plus" :size="13" />添加吊销设备
                </UiButton>
              </div>
            </div>
            <UiTable :columns="crlCols" :rows="crlList" :loading="crlLoading" dense empty="暂无吊销记录">
              <template #deviceId="{ row }"><span class="font-mono text-body">{{ row.deviceId }}</span></template>
              <template #revokedAt="{ row }"><span class="font-mono">{{ fmtTime(row.revokedAt || row.ts || row.createdAt) }}</span></template>
              <template #reason="{ row }">{{ row.reason || '手动吊销' }}</template>
            </UiTable>
            <p class="mt-2 text-xs text-placeholder">
              设备 ID 一经吊销将无法再接入 IDP 服务。
            </p>
          </div>
        </UiCard>
      </template>
    </div>

    <!-- 添加吊销设备对话框 -->
    <UiDialog v-model:open="crlAddVisible" title="添加吊销设备" width="max-w-md">
      <div class="space-y-2">
        <p class="text-sm text-body">输入吊销设备 ID（多行，每行一个）</p>
        <textarea
          v-model="crlAddText"
          rows="6"
          placeholder="设备 ID，每行一个"
          class="w-full rounded-chrome border border-line bg-surface px-3 py-2 font-mono text-sm text-ink outline-none focus-visible:border-primary"
        />
      </div>
      <template #footer>
        <UiButton @click="crlAddVisible = false">取消</UiButton>
        <UiButton variant="primary" :disabled="crlAdding" @click="submitCrlAdd">{{ crlAdding ? '提交中…' : '确定' }}</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
