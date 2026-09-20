<script setup lang="ts">
// 主布局：深色侧栏(#101113) + 顶栏（项目切换/全局搜索/节点健康/消息/用户）
// 对齐 PRD §4 信息架构与 §9.1（待确认计数、节点健康小图标、全局搜索）
const { user, projects, currentProject, switchProject, loadMe, logout } = useAuth()
const { t, locale, setLocale } = useI18n()
const route = useRoute()
const router = useRouter()
const {
  tasks, open: taskOpen, loading: taskLoading, running: runningTasks,
  upsert, load: loadTasks, remove: removeTask, clear: clearTasks, cancel: cancelTask
} = useTasks()
const confirm = useConfirm()
const toast = useToast()

// 抽屉内状态筛选：全部 / 进行中 / 失败
const taskFilter = ref<'' | 'running' | 'failed'>('')
// 展开查看下载产物的任务 id
const taskExpanded = ref('')

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
  // 任务以服务端为准：登录后先同步一次，刷新页面不再丢失
  loadTasks().catch(() => {})
})

// 打开抽屉时按当前筛选重新拉取，保证看到的是最新数据
watch(taskOpen, (v) => { if (v) reloadTasks() })
watch(taskFilter, () => { if (taskOpen.value) reloadTasks() })

async function reloadTasks() {
  try {
    await loadTasks(taskFilter.value ? { status: taskFilter.value } : {})
  } catch {}
}

// 清理任务：finished 清已完成 / all 清全部终态（进行中的不受影响）
async function onClearTasks(scope: 'finished' | 'all') {
  const okd = await confirm.ask({
    title: scope === 'finished' ? '清除已完成任务' : '清除全部任务',
    message: scope === 'finished'
      ? '将删除所有已完成与部分成功的任务记录。'
      : '将删除所有已结束的任务记录（进行中的任务不受影响）。',
    confirmText: '确认清除',
    danger: true
  })
  if (!okd) return
  try {
    const n = await clearTasks(scope)
    toast.success(n > 0 ? `已清除 ${n} 条任务` : '没有可清除的任务')
  } catch (e: any) {
    toast.error(e?.msg || '清除失败')
  }
}

// 删除单条任务
async function onRemoveTask(tk: any) {
  const okd = await confirm.ask({
    title: '删除任务',
    message: `确认删除任务「${tk.title || tk.id}」？`,
    danger: true
  })
  if (!okd) return
  try {
    await removeTask(tk.id)
    toast.success('已删除')
  } catch (e: any) {
    toast.error(e?.msg || '删除失败')
  }
}

// 取消进行中的任务
async function onCancelTask(tk: any) {
  try {
    await cancelTask(tk.id)
    toast.success('已取消')
  } catch (e: any) {
    toast.error(e?.msg || '取消失败')
  }
}

async function refreshBadges() {
  const api = useApi()
  try {
    const res: any = await api.get('/alarms', { unread: 1, pageSize: 1 })
    unread.value = res?.total ?? 0
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
    // 后端统一以 taskId 下发，兼容历史 id 字段
    const id = d.taskId || d.id
    if (id) {
      upsert({
        id,
        type: d.type || 'generic',
        title: d.title || id,
        status: d.status,
        progress: d.progress ?? 0,
        detail: d.detail,
        result: d.result,
        createdAt: d.createdAt || Date.now(),
        updatedAt: d.updatedAt
      })
    }
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
  {
    label: t('nav.devices'), icon: 'video', children: [
      { label: t('nav.deviceList'), path: '/devices' },
      { label: t('nav.addDevice'), path: '/devices?add=1' },
      { label: t('nav.pending'), path: '/devices/pending', badge: pendingCount.value }
    ]
  },
  { label: t('nav.live'), icon: 'monitor', path: '/live' },
  { label: t('nav.playback'), icon: 'film', path: '/playback' },
  {
    label: t('nav.alarms'), icon: 'bell', children: [
      { label: t('nav.messageCenter'), path: '/alarms' },
      { label: t('nav.alarmRules'), path: '/alarms/rules' },
      { label: t('nav.alarmTemplates'), path: '/alarms/templates' }
    ]
  },
  {
    label: t('nav.record'), icon: 'film', children: [
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
      { label: t('nav.audit'), path: '/system/audit' },
      { label: t('nav.tasks'), path: '/system/tasks', badge: runningTasks() }
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
  if (path.includes('?add=1')) return route.path === '/devices' && route.query.add === '1'
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
      <div class="flex h-13 items-center gap-2 border-b border-white/5 px-5" style="height: 52px">
        <span class="flex h-6 w-6 items-center justify-center rounded bg-primary text-white"><Icon name="video" :size="14" /></span>
        <span class="text-[17px] font-bold tracking-wide text-white">IpcCloud</span>
      </div>
      <nav class="flex-1 overflow-y-auto py-2">
        <template v-for="m in menu" :key="m.label">
          <div
            v-if="!m.children"
            class="mx-2 flex cursor-pointer items-center gap-2.5 rounded px-3 py-2 text-sm transition-colors"
            :class="isActive(m.path) ? 'bg-primary font-medium text-white' : 'text-sidebar-text hover:bg-sidebar-hover hover:text-white'"
            @click="navigateTo(m.path)"
          >
            <Icon :name="m.icon" :size="16" />{{ m.label }}
          </div>
          <template v-else>
            <div
              class="mx-2 flex cursor-pointer items-center gap-2.5 rounded px-3 py-2 text-sm transition-colors"
              :class="groupActive(m) ? 'text-white' : 'text-sidebar-text hover:bg-sidebar-hover hover:text-white'"
              @click="toggleGroup(m.label)"
            >
              <Icon :name="m.icon" :size="16" />{{ m.label }}
              <Icon name="chevron-down" :size="13" class="ml-auto transition-transform" :class="expanded[m.label] ? '' : '-rotate-90'" />
            </div>
            <div v-show="expanded[m.label]" class="mb-1 ml-4 border-l border-white/8 pl-2">
              <div
                v-for="s in m.children" :key="s.path"
                class="flex cursor-pointer items-center justify-between rounded px-3 py-1.5 text-[13px] transition-colors"
                :class="isActive(s.path) ? 'bg-primary font-medium text-white' : 'text-sidebar-text hover:bg-sidebar-hover hover:text-white'"
                @click="navigateTo(s.path)"
              >
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
        <!-- 返回首页按钮（严格对齐图 4 顶栏左侧） -->
        <button
          v-if="route.path !== '/'"
          type="button"
          class="flex items-center gap-1 rounded border border-[#e5e6eb] bg-white px-2.5 py-1 text-xs text-[#4e5969] hover:border-[#1785E6] hover:text-[#1785E6] transition-colors"
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
            class="p-1.5 rounded text-[#86909c] hover:bg-[#f2f3f5] hover:text-[#1785E6]"
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
          <div v-if="searchOpen && searchResults.length" class="absolute left-0 top-9 z-50 w-72 rounded border border-line bg-surface p-1 shadow-pop">
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
      <!-- 状态筛选 + 刷新 -->
      <div class="flex items-center justify-between gap-2 border-b border-line-soft px-5 py-2">
        <div class="flex gap-1">
          <button
            v-for="f in [{ v: '', l: '全部' }, { v: 'running', l: '进行中' }, { v: 'failed', l: '失败' }]"
            :key="f.v"
            class="rounded-md px-2 py-1 text-xs transition"
            :class="taskFilter === f.v ? 'bg-primary-soft text-primary' : 'text-muted hover:text-ink'"
            @click="taskFilter = f.v as any"
          >{{ f.l }}</button>
        </div>
        <button class="text-muted transition hover:text-primary" title="刷新" @click="reloadTasks">
          <Icon name="refresh" :size="15" />
        </button>
      </div>

      <div v-if="taskLoading && !tasks.length" class="p-5 text-center text-sm text-placeholder">加载中…</div>
      <div v-else-if="!tasks.length" class="p-5"><UiEmptyState text="暂无任务" /></div>
      <ul v-else class="divide-y divide-line-soft">
        <li v-for="tk in tasks" :key="tk.id" class="group px-5 py-3">
          <div class="flex items-center justify-between gap-2">
            <span class="truncate text-sm text-ink">{{ tk.title || tk.id }}</span>
            <div class="flex shrink-0 items-center gap-1">
              <!-- 操作区：进行中可取消，已结束可删除 -->
              <button
                v-if="!isTaskFinal(tk.status)"
                class="opacity-0 transition group-hover:opacity-100 text-muted hover:text-warning"
                title="取消任务"
                @click="onCancelTask(tk)"
              ><Icon name="x" :size="14" /></button>
              <button
                v-else
                class="opacity-0 transition group-hover:opacity-100 text-muted hover:text-danger"
                title="删除任务"
                @click="onRemoveTask(tk)"
              ><Icon name="trash" :size="14" /></button>
              <UiTag :color="taskTagColor(tk.status)">{{ taskStatusText(tk.status) }}</UiTag>
            </div>
          </div>
          <div class="mt-2 h-1 overflow-hidden rounded-full bg-line">
            <div class="h-full rounded-full bg-primary transition-all" :style="{ width: (tk.progress || 0) + '%' }" />
          </div>
          <div class="mt-1 flex items-center justify-between gap-2">
            <p class="truncate text-xs text-placeholder">{{ tk.detail || taskTypeText(tk.type) }}</p>
            <!-- 下载类任务：展开取产物链接 -->
            <button
              v-if="tk.type === 'download' && (tk.result?.files || []).length"
              class="shrink-0 text-xs text-primary hover:underline"
              @click="taskExpanded = taskExpanded === tk.id ? '' : tk.id"
            >{{ taskExpanded === tk.id ? '收起' : `文件 ${tk.result.files.length}` }}</button>
          </div>
          <ul v-if="taskExpanded === tk.id" class="mt-2 space-y-1 rounded-md bg-zone p-2">
            <li v-for="(f, i) in tk.result?.files || []" :key="i" class="flex items-center justify-between gap-2 text-xs">
              <span class="truncate text-muted">片段 {{ i + 1 }}</span>
              <a :href="f.url" target="_blank" download class="flex shrink-0 items-center gap-1 text-primary hover:underline">
                <Icon name="download" :size="12" />下载
              </a>
            </li>
          </ul>
        </li>
      </ul>

      <template #footer>
        <div class="flex items-center justify-between gap-2">
          <div class="flex gap-2">
            <UiButton size="sm" @click="onClearTasks('finished')">清除已完成</UiButton>
            <UiButton size="sm" variant="danger" @click="onClearTasks('all')">全部清除</UiButton>
          </div>
          <button class="text-xs text-primary hover:underline" @click="taskOpen = false; router.push('/system/tasks')">
            查看全部 →
          </button>
        </div>
      </template>
    </UiDrawer>
  </div>
</template>
