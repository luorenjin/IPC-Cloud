<script setup lang="ts">
// 项目与分组管理（ACC-03/04）：左侧项目列表与切换，右侧当前项目的分组树
const api = useApi()
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
    ElMessage.error(e?.msg || '加载项目列表失败')
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
  saving: false
})

function openProjDlg(mode: 'create' | 'edit', row?: any) {
  projDlg.mode = mode
  projDlg.id = row?.id ?? null
  projDlg.name = row?.name || ''
  projDlg.visible = true
}

async function saveProj() {
  if (!projDlg.name.trim()) return ElMessage.warning('请输入项目名称')
  projDlg.saving = true
  try {
    if (projDlg.mode === 'create') {
      await api.post('/projects', { name: projDlg.name.trim() })
      ElMessage.success('项目已创建')
    } else {
      await api.put('/projects/' + projDlg.id, { name: projDlg.name.trim() })
      ElMessage.success('已重命名')
    }
    projDlg.visible = false
    await loadProjects()
    loadMe() // 同步顶栏项目选择器
  } catch (e: any) {
    ElMessage.error(e?.msg || '保存失败')
  } finally {
    projDlg.saving = false
  }
}

/* 启用/停用项目 */
async function toggleProject(row: any) {
  try {
    await api.put('/projects/' + row.id, { enabled: row.enabled })
    ElMessage.success(row.enabled ? '已启用' : '已停用')
    loadMe()
  } catch (e: any) {
    ElMessage.error(e?.msg || '操作失败')
    await loadProjects() // 失败回显真实状态
  }
}

/* 切换当前项目 */
function useProject(row: any) {
  switchProject(row)
  ElMessage.success('已切换到项目「' + row.name + '」')
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
    ElMessage.error(e?.msg || '加载分组失败')
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
  if (!grpDlg.name.trim()) return ElMessage.warning('请输入分组名称')
  grpDlg.saving = true
  try {
    if (grpDlg.mode === 'edit') {
      await api.put('/groups/' + grpDlg.id, { name: grpDlg.name.trim() })
    } else {
      await api.post('/groups', { name: grpDlg.name.trim(), parentId: grpDlg.parentId })
    }
    ElMessage.success('已保存')
    grpDlg.visible = false
    await loadGroups()
  } catch (e: any) {
    ElMessage.error(e?.msg || '保存失败')
  } finally {
    grpDlg.saving = false
  }
}

/* 删除分组（失败时提示后端 msg，如"分组下存在设备"） */
async function removeGroup(node: any) {
  try {
    await ElMessageBox.confirm('确定删除分组「' + node.name + '」？', '删除分组', {
      type: 'warning',
      confirmButtonText: '删除',
      cancelButtonText: '取消'
    })
  } catch {
    return
  }
  try {
    await api.del('/groups/' + node.id)
    ElMessage.success('已删除')
    await loadGroups()
  } catch (e: any) {
    ElMessage.error(e?.msg || '删除失败')
  }
}

/* 切换项目后刷新分组树 */
watch(currentProject, () => loadGroups())

const fmtTime = (ts: any) =>
  ts ? new Date(typeof ts === 'string' ? Date.parse(ts) : ts).toLocaleString() : '-'

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await Promise.all([loadProjects(), loadGroups()])
})
</script>

<template>
  <div class="page">
    <!-- 左侧：项目列表 -->
    <el-card class="left" shadow="never">
      <template #header>
        <div class="card-head">
          <span>项目列表</span>
          <el-button type="primary" size="small" @click="openProjDlg('create')">新建项目</el-button>
        </div>
      </template>
      <div v-loading="projLoading" class="proj-list">
        <div
          v-for="p in projects"
          :key="p.id"
          class="proj-item"
          :class="{ active: p.id === currentProject?.id }"
        >
          <div class="proj-name">
            {{ p.name }}
            <el-tag v-if="!p.enabled" size="small" type="info">已停用</el-tag>
          </div>
          <div class="proj-meta">时区 {{ p.tz || '-' }} · {{ fmtTime(p.createdAt) }}</div>
          <div class="proj-ops">
            <el-switch
              v-model="p.enabled"
              size="small"
              :active-value="true"
              :inactive-value="false"
              @change="toggleProject(p)"
            />
            <el-button link type="primary" size="small" @click="openProjDlg('edit', p)">重命名</el-button>
            <el-button
              v-if="p.id !== currentProject?.id"
              link
              type="success"
              size="small"
              @click="useProject(p)"
            >切换到此项目</el-button>
            <el-tag v-else size="small" type="success">当前项目</el-tag>
          </div>
        </div>
        <el-empty v-if="!projLoading && !projects.length" description="暂无项目" :image-size="60" />
      </div>
    </el-card>

    <!-- 右侧：当前项目的分组树 -->
    <el-card class="right" shadow="never">
      <template #header>
        <div class="card-head">
          <span>分组管理{{ currentProject ? ' - ' + currentProject.name : '' }}</span>
          <el-button type="primary" size="small" @click="openGrpDlg('create')">新增根分组</el-button>
        </div>
      </template>
      <el-tree
        v-loading="treeLoading"
        :data="treeData"
        node-key="id"
        default-expand-all
        :expand-on-click-node="false"
        :props="{ label: 'name', children: 'children' }"
      >
        <template #default="{ data }">
          <div class="grp-node">
            <span class="grp-name">{{ data.name }}</span>
            <span class="grp-ops" @click.stop>
              <el-button link type="primary" size="small" @click="openGrpDlg('create', data)">新增子分组</el-button>
              <el-button link type="primary" size="small" @click="openGrpDlg('edit', data)">重命名</el-button>
              <el-button link type="danger" size="small" @click="removeGroup(data)">删除</el-button>
            </span>
          </div>
        </template>
      </el-tree>
      <el-empty v-if="!treeLoading && !treeData.length" description="当前项目暂无分组" :image-size="60" />
    </el-card>

    <!-- 项目新建/重命名对话框 -->
    <el-dialog
      v-model="projDlg.visible"
      :title="projDlg.mode === 'create' ? '新建项目' : '重命名项目'"
      width="420px"
    >
      <el-form label-width="80px" @submit.prevent>
        <el-form-item label="项目名称">
          <el-input v-model="projDlg.name" placeholder="请输入项目名称" maxlength="50" @keyup.enter="saveProj" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="projDlg.visible = false">取消</el-button>
        <el-button type="primary" :loading="projDlg.saving" @click="saveProj">确定</el-button>
      </template>
    </el-dialog>

    <!-- 分组新增/重命名对话框 -->
    <el-dialog
      v-model="grpDlg.visible"
      :title="grpDlg.mode === 'create' ? (grpDlg.parentId ? '新增子分组' : '新增根分组') : '重命名分组'"
      width="420px"
    >
      <el-form label-width="80px" @submit.prevent>
        <el-form-item label="上级分组">
          <el-input :model-value="grpDlg.parentId ? groupName(grpDlg.parentId) : '（根分组）'" disabled />
        </el-form-item>
        <el-form-item label="分组名称">
          <el-input v-model="grpDlg.name" placeholder="请输入分组名称" maxlength="50" @keyup.enter="saveGrp" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="grpDlg.visible = false">取消</el-button>
        <el-button type="primary" :loading="grpDlg.saving" @click="saveGrp">确定</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.page { display: flex; gap: 16px; align-items: flex-start; }
.left { width: 380px; flex-shrink: 0; }
.right { flex: 1; min-width: 0; }
.card-head { display: flex; justify-content: space-between; align-items: center; }
.proj-item {
  border: 1px solid #e4e7ed; border-radius: 6px;
  padding: 10px 12px; margin-bottom: 10px;
}
.proj-item.active { border-color: #409eff; background: #ecf5ff; }
.proj-name { font-weight: 600; display: flex; align-items: center; gap: 8px; }
.proj-meta { color: #909399; font-size: 12px; margin-top: 4px; }
.proj-ops { display: flex; align-items: center; gap: 6px; margin-top: 8px; flex-wrap: wrap; }
.grp-node {
  display: flex; align-items: center; justify-content: space-between;
  flex: 1; min-width: 0; padding-right: 8px;
}
.grp-name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.grp-ops { visibility: hidden; flex-shrink: 0; }
.grp-node:hover .grp-ops { visibility: visible; }
</style>