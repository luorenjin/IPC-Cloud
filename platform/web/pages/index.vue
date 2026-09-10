<script setup lang="ts">
// 首页/仪表盘："值守总览"：弧形在线率量规 + 协议来源分布 + 实时告警流 + 功能入口（见重设计方案「Watch Desk」一节）
const api = useApi()
const router = useRouter()
const { currentProject } = useAuth()
// 变量名不用 dash——会遮蔽 utils/format 自动导入的 dash() 空值兜底函数
const dashboard = ref<any>(null)

// 附加 pct：迷你分布条的相对宽度（相对最大类目），纯展示用，最小 6% 保证非零占比可见
const srcRows = computed(() => {
  // 来源展示名/颜色见 utils/enums.ts SOURCE_MAP（PRD §9.1 四色语义固定）
  const rows = SOURCES.map((k) => ({
    key: k, label: SOURCE_MAP[k].longLabel, color: SOURCE_MAP[k].cssVar, tag: SOURCE_MAP[k].color,
    count: dashboard.value?.bySource?.[k] ?? 0
  }))
  const max = Math.max(1, ...rows.map((r) => r.count))
  return rows.map((r) => ({ ...r, pct: r.count ? Math.max(6, Math.round((r.count / max) * 100)) : 0 }))
})

const onlineRate = computed(() => {
  const t = dashboard.value?.deviceTotal || 0
  const o = dashboard.value?.deviceOnline || 0
  return t ? ((o / t) * 100).toFixed(1) + '%' : '-'
})

// 弧形量规：纯展示计算属性（SVG 半圆弧长 = πr，stroke-dashoffset 技术实现进度可视化）
const GAUGE_R = 80
const GAUGE_PATH = 'M20 100 A80 80 0 1 1 180 100'
const GAUGE_CIRC = Math.PI * GAUGE_R
const onlineFrac = computed(() => {
  const t = dashboard.value?.deviceTotal || 0
  const o = dashboard.value?.deviceOnline || 0
  return t ? Math.min(1, o / t) : 0
})
const gaugeOffset = computed(() => GAUGE_CIRC * (1 - onlineFrac.value))
const gaugeColor = computed(() => {
  const f = onlineFrac.value
  if (f >= 1) return 'var(--color-success)'
  if (f > 0.5) return 'var(--color-warning)'
  return 'var(--color-danger)'
})

// 告警类型与级别映射见 utils/enums.ts（全站唯一来源）
function levelBar(level: string) { return alarmLevelInfo(level).cssVar }

// 设备/通道名称映射：/dashboard 的 recentAlarms 只带 deviceId/channelId，需要另查名称用于告警流展示
const deviceNameMap = ref<Record<string, string>>({})
const channelNameMap = ref<Record<string, string>>({})
async function loadNames() {
  try {
    const [dRes, cRes]: any[] = await Promise.all([api.get('/devices'), api.get('/channels')])
    deviceNameMap.value = Object.fromEntries((dRes.items || dRes || []).map((d: any) => [d.id, d.name]))
    channelNameMap.value = Object.fromEntries((cRes.items || cRes || []).map((c: any) => [c.id, c.name]))
  } catch {}
}

const alarmRows = computed(() => (dashboard.value?.recentAlarms || []).slice(0, 8).map((a: any) => ({
  id: a.id,
  level: a.level || 'info',
  msg: a.data?.error?.msg || a.data?.name || alarmKindName(a.kind) || '告警事件',
  src: channelNameMap.value[a.channelId] || deviceNameMap.value[a.deviceId] || '通道',
  ts: a.ts || 0
})))


async function load() {
  try { dashboard.value = await api.get('/dashboard') } catch (e: any) { useToast().error({ title: e.msg || '加载失败' }) }
}
onMounted(() => {
  load()
  loadNames()
})

useWs((ev: any) => {
  if (['alarm.new', 'device.online', 'device.offline'].includes(ev.type)) load()
})

// 功能入口：7 大真实模块（去除营销化命名，标签取自 PRD 与侧栏导航一致的功能名）
const apps = [
  { title: '设备管理', desc: '接入、分组与远程配置', path: '/devices', icon: 'video' },
  { title: '直播预览', desc: '多画面实时视频与云台控制', path: '/live', icon: 'monitor' },
  { title: '录像回放', desc: '按时间轴检索与下载录像', path: '/playback', icon: 'film' },
  { title: '告警中心', desc: '事件列表、规则与布防模板', path: '/alarms', icon: 'bell' },
  { title: '录像设置', desc: '录像计划与模板管理', path: '/record/plans', icon: 'calendar' },
  { title: '系统管理', desc: '项目、角色与系统参数', path: '/system/settings', icon: 'settings' },
  { title: '媒体节点', desc: '节点状态与推拉流调度', path: '/system/nodes', icon: 'server' }
]
</script>

<template>
  <div class="space-y-4">
    <!-- 值守总览：弧形在线率量规 + 协议来源分布 -->
    <UiCard body-class="p-0">
      <template #header>
        <div>
          <span class="flex items-center gap-1.5 text-sm font-semibold text-ink">
            <Icon name="gauge" :size="15" class="text-primary" />值守总览
          </span>
          <p class="mt-0.5 text-xs text-muted">{{ currentProject?.name || '未命名项目' }}</p>
        </div>
      </template>
      <template #extra>
        <button
          type="button"
          class="flex items-center gap-1.5 text-xs text-muted transition-colors hover:text-primary"
          @click="router.push('/devices')"
        >
          设备总数 <b class="font-semibold text-ink">{{ dash?.deviceTotal ?? 0 }}</b>
          <span class="text-line">·</span>
          在线 <b class="font-semibold text-success">{{ dash?.deviceOnline ?? 0 }}</b>
          <Icon name="chevron-right" :size="12" />
        </button>
      </template>

      <div class="grid grid-cols-1 lg:grid-cols-[260px_1fr]">
        <!-- 弧形量规 -->
        <div class="flex flex-col items-center justify-center border-b border-line-soft px-6 py-6 lg:border-b-0 lg:border-r">
          <svg viewBox="0 0 200 118" class="w-full max-w-[220px]">
            <path :d="GAUGE_PATH" fill="none" stroke="var(--color-line)" stroke-width="14" stroke-linecap="round" />
            <path
              :d="GAUGE_PATH" fill="none" stroke-width="14" stroke-linecap="round"
              :stroke="gaugeColor" :stroke-dasharray="GAUGE_CIRC" :stroke-dashoffset="gaugeOffset"
              style="transition: stroke-dashoffset .4s ease, stroke .4s ease"
            />
            <text x="100" y="90" text-anchor="middle" fill="currentColor" class="text-ink" style="font-size:26px; font-weight:700">
              {{ dash?.deviceOnline ?? 0 }}/{{ dash?.deviceTotal ?? 0 }}
            </text>
            <text x="100" y="108" text-anchor="middle" fill="currentColor" class="text-muted" style="font-size:11px">
              在线率 {{ onlineRate }}
            </text>
          </svg>
        </div>

        <!-- 协议来源迷你分布条 -->
        <div class="space-y-3 px-6 py-6">
          <p class="text-xs font-medium text-muted">设备来源分布</p>
          <div v-for="r in srcRows" :key="r.key" class="flex items-center gap-3">
            <span class="w-16 shrink-0 text-xs text-body">{{ r.label }}</span>
            <div class="h-2 flex-1 overflow-hidden rounded-full bg-zone">
              <div class="h-full rounded-full transition-all duration-500" :style="{ width: r.pct + '%', background: r.color }" />
            </div>
            <span class="w-8 shrink-0 text-right text-xs font-semibold text-ink">{{ r.count }}</span>
          </div>
        </div>
      </div>
    </UiCard>

    <!-- 实时告警流：滚动列表，左侧色条标级别（非卡片墙） -->
    <UiCard body-class="p-0">
      <template #header>
        <span class="flex items-center gap-1.5 text-sm font-semibold text-ink">
          <Icon name="activity" :size="15" class="text-primary" />实时告警
        </span>
      </template>
      <template #extra>
        <button type="button" class="flex items-center gap-0.5 text-xs text-muted transition-colors hover:text-primary" @click="router.push('/alarms')">
          查看全部<Icon name="chevron-right" :size="12" />
        </button>
      </template>

      <div v-if="!alarmRows.length" class="py-10 text-center text-xs text-placeholder">暂无告警记录，系统运行良好</div>
      <div v-else class="max-h-72 divide-y divide-line-soft overflow-y-auto">
        <div v-for="a in alarmRows" :key="a.id" class="flex items-center gap-3 px-4 py-2.5 text-xs">
          <span class="h-6 w-1 shrink-0 rounded-full" :style="{ background: levelBar(a.level) }" />
          <span class="w-11 shrink-0 font-mono text-placeholder">{{ fmtHm(a.ts) }}</span>
          <span class="w-28 shrink-0 truncate font-medium text-ink">{{ a.src }}</span>
          <span class="min-w-0 flex-1 truncate text-body">{{ a.msg }}</span>
          <span class="shrink-0 text-placeholder">{{ ago(a.ts) }}</span>
        </div>
      </div>
    </UiCard>

    <!-- 功能入口：真实模块方形面板（深色描边 + 信号色 icon，无渐变） -->
    <div>
      <p class="mb-2 text-xs font-medium text-muted">功能入口</p>
      <div class="grid grid-cols-2 gap-3 sm:grid-cols-4 lg:grid-cols-7">
        <div
          v-for="app in apps"
          :key="app.title"
          class="group flex cursor-pointer flex-col items-center justify-center gap-2 rounded-signal border border-line bg-surface px-3 py-5 text-center transition-colors hover:border-primary hover:bg-primary-softer"
          @click="router.push(app.path)"
        >
          <span class="flex h-10 w-10 items-center justify-center rounded-signal border border-line-soft bg-zone text-primary transition-colors group-hover:border-primary/40 group-hover:bg-primary-soft">
            <Icon :name="app.icon" :size="20" />
          </span>
          <span class="text-sm font-medium text-ink">{{ app.title }}</span>
          <span class="line-clamp-1 text-[11px] text-muted">{{ app.desc }}</span>
        </div>
      </div>
    </div>
  </div>
</template>
