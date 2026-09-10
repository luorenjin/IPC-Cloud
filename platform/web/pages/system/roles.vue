<script setup lang="ts">
// 角色与成员管理（ACC-05/06/07）：角色权限矩阵（A17）+ 项目成员管理
const api = useApi()
const toast = useToast()
const { t } = useI18n()
const confirm = useConfirm()
const { currentProject, loadMe } = useAuth()

const activeTab = ref('roles')

/* ---------------- 权限模型 ---------------- */
// 模块清单（左栏）
// label 需与侧栏菜单文案保持一致（管理员在此授权，看到的名称必须与导航对得上）；
// value 是落库的权限键（Role.Perms["menus"]），任何情况下都不得改动。
const MODULES = computed(() => [
  { value: 'dashboard', label: t('system.roles.modDashboard') },
  { value: 'devices', label: t('system.roles.modDevices') },
  { value: 'live', label: t('system.roles.modLive') },
  { value: 'playback', label: t('system.roles.modPlayback') },
  { value: 'alarms', label: t('system.roles.modAlarms') },
  { value: 'record', label: t('system.roles.modRecord') },
  { value: 'system', label: t('system.roles.modSystem') }
])
// 操作权限（右栏）
const ACTION_OPTIONS = computed(() => [
  { value: 'view', label: t('system.roles.actView') },
  { value: 'preview', label: t('system.roles.actPreview') },
  { value: 'playback', label: t('system.roles.actPlayback') },
  { value: 'ptz', label: t('system.roles.actPtz') },
  { value: 'config', label: t('system.roles.actConfig') },
  { value: 'delete', label: t('system.roles.actDelete') }
])
const SCOPE_OPTIONS = computed(() => [
  { label: t('system.roles.scopeAll'), value: 'all' },
  { label: t('system.roles.scopeGroups'), value: 'groups' },
  { label: t('system.roles.scopeChannels'), value: 'channels' }
])

interface ModPerm { actions: string[]; scope: string; groups: string[]; channels: string[] }
function emptyPerm(): ModPerm { return { actions: [], scope: 'all', groups: [], channels: [] } }
function emptyMatrix(): Record<string, ModPerm> {
  const m: Record<string, ModPerm> = {}
  MODULES.value.forEach((mod) => { m[mod.value] = emptyPerm() })
  return m
}

// 兼容旧数据：无 matrix 时把 menus+actions 平铺到各模块
function matrixFromRole(row: any): Record<string, ModPerm> {
  const m = emptyMatrix()
  if (row?.perms?.matrix && typeof row.perms.matrix === 'object') {
    MODULES.value.forEach((mod) => {
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
  const menus = MODULES.value.filter((mod) => matrix[mod.value].actions.length > 0).map((mod) => mod.value)
  const actionSet = new Set<string>()
  const groups: string[] = []
  const channels: string[] = []
  MODULES.value.forEach((mod) => {
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
  const menuLabels = menus.map((v) => MODULES.value.find((m) => m.value === v)?.label || v)
  const actionLabels = actions.map((v) => ACTION_OPTIONS.value.find((a) => a.value === v)?.label || v)
  return [menuLabels.join(t('system.roles.listSep')) || t('system.roles.noMenu'), actionLabels.join('/') || t('system.roles.noAction')].join(' · ')
}

/* 权限摘要改为按模块分 chip 展示（纯展示，底层数据仍取自 perms.menus/matrix） */
function permChips(row: any): { label: string; count: number }[] {
  const menus: string[] = row?.perms?.menus || []
  return menus.map((v) => ({
    label: MODULES.value.find((m) => m.value === v)?.label || v,
    count: (row?.perms?.matrix?.[v]?.actions || []).length
  }))
}

/* 模块图标（纯展示） */
const MODULE_ICONS: Record<string, string> = {
  dashboard: 'gauge', devices: 'camera', live: 'monitor', playback: 'film',
  alarms: 'bell', record: 'clipboard', system: 'settings'
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
    toastApiError(e, t('system.msg.rolesLoadFailed'))
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
  selMod.value = MODULES.value[0].value
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
  roleDlg.name = row.name + t('system.roles.copySuffix')
  roleDlg.copyFrom = row.id
}

function nextStep() {
  if (!roleDlg.name.trim()) return toast.warning(t('system.msg.needRoleName'))
  roleDlg.step = 1
}

function toggleAction(mod: string, act: string, v: boolean) {
  const p = roleDlg.matrix[mod]
  if (v) { if (!p.actions.includes(act)) p.actions.push(act) }
  else p.actions = p.actions.filter((a) => a !== act)
}

async function saveRole() {
  if (!roleDlg.name.trim()) return toast.warning(t('system.msg.needRoleName'))
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
      toast.success(t('system.msg.roleCreated'))
    } else {
      await api.put('/roles/' + roleDlg.id, { name: roleDlg.name.trim(), perms, scope })
      toast.success(t('system.msg.roleUpdated'))
    }
    roleDlg.visible = false
    await loadRoles()
  } catch (e: any) {
    toastApiError(e, t('common.saveFailed'))
  } finally {
    roleDlg.saving = false
  }
}

async function removeRole(row: any) {
  const ok = await confirm.ask({
    title: t('system.roles.deleteRoleTitle'),
    message: t('system.roles.deleteRoleMsg', { name: row.name }),
    detail: t('system.roles.deleteRoleDetail'),
    danger: true,
    confirmText: t('common.delete')
  })
  if (!ok) return
  try {
    await api.del('/roles/' + row.id)
    toast.success(t('common.deletedOk'))
    await loadRoles()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
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
    toastApiError(e, t('system.msg.scopeLoadFailed'))
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

/* 矩阵改版后每行都可直接打开资源范围弹窗：先定位目标模块，再复用原有 openScopeDlg */
function openModuleScope(mod: string, kind: 'groups' | 'channels') {
  selMod.value = mod
  openScopeDlg(kind)
}

/* 权限矩阵列定义：模块（行头）+ 各操作权限（纯展示） */
const MATRIX_COLS = computed(() => [
  { key: 'module', label: t('system.roles.matrixModule'), width: '128px', ellipsis: false },
  ...ACTION_OPTIONS.value.map((a) => ({ key: a.value, label: a.label, width: '64px', align: 'center' as const, ellipsis: false })),
  { key: 'scope', label: t('system.roles.matrixScope'), ellipsis: false }
])

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
    arr.push({ label: c.name || t('system.roles.channelIdx', { n: c.idx }), value: String(c.id) })
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
    toastApiError(e, t('system.msg.usersLoadFailed'))
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
    toast.success(t('system.msg.roleUpdated'))
  } catch (e: any) {
    toastApiError(e, t('system.msg.updateFailed'))
    await loadUsers()
  }
}

/* 启用/停用成员 */
async function toggleUser(row: any, v: boolean) {
  const status = v ? 'active' : 'disabled'
  try {
    await api.put('/users/' + row.id, { status })
    row.status = status
    toast.success(v ? t('system.msg.userEnabled') : t('system.msg.userDisabled'))
  } catch (e: any) {
    toastApiError(e, t('system.msg.opFailed'))
    await loadUsers()
  }
}

/* 重置密码：先确认，再输入新密码（≥8 位） */
const pwdDlg = reactive({ visible: false, user: null as any, password: '', saving: false })

async function openPwdDlg(row: any) {
  const ok = await confirm.ask({
    title: t('system.members.resetPwdTitle'),
    message: t('system.members.resetPwdMsg', { name: row.name || row.username }),
    detail: t('system.members.resetPwdDetail'),
    confirmText: t('system.members.resetPwdContinue')
  })
  if (!ok) return
  pwdDlg.user = row
  pwdDlg.password = ''
  pwdDlg.visible = true
}

async function savePwd() {
  if (pwdDlg.password.length < 8) return toast.warning(t('system.msg.pwdTooShort'))
  pwdDlg.saving = true
  try {
    await api.post('/users/' + pwdDlg.user.id + '/reset-password', { password: pwdDlg.password })
    toast.success(t('system.msg.pwdReset'))
    pwdDlg.visible = false
  } catch (e: any) {
    toastApiError(e, t('system.msg.pwdResetFailed'))
  } finally {
    pwdDlg.saving = false
  }
}

/* 删除成员（超级管理员不可删除） */
async function removeUser(row: any) {
  const ok = await confirm.ask({
    title: t('system.members.deleteTitle'),
    message: t('system.members.deleteMsg', { name: row.name || row.username }),
    danger: true,
    confirmText: t('common.delete')
  })
  if (!ok) return
  try {
    await api.del('/users/' + row.id)
    toast.success(t('common.deletedOk'))
    await loadUsers()
  } catch (e: any) {
    toastApiError(e, t('common.deleteFailed'))
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
  if (!userDlg.username.trim() || !userDlg.password) return toast.warning(t('system.msg.needUserAndPwd'))
  if (userDlg.password.length < 8) return toast.warning(t('system.msg.initPwdTooShort'))
  if (!userDlg.roleId) return toast.warning(t('system.msg.needRole'))
  userDlg.saving = true
  try {
    await api.post('/users', {
      username: userDlg.username.trim(),
      name: userDlg.name.trim(),
      contact: userDlg.contact.trim(),
      password: userDlg.password,
      roleId: userDlg.roleId
    })
    toast.success(t('system.msg.userCreated'))
    userDlg.visible = false
    await loadUsers()
  } catch (e: any) {
    toastApiError(e, t('system.msg.createFailed'))
  } finally {
    userDlg.saving = false
  }
}


const roleCols = computed(() => [
  { key: 'name', label: t('system.roles.colName'), width: '180px' },
  { key: 'builtin', label: t('system.roles.colBuiltin'), width: '90px', align: 'center' as const },
  { key: 'perms', label: t('system.roles.colPerms'), width: '260px', ellipsis: false },
  { key: 'members', label: t('system.roles.colMembers'), width: '90px', align: 'center' as const },
  { key: 'ops', label: t('common.action'), width: '170px', align: 'center' as const, fixed: true }
])
const userCols = computed(() => [
  { key: 'username', label: t('system.members.colUsername'), width: '140px' },
  { key: 'name', label: t('system.members.colName'), width: '110px' },
  { key: 'contact', label: t('system.members.colContact'), width: '150px' },
  { key: 'roleId', label: t('system.members.colRole'), width: '170px' },
  { key: 'status', label: t('common.status'), width: '90px', align: 'center' as const },
  { key: 'createdAt', label: t('system.members.colCreatedAt'), width: '170px' },
  { key: 'ops', label: t('common.action'), width: '160px', align: 'center' as const, fixed: true }
])

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await Promise.all([loadRoles(), loadUsers()])
})

/* 客户端分页（E7）：角色/成员接口一次性返回全部数据，此前全量渲染。
   服务端分页需后端配合，属后续工作。 */
const { page: rolePage, pageSize: roleSize, total: roleTotal, pageItems: roleItems } = useClientPage(roles)
const { page: userPage, pageSize: userSize, total: userTotal, pageItems: userItems } = useClientPage(users)
</script>

<template>
  <div class="space-y-3">
    <UiCard flat body-class="p-0">
      <UiTabs v-model="activeTab" :items="[{ label: t('system.roles.tabRoles'), value: 'roles' }, { label: t('system.roles.tabMembers'), value: 'users' }]">
        <div class="p-4">
          <!-- 角色管理 -->
          <div v-if="activeTab === 'roles'">
            <div class="mb-3 flex flex-wrap items-center gap-2">
              <UiButton variant="primary" @click="openRoleDlg('create')">
                <UiIcon name="plus" :size="14" />{{ t('system.roles.createRole') }}
              </UiButton>
              <UiButton :disabled="rolesLoading" @click="loadRoles">
                <UiIcon name="refresh" :size="14" />{{ t('common.refresh') }}
              </UiButton>
            </div>
            <UiTable :columns="roleCols" :rows="roleItems" :loading="rolesLoading" :empty="t('system.roles.emptyRoles')">
              <template #builtin="{ row }">
                <UiTag v-if="row.builtin" color="warning">{{ t('system.roles.builtin') }}</UiTag>
                <UiTag v-else color="default">{{ t('system.roles.custom') }}</UiTag>
              </template>
              <template #perms="{ row }">
                <div v-if="permChips(row).length" class="flex flex-wrap gap-1" :title="permSummary(row)">
                  <UiTag v-for="c in permChips(row)" :key="c.label">{{ c.label }}<span class="text-placeholder">·{{ c.count }}</span></UiTag>
                </div>
                <span v-else class="text-placeholder">—</span>
              </template>
              <template #members="{ row }">{{ memberCountMap[row.id] || 0 }}</template>
              <template #ops="{ row }">
                <span class="inline-flex items-center gap-1">
                  <UiTooltip v-if="row.builtin" :label="t('system.roles.builtinNoEdit')">
                    <span><UiButton variant="text" size="sm" disabled>{{ t('common.edit') }}</UiButton></span>
                  </UiTooltip>
                  <UiButton v-else variant="text" size="sm" @click="openRoleDlg('edit', row)">{{ t('common.edit') }}</UiButton>
                  <UiButton variant="text" size="sm" @click="copyRole(row)">{{ t('system.roles.copy') }}</UiButton>
                  <UiButton v-if="!row.builtin" variant="dangerText" size="sm" @click="removeRole(row)">{{ t('common.delete') }}</UiButton>
                  <UiTooltip v-else :label="t('system.roles.builtinNoDelete')">
                    <span><UiButton variant="dangerText" size="sm" disabled>{{ t('common.delete') }}</UiButton></span>
                  </UiTooltip>
                </span>
              </template>
              <template #empty-action>
                <UiButton variant="primary" @click="openRoleDlg('create')">{{ t('system.roles.createRole') }}</UiButton>
              </template>
            </UiTable>
            <div v-if="roleTotal > roleSize" class="mt-3 flex justify-end">
              <UiPagination v-model:page="rolePage" v-model:page-size="roleSize" :total="roleTotal" />
            </div>
          </div>

          <!-- 成员管理 -->
          <div v-else>
            <div class="mb-3 flex flex-wrap items-center gap-2">
              <UiButton variant="primary" @click="openUserDlg">
                <UiIcon name="plus" :size="14" />{{ t('system.members.add') }}
              </UiButton>
              <UiButton :disabled="usersLoading" @click="loadUsers">
                <UiIcon name="refresh" :size="14" />{{ t('common.refresh') }}
              </UiButton>
            </div>
            <UiTable :columns="userCols" :rows="userItems" :loading="usersLoading" :empty="t('system.members.empty')">
              <template #name="{ row }">{{ row.name || '-' }}</template>
              <template #contact="{ row }">{{ row.contact || '-' }}</template>
              <template #roleId="{ row }">
                <UiTooltip v-if="isSuper(row)" :label="t('system.members.superNoDowngrade')">
                  <span><UiSelect :model-value="row.roleId" :options="roleOptions" disabled width="w-40" size="sm" /></span>
                </UiTooltip>
                <UiSelect
                  v-else :model-value="row.roleId" :options="roleOptions" :placeholder="t('system.members.roleUnassigned')"
                  width="w-40" size="sm" @update:model-value="(v: any) => changeRole(row, v)"
                />
              </template>
              <template #status="{ row }">
                <UiSwitch
                  :model-value="row.status === 'active'" size="sm" :disabled="isSuper(row)"
                  :aria-label="t('system.members.enableAria', { name: row.username || row.name || row.id })"
                  @update:model-value="(v: boolean) => toggleUser(row, v)"
                />
              </template>
              <template #createdAt="{ row }">{{ fmtTime(row.createdAt) }}</template>
              <template #ops="{ row }">
                <span class="inline-flex items-center gap-1">
                  <UiButton variant="text" size="sm" @click="openPwdDlg(row)">{{ t('system.members.resetPwd') }}</UiButton>
                  <UiTooltip v-if="isSuper(row)" :label="t('system.members.superNoDelete')">
                    <span><UiButton variant="dangerText" size="sm" disabled>{{ t('common.delete') }}</UiButton></span>
                  </UiTooltip>
                  <UiButton v-else variant="dangerText" size="sm" @click="removeUser(row)">{{ t('common.delete') }}</UiButton>
                </span>
              </template>
              <template #empty-action>
                <UiButton variant="primary" @click="openUserDlg">{{ t('system.members.add') }}</UiButton>
              </template>
            </UiTable>
            <div v-if="userTotal > userSize" class="mt-3 flex justify-end">
              <UiPagination v-model:page="userPage" v-model:page-size="userSize" :total="userTotal" />
            </div>
          </div>
        </div>
      </UiTabs>
    </UiCard>

    <!-- 角色新建/编辑对话框（A17 权限矩阵：基本信息 -> 选择权限） -->
    <UiDialog
      v-model:open="roleDlg.visible"
      :title="roleDlg.mode === 'create' ? t('system.roles.createRole') : t('system.roles.editRole')"
      :width="roleDlg.step === 0 ? 'max-w-md' : 'max-w-4xl'"
    >
      <UiSteps :steps="[t('system.roles.stepBasic'), t('system.roles.stepPerms')]" :current="roleDlg.step" class="mb-4" />

      <!-- 第一步：基本信息 -->
      <div v-if="roleDlg.step === 0" class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('system.roles.roleName') }}</span>
        <UiInput v-model="roleDlg.name" :placeholder="t('system.roles.roleNamePlaceholder')" :maxlength="50" @enter="nextStep" />
        <template v-if="roleDlg.mode === 'create'">
          <span class="text-right text-sm text-body">{{ t('system.roles.copyFrom') }}</span>
          <UiSelect
            v-model="roleDlg.copyFrom" :options="[{ label: t('system.roles.copyFromNone'), value: 'none' }, ...roleOptions]"
            :placeholder="t('system.roles.copyFromPlaceholder')"
          />
        </template>
      </div>

      <!-- 第二步：权限矩阵（行=模块，列=操作权限，末列为该模块的资源范围） -->
      <div v-else>
        <UiTable :columns="MATRIX_COLS" :rows="MODULES" row-key="value" :empty="t('system.roles.noModule')">
          <template #module="{ row }">
            <span class="inline-flex items-center gap-1.5 font-medium text-ink">
              <UiIcon :name="MODULE_ICONS[row.value] || 'square'" :size="14" class="text-muted" />
              {{ row.label }}
            </span>
          </template>
          <template #view="{ row }">
            <UiCheckbox
              :model-value="roleDlg.matrix[row.value].actions.includes('view')"
              @update:model-value="(v: boolean) => toggleAction(row.value, 'view', v)"
            />
          </template>
          <template #preview="{ row }">
            <UiCheckbox
              :model-value="roleDlg.matrix[row.value].actions.includes('preview')"
              @update:model-value="(v: boolean) => toggleAction(row.value, 'preview', v)"
            />
          </template>
          <template #playback="{ row }">
            <UiCheckbox
              :model-value="roleDlg.matrix[row.value].actions.includes('playback')"
              @update:model-value="(v: boolean) => toggleAction(row.value, 'playback', v)"
            />
          </template>
          <template #ptz="{ row }">
            <UiCheckbox
              :model-value="roleDlg.matrix[row.value].actions.includes('ptz')"
              @update:model-value="(v: boolean) => toggleAction(row.value, 'ptz', v)"
            />
          </template>
          <template #config="{ row }">
            <UiCheckbox
              :model-value="roleDlg.matrix[row.value].actions.includes('config')"
              @update:model-value="(v: boolean) => toggleAction(row.value, 'config', v)"
            />
          </template>
          <template #delete="{ row }">
            <UiCheckbox
              :model-value="roleDlg.matrix[row.value].actions.includes('delete')"
              @update:model-value="(v: boolean) => toggleAction(row.value, 'delete', v)"
            />
          </template>
          <template #scope="{ row }">
            <div class="flex flex-wrap items-center gap-2">
              <UiSegmented v-model="roleDlg.matrix[row.value].scope" :items="SCOPE_OPTIONS" />
              <UiButton
                v-if="roleDlg.matrix[row.value].scope === 'groups'" variant="default" size="sm"
                @click="openModuleScope(row.value, 'groups')"
              >
                <UiIcon name="folder" :size="13" />{{ roleDlg.matrix[row.value].groups.length }}
              </UiButton>
              <UiButton
                v-if="roleDlg.matrix[row.value].scope === 'channels'" variant="default" size="sm"
                @click="openModuleScope(row.value, 'channels')"
              >
                <UiIcon name="video" :size="13" />{{ roleDlg.matrix[row.value].channels.length }}
              </UiButton>
            </div>
          </template>
        </UiTable>
        <p class="mt-3 border-t border-line-soft pt-2 text-xs text-placeholder">
          {{ t('system.roles.matrixTip') }}
        </p>
      </div>

      <template #footer>
        <UiButton @click="roleDlg.visible = false">{{ t('common.cancel') }}</UiButton>
        <UiButton v-if="roleDlg.step === 1" @click="roleDlg.step = 0">{{ t('system.roles.prevStep') }}</UiButton>
        <UiButton v-if="roleDlg.step === 0" variant="primary" @click="nextStep">{{ t('system.roles.nextStep') }}</UiButton>
        <UiButton v-else variant="primary" :disabled="roleDlg.saving" @click="saveRole">
          {{ roleDlg.saving ? t('common.saving') : t('common.save') }}
        </UiButton>
      </template>
    </UiDialog>

    <!-- 资源范围选择（分组/通道树多选） -->
    <UiDialog
      v-model:open="scopeDlg.visible"
      :title="scopeDlg.kind === 'groups' ? t('system.roles.pickGroups') : t('system.roles.pickChannels')"
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
          <UiEmptyState v-if="!scopeDlg.loading && !scopeTreeNodes.length" :text="scopeDlg.kind === 'groups' ? t('system.roles.noGroups') : t('system.roles.noChannels')" />
        </div>
      </UiLoading>
      <template #footer>
        <span class="mr-auto text-xs text-placeholder">{{ t('system.roles.selectedCount', { n: scopeDlg.checked.length }) }}</span>
        <UiButton @click="scopeDlg.visible = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" @click="confirmScopeDlg">{{ t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 新建成员对话框 -->
    <UiDialog v-model:open="userDlg.visible" :title="t('system.members.add')" width="max-w-md">
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('system.members.colUsername') }}</span>
        <UiInput v-model="userDlg.username" :placeholder="t('system.members.usernamePlaceholder')" :maxlength="50" />
        <span class="text-right text-sm text-body">{{ t('system.members.colName') }}</span>
        <UiInput v-model="userDlg.name" :placeholder="t('system.members.namePlaceholder')" :maxlength="50" />
        <span class="text-right text-sm text-body">{{ t('system.members.contactLabel') }}</span>
        <UiInput v-model="userDlg.contact" :placeholder="t('system.members.contactPlaceholder')" :maxlength="100" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('system.members.initPassword') }}</span>
        <UiInput v-model="userDlg.password" type="password" :placeholder="t('system.members.pwdPlaceholder')" :maxlength="64" />
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('system.members.colRole') }}</span>
        <UiSelect v-model="userDlg.roleId" :options="roleOptions" :placeholder="t('system.members.pickRole')" />
      </div>
      <template #footer>
        <UiButton @click="userDlg.visible = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="userDlg.saving" @click="saveUser">{{ userDlg.saving ? t('common.saving') : t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>

    <!-- 重置密码对话框 -->
    <UiDialog
      v-model:open="pwdDlg.visible"
      :title="t('system.members.resetPwdDlgTitle', { name: pwdDlg.user?.name || pwdDlg.user?.username || '' })"
      width="max-w-md"
    >
      <div class="grid grid-cols-[100px_1fr] items-center gap-x-3 gap-y-3">
        <span class="text-right text-sm text-body"><span class="text-danger">*</span>{{ t('system.members.newPassword') }}</span>
        <UiInput v-model="pwdDlg.password" type="password" :placeholder="t('system.members.pwdPlaceholder')" :maxlength="64" @enter="savePwd" />
      </div>
      <template #footer>
        <UiButton @click="pwdDlg.visible = false">{{ t('common.cancel') }}</UiButton>
        <UiButton variant="primary" :disabled="pwdDlg.saving" @click="savePwd">{{ pwdDlg.saving ? t('system.settings.submitting') : t('common.confirm') }}</UiButton>
      </template>
    </UiDialog>
  </div>
</template>
