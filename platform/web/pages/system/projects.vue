<script setup lang="ts">
// 项目与分组管理（ACC-03/04）：左侧项目列表与启停/重命名，右侧当前项目分组树（≤4 级、同级唯一、拖拽同级排序）
const api = useApi()
const toast = useToast()
const confirm = useConfirm()
const { currentProject, switchProject, loadMe } = useAuth()

/* ---------------- 项目列表 ---------------- */
const projects = ref<any[]>([])
const projLoading = ref(false)

async function loadProjects() {
  projLoading.value = true
  try {
    const res: any = await api.get('/projects')
    projects.value = res?.items || []
  } catch (e: any) {
    toastApiError(e, '加载项目列表失败')
  } finally {
    projLoading.value = false
  }
}

/* 新建/重命名项目对话框 */
const projDlg = reactive({
  visible: false,
  mode: 'create' as 'create' | 'edit',
  id: null as any,
  name: '',
  tz: 'Asia/Shanghai',
  saving: false
})

function openProjDlg(mode: 'create' | 'edit', row?: any) {
  projDlg.mode = mode
  projDlg.id = row?.id ?? null
  projDlg.name = row?.name || ''
  projDlg.tz = row?.tz || 'Asia/Shanghai'
  projDlg.visible = true
}

async function saveProj() {
  if (!projDlg.name.trim()) return toast.warning('请输入项目名称')
  projDlg.saving = true
  try {
    if (projDlg.mode === 'create') {
      await api.post('/projects', { name: projDlg.name.trim(), tz: projDlg.tz.trim() || undefined })
      toast.success('项目已创建')
    } else {
      await api.put('/projects/' + projDlg.id, { name: projDlg.name.trim(), tz: projDlg.tz.trim() || undefined })
      toast.success('已重命名')
    }
    projDlg.visible = false
    await loadProjects()
    loadMe() // 同步顶栏项目选择器
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    projDlg.saving = false
  }
}

/* 启用/停用项目 */
async function toggleProject(row: any, v: boolean) {
  try {
    await api.put('/projects/' + row.id, { enabled: v })
    row.enabled = v
    toast.success(v ? '已启用' : '已停用')
    loadMe()
  } catch (e: any) {
    toastApiError(e, '操作失败')
    await loadProjects() // 失败回显真实状态
  }
}

/* 删除项目（仅空项目可删，含设备/通道时后端拦截并给出具体数量） */
async function removeProject(row: any) {
  const ok = await confirm.ask({
    title: '删除项目',
    message: '确定删除项目「' + row.name + '」？',
    detail:
      '仅可删除空项目，项目下存在设备或通道时无法删除。删除后该项目的分组、角色、成员授权及告警与录像模板将一并清除，且不可恢复。',
    danger: true,
    confirmText: '删除'
  })
  if (!ok) return
  const wasCurrent = row.id === currentProject.value?.id
  try {
    await api.del('/projects/' + row.id)
    toast.success('项目「' + row.name + '」已删除')
    await loadProjects()
    // 删掉的是当前项目时，切到剩余项目（优先启用中的），避免顶栏与列表停留在已删项目上
    if (wasCurrent) {
      const next = projects.value.find((p) => p.enabled) || projects.value[0]
      if (next) switchProject(next)
    }
    loadMe()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

/* 切换当前项目 */
function useProject(row: any) {
  switchProject(row)
  toast.success('已切换到项目「' + row.name + '」')
}

/* ---------------- 分组树 ---------------- */
const groups = ref<any[]>([])
const treeLoading = ref(false)

async function loadGroups() {
  if (!currentProject.value) return
  treeLoading.value = true
  try {
    const res: any = await api.get('/groups', { projectId: currentProject.value.id })
    groups.value = res?.items || []
  } catch (e: any) {
    toastApiError(e, '加载分组失败')
  } finally {
    treeLoading.value = false
  }
}

// 扁平分组列表 -> 树结构（按 sort 排序）
function buildTree(items: any[]) {
  const map = new Map<any, any>()
  items.forEach((it) => map.set(it.id, { ...it, children: [] }))
  const roots: any[] = []
  items.forEach((it) => {
    const node = map.get(it.id)
    const parent = it.parentId ? map.get(it.parentId) : null
    if (parent) parent.children.push(node)
    else roots.push(node)
  })
  const sortRec = (arr: any[]) => {
    arr.sort((a, b) => (a.sort ?? 0) - (b.sort ?? 0))
    arr.forEach((n) => sortRec(n.children || []))
  }
  sortRec(roots)
  return roots
}
const treeData = computed(() => buildTree(groups.value))

// UiTree 节点映射（meta 携带原始分组数据）
function toTreeNodes(arr: any[]): any[] {
  return arr.map((n) => ({ label: n.name, value: String(n.id), meta: n, children: toTreeNodes(n.children || []) }))
}
const treeNodes = computed(() => toTreeNodes(treeData.value))

function depthOf(node: any): number {
  let d = 1
  let p = node.parentId
  while (p) {
    const g = groups.value.find((x) => x.id === p)
    if (!g) break
    d++
    p = g.parentId
  }
  return d
}

function groupName(id: any) {
  const g = groups.value.find((x) => x.id === id)
  return g?.name || String(id)
}

/* 新增根分组 / 新增子分组 / 重命名分组 对话框 */
const grpDlg = reactive({
  visible: false,
  mode: 'create' as 'create' | 'edit',
  id: null as any,
  parentId: null as any,
  name: '',
  saving: false
})

// create：传 node 则为"新增子分组"（父节点为 node），否则为"新增根分组"
function openGrpDlg(mode: 'create' | 'edit', node?: any) {
  if (mode === 'create' && node && depthOf(node) >= 4) {
    return toast.warning('分组层级最多 4 级，不能在末级分组下继续新增')
  }
  grpDlg.mode = mode
  if (mode === 'edit') {
    grpDlg.id = node.id
    grpDlg.parentId = node.parentId ?? null
    grpDlg.name = node.name
  } else {
    grpDlg.id = null
    grpDlg.parentId = node?.id ?? null
    grpDlg.name = ''
  }
  grpDlg.visible = true
}

async function saveGrp() {
  const name = grpDlg.name.trim()
  if (!name) return toast.warning('请输入分组名称')
  // 同级唯一（前端预检，后端同样校验）
  const dup = groups.value.some(
    (g) => (g.parentId || '') === (grpDlg.parentId || '') && g.name === name && g.id !== grpDlg.id
  )
  if (dup) return toast.warning('同级分组名已存在，请更换名称')
  grpDlg.saving = true
  try {
    if (grpDlg.mode === 'edit') {
      await api.put('/groups/' + grpDlg.id, { name })
    } else {
      await api.post('/groups', { name, parentId: grpDlg.parentId })
    }
    toast.success('已保存')
    grpDlg.visible = false
    await loadGroups()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    grpDlg.saving = false
  }
}

/* 删除分组（含设备的分组后端拦截："分组下存在设备，请先转移"） */
async function removeGroup(node: any) {
  const ok = await confirm.ask({
    title: '删除分组',
    message: '确定删除分组「' + node.name + '」？',
    detail: '分组下存在设备或子分组时无法删除，请先转移。',
    danger: true,
    confirmText: '删除'
  })
  if (!ok) return
  try {
    await api.del('/groups/' + node.id)
    toast.success('已删除')
    await loadGroups()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

/* ---------------- 拖拽同级排序 ---------------- */
const dragId = ref<string | null>(null)
const dropOverId = ref<string | null>(null)

function onNodeDragStart(id: string, e: DragEvent) {
  dragId.value = id
  if (e.dataTransfer) {
    e.dataTransfer.effectAllowed = 'move'
    e.dataTransfer.setData('text/plain', id)
  }
}
function onNodeDragOver(id: string, e: DragEvent) {
  if (!dragId.value || dragId.value === id) return
  e.preventDefault()
  dropOverId.value = id
}
async function onNodeDrop(targetId: string) {
  const src = dragId.value
  dragId.value = null
  dropOverId.value = null
  if (!src || src === targetId) return
  const g1 = groups.value.find((g) => String(g.id) === String(src))
  const g2 = groups.value.find((g) => String(g.id) === String(targetId))
  if (!g1 || !g2) return
  if ((g1.parentId || '') !== (g2.parentId || '')) {
    toast.warning('分组暂不支持跨级移动，仅支持同级拖拽排序')
    return
  }
  const parentId = g1.parentId || ''
  const sibs = groups.value
    .filter((g) => (g.parentId || '') === parentId)
    .sort((a, b) => (a.sort ?? 0) - (b.sort ?? 0))
  const from = sibs.findIndex((g) => g.id === g1.id)
  const to = sibs.findIndex((g) => g.id === g2.id)
  if (from < 0 || to < 0) return
  sibs.splice(to, 0, ...sibs.splice(from, 1))
  const updates: Array<{ g: any; sort: number }> = []
  sibs.forEach((g, i) => {
    if (g.sort !== i) {
      updates.push({ g, sort: i })
      g.sort = i
    }
  })
  if (!updates.length) return
  try {
    await Promise.all(updates.map((u) => api.put('/groups/' + u.g.id, { sort: u.sort })))
    toast.success('分组顺序已更新')
  } catch (e: any) {
    toastApiError(e, '分组排序保存失败')
    await loadGroups() // 回滚本地顺序为后端实际状态
  }
}

/* 切换项目后刷新分组树 */
watch(currentProject, () => loadGroups())


// 停流等待时长换算为迷你数据条百分比（有效范围 5-600 秒，纯展示用）
function idlePct(p: any) {
  const v = Number(p?.settings?.streamIdleSec ?? 30)
  if (!Number.isFinite(v) || v <= 0) return 4
  return Math.max(4, Math.min(100, Math.round((v / 600) * 100)))
}

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await Promise.all([loadProjects(), loadGroups()])
})
</script>

<template>
  <div class="flex items-start gap-3">
    <!-- 左侧：项目列表 -->
    <UiCard title="项目列表" class="w-[400px] shrink-0 self-start">
      <template #extra>
        <UiButton variant="primary" size="sm" @click="openProjDlg('create')">
          <UiIcon name="plus" :size="14" />新建项目
        </UiButton>
      </template>
      <UiLoading :loading="projLoading">
        <div v-if="projects.length" class="space-y-2.5">
          <div
            v-for="p in projects" :key="p.id"
            class="rounded-signal border p-3.5 transition-colors"
            :class="p.id === currentProject?.id ? 'border-primary bg-primary-softer' : 'border-line bg-surface hover:border-placeholder'"
          >
            <div class="flex items-start gap-2.5">
              <span
                class="mt-0.5 flex h-7 w-7 shrink-0 items-center justify-center rounded-signal"
                :class="p.id === currentProject?.id ? 'bg-primary-soft text-primary' : 'bg-zone text-placeholder'"
              >
                <UiIcon name="folder" :size="14" />
              </span>
              <div class="min-w-0 flex-1">
                <div class="flex flex-wrap items-center gap-1.5">
                  <span class="truncate text-sm font-semibold text-ink">{{ p.name }}</span>
                  <UiTag v-if="!p.enabled" color="info">已停用</UiTag>
                  <UiTag v-else-if="p.id === currentProject?.id" color="primary" dot>当前项目</UiTag>
                </div>
                <div class="mt-1 flex flex-wrap items-center gap-x-2 gap-y-0.5 text-xs text-placeholder">
                  <span>时区 {{ p.tz || '-' }}</span>
                  <span>·</span>
                  <span>{{ fmtTime(p.createdAt) }} 创建</span>
                  <template v-if="p.id === currentProject?.id">
                    <span>·</span>
                    <span>{{ groups.length }} 个分组</span>
                  </template>
                </div>
              </div>
            </div>

            <!-- 关键运行参数迷你数据条（替代纯文字堆砌） -->
            <div class="mt-3 space-y-1.5">
              <div class="flex items-center gap-2">
                <span class="w-16 shrink-0 text-[11px] text-placeholder">运行状态</span>
                <div class="h-1 flex-1 overflow-hidden rounded-full bg-line">
                  <div
                    class="h-full rounded-full transition-all"
                    :class="p.enabled ? 'bg-success' : 'bg-line'"
                    :style="{ width: p.enabled ? '100%' : '6%' }"
                  />
                </div>
                <span class="w-12 shrink-0 text-right text-[11px]" :class="p.enabled ? 'text-success' : 'text-muted'">
                  {{ p.enabled ? '启用中' : '已停用' }}
                </span>
              </div>
              <div class="flex items-center gap-2">
                <span class="w-16 shrink-0 text-[11px] text-placeholder">停流响应</span>
                <div class="h-1 flex-1 overflow-hidden rounded-full bg-line">
                  <div class="h-full rounded-full bg-primary transition-all" :style="{ width: idlePct(p) + '%' }" />
                </div>
                <span class="w-12 shrink-0 text-right font-mono text-[11px] text-muted">{{ p.settings?.streamIdleSec ?? '-' }}s</span>
              </div>
            </div>

            <div class="mt-3 flex flex-wrap items-center gap-2 border-t border-line-soft pt-2.5">
              <UiSwitch :model-value="!!p.enabled" size="sm" @update:model-value="(v: boolean) => toggleProject(p, v)" />
              <UiButton variant="text" size="sm" class="ml-auto" @click="openProjDlg('edit', p)">重命名</UiButton>
              <UiButton v-if="p.id !== currentProject?.id" variant="text" size="sm" @click="useProject(p)">切换到此项目</UiButton>
              <UiButton
                variant="dangerText"
                size="sm"
                :disabled="projects.length <= 1"
                :title="projects.length <= 1 ? '至少需要保留一个项目' : '仅可删除空项目'"
                @click="removeProject(p)"
              >
                删除
              </UiButton>
            </div>
          </div>
        </div>
        <UiEmptyState v-else-if="!projLoading" text="暂无项目">
          <template #action>
            <UiButton variant="primary" @click="openProjDlg('create')">新建项目</UiButton>
          </template>
        </UiEmptyState>
      </UiLoading>
    </UiCard>

    <!-- 右侧：当前项目的分组树 -->
    <UiCard :title="'分组管理' + (currentProject ? ' - ' + currentProject.name : '')" class="min-w-0 flex-1 self-start">
      <template #extra>
        <UiButton variant="primary" size="sm" @click="openGrpDlg('create')">
          <UiIcon name="plus" :size="14" />新增根分组
        </UiButton>
      </template>
      <UiLoading :loading="treeLoading">
        <template v-if="treeNodes.length">
          <UiTree :nodes="treeNodes">
            <template #node="{ node }">
              <span
                draggable="true"
                class="min-w-0 flex-1 cursor-grab truncate active:cursor-grabbing"
                :class="dropOverId === node.value ? 'border-t-2 border-primary' : ''"
                @dragstart="onNodeDragStart(node.value, $event)"
                @dragover="onNodeDragOver(node.value, $event)"
                @dragleave="dropOverId === node.value && (dropOverId = null)"
                @drop="onNodeDrop(node.value)"
                @dragend="dragId = null; dropOverId = null"
              >
                <UiIcon name="folder" :size="13" class="mr-1 inline-block align-[-2px] text-placeholder" />{{ node.label }}
              </span>
            </template>
            <template #node-extra="{ node }">
              <span class="ml-auto hidden shrink-0 items-center gap-1 group-hover/node:flex" @click.stop>
                <UiButton variant="text" size="sm" :disabled="depthOf(node.meta) >= 4" @click="openGrpDlg('create', node.meta)">子分组</UiButton>
                <UiButton variant="text" size="sm" @click="openGrpDlg('edit', node.meta)">重命名</UiButton>
                <UiButton variant="dangerText" size="sm" @click="removeGroup(node.meta)">删除</UiButton>
              </span>
            </template>
          </UiTree>
          <p class="mt-3 border-t border-line-soft pt-2 text-xs text-placeholder">
            提示：按住分组名称拖拽到同级其他分组上可调整顺序；层级最多 4 级，同级名称需唯一。
          </p>
        </template>
        <UiEmptyState v-else-if="!treeLoading" text="当前项目暂无分组">
          <template #action>
            <UiButton variant="primary" @click="openGrpDlg('create')">新增根分组</UiButton>
          </template>
        </UiEmptyState>
      </UiLoading>
    </UiCard>

    <!-- 项目新建/重命名对话框 -->
    <UiDialog v-model:open="projDlg.visible" :title="projDlg.mode === 'create' ? '新建项目' : '重命名项目'" width="max-w-md">
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>项目名称</span>
        <UiInput v-model="projDlg.name" placeholder="请输入项目名称" :maxlength="50" @enter="saveProj" />
        <span class="text-right text-sm text-body">时区</span>
        <UiInput v-model="projDlg.tz" placeholder="Asia/Shanghai" :maxlength="64" />
      </div>
      <template #footer>
        <UiButton @click="projDlg.visible = false">取消</UiButton>
        <UiButton variant="primary" :disabled="projDlg.saving" @click="saveProj">{{ projDlg.saving ? '保存中…' : '确定' }}</UiButton>
      </template>
    </UiDialog>

    <!-- 分组新增/重命名对话框 -->
    <UiDialog
      v-model:open="grpDlg.visible"
      :title="grpDlg.mode === 'create' ? (grpDlg.parentId ? '新增子分组' : '新增根分组') : '重命名分组'"
      width="max-w-md"
    >
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body">上级分组</span>
        <UiInput :model-value="grpDlg.parentId ? groupName(grpDlg.parentId) : '（根分组）'" disabled />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>分组名称</span>
        <UiInput v-model="grpDlg.name" placeholder="请输入分组名称" :maxlength="50" @enter="saveGrp" />
      </div>
      <template #footer>
        <UiButton @click="grpDlg.visible = false">取消</UiButton>
        <UiButton variant="primary" :disabled="grpDlg.saving" @click="saveGrp">{{ grpDlg.saving ? '保存中…' : '确定' }}</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
