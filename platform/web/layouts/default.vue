<script setup lang="ts">
// 主布局：深色侧栏(#101113) + 顶栏（项目切换/全局搜索/节点健康/消息/用户）
// 对齐 PRD §4 信息架构与 §9.1（待确认计数、节点健康小图标、全局搜索）
const { user, projects, currentProject, switchProject, loadMe, logout } = useAuth()
const { t, locale, setLocale } = useI18n()
const route = useRoute()
const router = useRouter()
const { tasks, open: taskOpen, running: runningTasks, upsert } = useTasks()

const unread = useState('unreadAlarms', () => 0)
const pendingCount = useState('gbPending', () => 0)
const nodeHealthy = useState('nodeHealthy', () => true)
const search = ref('')
const searchResults = ref<any[]>([])
const searchOpen = ref(false)

onMounted(async () => {
  await loadMe()
  if (!user.value) return navigateTo('/login')
  refreshBadges()
})

async function refreshBadges() {
  const api = useApi()
  try {
    const res: any = await api.get('/alarms', { pageSize: 1 })
    unread.value = res?.unread ?? 0
  } catch {}
  try {
    const res: any = await api.get('/devices/gb28181/pending')
    pendingCount.value = (res?.items || []).length
  } catch {}
  try {
    const res: any = await api.get('/media-nodes')
    const list = res?.items || res || []
    nodeHealthy.value = list.length === 0 || list.some((n: any) => n.status === 'online')
  } catch {}
}

useWs((ev: any) => {
  if (ev.type === 'alarm.new') unread.value++
  if (ev.type === 'alarms.readall') unread.value = 0
  if (ev.type === 'gb.pending') refreshBadges()
  if (ev.type === 'task.progress') {
    const d = ev.data || {}
    if (d.id) upsert({ id: d.id, type: d.type || 'task', title: d.title || d.id, status: d.status, progress: d.progress ?? 0, detail: d.detail })
  }
})

// 全局搜索（设备/通道）
let searchTimer: any
watch(search, (q) => {
  clearTimeout(searchTimer)
  if (!q.trim()) { searchResults.value = []; return }
  searchTimer = setTimeout(async () => {
    const api = useApi()
    try {
      const res: any = await api.get('/devices', { keyword: q.trim(), pageSize: 8 })
      searchResults.value = (res?.items || []).map((d: any) => ({
        label: d.name, sub: d.source || '', path: `/devices/${d.id}`, type: 'device'
      }))
    } catch { searchResults.value = [] }
    searchOpen.value = true
  }, 300)
})
function goto(r: any) {
  searchOpen.value = false
  search.value = ''
  searchResults.value = []
  router.push(r.path)
}

const menu = computed(() => [
  { label: t('nav.dashboard'), icon: 'gauge', path: '/' },
  // 设备为单条目：添加设备已是列表页工具栏按钮，国标待确认由列表页入口进入，
  // 待办计数上提至此一级菜单，保留"侧栏一眼可见有事待办"（ADD-05）。
  { label: t('nav.devices'), icon: 'video', path: '/devices', badge: pendingCount.value },
  // 预览与回放同属"看画面"（一个看实时、一个看过去），归入「视频」一组。
  {
    label: t('nav.video'), icon: 'monitor', children: [
      { label: t('nav.live'), path: '/live' },
      { label: t('nav.playback'), path: '/playback' }
    ]
  },
  {
    label: t('nav.alarms'), icon: 'bell', children: [
      { label: t('nav.messageCenter'), path: '/alarms' },
      { label: t('nav.alarmRules'), path: '/alarms/rules' },
      { label: t('nav.alarmTemplates'), path: '/alarms/templates' }
    ]
  },
  // 命名为「计划」而非「录像」：后者会与「视频」下的"录像回放"撞概念，
  // 而本组子项恰为"录像计划 / 计划模板"，「计划」既准确又无歧义。
  {
    label: t('nav.plan'), icon: 'calendar', children: [
      { label: t('nav.recordPlans'), path: '/record/plans' },
      { label: t('nav.recordTemplates'), path: '/record/templates' }
    ]
  },
  {
    label: t('nav.system'), icon: 'settings', children: [
      { label: t('nav.projects'), path: '/system/projects' },
      { label: t('nav.roles'), path: '/system/roles' },
      { label: t('nav.nodes'), path: '/system/nodes' },
      { label: t('nav.settings'), path: '/system/settings' },
      { label: t('nav.audit'), path: '/system/audit' }
    ]
  }
])

const expanded = reactive<Record<string, boolean>>({})
function toggleGroup(label: string) { expanded[label] = !expanded[label] }
function groupActive(m: any) {
  return (m.children || []).some((s: any) => isActive(s.path))
}
function isActive(path: string) {
  const p = path.split('?')[0]
  if (p === '/') return route.path === '/'
  return route.path.startsWith(p)
}
menu.value.forEach((m: any) => { if (m.children) expanded[m.label] = groupActive(m) })

const userMenu = [
  { label: t('user.profile'), value: 'profile' },
  { label: t('user.language') + '：' + (locale.value === 'zh-CN' ? '中文' : 'EN'), value: 'lang' },
  { label: t('user.logout'), value: 'logout', danger: true, divided: true }
]
function onUserMenu(v: string) {
  if (v === 'profile') navigateTo('/account')
  else if (v === 'lang') setLocale(locale.value === 'zh-CN' ? 'en' : 'zh-CN')
  else if (v === 'logout') logout()
}
</script>

<template>
  <div class="flex h-screen overflow-hidden bg-canvas">
    <!-- 侧栏 -->
    <aside class="flex w-55 shrink-0 flex-col bg-sidebar" style="width: 220px">
      <div class="flex h-13 items-center gap-2 border-b border-line px-5" style="height: 52px">
        <span class="flex h-6 w-6 items-center justify-center rounded-signal bg-primary text-sidebar"><Icon name="video" :size="14" /></span>
        <span class="text-[17px] font-bold tracking-wide text-ink">IpcCloud</span>
      </div>
      <nav class="flex-1 overflow-y-auto py-2">
        <template v-for="m in menu" :key="m.label">
          <div
            v-if="!m.children"
            class="relative mx-2 flex cursor-pointer items-center gap-2.5 rounded-signal px-3 py-2 text-sm transition-colors"
            :class="isActive(m.path) ? 'bg-sidebar-hover font-medium text-ink' : 'text-sidebar-text hover:bg-sidebar-hover hover:text-ink'"
            @click="navigateTo(m.path)"
          >
            <span class="absolute left-0 top-1/2 h-4 w-0.5 -translate-y-1/2 rounded-full bg-primary transition-opacity" :class="isActive(m.path) ? 'opacity-100' : 'opacity-0'" />
            <Icon :name="m.icon" :size="16" :class="isActive(m.path) ? 'text-primary' : ''" />{{ m.label }}
            <span v-if="m.badge" class="ml-auto rounded-full bg-danger px-1.5 text-[10px] leading-4 text-white">{{ m.badge > 99 ? '99+' : m.badge }}</span>
          </div>
          <template v-else>
            <div
              class="mx-2 flex cursor-pointer items-center gap-2.5 rounded-signal px-3 py-2 text-sm transition-colors"
              :class="groupActive(m) ? 'text-ink' : 'text-sidebar-text hover:bg-sidebar-hover hover:text-ink'"
              @click="toggleGroup(m.label)"
            >
              <Icon :name="m.icon" :size="16" :class="groupActive(m) ? 'text-primary' : ''" />{{ m.label }}
              <Icon name="chevron-down" :size="13" class="ml-auto transition-transform" :class="expanded[m.label] ? '' : '-rotate-90'" />
            </div>
            <div v-show="expanded[m.label]" class="mb-1 ml-4 border-l border-line pl-2">
              <div
                v-for="s in m.children" :key="s.path"
                class="relative flex cursor-pointer items-center justify-between rounded-signal px-3 py-1.5 text-[13px] transition-colors"
                :class="isActive(s.path) ? 'bg-sidebar-hover font-medium text-ink' : 'text-sidebar-text hover:bg-sidebar-hover hover:text-ink'"
                @click="navigateTo(s.path)"
              >
                <span class="absolute -left-2 top-1/2 h-3.5 w-0.5 -translate-y-1/2 rounded-full bg-primary transition-opacity" :class="isActive(s.path) ? 'opacity-100' : 'opacity-0'" />
                <span>{{ s.label }}</span>
                <span v-if="s.badge" class="rounded-full bg-danger px-1.5 text-[10px] leading-4 text-white">{{ s.badge > 99 ? '99+' : s.badge }}</span>
              </div>
            </div>
          </template>
        </template>
      </nav>
    </aside>

    <!-- 主区 -->
    <div class="flex min-w-0 flex-1 flex-col">
      <header class="flex h-13 shrink-0 items-center gap-3 border-b border-line bg-surface px-4" style="height: 52px">
        <!-- 返回首页按钮 -->
        <button
          v-if="route.path !== '/'"
          type="button"
          class="flex items-center gap-1 rounded-chrome border border-line bg-surface px-2.5 py-1 text-xs text-muted hover:border-primary hover:text-primary transition-colors"
          @click="navigateTo('/')"
        >
          <Icon name="chevron-left" :size="13" />返回首页
        </button>

        <div class="flex items-center gap-1.5">
          <UiSelect
            v-if="currentProject" :model-value="currentProject.id" width="w-44"
            :options="projects.map((p: any) => ({ label: p.name, value: p.id }))"
            @update:model-value="switchProject(projects.find((p: any) => p.id === $event))"
          />
          <button
            type="button"
            class="p-1.5 rounded-chrome text-muted hover:bg-zone hover:text-primary"
            title="所有企业及项目"
            @click="navigateTo('/console')"
          >
            <Icon name="grid" :size="15" />
          </button>
        </div>
        <!-- 全局搜索 -->
        <div class="relative ml-2 hidden md:block">
          <div class="flex h-8 w-64 items-center gap-1.5 rounded border border-line bg-canvas px-2.5 focus-within:border-primary focus-within:bg-surface">
            <Icon name="search" :size="14" class="text-placeholder" />
            <input
              v-model="search" class="w-full bg-transparent text-sm outline-none placeholder:text-placeholder"
              :placeholder="t('common.search') + '（设备 / 通道）'" @focus="searchResults.length && (searchOpen = true)"
            />
          </div>
          <div v-if="searchOpen && searchResults.length" class="absolute left-0 top-9 z-50 w-72 rounded-chrome border border-line bg-surface-2 p-1 shadow-pop">
            <div
              v-for="(r, i) in searchResults" :key="i"
              class="flex cursor-pointer items-center gap-2 rounded px-2.5 py-1.5 text-sm hover:bg-primary-soft"
              @click="goto(r)"
            >
              <Icon name="video" :size="14" class="text-placeholder" />
              <span class="truncate">{{ r.label }}</span>
              <span v-if="r.sub" class="ml-auto text-xs text-placeholder">{{ r.sub }}</span>
            </div>
          </div>
        </div>

        <div class="ml-auto flex items-center gap-4">
          <!-- 节点健康小图标 -->
          <UiTooltip :label="nodeHealthy ? '流媒体节点正常' : '存在离线节点'">
            <span class="flex items-center gap-1 text-xs" :class="nodeHealthy ? 'text-success' : 'text-danger'">
              <Icon name="server" :size="15" />
              <span class="h-1.5 w-1.5 rounded-full" :class="nodeHealthy ? 'bg-success' : 'bg-danger'" />
            </span>
          </UiTooltip>
          <!-- 任务中心 -->
          <UiTooltip :label="t('nav.tasks')">
            <button class="relative text-muted transition-colors hover:text-primary" @click="taskOpen = true">
              <Icon name="list" :size="18" />
              <span v-if="runningTasks()" class="absolute -right-1 -top-1 h-2 w-2 rounded-full bg-primary" />
            </button>
          </UiTooltip>
          <!-- 消息 -->
          <UiBadge :value="unread" class="mt-1">
            <button class="text-muted transition-colors hover:text-primary" @click="navigateTo('/alarms')">
              <Icon name="bell" :size="18" />
            </button>
          </UiBadge>
          <!-- 用户 -->
          <UiDropdown :items="userMenu" @select="onUserMenu">
            <span class="flex cursor-pointer items-center gap-1 text-sm text-body hover:text-primary">
              <Icon name="user" :size="15" />{{ user?.name || user?.username || '用户' }}
              <Icon name="chevron-down" :size="13" />
            </span>
          </UiDropdown>
        </div>
      </header>

      <main class="min-h-0 flex-1 overflow-auto p-4"><slot /></main>
    </div>

    <!-- 任务中心抽屉（P-18） -->
    <UiDrawer v-model:open="taskOpen" :title="t('nav.tasks')">
      <div v-if="!tasks.length" class="p-5"><UiEmptyState text="暂无任务" /></div>
      <ul v-else class="divide-y divide-line-soft">
        <li v-for="tk in tasks" :key="tk.id" class="px-5 py-3">
          <div class="flex items-center justify-between gap-2">
            <span class="truncate text-sm text-ink">{{ tk.title }}</span>
            <UiTag :color="tk.status === 'success' ? 'success' : tk.status === 'failed' ? 'danger' : tk.status === 'partial' ? 'warning' : 'primary'">
              {{ tk.status === 'running' ? '进行中' : tk.status === 'pending' ? '排队中' : tk.status === 'success' ? '完成' : tk.status === 'failed' ? '失败' : '部分成功' }}
            </UiTag>
          </div>
          <div class="mt-2 h-1 overflow-hidden rounded-full bg-line">
            <div class="h-full rounded-full bg-primary transition-all" :style="{ width: (tk.progress || 0) + '%' }" />
          </div>
          <p v-if="tk.detail" class="mt-1 text-xs text-placeholder">{{ tk.detail }}</p>
        </li>
      </ul>
    </UiDrawer>
  </div>
</template>
