<script setup lang="ts">
// 项目首页 / 仪表盘：严格对齐图 3（项目标题头 + 辅助快捷链 + 7大应用图标卡片矩阵 + 下方监控看板）
const api = useApi()
const router = useRouter()
const { currentProject, user } = useAuth()
const dash = ref<any>(null)

// 来源分布
const srcMeta: Record<string, { label: string; color: string; tag: string }> = {
  idp: { label: '自有设备', color: 'var(--color-src-idp)', tag: 'idp' },
  gb28181: { label: '国标', color: 'var(--color-src-gb)', tag: 'gb' },
  onvif: { label: 'ONVIF', color: 'var(--color-src-onvif)', tag: 'onvif' },
  rtsp: { label: 'RTSP', color: 'var(--color-src-rtsp)', tag: 'rtsp' }
}
const srcRows = computed(() =>
  Object.keys(srcMeta).map((k) => ({
    key: k, label: srcMeta[k].label, color: srcMeta[k].color, tag: srcMeta[k].tag,
    count: dash.value?.bySource?.[k] ?? 0
  })))

const onlineRate = computed(() => {
  const t = dash.value?.deviceTotal || 0
  const o = dash.value?.deviceOnline || 0
  return t ? ((o / t) * 100).toFixed(1) + '%' : '-'
})

const alarmRows = computed(() => (dash.value?.recentAlarms || []).slice(0, 8).map((a: any) => ({
  id: a.id,
  level: a.level || 'info',
  msg: a.msg || a.content || '告警事件',
  src: a.sourceName || a.deviceName || '通道',
  ts: a.ts || a.time || 0
})))

function ago(ts: number) {
  if (!ts) return '—'
  const s = Math.floor((Date.now() - ts) / 1000)
  if (s < 60) return '刚刚'
  if (s < 3600) return Math.floor(s / 60) + ' 分钟前'
  if (s < 86400) return Math.floor(s / 3600) + ' 小时前'
  return Math.floor(s / 86400) + ' 天前'
}

async function load() {
  try { dash.value = await api.get('/dashboard') } catch (e: any) { useToast().error({ title: e.msg || '加载失败' }) }
}
onMounted(load)

useWs((ev: any) => {
  if (['alarm.new', 'device.online', 'device.offline'].includes(ev.type)) load()
})

// 7 大应用矩阵（严格对齐图 3）
const apps = [
  {
    title: '设备管理',
    desc: '设备接入、分组拓扑、远程配置与诊断',
    path: '/devices',
    iconBg: 'bg-gradient-to-br from-[#2b333e] to-[#171b22]',
    iconColor: 'text-white',
    icon: 'video'
  },
  {
    title: '组织管理',
    desc: '企业组织树、分组架构与项目分配',
    path: '/system/projects',
    iconBg: 'bg-gradient-to-br from-[#ebf5ff] to-[#d6ebff]',
    iconColor: 'text-[#1785E6]',
    icon: 'folder'
  },
  {
    title: '工具箱',
    desc: '参数调优、证书管理、IDP CRL 吊销与自检',
    path: '/system/settings',
    iconBg: 'bg-gradient-to-br from-[#fef3eb] to-[#fde5d2]',
    iconColor: 'text-[#fa8c16]',
    icon: 'tool'
  },
  {
    title: '网络管理中心',
    desc: '流媒体节点负载、带宽吞吐与推拉流调度',
    path: '/system/nodes',
    iconBg: 'bg-gradient-to-br from-[#e8f7ff] to-[#cbeeff]',
    iconColor: 'text-[#0096fa]',
    icon: 'server'
  },
  {
    title: '安防管理中心',
    desc: '实时视频预览、分屏监控、云台与录像回放',
    path: '/live',
    iconBg: 'bg-gradient-to-br from-[#e8fcf4] to-[#cbf7e3]',
    iconColor: 'text-[#00b578]',
    icon: 'shield'
  },
  {
    title: '算法商城',
    desc: '人形检测、车辆识别、区域入侵模型库',
    path: '/alarms/rules',
    iconBg: 'bg-gradient-to-br from-[#fff0f0] to-[#ffdada]',
    iconColor: 'text-[#f53f3f]',
    icon: 'grid'
  },
  {
    title: 'AI算法巡检',
    desc: '智能布防策略、告警统计与巡检事件报表',
    path: '/alarms',
    iconBg: 'bg-gradient-to-br from-[#f2f3ff] to-[#e0e3ff]',
    iconColor: 'text-[#722ed1]',
    icon: 'activity'
  }
]
</script>

<template>
  <div class="space-y-6">
    <!-- 顶部项目看板区（严格对齐图 3） -->
    <div class="rounded-xl border border-[#e5e6eb] bg-white p-6 shadow-sm">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <div class="flex items-center gap-2">
            <h1 class="text-2xl font-bold text-[#1f2329]">{{ currentProject?.name || '深圳绿享' }}</h1>
            <Icon name="info" :size="16" class="text-[#86909c] cursor-pointer" />
          </div>
          <div class="mt-1 text-xs text-[#86909c]">@鲁班长(深圳)科技有限公司</div>
          <div class="mt-3 flex items-center gap-4 text-xs">
            <button
              class="flex items-center gap-1 font-medium text-[#1785E6] hover:underline"
              @click="router.push('/devices')"
            >
              设备概览 {{ dash?.deviceTotal ?? 2 }} &gt;
            </button>
            <span class="text-[#4e5969]">设备总数：<b class="text-[#1f2329]">{{ dash?.deviceTotal ?? 2 }}</b></span>
            <span class="text-[#4e5969]">在线：<b class="text-[#00b578]">{{ dash?.deviceOnline ?? 2 }}</b></span>
            <span class="text-[#4e5969]">离线：<b class="text-[#f53f3f]">{{ (dash?.deviceTotal ?? 2) - (dash?.deviceOnline ?? 2) }}</b></span>
          </div>
        </div>

        <!-- 右侧辅助功能导航链接（对齐图 3 右上角链接群） -->
        <div class="flex flex-wrap items-center gap-4 text-xs text-[#4e5969]">
          <button class="hover:text-[#1785E6]" @click="router.push('/system/roles')">成员管理</button>
          <span class="text-[#e5e6eb]">|</span>
          <button class="hover:text-[#1785E6]" @click="router.push('/alarms')">消息</button>
          <span class="text-[#e5e6eb]">|</span>
          <button class="hover:text-[#1785E6]" @click="router.push('/system/audit')">操作日志</button>
          <span class="text-[#e5e6eb]">|</span>
          <button class="hover:text-[#1785E6]" @click="router.push('/system/settings')">系统设置</button>
          <span class="text-[#e5e6eb]">|</span>
          <button class="hover:text-[#1785E6]" @click="router.push('/console')">切换企业与项目 &gt;</button>
        </div>
      </div>
    </div>

    <!-- 我的应用矩阵（严格对齐图 3 中间的 7 个大应用卡片） -->
    <div>
      <div class="mb-4 flex items-center justify-between">
        <span class="text-base font-bold text-[#1f2329]">我的应用 | {{ apps.length }}</span>
        <button class="text-xs text-[#86909c] hover:text-[#1785E6]">管理我的应用 &gt;</button>
      </div>

      <div class="grid grid-cols-2 gap-4 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-7">
        <div
          v-for="app in apps"
          :key="app.title"
          class="group flex flex-col items-center justify-center rounded-2xl border border-[#e5e6eb] bg-white p-5 text-center shadow-sm transition-all hover:-translate-y-1 hover:border-[#1785E6] hover:shadow-md cursor-pointer"
          @click="router.push(app.path)"
        >
          <!-- 图标容器 -->
          <div
            class="flex h-16 w-16 items-center justify-center rounded-2xl shadow-sm transition-transform group-hover:scale-105"
            :class="[app.iconBg, app.iconColor]"
          >
            <Icon :name="app.icon" :size="30" />
          </div>
          <span class="mt-3 text-sm font-bold text-[#1f2329] group-hover:text-[#1785E6] transition-colors">
            {{ app.title }}
          </span>
          <span class="mt-1 line-clamp-1 text-[11px] text-[#86909c] opacity-0 group-hover:opacity-100 transition-opacity">
            {{ app.desc }}
          </span>
        </div>
      </div>
    </div>

    <!-- 运行指标与统计看板 -->
    <div class="grid grid-cols-1 gap-4 lg:grid-cols-3">
      <!-- 协议接入分布 -->
      <div class="rounded-xl border border-[#e5e6eb] bg-white p-5 shadow-sm">
        <div class="mb-3 flex items-center justify-between">
          <span class="text-sm font-bold text-[#1f2329]">设备来源协议分布</span>
          <span class="text-xs text-[#86909c]">在线率 {{ onlineRate }}</span>
        </div>
        <div class="space-y-3">
          <div v-for="r in srcRows" :key="r.key" class="flex items-center justify-between text-xs">
            <div class="flex items-center gap-2">
              <UiTag :color="r.tag as any" plain>{{ r.label }}</UiTag>
              <span class="text-[#4e5969]">{{ r.key.toUpperCase() }}</span>
            </div>
            <span class="font-bold text-[#1f2329]">{{ r.count }} 台</span>
          </div>
        </div>
      </div>

      <!-- 最近告警消息 -->
      <div class="col-span-2 rounded-xl border border-[#e5e6eb] bg-white p-5 shadow-sm">
        <div class="mb-3 flex items-center justify-between">
          <span class="text-sm font-bold text-[#1f2329]">最新安防动态</span>
          <button class="text-xs text-[#1785E6] hover:underline" @click="router.push('/alarms')">查看全部 &gt;</button>
        </div>
        <div v-if="!alarmRows.length" class="py-8 text-center text-xs text-[#86909c]">
          暂无告警记录，系统运行良好
        </div>
        <div v-else class="divide-y divide-[#f2f3f5]">
          <div v-for="a in alarmRows" :key="a.id" class="flex items-center justify-between py-2.5 text-xs">
            <div class="flex items-center gap-2">
              <span class="h-1.5 w-1.5 rounded-full" :class="a.level === 'error' ? 'bg-[#f53f3f]' : 'bg-[#ff7d00]'" />
              <span class="font-medium text-[#1f2329]">{{ a.src }}</span>
              <span class="text-[#4e5969]">{{ a.msg }}</span>
            </div>
            <span class="text-[#86909c]">{{ ago(a.ts) }}</span>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
