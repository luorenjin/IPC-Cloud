<script setup lang="ts">
// 项目与分组管理（ACC-03/04）：左侧项目列表与启停/重命名，右侧当前项目分组树（≤4 级、同级唯一、拖拽同级排序）
const api = useApi()
const toast = useToast()
const { t } = useI18n()
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
    toastApiError(e, t('system.msg.projectsLoadFailed'))
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
  if (!projDlg.name.trim()) return toast.warning(t('system.msg.needProjectName'))
  projDlg.saving = true
  try {
    if (projDlg.mode === 'create') {
      await api.post('/projects', { name: projDlg.name.trim(), tz: projDlg.tz.trim() || undefined })
      toast.success(t('system.msg.projectCreated'))
    } else {
      await api.put('/projects/' + projDlg.id, { name: projDlg.name.trim(), tz: projDlg.tz.trim() || undefined })
      toast.success(t('system.msg.projectRenamed'))
    }
    projDlg.visible = false
    await loadProjects()
    loadMe() // 同步顶栏项目选择器
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    projDlg.saving = false
  }
}

/* 启用/停用项目 */
async function toggleProject(row: any, v: boolean) {
  try {
    await api.put('/projects/' + row.id, { enabled: v })
    row.enabled = v
    toast.success(v ? t('system.msg.projectEnabled') : t('system.msg.projectDisabled'))
    loadMe()
  } catch (e: any) {
    toastApiError(e, t('system.msg.opFailed'))
    await loadProjects() // 失败回显真实状态
  }
}

/* 删除项目（仅空项目可删，含设备/通道时后端拦截并给出具体数量） */
async function removeProject(row: any) {
  const ok = await confirm.ask({
    title: t('system.projects.deleteTitle'),
    message: t('system.projects.deleteMsg', { name: row.name }),
    detail: t('system.projects.deleteDetail'),
    danger: true,
    confirmText: t('common.delete')
  })
  if (!ok) return
  const wasCurrent = row.id === currentProject.value?.id
  try {
    await api.del('/projects/' + row.id)
    toast.success(t('system.msg.projectDeleted', { name: row.name }))
    await loadProjects()
    // 删掉的是当前项目时，切到剩余项目（优先启用中的），避免顶栏与列表停留在已删项目上
    if (wasCurrent) {
      const next = projects.value.find((p) => p.enabled) || projects.value[0]
      if (next) switchProject(next)
    }
    loadMe()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
  }
}

/* 切换当前项目 */
function useProject(row: any) {
  switchProject(row)
  toast.success(t('system.msg.projectSwitched', { name: row.name }))
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
    toastApiError(e, t('system.msg.groupsLoadFailed'))
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
    return toast.warning(t('system.msg.groupDepthLimit'))
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
  if (!name) return toast.warning(t('system.msg.needGroupName'))
  // 同级唯一（前端预检，后端同样校验）
  const dup = groups.value.some(
    (g) => (g.parentId || '') === (grpDlg.parentId || '') && g.name === name && g.id !== grpDlg.id
  )
  if (dup) return toast.warning(t('system.msg.groupNameDup'))
  grpDlg.saving = true
  try {
    if (grpDlg.mode === 'edit') {
      await api.put('/groups/' + grpDlg.id, { name })
    } else {
      await api.post('/groups', { name, parentId: grpDlg.parentId })
    }
    toast.success(t('common.savedOk'))
    grpDlg.visible = false
    await loadGroups()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    grpDlg.saving = false
  }
}

/* 删除分组（含设备的分组后端拦截："分组下存在设备，请先转移"） */
async function removeGroup(node: any) {
  const ok = await confirm.ask({
    title: t('system.groups.deleteTitle'),
    message: t('system.groups.deleteMsg', { name: node.name }),
    detail: t('system.groups.deleteDetail'),
    danger: true,
    confirmText: t('common.delete')
  })
  if (!ok) return
  try {
    await api.del('/groups/' + node.id)
    toast.success(t('common.deletedOk'))
    await loadGroups()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
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
    toast.warning(t('system.msg.groupCrossLevel'))
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
    toast.success(t('system.msg.groupSorted'))
  } catch (e: any) {
    toastApiError(e, t('system.msg.groupSortFailed'))
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
    <UiCard :title="t('system.projects.title')" class="w-[400px] shrink-0 self-start">
      <template #extra>
        <UiButton variant="primary" size="sm" @click="openProjDlg('create')">
          <UiIcon name="plus" :size="14" />{{ t('system.projects.create') }}
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
                  <UiTag v-if="!p.enabled" color="info">{{ t('system.projects.disabled') }}</UiTag>
                  <UiTag v-else-if="p.id === currentProject?.id" color="primary" dot>{{ t('system.projects.current') }}</UiTag>
                </div>
                <div class="mt-1 flex flex-wrap items-center gap-x-2 gap-y-0.5 text-xs text-placeholder">
                  <span>{{ t('system.projects.tz', { tz: p.tz || '-' }) }}</span>
                  <span>·</span>
                  <span>{{ t('system.projects.createdAt', { time: fmtTime(p.createdAt) }) }}</span>
                  <template v-if="p.id === currentProject?.id">
                    <span>·</span>
                    <span>{{ t('system.projects.groupCount', { n: groups.length }) }}</span>
                  </template>
                </div>
              </div>
            </div>

            <!-- 关键运行参数迷你数据条（替代纯文字堆砌） -->
            <div class="mt-3 space-y-1.5">
              <div class="flex items-center gap-2">
                <span class="w-16 shrink-0 text-[11px] text-placeholder">{{ t('system.projects.runState') }}</span>
                <div class="h-1 flex-1 overflow-hidden rounded-full bg-line">
                  <div
                    class="h-full rounded-full transition-all"
                    :class="p.enabled ? 'bg-success' : 'bg-line'"
                    :style="{ width: p.enabled ? '100%' : '6%' }"
                  />
                </div>
                <span class="w-12 shrink-0 text-right text-[11px]" :class="p.enabled ? 'text-success' : 'text-muted'">
                  {{ p.enabled ? t('system.projects.enabledState') : t('system.projects.disabled') }}
                </span>
              </div>
              <div class="flex items-center gap-2">
                <span class="w-16 shrink-0 text-[11px] text-placeholder">{{ t('system.projects.idleResponse') }}</span>
                <div class="h-1 flex-1 overflow-hidden rounded-full bg-line">
                  <div class="h-full rounded-full bg-primary transition-all" :style="{ width: idlePct(p) + '%' }" />
                </div>
                <span class="w-12 shrink-0 text-right font-mono text-[11px] text-muted">{{ p.settings?.streamIdleSec ?? '-' }}s</span>
              </div>
            </div>

            <div class="mt-3 flex flex-wrap items-center gap-2 border-t border-line-soft pt-2.5">
              <UiSwitch :model-value="!!p.enabled" size="sm" :aria-label="t('system.projects.enableAria', { name: p.name || p.id })" @update:model-value="(v: boolean) => toggleProject(p, v)" />
              <UiButton variant="text" size="sm" class="ml-auto" @click="openProjDlg('edit', p)">{{ t('system.projects.rename') }}</UiButton>
              <UiButton v-if="p.id !== currentProject?.id" variant="text" size="sm" @click="useProject(p)">{{ t('system.projects.switchTo') }}</UiButton>
              <UiButton
                variant="dangerText"
                size="sm"
                :disabled="projects.length <= 1"
                :title="projects.length <= 1 ? t('system.projects.keepOneTip') : t('system.projects.deleteEmptyOnlyTip')"
                @click="removeProject(p)"
              >
                {{ t('common.delete') }}
              </UiButton>
            </div>
          </div>
        </div>
        <UiEmptyState v-else-if="!projLoading" :text="t('system.projects.empty')">
          <template #action>
            <UiButton variant="primary" @click="openProjDlg('create')">{{ t('system.projects.create') }}</UiButton>
          </template>
        </UiEmptyState>
      </UiLoading>
    </UiCard>

    <!-- 右侧：当前项目的分组树 -->
    <UiCard :title="currentProject ? t('system.groups.titleWithProject', { name: currentProject.name }) : t('system.groups.title')" class="min-w-0 flex-1 self-start">
      <template #extra>
        <UiButton variant="primary" size="sm" @click="openGrpDlg('create')">
          <UiIcon name="plus" :size="14" />{{ t('system.groups.createRoot') }}
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
                <UiButton variant="text" size="sm" :disabled="depthOf(node.meta) >= 4" @click="openGrpDlg('create', node.meta)">{{ t('system.groups.child') }}</UiButton>
                <UiButton variant="text" size="sm" @click="openGrpDlg('edit', node.meta)">{{ t('system.groups.rename') }}</UiButton>
                <UiButton variant="dangerText" size="sm" @click="removeGroup(node.meta)">{{ t('common.delete') }}</UiButton>
              </span>
            </template>
          </UiTree>
          <p class="mt-3 border-t border-line-soft pt-2 text-xs text-placeholder">
            {{ t('system.groups.tip') }}
          </p>
        </template>
        <UiEmptyState v-else-if="!treeLoading" :text="t('system.groups.empty')">
          <template #action>
            <UiButton variant="primary" @click="openGrpDlg('create')">{{ t('system.groups.createRoot') }}</UiButton>
          </template>
        </UiEmptyState>
      </UiLoading>
    </UiCard>

    <!-- 项目新建/重命名对话框 -->
    <UiDialog v-model:open="projDlg.visible" :title="projDlg.mode === 'create' ? t('system.projects.createDlgTitle') : t('system.projects.renameDlgTitle')" width="max-w-md">
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('system.projects.name') }}</span>
        <UiInput v-model="projDlg.name" :placeholder="t('system.projects.namePlaceholder')" :maxlength="50" @enter="saveProj" />
        <span class="text-right text-sm text-body">{{ t('system.projects.tzLabel') }}</span>
        <UiInput v-model="projDlg.tz" placeholder="Asia/Shanghai" :maxlength="64" />
      </div>
      <template #footer>
        <UiButton @click="projDlg.visible = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="projDlg.saving" @click="saveProj">{{ projDlg.saving ? t('common.saving') : t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 分组新增/重命名对话框 -->
    <UiDialog
      v-model:open="grpDlg.visible"
      :title="grpDlg.mode === 'create' ? (grpDlg.parentId ? t('system.groups.createChild') : t('system.groups.createRoot')) : t('system.groups.renameTitle')"
      width="max-w-md"
    >
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body">{{ t('system.groups.parent') }}</span>
        <UiInput :model-value="grpDlg.parentId ? groupName(grpDlg.parentId) : t('system.groups.rootParent')" disabled />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('system.groups.name') }}</span>
        <UiInput v-model="grpDlg.name" :placeholder="t('system.groups.namePlaceholder')" :maxlength="50" @enter="saveGrp" />
      </div>
      <template #footer>
        <UiButton @click="grpDlg.visible = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="grpDlg.saving" @click="saveGrp">{{ grpDlg.saving ? t('common.saving') : t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
