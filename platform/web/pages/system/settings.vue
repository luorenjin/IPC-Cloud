<script setup lang="ts">
// 系统设置（SET-01/02）：全局参数表单 + IDP 服务状态与 CRL 吊销列表管理
const api = useApi()
const toast = useToast()
const { t } = useI18n()
const { currentProject, loadMe } = useAuth()

const loading = ref(false)
const saving = ref(false)

/* 左侧二级导航（纯展示分组，不影响下方任何数据/校验/提交逻辑） */
const sections = computed(() => [
  { key: 'general' as const, label: t('system.settings.navGeneral'), icon: 'sliders' },
  { key: 'idp' as const, label: t('system.settings.navIdp'), icon: 'shield' }
])
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
const ALARM_KINDS = computed(() => [
  { value: 'motion', label: t('system.settings.kindMotion') },
  { value: 'humanoid', label: t('system.settings.kindHumanoid') },
  { value: 'intrusion', label: t('system.settings.kindIntrusion') },
  { value: 'linecross', label: t('system.settings.kindLinecross') },
  { value: 'tamper', label: t('system.settings.kindTamper') },
  { value: 'io', label: t('system.settings.kindIo') }
])
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
    toastApiError(e, t('system.msg.settingsLoadFailed'))
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
    toastApiError(e, t('system.msg.defaultsLoadFailed'))
  }
}

/* 保存：每个字段单独 PUT /settings { key, value: { value }, scope: 'global' } */
async function saveSettings() {
  if (form.streamIdleSec < 5 || form.streamIdleSec > 600) return toast.warning(t('system.msg.idleRange'))
  if (form.playTokenTtlMin < 1 || form.playTokenTtlMin > 1440) return toast.warning(t('system.msg.tokenRange'))
  if (form.recordDefaults.enabled && !form.recordDefaults.templateId) {
    return toast.warning(t('system.msg.needRecordTemplate'))
  }
  if (form.alarmDefaults.enabled && !form.alarmDefaults.kinds.length) {
    return toast.warning(t('system.msg.needAlarmKind'))
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
    toast.success(t('system.msg.settingsSaved'))
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    saving.value = false
  }
}

function resetForm() {
  loadSettings()
  toast.info(t('system.msg.settingsReloaded'))
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
    toastApiError(e, t('system.msg.crlLoadFailed'))
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
    toastApiError(e, t('system.msg.crlQueryFailed'))
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
  if (!ids.length) return toast.warning(t('system.msg.crlNeedDeviceId'))
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
  if (done) {
    toast.success(
      failed
        ? t('system.msg.crlRevokedPartial', { n: done, failed })
        : t('system.msg.crlRevoked', { n: done })
    )
  } else {
    toast.error({
      title: t('system.msg.crlSubmitFailed'),
      suggest: t('system.msg.crlSubmitFailedSuggest', { n: failed })
    })
  }
  if (done) crlAddVisible.value = false
  await loadCRL()
}

const crlCols = computed(() => [
  { key: 'deviceId', label: t('system.settings.crlDeviceId'), width: '200px' },
  { key: 'revokedAt', label: t('system.settings.crlRevokedAt'), width: '170px' },
  { key: 'reason', label: t('system.settings.crlReason'), width: '140px' }
])


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
        <UiCard :title="t('system.settings.streamCard')" flat>
          <UiLoading :loading="loading">
            <div class="grid grid-cols-[160px_1fr] items-center gap-x-3 gap-y-3">
              <span class="text-right text-sm text-body">{{ t('system.settings.streamIdle') }}</span>
              <div class="flex items-center gap-2">
                <UiInput v-model="form.streamIdleSec" width="w-28" :maxlength="4" />
                <span class="text-xs text-placeholder">{{ t('system.settings.streamIdleHint') }}</span>
              </div>
              <span class="text-right text-sm text-body">{{ t('system.settings.playToken') }}</span>
              <div class="flex items-center gap-2">
                <UiInput v-model="form.playTokenTtlMin" width="w-28" :maxlength="5" />
                <span class="text-xs text-placeholder">{{ t('system.settings.playTokenHint') }}</span>
              </div>
              <span class="text-right text-sm text-body">{{ t('system.settings.recordDefaults') }}</span>
              <div class="flex flex-wrap items-center gap-2">
                <UiSwitch v-model="form.recordDefaults.enabled" :aria-label="t('system.settings.recordDefaultsAria')" />
                <span class="text-xs text-placeholder">{{ t('system.settings.recordDefaultsHint') }}</span>
                <template v-if="form.recordDefaults.enabled">
                  <div class="flex w-full items-center gap-2 pt-1">
                    <UiSelect
                      v-model="form.recordDefaults.templateId"
                      :options="recordTemplateOptions"
                      :placeholder="t('system.settings.pickRecordTemplate')"
                      width="w-44"
                    />
                    <UiSelect
                      v-model="form.recordDefaults.profile"
                      :options="[{ label: t('system.settings.profileMain'), value: 'main' }, { label: t('system.settings.profileSub'), value: 'sub' }]"
                      width="w-28"
                    />
                    <span v-if="!recordTemplateOptions.length" class="text-xs text-warning">
                      {{ t('system.settings.noRecordTemplate') }}
                    </span>
                  </div>
                </template>
              </div>
              <span class="text-right text-sm text-body">{{ t('system.settings.alarmDefaults') }}</span>
              <div class="flex flex-wrap items-center gap-2">
                <UiSwitch v-model="form.alarmDefaults.enabled" :aria-label="t('system.settings.alarmDefaultsAria')" />
                <span class="text-xs text-placeholder">{{ t('system.settings.alarmDefaultsHint') }}</span>
                <template v-if="form.alarmDefaults.enabled">
                  <div class="flex w-full flex-wrap items-center gap-2 pt-1">
                    <UiSelect
                      v-model="form.alarmDefaults.templateId"
                      :options="alarmTemplateOptions"
                      :placeholder="t('system.settings.pickAlarmTemplate')"
                      width="w-44"
                    />
                    <span v-if="!alarmTemplateOptions.length" class="text-xs text-warning">
                      {{ t('system.settings.noAlarmTemplate') }}
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
                  {{ t('system.settings.alarmDefaultsOffHint') }}
                </span>
              </div>
              <span class="text-right text-sm text-body">{{ t('system.settings.tz') }}</span>
              <UiInput v-model="form.tz" placeholder="Asia/Shanghai" :maxlength="64" width="w-60" />
            </div>
          </UiLoading>
        </UiCard>

        <UiCard :title="t('system.settings.signalCard')" flat>
          <div class="grid grid-cols-[160px_1fr] items-center gap-x-3 gap-y-3">
            <span class="text-right text-sm text-body">{{ t('system.settings.gbPassword') }}</span>
            <div class="flex items-center gap-2">
              <UiInput v-model="form.gbPassword" type="password" :placeholder="t('system.settings.gbPasswordPlaceholder')" :maxlength="64" width="w-72" />
              <span class="text-xs text-placeholder">{{ t('system.settings.gbPasswordHint') }}</span>
            </div>
            <span class="text-right text-sm text-body">{{ t('system.settings.gbParams') }}</span>
            <div class="flex flex-wrap items-center gap-2 text-xs text-muted">
              <template v-if="gbParams">
                <UiTag color="default">{{ t('system.settings.gbServerId') }} <span class="font-mono">{{ gbParams.serverId || '—' }}</span></UiTag>
                <UiTag color="default">{{ t('system.settings.gbDomain') }} <span class="font-mono">{{ gbParams.domain || '—' }}</span></UiTag>
                <UiTag color="default">{{ t('system.settings.gbPort') }} <span class="font-mono">{{ gbParams.port || '—' }}</span></UiTag>
                <UiTag color="default">
                  {{ gbParams.transport || 'UDP' }} · {{ t('system.settings.gbRegister') }} <span class="font-mono">{{ gbParams.expires || 3600 }}s</span>
                  · {{ t('system.settings.gbKeepalive') }} <span class="font-mono">{{ gbParams.keepalive || 60 }}s</span>
                </UiTag>
                <span class="text-placeholder">{{ t('system.settings.gbReadonly') }}</span>
              </template>
              <span v-else class="text-placeholder">{{ t('system.settings.gbNoParams') }}</span>
            </div>
            <span class="text-right text-sm text-body">{{ t('system.settings.captchaRate') }}</span>
            <div class="flex items-center gap-2">
              <UiInput v-model="form.captchaRate" width="w-28" :maxlength="3" />
              <span class="text-xs text-placeholder">{{ t('system.settings.captchaRateHint') }}</span>
            </div>
          </div>
        </UiCard>

        <div class="flex items-center justify-end gap-2 pb-2">
          <UiButton @click="resetForm">{{ t('common.reset') }}</UiButton>
          <UiTooltip v-if="defaultsLoadFailed" :label="t('system.settings.defaultsLoadFailedTip')">
            <span><UiButton variant="primary" disabled>{{ t('system.settings.saveSettings') }}</UiButton></span>
          </UiTooltip>
          <UiButton v-else variant="primary" :disabled="saving || loading" @click="saveSettings">
            {{ saving ? t('common.saving') : t('system.settings.saveSettings') }}
          </UiButton>
        </div>
      </template>

      <!-- IDP 证书 / CRL -->
      <template v-else>
        <UiCard :title="t('system.settings.idpCard')" flat>
          <template #extra>
            <UiButton size="sm" :disabled="idpLoading" @click="loadIdp">
              <UiIcon name="refresh" :size="13" />{{ t('common.refresh') }}
            </UiButton>
          </template>
          <UiLoading :loading="idpLoading">
            <div v-if="idp" class="space-y-3">
              <div class="grid grid-cols-[160px_1fr] items-center gap-x-3 gap-y-3">
                <span class="text-right text-sm text-body">{{ t('system.settings.brokerStatus') }}</span>
                <div class="flex items-center gap-2">
                  <UiTag v-if="brokerOnline === true" color="success" dot>{{ t('common.online') }}</UiTag>
                  <UiTag v-else-if="brokerOnline === false" color="danger" dot>{{ t('common.offline') }}</UiTag>
                  <UiTag v-else color="info" dot>{{ t('common.unknown') }}</UiTag>
                  <span class="text-xs text-placeholder" :class="idp.broker ? 'font-mono' : ''">{{ idp.broker || t('system.settings.brokerUnset') }}</span>
                </div>
                <span class="text-right text-sm text-body">TLS</span>
                <UiTag :color="idp.tls ? 'success' : 'info'">{{ idp.tls ? t('common.enabled') : t('common.disabled') }}</UiTag>
                <span class="text-right text-sm text-body">{{ t('system.settings.caInfo') }}</span>
                <div class="flex flex-wrap items-center gap-2 text-sm text-body">
                  <span>{{ t('system.settings.caStatus') }}<UiTag :color="idp.caStatus === 'active' ? 'success' : 'warning'">{{ idp.caStatus || '—' }}</UiTag></span>
                  <span v-if="idp.idpCount !== undefined" class="text-xs text-placeholder">{{ t('system.settings.idpCount', { n: idp.idpCount }) }}</span>
                </div>
              </div>
            </div>
            <UiEmptyState v-else :text="t('system.settings.idpUnconfigured')" />
          </UiLoading>

          <!-- CRL 吊销列表管理 -->
          <div class="mt-4 border-t border-line-soft pt-3">
            <div class="mb-3 flex flex-wrap items-center gap-2">
              <span class="text-sm font-semibold text-ink">{{ t('system.settings.crlTitle') }}</span>
              <UiInput v-model="crlQuery" :placeholder="t('system.settings.crlQueryPlaceholder')" clearable width="w-52" size="sm" @enter="queryCRL" @clear="loadCRL" />
              <UiButton size="sm" :disabled="crlLoading" @click="queryCRL">
                <UiIcon name="search" :size="13" />{{ t('common.query') }}
              </UiButton>
              <div class="ml-auto flex items-center gap-2">
                <UiButton variant="primary" size="sm" @click="openCrlAddDlg">
                  <UiIcon name="plus" :size="13" />{{ t('system.settings.crlAdd') }}
                </UiButton>
              </div>
            </div>
            <UiTable :columns="crlCols" :rows="crlList" :loading="crlLoading" dense :empty="t('system.settings.crlEmpty')">
              <template #deviceId="{ row }"><span class="font-mono text-body">{{ row.deviceId }}</span></template>
              <template #revokedAt="{ row }"><span class="font-mono">{{ fmtTime(row.revokedAt || row.ts || row.createdAt) }}</span></template>
              <template #reason="{ row }">{{ row.reason || t('system.settings.crlReasonManual') }}</template>
            </UiTable>
            <p class="mt-2 text-xs text-placeholder">
              {{ t('system.settings.crlNote') }}
            </p>
          </div>
        </UiCard>
      </template>
    </div>

    <!-- 添加吊销设备对话框 -->
    <UiDialog v-model:open="crlAddVisible" :title="t('system.settings.crlAdd')" width="max-w-md">
      <div class="space-y-2">
        <p class="text-sm text-body">{{ t('system.settings.crlAddHint') }}</p>
        <textarea
          v-model="crlAddText"
          rows="6"
          :placeholder="t('system.settings.crlAddPlaceholder')"
          class="w-full rounded-chrome border border-line bg-surface px-3 py-2 font-mono text-sm text-ink outline-none focus-visible:border-primary"
        />
      </div>
      <template #footer>
        <UiButton @click="crlAddVisible = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="crlAdding" @click="submitCrlAdd">{{ crlAdding ? t('system.settings.submitting') : t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
