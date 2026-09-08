<script setup lang="ts">
// 角色与成员管理（ACC-05/06/07）：角色权限矩阵（A17）+ 项目成员管理
const api = useApi()
const toast = useToast()
const confirm = useConfirm()
const { currentProject, loadMe } = useAuth()

const activeTab = ref('roles')

/* ---------------- 权限模型 ---------------- */
// 模块清单（左栏）
const MODULES = [
  { value: 'dashboard', label: '仪表盘' },
  { value: 'devices', label: '设备' },
  { value: 'live', label: '实时预览' },
  { value: 'playback', label: '录像回放' },
  { value: 'alarms', label: '告警' },
  { value: 'record', label: '录像设置' },
  { value: 'system', label: '系统' }
]
// 操作权限（右栏）
const ACTION_OPTIONS = [
  { value: 'view', label: '查看' },
  { value: 'preview', label: '预览' },
  { value: 'playback', label: '回放' },
  { value: 'ptz', label: '控制PTZ' },
  { value: 'config', label: '配置' },
  { value: 'delete', label: '删除' }
]
const SCOPE_OPTIONS = [
  { label: '全部', value: 'all' },
  { label: '指定分组', value: 'groups' },
  { label: '指定通道', value: 'channels' }
]

interface ModPerm { actions: string[]; scope: string; groups: string[]; channels: string[] }
function emptyPerm(): ModPerm { return { actions: [], scope: 'all', groups: [], channels: [] } }
function emptyMatrix(): Record<string, ModPerm> {
  const m: Record<string, ModPerm> = {}
  MODULES.forEach((mod) => { m[mod.value] = emptyPerm() })
  return m
}

// 兼容旧数据：无 matrix 时把 menus+actions 平铺到各模块
function matrixFromRole(row: any): Record<string, ModPerm> {
  const m = emptyMatrix()
  if (row?.perms?.matrix && typeof row.perms.matrix === 'object') {
    MODULES.forEach((mod) => {
      const p = row.perms.matrix[mod.value]
      if (p) m[mod.value] = {
        actions: [...(p.actions || [])],
        scope: p.scope || 'all',
        groups: [...(p.groups || [])],
        channels: [...(p.channels || [])]
      }
    })
    return m
  }
  const menus: string[] = row?.perms?.menus || []
  const actions: string[] = row?.perms?.actions || []
  menus.forEach((menu) => { if (m[menu]) m[menu].actions = [...actions] })
  return m
}

function permsFromMatrix(matrix: Record<string, ModPerm>) {
  const menus = MODULES.filter((mod) => matrix[mod.value].actions.length > 0).map((mod) => mod.value)
  const actionSet = new Set<string>()
  const groups: string[] = []
  const channels: string[] = []
  MODULES.forEach((mod) => {
    const p = matrix[mod.value]
    p.actions.forEach((a) => actionSet.add(a))
    if (p.scope === 'groups') groups.push(...p.groups)
    if (p.scope === 'channels') channels.push(...p.channels)
  })
  return {
    perms: { menus, actions: [...actionSet], matrix: JSON.parse(JSON.stringify(matrix)) },
    scope: { groups: [...new Set(groups)], channels: [...new Set(channels)] }
  }
}

function permSummary(row: any): string {
  const menus: string[] = row?.perms?.menus || []
  const actions: string[] = row?.perms?.actions || []
  if (!menus.length && !actions.length) return '—'
  const menuLabels = menus.map((v) => MODULES.find((m) => m.value === v)?.label || v)
  const actionLabels = actions.map((v) => ACTION_OPTIONS.find((a) => a.value === v)?.label || v)
  return [menuLabels.join('、') || '无菜单', actionLabels.join('/') || '无操作'].join(' · ')
}

/* ---------------- 角色 ---------------- */
const roles = ref<any[]>([])
const rolesLoading = ref(false)

async function loadRoles() {
  rolesLoading.value = true
  try {
    const res: any = await api.get('/roles')
    roles.value = res?.items || []
  } catch (e: any) {
    toastApiError(e, '加载角色失败')
  } finally {
    rolesLoading.value = false
  }
}

/* 成员数（来自成员列表按 roleId 统计） */
const memberCountMap = computed(() => {
  const m: Record<string, number> = {}
  users.value.forEach((u: any) => {
    if (u.roleId) m[u.roleId] = (m[u.roleId] || 0) + 1
  })
  return m
})

/* 新建/编辑角色对话框（两步：基本信息 -> 选择权限） */
const roleDlg = reactive({
  visible: false,
  mode: 'create' as 'create' | 'edit',
  id: null as any,
  name: '',
  copyFrom: 'none' as any,
  step: 0,
  matrix: emptyMatrix() as Record<string, ModPerm>,
  saving: false
})
const selMod = ref('dashboard')

function openRoleDlg(mode: 'create' | 'edit', row?: any) {
  roleDlg.mode = mode
  roleDlg.step = 0
  selMod.value = MODULES[0].value
  if (mode === 'edit') {
    roleDlg.id = row.id
    roleDlg.name = row.name
    roleDlg.copyFrom = 'none'
    roleDlg.matrix = matrixFromRole(row)
  } else {
    roleDlg.id = null
    roleDlg.name = ''
    roleDlg.copyFrom = 'none'
    roleDlg.matrix = emptyMatrix()
  }
  roleDlg.visible = true
}

// "复制自"：选择已有角色后预填权限矩阵
watch(
  () => roleDlg.copyFrom,
  (id) => {
    if (roleDlg.mode !== 'create' || !id || id === 'none') return
    const src = roles.value.find((r) => r.id === id)
    if (src) roleDlg.matrix = matrixFromRole(src)
  }
)

// 操作列"复制"：以已有角色为模板新建
function copyRole(row: any) {
  openRoleDlg('create')
  roleDlg.name = row.name + '（副本）'
  roleDlg.copyFrom = row.id
}

function nextStep() {
  if (!roleDlg.name.trim()) return toast.warning('请输入角色名称')
  roleDlg.step = 1
}

function toggleAction(mod: string, act: string, v: boolean) {
  const p = roleDlg.matrix[mod]
  if (v) { if (!p.actions.includes(act)) p.actions.push(act) }
  else p.actions = p.actions.filter((a) => a !== act)
}

async function saveRole() {
  if (!roleDlg.name.trim()) return toast.warning('请输入角色名称')
  roleDlg.saving = true
  try {
    const { perms, scope } = permsFromMatrix(roleDlg.matrix)
    if (roleDlg.mode === 'create') {
      await api.post('/roles', {
        name: roleDlg.name.trim(),
        perms,
        scope,
        copyFrom: roleDlg.copyFrom && roleDlg.copyFrom !== 'none' ? roleDlg.copyFrom : undefined
      })
      toast.success('角色已创建')
    } else {
      await api.put('/roles/' + roleDlg.id, { name: roleDlg.name.trim(), perms, scope })
      toast.success('角色已更新')
    }
    roleDlg.visible = false
    await loadRoles()
  } catch (e: any) {
    toastApiError(e, '保存失败')
  } finally {
    roleDlg.saving = false
  }
}

async function removeRole(row: any) {
  const ok = await confirm.ask({
    title: '删除角色',
    message: '确定删除角色「' + row.name + '」？',
    detail: '角色已被成员使用时无法删除。',
    danger: true,
    confirmText: '删除'
  })
  if (!ok) return
  try {
    await api.del('/roles/' + row.id)
    toast.success('已删除')
    await loadRoles()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

/* ---------------- 资源范围选择（分组 / 通道） ---------------- */
const scopeDlg = reactive({
  visible: false,
  kind: 'groups' as 'groups' | 'channels',
  loading: false,
  checked: [] as string[]
})
const allGroups = ref<any[]>([])
const allChannels = ref<any[]>([])
const allDevices = ref<any[]>([])

async function ensureScopeData() {
  if (allGroups.value.length && (allChannels.value.length || scopeDlg.kind === 'groups')) return
  try {
    if (!allGroups.value.length) {
      const res: any = await api.get('/groups', { projectId: currentProject.value?.id })
      allGroups.value = res?.items || []
    }
    if (scopeDlg.kind === 'channels' && !allChannels.value.length) {
      const [c, d] = await Promise.all([
        api.get('/channels'),
        api.get('/devices', { pageSize: 200 })
      ])
      allChannels.value = (c as any)?.items || []
      allDevices.value = (d as any)?.items || []
    }
  } catch (e: any) {
    toastApiError(e, '加载资源列表失败')
  }
}

function openScopeDlg(kind: 'groups' | 'channels') {
  scopeDlg.kind = kind
  const p = roleDlg.matrix[selMod.value]
  scopeDlg.checked = [...(kind === 'groups' ? p.groups : p.channels)]
  scopeDlg.visible = true
  scopeDlg.loading = true
  ensureScopeData().finally(() => { scopeDlg.loading = false })
}

function buildGroupTree() {
  const map = new Map<any, any>()
  allGroups.value.forEach((g) => map.set(g.id, { label: g.name, value: String(g.id), children: [] }))
  const roots: any[] = []
  allGroups.value.forEach((g) => {
    const node = map.get(g.id)
    const parent = g.parentId ? map.get(g.parentId) : null
    if (parent) parent.children.push(node)
    else roots.push(node)
  })
  return roots
}
function buildChannelTree() {
  const byDev = new Map<string, any[]>()
  allChannels.value.forEach((c) => {
    const arr = byDev.get(c.deviceId) || []
    arr.push({ label: c.name || ('通道 ' + c.idx), value: String(c.id) })
    byDev.set(c.deviceId, arr)
  })
  return allDevices.value
    .filter((d) => (byDev.get(d.id) || []).length)
    .map((d) => ({ label: d.name || d.id, value: 'dev_' + d.id, children: byDev.get(d.id) }))
}
const scopeTreeNodes = computed(() =>
  scopeDlg.kind === 'groups' ? buildGroupTree() : buildChannelTree()
)

function toggleScopeCheck(id: string, v: boolean) {
  if (v) { if (!scopeDlg.checked.includes(id)) scopeDlg.checked.push(id) }
  else scopeDlg.checked = scopeDlg.checked.filter((x) => x !== id)
}
function confirmScopeDlg() {
  const p = roleDlg.matrix[selMod.value]
  if (scopeDlg.kind === 'groups') p.groups = [...scopeDlg.checked]
  else p.channels = [...scopeDlg.checked]
  scopeDlg.visible = false
}

/* ---------------- 成员 ---------------- */
const users = ref<any[]>([])
const usersLoading = ref(false)

async function loadUsers() {
  usersLoading.value = true
  try {
    const res: any = await api.get('/users')
    users.value = res?.items || []
  } catch (e: any) {
    toastApiError(e, '加载成员失败')
  } finally {
    usersLoading.value = false
  }
}

const roleOptions = computed(() => roles.value.map((r) => ({ label: r.name, value: r.id })))
const isSuper = (row: any) => row.roleId === 'role_super'

/* 表格内联修改角色（超级管理员不可降权） */
async function changeRole(row: any, roleId: any) {
  if (isSuper(row)) return
  try {
    await api.put('/users/' + row.id, { roleId })
    row.roleId = roleId
    toast.success('角色已更新')
  } catch (e: any) {
    toastApiError(e, '修改失败')
    await loadUsers()
  }
}

/* 启用/停用成员 */
async function toggleUser(row: any, v: boolean) {
  const status = v ? 'active' : 'disabled'
  try {
    await api.put('/users/' + row.id, { status })
    row.status = status
    toast.success(v ? '已启用' : '已停用')
  } catch (e: any) {
    toastApiError(e, '操作失败')
    await loadUsers()
  }
}

/* 重置密码：先确认，再输入新密码（≥8 位） */
const pwdDlg = reactive({ visible: false, user: null as any, password: '', saving: false })

async function openPwdDlg(row: any) {
  const ok = await confirm.ask({
    title: '重置密码',
    message: '确定重置成员「' + (row.name || row.username) + '」的登录密码？',
    detail: '重置后需将新密码告知该成员。',
    confirmText: '继续'
  })
  if (!ok) return
  pwdDlg.user = row
  pwdDlg.password = ''
  pwdDlg.visible = true
}

async function savePwd() {
  if (pwdDlg.password.length < 8) return toast.warning('新密码至少 8 位')
  pwdDlg.saving = true
  try {
    await api.post('/users/' + pwdDlg.user.id + '/reset-password', { password: pwdDlg.password })
    toast.success('密码已重置')
    pwdDlg.visible = false
  } catch (e: any) {
    toastApiError(e, '重置失败')
  } finally {
    pwdDlg.saving = false
  }
}

/* 删除成员（超级管理员不可删除） */
async function removeUser(row: any) {
  const ok = await confirm.ask({
    title: '删除成员',
    message: '确定删除成员「' + (row.name || row.username) + '」？',
    danger: true,
    confirmText: '删除'
  })
  if (!ok) return
  try {
    await api.del('/users/' + row.id)
    toast.success('已删除')
    await loadUsers()
  } catch (e: any) {
    toastApiError(e, '删除失败')
  }
}

/* 新建成员 */
const userDlg = reactive({
  visible: false,
  username: '',
  name: '',
  contact: '',
  password: '',
  roleId: '' as any,
  saving: false
})

function openUserDlg() {
  userDlg.username = ''
  userDlg.name = ''
  userDlg.contact = ''
  userDlg.password = ''
  userDlg.roleId = roles.value[0]?.id ?? ''
  userDlg.visible = true
}

async function saveUser() {
  if (!userDlg.username.trim() || !userDlg.password) return toast.warning('请填写用户名与初始密码')
  if (userDlg.password.length < 8) return toast.warning('初始密码至少 8 位')
  if (!userDlg.roleId) return toast.warning('请选择角色')
  userDlg.saving = true
  try {
    await api.post('/users', {
      username: userDlg.username.trim(),
      name: userDlg.name.trim(),
      contact: userDlg.contact.trim(),
      password: userDlg.password,
      roleId: userDlg.roleId
    })
    toast.success('成员已创建')
    userDlg.visible = false
    await loadUsers()
  } catch (e: any) {
    toastApiError(e, '创建失败')
  } finally {
    userDlg.saving = false
  }
}

const fmtTime = (ts: any) =>
  ts ? new Date(typeof ts === 'string' ? Date.parse(ts) : ts).toLocaleString() : '-'

const roleCols = [
  { key: 'name', label: '名称', width: '180px' },
  { key: 'builtin', label: '内置', width: '90px', align: 'center' as const },
  { key: 'perms', label: '权限摘要', width: '220px' },
  { key: 'members', label: '成员数', width: '90px', align: 'center' as const },
  { key: 'ops', label: '操作', width: '170px', align: 'center' as const, fixed: true }
]
const userCols = [
  { key: 'username', label: '用户名', width: '140px' },
  { key: 'name', label: '姓名', width: '110px' },
  { key: 'contact', label: '联系方式', width: '150px' },
  { key: 'roleId', label: '角色', width: '170px' },
  { key: 'status', label: '状态', width: '90px', align: 'center' as const },
  { key: 'createdAt', label: '创建时间', width: '170px' },
  { key: 'ops', label: '操作', width: '160px', align: 'center' as const, fixed: true }
]

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await Promise.all([loadRoles(), loadUsers()])
})
</script>

<template>
  <div class="space-y-3">
    <UiCard flat body-class="p-0">
      <UiTabs v-model="activeTab" :items="[{ label: '角色', value: 'roles' }, { label: '成员', value: 'users' }]">
        <div class="p-4">
          <!-- 角色管理 -->
          <div v-if="activeTab === 'roles'">
            <div class="mb-3 flex flex-wrap items-center gap-2">
              <UiButton variant="primary" @click="openRoleDlg('create')">
                <UiIcon name="plus" :size="14" />新建角色
              </UiButton>
              <UiButton :disabled="rolesLoading" @click="loadRoles">
                <UiIcon name="refresh" :size="14" />刷新
              </UiButton>
            </div>
            <UiTable :columns="roleCols" :rows="roles" :loading="rolesLoading" empty="暂无角色">
              <template #builtin="{ row }">
                <UiTag v-if="row.builtin" color="warning">内置</UiTag>
                <UiTag v-else color="default">自定义</UiTag>
              </template>
              <template #perms="{ row }">
                <span class="block truncate" :title="permSummary(row)">{{ permSummary(row) }}</span>
              </template>
              <template #members="{ row }">{{ memberCountMap[row.id] || 0 }}</template>
              <template #ops="{ row }">
                <span class="inline-flex items-center gap-1">
                  <UiTooltip v-if="row.builtin" label="内置角色不可修改或删除">
                    <span><UiButton variant="text" size="sm" disabled>编辑</UiButton></span>
                  </UiTooltip>
                  <UiButton v-else variant="text" size="sm" @click="openRoleDlg('edit', row)">编辑</UiButton>
                  <UiButton variant="text" size="sm" @click="copyRole(row)">复制</UiButton>
                  <UiButton v-if="!row.builtin" variant="dangerText" size="sm" @click="removeRole(row)">删除</UiButton>
                  <UiTooltip v-else label="内置角色不可删除">
                    <span><UiButton variant="dangerText" size="sm" disabled>删除</UiButton></span>
                  </UiTooltip>
                </span>
              </template>
              <template #empty-action>
                <UiButton variant="primary" @click="openRoleDlg('create')">新建角色</UiButton>
              </template>
            </UiTable>
          </div>

          <!-- 成员管理 -->
          <div v-else>
            <div class="mb-3 flex flex-wrap items-center gap-2">
              <UiButton variant="primary" @click="openUserDlg">
                <UiIcon name="plus" :size="14" />添加成员
              </UiButton>
              <UiButton :disabled="usersLoading" @click="loadUsers">
                <UiIcon name="refresh" :size="14" />刷新
              </UiButton>
            </div>
            <UiTable :columns="userCols" :rows="users" :loading="usersLoading" empty="暂无成员">
              <template #name="{ row }">{{ row.name || '-' }}</template>
              <template #contact="{ row }">{{ row.contact || '-' }}</template>
              <template #roleId="{ row }">
                <UiTooltip v-if="isSuper(row)" label="超级管理员不可降权">
                  <span><UiSelect :model-value="row.roleId" :options="roleOptions" disabled width="w-40" size="sm" /></span>
                </UiTooltip>
                <UiSelect
                  v-else :model-value="row.roleId" :options="roleOptions" placeholder="未分配"
                  width="w-40" size="sm" @update:model-value="(v: any) => changeRole(row, v)"
                />
              </template>
              <template #status="{ row }">
                <UiSwitch
                  :model-value="row.status === 'active'" size="sm" :disabled="isSuper(row)"
                  @update:model-value="(v: boolean) => toggleUser(row, v)"
                />
              </template>
              <template #createdAt="{ row }">{{ fmtTime(row.createdAt) }}</template>
              <template #ops="{ row }">
                <span class="inline-flex items-center gap-1">
                  <UiButton variant="text" size="sm" @click="openPwdDlg(row)">重置密码</UiButton>
                  <UiTooltip v-if="isSuper(row)" label="超级管理员不可删除">
                    <span><UiButton variant="dangerText" size="sm" disabled>删除</UiButton></span>
                  </UiTooltip>
                  <UiButton v-else variant="dangerText" size="sm" @click="removeUser(row)">删除</UiButton>
                </span>
              </template>
              <template #empty-action>
                <UiButton variant="primary" @click="openUserDlg">添加成员</UiButton>
              </template>
            </UiTable>
          </div>
        </div>
      </UiTabs>
    </UiCard>

    <!-- 角色新建/编辑对话框（A17 权限矩阵：基本信息 -> 选择权限） -->
    <UiDialog
      v-model:open="roleDlg.visible"
      :title="roleDlg.mode === 'create' ? '新建角色' : '编辑角色'"
      :width="roleDlg.step === 0 ? 'max-w-md' : 'max-w-3xl'"
    >
      <UiSteps :steps="['基本信息', '选择权限']" :current="roleDlg.step" class="mb-4" />

      <!-- 第一步：基本信息 -->
      <div v-if="roleDlg.step === 0" class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>角色名称</span>
        <UiInput v-model="roleDlg.name" placeholder="请输入角色名称" :maxlength="50" @enter="nextStep" />
        <template v-if="roleDlg.mode === 'create'">
          <span class="text-right text-sm text-body">复制自</span>
          <UiSelect
            v-model="roleDlg.copyFrom" :options="[{ label: '（不复制）', value: 'none' }, ...roleOptions]"
            placeholder="可选：从已有角色复制权限"
          />
        </template>
      </div>

      <!-- 第二步：权限矩阵（左模块 / 右操作权限+资源范围） -->
      <div v-else class="grid grid-cols-[180px_1fr] gap-4">
        <div class="border-r border-line-soft pr-2">
          <p class="mb-1.5 px-1 text-xs text-placeholder">模块</p>
          <UiTree :nodes="MODULES.map((m) => ({ label: m.label, value: m.value }))" :selected="selMod" @select="(n: any) => (selMod = n.value)" />
        </div>
        <div class="min-w-0">
          <p class="mb-2 text-sm font-semibold text-ink">{{ MODULES.find((m) => m.value === selMod)?.label }}</p>
          <div class="mb-4">
            <p class="mb-1.5 text-xs text-muted">操作权限</p>
            <div class="flex flex-wrap gap-x-5 gap-y-2">
              <UiCheckbox
                v-for="a in ACTION_OPTIONS" :key="a.value"
                :model-value="roleDlg.matrix[selMod].actions.includes(a.value)"
                :label="a.label"
                @update:model-value="(v: boolean) => toggleAction(selMod, a.value, v)"
              />
            </div>
          </div>
          <div>
            <p class="mb-1.5 text-xs text-muted">资源范围</p>
            <div class="flex flex-wrap items-center gap-3">
              <UiSegmented v-model="roleDlg.matrix[selMod].scope" :items="SCOPE_OPTIONS" />
              <UiButton
                v-if="roleDlg.matrix[selMod].scope === 'groups'" variant="default" size="sm"
                @click="openScopeDlg('groups')"
              >
                <UiIcon name="folder" :size="13" />选择分组（{{ roleDlg.matrix[selMod].groups.length }}）
              </UiButton>
              <UiButton
                v-if="roleDlg.matrix[selMod].scope === 'channels'" variant="default" size="sm"
                @click="openScopeDlg('channels')"
              >
                <UiIcon name="video" :size="13" />选择通道（{{ roleDlg.matrix[selMod].channels.length }}）
              </UiButton>
              <span v-if="roleDlg.matrix[selMod].scope !== 'all'" class="text-xs text-placeholder">
                未选择时该模块仅对空范围生效
              </span>
            </div>
          </div>
          <p class="mt-4 border-t border-line-soft pt-2 text-xs text-placeholder">
            提示：勾选某模块的操作权限即授予该模块；资源范围控制可见的设备分组/通道。
          </p>
        </div>
      </div>

      <template #footer>
        <UiButton @click="roleDlg.visible = false">取消</UiButton>
        <UiButton v-if="roleDlg.step === 1" @click="roleDlg.step = 0">上一步</UiButton>
        <UiButton v-if="roleDlg.step === 0" variant="primary" @click="nextStep">下一步</UiButton>
        <UiButton v-else variant="primary" :disabled="roleDlg.saving" @click="saveRole">
          {{ roleDlg.saving ? '保存中…' : '保存' }}
        </UiButton>
      </template>
    </UiDialog>

    <!-- 资源范围选择（分组/通道树多选） -->
    <UiDialog
      v-model:open="scopeDlg.visible"
      :title="scopeDlg.kind === 'groups' ? '选择分组' : '选择通道'"
      width="max-w-md"
    >
      <UiLoading :loading="scopeDlg.loading">
        <div class="max-h-[50vh] overflow-y-auto">
          <UiTree :nodes="scopeTreeNodes">
            <template #node="{ node }">
              <UiCheckbox
                :model-value="scopeDlg.checked.includes(node.value)"
                :label="node.label"
                @update:model-value="(v: boolean) => toggleScopeCheck(node.value, v)"
              />
            </template>
          </UiTree>
          <UiEmptyState v-if="!scopeDlg.loading && !scopeTreeNodes.length" :text="scopeDlg.kind === 'groups' ? '暂无分组' : '暂无通道'" />
        </div>
      </UiLoading>
      <template #footer>
        <span class="mr-auto text-xs text-placeholder">已选 {{ scopeDlg.checked.length }} 项</span>
        <UiButton @click="scopeDlg.visible = false">取消</UiButton>
        <UiButton variant="primary" @click="confirmScopeDlg">确定</UiButton>
      </template>
    </UiDialog>

    <!-- 新建成员对话框 -->
    <UiDialog v-model:open="userDlg.visible" title="添加成员" width="max-w-md">
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>用户名</span>
        <UiInput v-model="userDlg.username" placeholder="登录用户名" :maxlength="50" />
        <span class="text-right text-sm text-body">姓名</span>
        <UiInput v-model="userDlg.name" placeholder="姓名" :maxlength="50" />
        <span class="text-right text-sm text-body">手机/邮箱</span>
        <UiInput v-model="userDlg.contact" placeholder="手机号 / 邮箱" :maxlength="100" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>初始密码</span>
        <UiInput v-model="userDlg.password" type="password" placeholder="至少 8 位" :maxlength="64" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>角色</span>
        <UiSelect v-model="userDlg.roleId" :options="roleOptions" placeholder="选择角色" />
      </div>
      <template #footer>
        <UiButton @click="userDlg.visible = false">取消</UiButton>
        <UiButton variant="primary" :disabled="userDlg.saving" @click="saveUser">{{ userDlg.saving ? '保存中…' : '确定' }}</UiButton>
      </template>
    </UiDialog>

    <!-- 重置密码对话框 -->
    <UiDialog
      v-model:open="pwdDlg.visible"
      :title="'重置密码 - ' + (pwdDlg.user?.name || pwdDlg.user?.username || '')"
      width="max-w-md"
    >
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>新密码</span>
        <UiInput v-model="pwdDlg.password" type="password" placeholder="至少 8 位" :maxlength="64" @enter="savePwd" />
      </div>
      <template #footer>
        <UiButton @click="pwdDlg.visible = false">取消</UiButton>
        <UiButton variant="primary" :disabled="pwdDlg.saving" @click="savePwd">{{ pwdDlg.saving ? '提交中…' : '确定' }}</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
