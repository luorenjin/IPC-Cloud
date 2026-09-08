<script setup lang="ts">
// 控制台首页（企业及项目选择页）：严格对齐图 2（独立全屏页面，无业务侧边栏）
definePageMeta({ layout: 'console' })

const api = useApi()
const toast = useToast()
const router = useRouter()
const { user, projects, currentProject, switchProject, loadMe } = useAuth()

const search = ref('')
const viewMode = ref<'grid' | 'list'>('grid')
const selectedEnterprise = ref('ent-1')

// 模拟或真实租户/企业列表（对齐图 2）
const enterprises = ref([
  { id: 'ent-1', name: '鲁班长(深圳)科技有限公司', projectCount: 1, tag: '@hz', createdAt: '2026-06-15 12:39' },
  { id: 'ent-2', name: '成都绿科环保科技', projectCount: 1, sourceUser: '132****2078' }
])

// 项目列表与统计数据
const projectStats = ref<Record<string, { total: number; offline: number }>>({})

async function loadProjectStats() {
  for (const p of projects.value) {
    try {
      const res: any = await api.get('/devices', { projectId: p.id, pageSize: 1 })
      // 统计数据拉取
      const total = res?.total || 0
      const offlineRes: any = await api.get('/devices', { projectId: p.id, status: 'offline', pageSize: 1 })
      const offline = offlineRes?.total || 0
      projectStats.value[p.id] = { total, offline }
    } catch {
      projectStats.value[p.id] = { total: 0, offline: 0 }
    }
  }
}

// 搜索过滤后的项目
const filteredProjects = computed(() => {
  const q = search.value.trim().toLowerCase()
  if (!q) return projects.value
  return projects.value.filter((p: any) => p.name.toLowerCase().includes(q))
})

// 进入项目首页（对齐图 3）
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
  <div class="min-h-full bg-[#f0f3f7] p-8">
    <div class="mx-auto max-w-7xl">
      <!-- 页面头部标题与搜索框（严格对齐图 2） -->
      <div class="mb-6 flex flex-wrap items-center justify-between gap-4">
        <div>
          <div class="flex items-center gap-2">
            <h1 class="text-2xl font-bold tracking-tight text-[#1f2329]">所有企业及项目</h1>
            <button class="rounded-full p-1 text-[#86909c] hover:bg-[#e5e6eb] hover:text-[#1f2329]" @click="router.push('/')">
              <Icon name="x" :size="18" />
            </button>
          </div>
          <p class="mt-1 text-sm text-[#86909c]">请选择一个项目进入</p>
        </div>

        <!-- 搜索框（大输入条） -->
        <div class="relative w-80">
          <div class="flex h-10 items-center rounded border border-[#c9cdd4] bg-white px-3 shadow-sm focus-within:border-[#1785E6] focus-within:ring-1 focus-within:ring-[#1785E6]">
            <Icon name="search" :size="16" class="mr-2 text-[#86909c]" />
            <input
              v-model="search"
              type="text"
              class="w-full bg-transparent text-sm outline-none placeholder:text-[#86909c]"
              placeholder="搜索企业/项目"
            />
          </div>
        </div>
      </div>

      <!-- 分栏内容：左企业列表，右项目列表 -->
      <div class="flex gap-6 items-start">
        <!-- 左侧：企业列表 -->
        <div class="w-72 shrink-0">
          <div class="mb-3 flex items-center justify-between text-sm">
            <span class="font-bold text-[#1f2329]">企业列表 | {{ enterprises.length }}</span>
            <span class="text-xs text-[#86909c]">拖住条目按需排序</span>
          </div>

          <div class="space-y-3">
            <div
              v-for="ent in enterprises"
              :key="ent.id"
              class="cursor-pointer rounded-lg bg-white p-4 shadow-sm transition-all border"
              :class="selectedEnterprise === ent.id ? 'border-[#1785E6] ring-1 ring-[#1785E6]' : 'border-[#e5e6eb] hover:border-[#b4bccc]'"
              @click="selectedEnterprise = ent.id"
            >
              <div class="flex items-center justify-between">
                <span class="font-bold text-[#1f2329] truncate">{{ ent.name }}</span>
                <Icon name="grip-vertical" :size="14" class="text-[#86909c]" />
              </div>
              <div class="mt-2 text-xs text-[#86909c]">
                项目数：{{ ent.projectCount }} {{ ent.tag || '' }}
              </div>
              <div v-if="ent.createdAt" class="mt-1 text-xs text-[#c9cdd4]">
                创建于：{{ ent.createdAt }}
              </div>
              <div v-if="ent.sourceUser" class="mt-1 text-xs text-[#c9cdd4]">
                来自账户：{{ ent.sourceUser }}
              </div>
            </div>
          </div>
        </div>

        <!-- 右侧：项目卡片区 -->
        <div class="min-w-0 flex-1">
          <!-- 头部工具栏 -->
          <div class="mb-4 flex items-center justify-between">
            <div class="flex items-center gap-3">
              <span class="font-bold text-[#1f2329]">项目 | {{ filteredProjects.length }}</span>
              <div class="flex items-center rounded border border-[#e5e6eb] bg-white p-0.5">
                <button
                  class="rounded p-1 text-[#4e5969] transition-colors"
                  :class="viewMode === 'grid' ? 'bg-[#ebf5ff] text-[#1785E6]' : 'hover:text-[#1f2329]'"
                  @click="viewMode = 'grid'"
                >
                  <Icon name="grid" :size="15" />
                </button>
                <button
                  class="rounded p-1 text-[#4e5969] transition-colors"
                  :class="viewMode === 'list' ? 'bg-[#ebf5ff] text-[#1785E6]' : 'hover:text-[#1f2329]'"
                  @click="viewMode = 'list'"
                >
                  <Icon name="list" :size="15" />
                </button>
              </div>
            </div>

            <!-- 添加项目按钮 -->
            <button
              class="flex items-center gap-1.5 text-sm font-medium text-[#1785E6] hover:text-[#0a6bcc]"
              @click="addProjDlg.show = true"
            >
              <Icon name="plus" :size="16" />添加项目
            </button>
          </div>

          <!-- 项目卡片网格（对齐图 2） -->
          <div v-if="viewMode === 'grid'" class="grid grid-cols-1 gap-4 sm:grid-cols-2 lg:grid-cols-3">
            <div
              v-for="p in filteredProjects"
              :key="p.id"
              class="group relative flex flex-col justify-between rounded-xl bg-white p-6 shadow-sm transition-all hover:shadow-md border border-[#e5e6eb] hover:border-[#1785E6] cursor-pointer"
              @click="enterProject(p)"
            >
              <div>
                <div class="flex items-start justify-between">
                  <div>
                    <h3 class="text-base font-bold text-[#1f2329] group-hover:text-[#1785E6] transition-colors">
                      {{ p.name }}
                    </h3>
                    <div class="mt-1 flex items-center gap-2">
                      <span class="text-xs text-[#86909c]">创建于 2026-04-08</span>
                      <span class="rounded bg-[#e8f3fc] px-1.5 py-0.5 text-[10px] font-medium text-[#1785E6]">通用型</span>
                    </div>
                  </div>
                  <button class="text-[#86909c] hover:text-[#1f2329] p-1" @click.stop>
                    <Icon name="more-horizontal" :size="16" />
                  </button>
                </div>
              </div>

              <!-- 下方指标展示：设备总数、离线 -->
              <div class="mt-6 flex items-baseline gap-8 border-t border-[#f2f3f5] pt-4">
                <div>
                  <div class="text-xs text-[#86909c]">设备总数</div>
                  <div class="mt-1 text-2xl font-bold text-[#1f2329]">
                    {{ projectStats[p.id]?.total ?? 0 }} <span class="text-xs font-normal text-[#86909c]">台</span>
                  </div>
                </div>
                <div>
                  <div class="text-xs text-[#f53f3f]">离线</div>
                  <div class="mt-1 text-2xl font-bold text-[#f53f3f]">
                    {{ projectStats[p.id]?.offline ?? 0 }} <span class="text-xs font-normal text-[#f53f3f]">台</span>
                  </div>
                </div>
              </div>
            </div>
          </div>

          <!-- 列表视图 -->
          <div v-else class="rounded-xl border border-[#e5e6eb] bg-white overflow-hidden shadow-sm">
            <table class="w-full text-left text-sm">
              <thead class="bg-[#f7f8fa] text-xs text-[#86909c] border-b border-[#e5e6eb]">
                <tr>
                  <th class="py-3 px-4">项目名称</th>
                  <th class="py-3 px-4">创建时间</th>
                  <th class="py-3 px-4">类型</th>
                  <th class="py-3 px-4 text-center">设备总数</th>
                  <th class="py-3 px-4 text-center">离线设备</th>
                  <th class="py-3 px-4 text-right">操作</th>
                </tr>
              </thead>
              <tbody class="divide-y divide-[#e5e6eb]">
                <tr
                  v-for="p in filteredProjects"
                  :key="p.id"
                  class="hover:bg-[#f2f8fe] cursor-pointer"
                  @click="enterProject(p)"
                >
                  <td class="py-3 px-4 font-bold text-[#1f2329]">{{ p.name }}</td>
                  <td class="py-3 px-4 text-xs text-[#86909c]">2026-04-08</td>
                  <td class="py-3 px-4"><span class="rounded bg-[#e8f3fc] px-1.5 py-0.5 text-[10px] text-[#1785E6]">通用型</span></td>
                  <td class="py-3 px-4 text-center font-bold">{{ projectStats[p.id]?.total ?? 0 }}</td>
                  <td class="py-3 px-4 text-center font-bold text-[#f53f3f]">{{ projectStats[p.id]?.offline ?? 0 }}</td>
                  <td class="py-3 px-4 text-right text-[#1785E6] hover:underline">进入项目 &gt;</td>
                </tr>
              </tbody>
            </table>
          </div>

          <div class="mt-8 text-center text-xs text-[#c9cdd4]">没有更多了</div>
        </div>
      </div>
    </div>

    <!-- 新建项目弹窗 -->
    <UiDialog v-model:open="addProjDlg.show" title="添加新项目" width="max-w-md">
      <div class="space-y-4">
        <div>
          <label class="mb-1.5 block text-xs font-medium text-[#4e5969]">项目名称 *</label>
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
