<script setup lang="ts">
// 控制台首页（项目选择页）：私有化部署场景项目数量通常不多，
// 用单列列表替代消费云式的企业/项目卡片矩阵——见重新设计方案「登录与准入」一节
definePageMeta({ layout: 'console' })

const api = useApi()
const toast = useToast()
const router = useRouter()
const { user, projects, currentProject, switchProject, loadMe } = useAuth()

const search = ref('')

// 项目列表与统计数据
const projectStats = ref<Record<string, { total: number; offline: number }>>({})

async function loadProjectStats() {
  await Promise.all(
    projects.value.map(async (p: any) => {
      try {
        const [res, offlineRes]: any[] = await Promise.all([
          api.get('/devices', { projectId: p.id, pageSize: 1 }),
          api.get('/devices', { projectId: p.id, status: 'offline', pageSize: 1 })
        ])
        const total = res?.total || 0
        const offline = offlineRes?.total || 0
        projectStats.value[p.id] = { total, offline }
      } catch {
        projectStats.value[p.id] = { total: 0, offline: 0 }
      }
    })
  )
}

function onlineRatio(id: string) {
  const s = projectStats.value[id]
  if (!s || !s.total) return 1
  return Math.max(0, (s.total - s.offline) / s.total)
}

// 搜索过滤后的项目
const filteredProjects = computed(() => {
  const q = search.value.trim().toLowerCase()
  if (!q) return projects.value
  return projects.value.filter((p: any) => p.name.toLowerCase().includes(q))
})

// 进入项目首页
function enterProject(p: any) {
  switchProject(p)
  router.push('/')
}

// 新建项目弹窗
const addProjDlg = reactive({ show: false, name: '', saving: false })
async function createProject() {
  if (!addProjDlg.name.trim()) return toast.warning('请输入项目名称')
  addProjDlg.saving = true
  try {
    const res: any = await api.post('/projects', { name: addProjDlg.name.trim() })
    toast.success('项目创建成功')
    addProjDlg.show = false
    addProjDlg.name = ''
    await loadMe()
    await loadProjectStats()
  } catch (e: any) {
    toastApiError(e, '创建项目失败')
  } finally {
    addProjDlg.saving = false
  }
}

onMounted(async () => {
  await loadMe()
  await loadProjectStats()
})
</script>

<template>
  <div class="min-h-full bg-canvas p-8">
    <div class="mx-auto max-w-2xl">
      <!-- 页面头部：标题 + 搜索 -->
      <div class="mb-6 flex flex-wrap items-center justify-between gap-4">
        <div>
          <h1 class="text-xl font-semibold tracking-tight text-ink">选择项目</h1>
          <p class="mt-1 text-sm text-muted">选择一个项目进入值守</p>
        </div>
        <div class="relative w-64">
          <UiInput v-model="search" placeholder="搜索项目">
            <template #prefix><Icon name="search" :size="14" class="mr-1.5 text-placeholder" /></template>
          </UiInput>
        </div>
      </div>

      <!-- 项目单列列表 -->
      <div class="overflow-hidden rounded-signal border border-line bg-surface">
        <div
          v-for="(p, i) in filteredProjects"
          :key="p.id"
          class="group flex cursor-pointer items-center gap-4 px-5 py-4 transition-colors hover:bg-primary-softer"
          :class="i > 0 ? 'border-t border-line-soft' : ''"
          @click="enterProject(p)"
        >
          <span class="flex h-9 w-9 shrink-0 items-center justify-center rounded-signal bg-zone text-muted group-hover:text-primary">
            <Icon name="folder" :size="16" />
          </span>
          <div class="min-w-0 flex-1">
            <h3 class="truncate text-sm font-medium text-ink group-hover:text-primary">{{ p.name }}</h3>
            <div class="mt-1 flex items-center gap-3 text-xs text-muted">
              <span>{{ projectStats[p.id]?.total ?? 0 }} 台设备</span>
              <span v-if="projectStats[p.id]?.offline" class="text-danger">{{ projectStats[p.id].offline }} 台离线</span>
              <span v-else class="text-success">全部在线</span>
            </div>
          </div>
          <!-- 在线率迷你信号条 -->
          <div class="hidden w-24 shrink-0 items-center gap-2 sm:flex">
            <div class="h-1 flex-1 overflow-hidden rounded-full bg-line">
              <div
                class="h-full rounded-full"
                :class="onlineRatio(p.id) === 1 ? 'bg-success' : onlineRatio(p.id) > 0.5 ? 'bg-warning' : 'bg-danger'"
                :style="{ width: (onlineRatio(p.id) * 100) + '%' }"
              />
            </div>
          </div>
          <Icon name="chevron-right" :size="16" class="shrink-0 text-placeholder group-hover:text-primary" />
        </div>

        <div v-if="!filteredProjects.length" class="p-8">
          <UiEmptyState text="没有匹配的项目" />
        </div>
      </div>

      <button
        class="mt-4 flex w-full items-center justify-center gap-1.5 rounded-signal border border-dashed border-line py-3 text-sm text-muted transition-colors hover:border-primary hover:text-primary"
        @click="addProjDlg.show = true"
      >
        <Icon name="plus" :size="15" />添加项目
      </button>
    </div>

    <!-- 新建项目弹窗 -->
    <UiDialog v-model:open="addProjDlg.show" title="添加新项目" width="max-w-md">
      <div class="space-y-4">
        <div>
          <label class="mb-1.5 block text-xs font-medium text-muted">项目名称 *</label>
          <UiInput v-model="addProjDlg.name" placeholder="请输入新项目名称，如：深圳绿享、杭州仓储" />
        </div>
      </div>
      <template #footer>
        <UiButton @click="addProjDlg.show = false">取消</UiButton>
        <UiButton variant="primary" :loading="addProjDlg.saving" @click="createProject">创建项目</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
