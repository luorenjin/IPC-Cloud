<script setup lang="ts">
// 角色与成员管理（ACC-05/06/07）：角色权限配置 + 项目成员管理
const api = useApi()
const { currentProject, loadMe } = useAuth()

const activeTab = ref('roles')

/* ---------------- 角色 ---------------- */
// 可分配的菜单权限
const MENU_OPTIONS = [
  { value: 'dashboard', label: '仪表盘' },
  { value: 'devices', label: '设备' },
  { value: 'live', label: '实时预览' },
  { value: 'playback', label: '录像回放' },
  { value: 'alarms', label: '告警' },
  { value: 'record', label: '录像设置' },
  { value: 'system', label: '系统' }
]
// 可分配的操作权限
const ACTION_OPTIONS = [
  { value: 'view', label: '查看' },
  { value: 'preview', label: '预览' },
  { value: 'playback', label: '回放' },
  { value: 'ptz', label: '云台' },
  { value: 'config', label: '配置' },
  { value: 'delete', label: '删除' }
]

const roles = ref<any[]>([])
const rolesLoading = ref(false)

async function loadRoles() {
  rolesLoading.value = true
  try {
    const res: any = await api.get('/roles')
    roles.value = res?.items || []
  } catch (e: any) {
    ElMessage.error(e?.msg || '加载角色失败')
  } finally {
    rolesLoading.value = false
  }
}

/* 新建/编辑角色对话框 */
const roleDlg = reactive({
  visible: false,
  mode: 'create' as 'create' | 'edit',
  id: null as any,
  name: '',
  copyFrom: '' as any,
  menus: [] as string[],
  actions: [] as string[],
  saving: false
})

function openRoleDlg(mode: 'create' | 'edit', row?: any) {
  roleDlg.mode = mode
  if (mode === 'edit') {
    roleDlg.id = row.id
    roleDlg.name = row.name
    roleDlg.menus = [...(row.perms?.menus || [])]
    roleDlg.actions = [...(row.perms?.actions || [])]
    roleDlg.copyFrom = ''
  } else {
    roleDlg.id = null
    roleDlg.name = ''
    roleDlg.copyFrom = ''
    roleDlg.menus = []
    roleDlg.actions = []
  }
  roleDlg.visible = true
}

// "复制自"：选择已有角色后预填权限勾选
watch(
  () => roleDlg.copyFrom,
  (id) => {
    if (roleDlg.mode !== 'create' || !id) return
    const src = roles.value.find((r) => r.id === id)
    if (src) {
      roleDlg.menus = [...(src.perms?.menus || [])]
      roleDlg.actions = [...(src.perms?.actions || [])]
    }
  }
)

async function saveRole() {
  if (!roleDlg.name.trim()) return ElMessage.warning('请输入角色名称')
  roleDlg.saving = true
  try {
    const perms = { menus: [...roleDlg.menus], actions: [...roleDlg.actions] }
    if (roleDlg.mode === 'create') {
      await api.post('/roles', {
        name: roleDlg.name.trim(),
        perms,
        scope: {},
        copyFrom: roleDlg.copyFrom || undefined
      })
      ElMessage.success('角色已创建')
    } else {
      await api.put('/roles/' + roleDlg.id, { name: roleDlg.name.trim(), perms })
      ElMessage.success('角色已更新')
    }
    roleDlg.visible = false
    await loadRoles()
  } catch (e: any) {
    ElMessage.error(e?.msg || '保存失败')
  } finally {
    roleDlg.saving = false
  }
}

async function removeRole(row: any) {
  try {
    await ElMessageBox.confirm('确定删除角色「' + row.name + '」？', '删除角色', {
      type: 'warning',
      confirmButtonText: '删除',
      cancelButtonText: '取消'
    })
  } catch {
    return
  }
  try {
    await api.del('/roles/' + row.id)
    ElMessage.success('已删除')
    await loadRoles()
  } catch (e: any) {
    ElMessage.error(e?.msg || '删除失败')
  }
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
    ElMessage.error(e?.msg || '加载成员失败')
  } finally {
    usersLoading.value = false
  }
}

// 角色 id -> 名称映射
const roleMap = computed(() => {
  const m = new Map<any, string>()
  roles.value.forEach((r) => m.set(r.id, r.name))
  return m
})

/* 表格内联修改角色 */
async function changeRole(row: any, roleId: any) {
  try {
    await api.put('/users/' + row.id, { roleId })
    row.roleId = roleId
    ElMessage.success('角色已更新')
  } catch (e: any) {
    ElMessage.error(e?.msg || '修改失败')
    await loadUsers()
  }
}

/* 启用/停用成员 */
async function toggleUser(row: any) {
  try {
    await api.put('/users/' + row.id, { status: row.status })
    ElMessage.success(row.status === 'active' ? '已启用' : '已停用')
  } catch (e: any) {
    ElMessage.error(e?.msg || '操作失败')
    await loadUsers()
  }
}

/* 重置密码 */
const pwdDlg = reactive({ visible: false, user: null as any, password: '', saving: false })

function openPwdDlg(row: any) {
  pwdDlg.user = row
  pwdDlg.password = ''
  pwdDlg.visible = true
}

async function savePwd() {
  if (!pwdDlg.password) return ElMessage.warning('请输入新密码')
  pwdDlg.saving = true
  try {
    await api.post('/users/' + pwdDlg.user.id + '/reset-password', { password: pwdDlg.password })
    ElMessage.success('密码已重置')
    pwdDlg.visible = false
  } catch (e: any) {
    ElMessage.error(e?.msg || '重置失败')
  } finally {
    pwdDlg.saving = false
  }
}

/* 删除成员 */
async function removeUser(row: any) {
  try {
    await ElMessageBox.confirm('确定删除成员「' + (row.name || row.username) + '」？', '删除成员', {
      type: 'warning',
      confirmButtonText: '删除',
      cancelButtonText: '取消'
    })
  } catch {
    return
  }
  try {
    await api.del('/users/' + row.id)
    ElMessage.success('已删除')
    await loadUsers()
  } catch (e: any) {
    ElMessage.error(e?.msg || '删除失败')
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
  if (!userDlg.username.trim() || !userDlg.password) {
    return ElMessage.warning('请填写用户名与初始密码')
  }
  userDlg.saving = true
  try {
    await api.post('/users', {
      username: userDlg.username.trim(),
      name: userDlg.name.trim(),
      contact: userDlg.contact.trim(),
      password: userDlg.password,
      roleId: userDlg.roleId || undefined
    })
    ElMessage.success('成员已创建')
    userDlg.visible = false
    await loadUsers()
  } catch (e: any) {
    ElMessage.error(e?.msg || '创建失败')
  } finally {
    userDlg.saving = false
  }
}

const fmtTime = (ts: any) =>
  ts ? new Date(typeof ts === 'string' ? Date.parse(ts) : ts).toLocaleString() : '-'

onMounted(async () => {
  // 布局可能尚未完成会话加载，兜底拉取一次
  if (!currentProject.value) await loadMe()
  await Promise.all([loadRoles(), loadUsers()])
})
</script>

<template>
  <div class="page">
    <el-card shadow="never">
      <el-tabs v-model="activeTab">
        <!-- 角色管理 -->
        <el-tab-pane label="角色" name="roles">
          <div class="toolbar">
            <el-button type="primary" @click="openRoleDlg('create')">新建角色</el-button>
            <el-button :loading="rolesLoading" @click="loadRoles">刷新</el-button>
          </div>
          <el-table v-loading="rolesLoading" :data="roles" border>
            <el-table-column prop="name" label="名称" min-width="180" />
            <el-table-column label="内置" width="100" align="center">
              <template #default="{ row }">
                <el-tag v-if="row.builtin" size="small">内置</el-tag>
                <el-tag v-else size="small" type="info">自定义</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="160" align="center">
              <template #default="{ row }">
                <template v-if="row.builtin">
                  <el-button link type="primary" disabled>编辑</el-button>
                  <el-button link type="danger" disabled>删除</el-button>
                </template>
                <template v-else>
                  <el-button link type="primary" @click="openRoleDlg('edit', row)">编辑</el-button>
                  <el-button link type="danger" @click="removeRole(row)">删除</el-button>
                </template>
              </template>
            </el-table-column>
          </el-table>
        </el-tab-pane>

        <!-- 成员管理 -->
        <el-tab-pane label="成员" name="users">
          <div class="toolbar">
            <el-button type="primary" @click="openUserDlg">新建成员</el-button>
            <el-button :loading="usersLoading" @click="loadUsers">刷新</el-button>
          </div>
          <el-table v-loading="usersLoading" :data="users" border>
            <el-table-column prop="username" label="用户名" min-width="120" />
            <el-table-column label="姓名" min-width="100">
              <template #default="{ row }">{{ row.name || '-' }}</template>
            </el-table-column>
            <el-table-column label="联系方式" min-width="130">
              <template #default="{ row }">{{ row.contact || '-' }}</template>
            </el-table-column>
            <el-table-column label="角色" min-width="150">
              <template #default="{ row }">
                <el-select
                  :model-value="row.roleId"
                  size="small"
                  style="width: 100%"
                  placeholder="未分配"
                  @change="(v: any) => changeRole(row, v)"
                >
                  <el-option v-for="r in roles" :key="r.id" :label="r.name" :value="r.id" />
                </el-select>
              </template>
            </el-table-column>
            <el-table-column label="状态" width="90" align="center">
              <template #default="{ row }">
                <el-switch
                  v-model="row.status"
                  size="small"
                  active-value="active"
                  inactive-value="disabled"
                  @change="toggleUser(row)"
                />
              </template>
            </el-table-column>
            <el-table-column label="创建时间" width="170">
              <template #default="{ row }">{{ fmtTime(row.createdAt) }}</template>
            </el-table-column>
            <el-table-column label="操作" width="150" align="center">
              <template #default="{ row }">
                <el-button link type="primary" @click="openPwdDlg(row)">重置密码</el-button>
                <el-button link type="danger" @click="removeUser(row)">删除</el-button>
              </template>
            </el-table-column>
          </el-table>
        </el-tab-pane>
      </el-tabs>
    </el-card>

    <!-- 角色新建/编辑对话框 -->
    <el-dialog
      v-model="roleDlg.visible"
      :title="roleDlg.mode === 'create' ? '新建角色' : '编辑角色'"
      width="560px"
    >
      <el-form label-width="80px" @submit.prevent>
        <el-form-item label="角色名称">
          <el-input v-model="roleDlg.name" placeholder="请输入角色名称" maxlength="50" />
        </el-form-item>
        <el-form-item v-if="roleDlg.mode === 'create'" label="复制自">
          <el-select v-model="roleDlg.copyFrom" placeholder="可选：从已有角色复制权限" clearable style="width: 100%">
            <el-option v-for="r in roles" :key="r.id" :label="r.name" :value="r.id" />
          </el-select>
        </el-form-item>
        <el-form-item label="菜单权限">
          <el-checkbox-group v-model="roleDlg.menus">
            <el-checkbox v-for="m in MENU_OPTIONS" :key="m.value" :value="m.value">{{ m.label }}</el-checkbox>
          </el-checkbox-group>
        </el-form-item>
        <el-form-item label="操作权限">
          <el-checkbox-group v-model="roleDlg.actions">
            <el-checkbox v-for="a in ACTION_OPTIONS" :key="a.value" :value="a.value">{{ a.label }}</el-checkbox>
          </el-checkbox-group>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="roleDlg.visible = false">取消</el-button>
        <el-button type="primary" :loading="roleDlg.saving" @click="saveRole">确定</el-button>
      </template>
    </el-dialog>

    <!-- 新建成员对话框 -->
    <el-dialog v-model="userDlg.visible" title="新建成员" width="480px">
      <el-form label-width="80px" @submit.prevent>
        <el-form-item label="用户名" required>
          <el-input v-model="userDlg.username" placeholder="登录用户名" maxlength="50" />
        </el-form-item>
        <el-form-item label="姓名">
          <el-input v-model="userDlg.name" placeholder="姓名" maxlength="50" />
        </el-form-item>
        <el-form-item label="联系方式">
          <el-input v-model="userDlg.contact" placeholder="手机号 / 邮箱" maxlength="100" />
        </el-form-item>
        <el-form-item label="初始密码" required>
          <el-input v-model="userDlg.password" show-password placeholder="初始登录密码" maxlength="64" />
        </el-form-item>
        <el-form-item label="角色">
          <el-select v-model="userDlg.roleId" placeholder="选择角色" clearable style="width: 100%">
            <el-option v-for="r in roles" :key="r.id" :label="r.name" :value="r.id" />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="userDlg.visible = false">取消</el-button>
        <el-button type="primary" :loading="userDlg.saving" @click="saveUser">确定</el-button>
      </template>
    </el-dialog>

    <!-- 重置密码对话框 -->
    <el-dialog v-model="pwdDlg.visible" :title="'重置密码 - ' + (pwdDlg.user?.name || pwdDlg.user?.username || '')" width="420px">
      <el-form label-width="80px" @submit.prevent>
        <el-form-item label="新密码" required>
          <el-input
            v-model="pwdDlg.password"
            show-password
            placeholder="请输入新密码"
            maxlength="64"
            @keyup.enter="savePwd"
          />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="pwdDlg.visible = false">取消</el-button>
        <el-button type="primary" :loading="pwdDlg.saving" @click="savePwd">确定</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.toolbar { display: flex; gap: 8px; margin-bottom: 12px; }
</style>