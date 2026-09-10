<script setup lang="ts">
// 主布局：深色侧栏(#101113) + 顶栏（项目切换/全局搜索/节点健康/消息/用户）
// 对齐 PRD §4 信息架构与 §9.1（待确认计数、节点健康小图标、全局搜索）
const { user, projects, currentProject, switchProject, loadMe, logout } = useAuth()
const { t, locale, setLocale } = useI18n()
const route = useRoute()
const router = useRouter()
const { tasks, open: taskOpen, loading: tasksLoading, running: runningTasks, load: loadTasks, applyEvent: applyTaskEvent } = useTasks()

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
  if (ev.type === 'task.progress') applyTaskEvent(ev.data || {})
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
      { label: t('nav.alarmTemplates'), path: '/alarms/templates' },
      { label: t('nav.alarmPolicies'), path: '/alarms/policies' }
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

/* ---------------- 侧栏三档形态 ----------------
 * full  —— 宽屏默认：220px 展开，文字+图标
 * mini  —— 手动收起：56px 仅图标，靠 title/aria-label 提供名称
 * drawer—— 窄屏(<1024px)：抽屉浮层，点遮罩或选中条目后关闭
 * 用户的手动选择记在 localStorage，但窄屏一律走 drawer，不受记忆影响。
 */
const sidebarMini = ref(false)
const drawerOpen = ref(false)
const isNarrow = ref(false)

let mq: MediaQueryList | null = null
function syncNarrow(e: { matches: boolean }) {
  isNarrow.value = e.matches
  if (!e.matches) drawerOpen.value = false // 回到宽屏时关掉抽屉，避免残留遮罩
}
onMounted(() => {
  try {
    sidebarMini.value = localStorage.getItem('ipc_sidebar_mini') === '1'
  } catch {
    // 隐私模式下 localStorage 可能抛错，用默认展开即可
  }
  mq = window.matchMedia('(max-width: 1023px)')
  syncNarrow(mq)
  mq.addEventListener('change', syncNarrow)
})
onBeforeUnmount(() => mq?.removeEventListener('change', syncNarrow))

function toggleSidebar() {
  if (isNarrow.value) {
    drawerOpen.value = !drawerOpen.value
    return
  }
  sidebarMini.value = !sidebarMini.value
  try {
    localStorage.setItem('ipc_sidebar_mini', sidebarMini.value ? '1' : '0')
  } catch {
    // 记不住就下次重来，不影响本次使用
  }
}
/** 仅在窄屏抽屉态下，选中条目后需要收起抽屉 */
function afterNavigate() {
  if (isNarrow.value) drawerOpen.value = false
}
/** mini 档不显示文字，分组也无法展开，点击直接进入首个子项 */
const collapsed = computed(() => sidebarMini.value && !isNarrow.value)

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

// computed：语言切换后菜单文案与当前语言标记要跟着变
const userMenu = computed(() => [
  { label: t('user.profile'), value: 'profile' },
  { label: t('user.language') + t('common.colon') + (locale.value === 'zh-CN' ? t('lang.zh') : t('lang.en')), value: 'lang' },
  { label: t('user.logout'), value: 'logout', danger: true, divided: true }
])
/** 打开任务中心：每次打开拉一次最新列表，避免常驻轮询 */
function openTasks() {
  taskOpen.value = true
  loadTasks()
}

function onUserMenu(v: string) {
  if (v === 'profile') navigateTo('/account')
  else if (v === 'lang') setLocale(locale.value === 'zh-CN' ? 'en' : 'zh-CN')
  else if (v === 'logout') logout()
}
</script>

<template>
  <div class="flex h-screen overflow-hidden bg-canvas" @keydown.esc="drawerOpen = false">
    <!-- 键盘用户跳过导航直达正文 -->
    <a href="#ipc-main" class="ipc-skip-link">{{ t('nav.skipToMain') }}</a>

    <!-- 侧栏 -->
    <!-- 窄屏抽屉遮罩：点击关闭；Esc 由 @keydown.esc 在根容器处理 -->
    <div
      v-if="isNarrow && drawerOpen"
      class="fixed inset-0 z-40 bg-black/60"
      @click="drawerOpen = false"
    />

    <!-- 侧栏：宽屏 full/mini 两档，窄屏为抽屉浮层 -->
    <aside
      class="flex shrink-0 flex-col bg-sidebar transition-[width] duration-200"
      :class="[
        isNarrow
          ? ['fixed inset-y-0 left-0 z-50 w-sidebar shadow-pop', drawerOpen ? '' : '-translate-x-full']
          : (collapsed ? 'w-sidebar-mini' : 'w-sidebar')
      ]"
      :inert="isNarrow && !drawerOpen ? true : undefined"
    >
      <div class="flex h-bar items-center gap-2 border-b border-line" :class="collapsed ? 'justify-center px-0' : 'px-5'">
        <span class="flex h-6 w-6 shrink-0 items-center justify-center rounded-signal bg-primary text-sidebar"><Icon name="video" :size="14" /></span>
        <span v-if="!collapsed" class="text-[17px] font-bold tracking-wide text-ink">IpcCloud</span>
      </div>
      <nav class="flex-1 overflow-y-auto py-2" :aria-label="t('nav.main')">
        <template v-for="m in menu" :key="m.label">
          <!-- 单条目：语义上是"去某处"，用链接而非按钮 -->
          <NuxtLink
            v-if="!m.children"
            :to="m.path"
            :title="collapsed ? m.label : undefined"
            :aria-label="collapsed ? m.label : undefined"
            :aria-current="isActive(m.path) ? 'page' : undefined"
            class="relative mx-2 flex items-center gap-2.5 rounded-signal py-2 text-sm transition-colors"
            :class="[
              collapsed ? 'justify-center px-0' : 'px-3',
              isActive(m.path) ? 'bg-sidebar-hover font-medium text-ink' : 'text-sidebar-text hover:bg-sidebar-hover hover:text-ink'
            ]"
            @click="afterNavigate"
          >
            <span class="absolute left-0 top-1/2 h-4 w-0.5 -translate-y-1/2 rounded-full bg-primary transition-opacity" :class="isActive(m.path) ? 'opacity-100' : 'opacity-0'" />
            <Icon :name="m.icon" :size="16" :class="isActive(m.path) ? 'text-primary' : ''" />
            <template v-if="!collapsed">{{ m.label }}</template>
            <span
              v-if="m.badge"
              class="rounded-full bg-danger text-[10px] leading-4 text-white"
              :class="collapsed ? 'absolute right-1 top-1 h-1.5 w-1.5 p-0' : 'ml-auto px-1.5'"
            >
              <template v-if="!collapsed">{{ m.badge > 99 ? '99+' : m.badge }}</template>
              <span v-else class="sr-only">{{ t('nav.pendingCount', { n: m.badge }) }}</span>
            </span>
          </NuxtLink>

          <template v-else>
            <!-- 分组头：展开/收起是状态切换，用按钮并声明 aria-expanded -->
            <button
              type="button"
              class="mx-2 flex w-[calc(100%-1rem)] items-center gap-2.5 rounded-signal py-2 text-sm transition-colors"
              :class="[
                collapsed ? 'justify-center px-0' : 'px-3',
                groupActive(m) ? 'text-ink' : 'text-sidebar-text hover:bg-sidebar-hover hover:text-ink'
              ]"
              :title="collapsed ? m.label : undefined"
              :aria-label="collapsed ? m.label : undefined"
              :aria-expanded="collapsed ? undefined : !!expanded[m.label]"
              @click="collapsed ? navigateTo(m.children[0].path) : toggleGroup(m.label)"
            >
              <Icon :name="m.icon" :size="16" :class="groupActive(m) ? 'text-primary' : ''" />
              <template v-if="!collapsed">
                {{ m.label }}
                <Icon name="chevron-down" :size="13" class="ml-auto transition-transform" :class="expanded[m.label] ? '' : '-rotate-90'" />
              </template>
            </button>
            <div v-show="expanded[m.label] && !collapsed" class="mb-1 ml-4 border-l border-line pl-2">
              <NuxtLink
                v-for="s in m.children" :key="s.path"
                :to="s.path"
                :aria-current="isActive(s.path) ? 'page' : undefined"
                class="relative flex items-center justify-between rounded-signal px-3 py-1.5 text-[13px] transition-colors"
                :class="isActive(s.path) ? 'bg-sidebar-hover font-medium text-ink' : 'text-sidebar-text hover:bg-sidebar-hover hover:text-ink'"
                @click="afterNavigate"
              >
                <span class="absolute -left-2 top-1/2 h-3.5 w-0.5 -translate-y-1/2 rounded-full bg-primary transition-opacity" :class="isActive(s.path) ? 'opacity-100' : 'opacity-0'" />
                <span>{{ s.label }}</span>
                <span v-if="s.badge" class="rounded-full bg-danger px-1.5 text-[10px] leading-4 text-white">{{ s.badge > 99 ? '99+' : s.badge }}</span>
              </NuxtLink>
            </div>
          </template>
        </template>
      </nav>
    </aside>

    <!-- 主区 -->
    <div class="flex min-w-0 flex-1 flex-col">
      <header class="flex h-bar shrink-0 items-center gap-3 border-b border-line bg-surface px-4">
        <!-- 侧栏开关：宽屏切 mini/full，窄屏开合抽屉 -->
        <button
          type="button"
          class="rounded-chrome p-1.5 text-muted transition-colors hover:bg-zone hover:text-primary"
          :aria-label="isNarrow ? (drawerOpen ? t('nav.closeNav') : t('nav.openNav')) : (collapsed ? t('nav.expandSidebar') : t('nav.collapseSidebar'))"
          :aria-expanded="isNarrow ? drawerOpen : !collapsed"
          @click="toggleSidebar"
        >
          <Icon name="panel-left" :size="17" />
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
            :aria-label="t('nav.allProjects')"
            :title="t('nav.allProjects')"
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
              :placeholder="t('nav.searchPlaceholder')" @focus="searchResults.length && (searchOpen = true)"
            />
          </div>
          <div v-if="searchOpen && searchResults.length" class="absolute left-0 top-9 z-50 w-72 rounded-chrome border border-line bg-surface-2 p-1 shadow-pop">
            <button
              v-for="(r, i) in searchResults" :key="i"
              type="button"
              class="flex w-full items-center gap-2 rounded px-2.5 py-1.5 text-left text-sm hover:bg-primary-soft"
              @click="goto(r)"
            >
              <Icon name="video" :size="14" class="text-placeholder" />
              <span class="truncate">{{ r.label }}</span>
              <span v-if="r.sub" class="ml-auto text-xs text-placeholder">{{ r.sub }}</span>
            </button>
          </div>
        </div>

        <div class="ml-auto flex items-center gap-4">
          <!-- 节点健康小图标 -->
          <UiTooltip :label="nodeHealthy ? t('nav.nodeHealthy') : t('nav.nodeUnhealthy')">
            <span class="flex items-center gap-1 text-xs" :class="nodeHealthy ? 'text-success' : 'text-danger'">
              <Icon name="server" :size="15" />
              <span class="h-1.5 w-1.5 rounded-full" :class="nodeHealthy ? 'bg-success' : 'bg-danger'" />
            </span>
          </UiTooltip>
          <!-- 任务中心 -->
          <UiTooltip :label="t('nav.tasks')">
            <button type="button" class="relative rounded-chrome text-muted transition-colors hover:text-primary" :aria-label="t('nav.tasks')" @click="openTasks">
              <Icon name="list" :size="18" />
              <span v-if="runningTasks()" class="absolute -right-1 -top-1 h-2 w-2 rounded-full bg-primary" />
            </button>
          </UiTooltip>
          <!-- 消息 -->
          <UiBadge :value="unread" class="mt-1">
            <button type="button" class="rounded-chrome text-muted transition-colors hover:text-primary" :aria-label="unread ? t('nav.messageCenter') + t('common.comma') + t('nav.unreadCount', { n: unread }) : t('nav.messageCenter')" @click="navigateTo('/alarms')">
              <Icon name="bell" :size="18" />
            </button>
          </UiBadge>
          <!-- 用户 -->
          <UiDropdown :items="userMenu" @select="onUserMenu">
            <span class="flex cursor-pointer items-center gap-1 text-sm text-body hover:text-primary">
              <Icon name="user" :size="15" />{{ user?.name || user?.username || t('user.fallbackName') }}
              <Icon name="chevron-down" :size="13" />
            </span>
          </UiDropdown>
        </div>
      </header>

      <main id="ipc-main" tabindex="-1" class="min-h-0 flex-1 overflow-auto p-4"><slot /></main>
    </div>

    <!-- 任务中心抽屉（P-18） -->
    <UiDrawer v-model:open="taskOpen" :title="t('nav.tasks')">
      <div v-if="tasksLoading && !tasks.length" class="flex justify-center p-8">
        <Icon name="refresh" :size="20" class="ipc-spin text-primary" />
      </div>
      <div v-else-if="!tasks.length" class="p-5">
        <UiEmptyState :text="t('task.empty')" :hint="t('task.emptyHint')" />
      </div>
      <ul v-else class="divide-y divide-line-soft">
        <li v-for="tk in tasks" :key="tk.id" class="px-5 py-3">
          <div class="flex items-center justify-between gap-2">
            <span class="truncate text-sm text-ink">{{ tk.title }}</span>
            <UiTag :color="taskStatusInfo(tk.status).color">{{ t(taskStatusInfo(tk.status).labelKey) }}</UiTag>
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
