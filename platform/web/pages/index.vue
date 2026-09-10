<script setup lang="ts">
// 首页「值守总览」（DASH-01/02）。
//
// 信息层级：值班员真正要看的是"哪台离线了、刚才报了什么警"，
// 因此实时告警流占主位；在线率等汇总指标压缩为一行紧凑指标条。
// 不再重复侧栏导航（原「功能入口」7 面板与侧栏 1:1 重复，纯占面积不给新信息）。
const api = useApi()
const router = useRouter()
const { currentProject } = useAuth()
const { t } = useI18n()

// 变量名不用 dash——会遮蔽 utils/format 自动导入的 dash() 空值兜底函数
const dashboard = ref<any>(null)
const loading = ref(true)
const loadErr = ref('')

async function load(silent = false) {
  if (!silent) loading.value = true
  loadErr.value = ''
  try {
    dashboard.value = await api.get('/dashboard')
  } catch (e: any) {
    // 首页整体加载失败要给出可重试的错误态，而不是留一片空白
    loadErr.value = e?.msg || t('account.msg.dashboardLoadFailed')
  } finally {
    loading.value = false
  }
}

// ---------- 汇总指标（DASH-01 六项） ----------
const onlineRate = computed(() => {
  const t = dashboard.value?.deviceTotal || 0
  const o = dashboard.value?.deviceOnline || 0
  return t ? Math.round((o / t) * 100) : null
})
const offlineCount = computed(() => {
  const t = dashboard.value?.deviceTotal || 0
  const o = dashboard.value?.deviceOnline || 0
  return Math.max(0, t - o)
})
/** 节点健康：全部在线为正常，否则给出离线台数 */
const nodeSummary = computed(() => {
  const list = dashboard.value?.nodes || []
  if (!list.length) return { text: t('account.dashboard.nodeUnset'), tone: 'muted' }
  const off = list.filter((n: any) => n.status !== 'online').length
  return off
    ? { text: t('account.dashboard.nodeOffline', { off, total: list.length }), tone: 'danger' }
    : { text: t('account.dashboard.nodeHealthy', { n: list.length }), tone: 'success' }
})

/** 指标条：数值为 null 表示后端未提供，显示 — 而不是编造 0 */
const metrics = computed(() => [
  { label: t('account.dashboard.deviceTotal'), value: dashboard.value?.deviceTotal ?? null, path: '/devices' },
  { label: t('account.dashboard.deviceOnline'), value: dashboard.value?.deviceOnline ?? null, tone: 'success', path: '/devices?status=online' },
  { label: t('account.dashboard.deviceOffline'), value: dashboard.value ? offlineCount.value : null, tone: offlineCount.value ? 'danger' : undefined, path: '/devices?status=offline' },
  { label: t('account.dashboard.channelTotal'), value: dashboard.value?.channelTotal ?? null, path: '/live' },
  { label: t('account.dashboard.playing'), value: dashboard.value?.playing ?? null, tone: 'primary', path: '/live' },
  { label: t('account.dashboard.alarmToday'), value: dashboard.value?.alarmToday ?? null, tone: dashboard.value?.alarmToday ? 'warning' : undefined, path: '/alarms' }
])

const TONE_CLASS: Record<string, string> = {
  success: 'text-success', danger: 'text-danger', warning: 'text-warning',
  primary: 'text-primary', muted: 'text-muted'
}

// ---------- 来源分布 ----------
const srcRows = computed(() => {
  // 来源展示名/颜色见 utils/enums.ts SOURCE_MAP（PRD §9.1 四色语义固定）
  const rows = SOURCES.map((k) => ({
    key: k, label: t(SOURCE_MAP[k].longLabelKey), color: SOURCE_MAP[k].cssVar,
    count: dashboard.value?.bySource?.[k] ?? 0
  }))
  const max = Math.max(1, ...rows.map((r) => r.count))
  // pct 相对最大类目，仅用于分布条宽度；最小 6% 保证非零占比可见
  return rows.map((r) => ({ ...r, pct: r.count ? Math.max(6, Math.round((r.count / max) * 100)) : 0 }))
})
const hasAnyDevice = computed(() => srcRows.value.some((r) => r.count > 0))

// ---------- 实时告警流 ----------
function levelBar(level: string) { return alarmLevelInfo(level).cssVar }

// /dashboard 的 recentAlarms 只带 deviceId/channelId，需要名称用于展示。
// 只查这几条用到的 id，不再为取名字全量拉 /devices + /channels。
const nameMap = ref<Record<string, string>>({})
async function loadNames(alarms: any[]) {
  const chIds = [...new Set(alarms.map((a) => a.channelId).filter(Boolean))]
  const devIds = [...new Set(alarms.map((a) => a.deviceId).filter((d) => d))]
  if (!chIds.length && !devIds.length) return
  try {
    const reqs: Promise<any>[] = []
    if (chIds.length) reqs.push(api.get('/channels', { ids: chIds.join(',') }))
    if (devIds.length) reqs.push(api.get('/devices', { ids: devIds.join(',') }))
    const res = await Promise.all(reqs)
    const m: Record<string, string> = { ...nameMap.value }
    res.forEach((r: any) => (r?.items || []).forEach((it: any) => { if (it?.id && it?.name) m[it.id] = it.name }))
    nameMap.value = m
  } catch {
    // 名称只是锦上添花，取不到就回落显示 ID，不打断告警流本身
  }
}

const alarmRows = computed(() => (dashboard.value?.recentAlarms || []).slice(0, 10).map((a: any) => ({
  id: a.id,
  level: a.level || 'info',
  kind: t(alarmKindKey(a.kind)),
  msg: a.data?.error?.msg || a.data?.name || t(alarmKindKey(a.kind)),
  src: nameMap.value[a.channelId] || nameMap.value[a.deviceId] || a.channelId || a.deviceId || '—',
  ts: a.ts || 0
})))

watch(() => dashboard.value?.recentAlarms, (list) => { if (list?.length) loadNames(list) })

/** 点击告警跳到消息中心并带上该条 id（DASH-02：最近告警可点击） */
function openAlarm(a: any) {
  router.push({ path: '/alarms', query: { focus: a.id } })
}

onMounted(() => load())

// 实时刷新：静默重载，避免整页闪 Skeleton
useWs((ev: any) => {
  if (['alarm.new', 'device.online', 'device.offline', 'node.status'].includes(ev.type)) load(true)
})
</script>

<template>
  <div class="space-y-3">
    <!-- 加载失败：给出原因与重试，而不是空白页。
         不用 UiErrorCard——那是深色半透明的播放器浮层，用在页面上会显得突兀 -->
    <div v-if="loadErr" class="flex items-center justify-between gap-3 rounded-signal border border-danger/40 bg-danger-soft px-4 py-3">
      <div class="flex items-center gap-2 text-sm">
        <Icon name="alert-circle" :size="16" class="shrink-0 text-danger" />
        <span class="text-body">{{ loadErr }}</span>
      </div>
      <UiButton size="sm" @click="load()">{{ t('common.retry') }}</UiButton>
    </div>

    <!-- 骨架屏：首屏加载时占位，避免布局跳动 -->
    <template v-else-if="loading && !dashboard">
      <div class="h-16 animate-pulse rounded-signal border border-line bg-surface" />
      <div class="h-72 animate-pulse rounded-signal border border-line bg-surface" />
    </template>

    <template v-else>
      <!-- 指标条：六项汇总压缩为一行，让位给下方告警流 -->
      <div class="grid grid-cols-2 gap-px overflow-hidden rounded-signal border border-line bg-line sm:grid-cols-3 lg:grid-cols-6">
        <NuxtLink
          v-for="m in metrics" :key="m.label"
          :to="m.path"
          class="flex flex-col gap-0.5 bg-surface px-4 py-3 transition-colors hover:bg-primary-softer"
        >
          <span class="text-xs text-muted">{{ m.label }}</span>
          <span class="font-mono text-xl font-semibold" :class="m.tone ? TONE_CLASS[m.tone] : 'text-ink'">
            {{ m.value ?? EMPTY }}
          </span>
        </NuxtLink>
      </div>

      <!-- 主区：告警流为主，右侧为在线率与来源分布 -->
      <div class="grid grid-cols-1 gap-3 lg:grid-cols-[1fr_300px]">
        <!-- 实时告警流 -->
        <section class="flex min-h-0 flex-col rounded-signal border border-line bg-surface">
          <header class="flex items-center justify-between gap-2 border-b border-line-soft px-4 py-2.5">
            <h2 class="flex items-center gap-1.5 text-sm font-semibold text-ink">
              <Icon name="activity" :size="15" class="text-primary" />{{ t('account.dashboard.recentAlarms') }}
            </h2>
            <NuxtLink to="/alarms" class="flex items-center gap-0.5 rounded-chrome text-xs text-muted transition-colors hover:text-primary">
              {{ t('account.dashboard.viewAll') }}<Icon name="chevron-right" :size="12" />
            </NuxtLink>
          </header>

          <div v-if="!alarmRows.length" class="flex-1">
            <UiEmptyState :text="t('account.dashboard.alarmEmpty')" :hint="t('account.dashboard.alarmEmptyHint')" />
          </div>
          <ul v-else class="max-h-[26rem] divide-y divide-line-soft overflow-y-auto">
            <li v-for="a in alarmRows" :key="a.id">
              <button
                type="button"
                class="flex w-full items-center gap-3 px-4 py-2.5 text-left text-xs transition-colors hover:bg-primary-softer"
                @click="openAlarm(a)"
              >
                <span class="h-6 w-1 shrink-0 rounded-full" :style="{ background: levelBar(a.level) }" />
                <span class="w-11 shrink-0 font-mono text-placeholder">{{ fmtHm(a.ts) }}</span>
                <span class="w-28 shrink-0 truncate font-medium text-ink">{{ a.src }}</span>
                <span class="min-w-0 flex-1 truncate text-body">{{ a.msg }}</span>
                <span class="shrink-0 text-placeholder">{{ ago(a.ts, t) }}</span>
              </button>
            </li>
          </ul>
        </section>

        <!-- 侧栏：在线率 + 来源分布 + 节点健康 -->
        <aside class="space-y-3">
          <section class="rounded-signal border border-line bg-surface px-4 py-4">
            <p class="text-xs text-muted">{{ t('account.dashboard.onlineRate') }}</p>
            <div class="mt-1 flex items-baseline gap-2">
              <span class="font-mono text-3xl font-semibold text-ink">
                {{ onlineRate === null ? EMPTY : onlineRate + '%' }}
              </span>
              <span v-if="offlineCount" class="text-xs text-danger">{{ t('account.dashboard.offlineDevices', { n: offlineCount }) }}</span>
            </div>
            <div class="mt-2.5 h-1.5 overflow-hidden rounded-full bg-line">
              <div
                class="h-full rounded-full transition-all duration-500"
                :style="{
                  width: (onlineRate ?? 0) + '%',
                  background: (onlineRate ?? 100) >= 90 ? 'var(--color-success)' : (onlineRate ?? 0) >= 50 ? 'var(--color-warning)' : 'var(--color-danger)'
                }"
              />
            </div>
          </section>

          <section class="rounded-signal border border-line bg-surface px-4 py-4">
            <p class="mb-2.5 text-xs text-muted">{{ t('account.dashboard.sourceDist') }}</p>
            <p v-if="!hasAnyDevice" class="text-xs text-placeholder">
              {{ t('account.dashboard.noDevice') }}<NuxtLink to="/devices" class="text-primary hover:underline">{{ t('account.dashboard.goAddDevice') }}</NuxtLink>
            </p>
            <div v-else class="space-y-2">
              <div v-for="r in srcRows" :key="r.key" class="flex items-center gap-2.5">
                <span class="w-14 shrink-0 truncate text-xs text-body" :title="r.label">{{ r.label }}</span>
                <div class="h-1.5 flex-1 overflow-hidden rounded-full bg-zone">
                  <div class="h-full rounded-full transition-all duration-500" :style="{ width: r.pct + '%', background: r.color }" />
                </div>
                <span class="w-6 shrink-0 text-right font-mono text-xs text-ink">{{ r.count }}</span>
              </div>
            </div>
          </section>

          <NuxtLink
            to="/system/nodes"
            class="flex items-center justify-between rounded-signal border border-line bg-surface px-4 py-3 transition-colors hover:border-primary"
          >
            <span class="flex items-center gap-1.5 text-xs text-muted">
              <Icon name="server" :size="14" />{{ t('account.dashboard.mediaNodes') }}
            </span>
            <span class="text-xs font-medium" :class="TONE_CLASS[nodeSummary.tone] || 'text-muted'">
              {{ nodeSummary.text }}
            </span>
          </NuxtLink>
        </aside>
      </div>
    </template>
  </div>
</template>
