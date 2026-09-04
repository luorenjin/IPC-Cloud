<script setup lang="ts">
// 主布局：顶栏 + 深色侧栏（PRD §4 信息架构）
const { user, projects, currentProject, switchProject, loadMe, logout } = useAuth()
const route = useRoute()
const unread = ref(0)

onMounted(async () => {
  await loadMe()
  if (!user.value) return navigateTo('/login')
})

useWs((ev: any) => {
  if (ev.type === 'alarm.new') unread.value++
  if (ev.type === 'alarms.readall') unread.value = 0
})

const menu = [
  { label: '仪表盘', icon: 'Odometer', path: '/' },
  {
    label: '设备', icon: 'VideoCamera', children: [
      { label: '设备列表', path: '/devices' },
      { label: '添加设备', path: '/devices?add=1' },
      { label: '待确认(国标)', path: '/devices/pending' }
    ]
  },
  { label: '实时预览', icon: 'Monitor', path: '/live' },
  { label: '录像回放', icon: 'Film', path: '/playback' },
  {
    label: '告警', icon: 'Bell', children: [
      { label: '消息中心', path: '/alarms' },
      { label: '告警规则', path: '/alarms/rules' }
    ]
  },
  {
    label: '录像设置', icon: 'VideoPlay', children: [
      { label: '录像计划', path: '/record/plans' },
      { label: '计划模板', path: '/record/templates' }
    ]
  },
  {
    label: '系统', icon: 'Setting', children: [
      { label: '项目与分组', path: '/system/projects' },
      { label: '角色与成员', path: '/system/roles' },
      { label: '媒体节点', path: '/system/nodes' },
      { label: '系统设置', path: '/system/settings' },
      { label: '操作日志', path: '/system/audit' }
    ]
  }
]

function isActive(path: string) {
  if (path === '/') return route.path === '/'
  return route.path.startsWith(path.split('?')[0])
}
</script>

<template>
  <el-container class="app-shell">
    <el-aside width="220px" class="sidebar">
      <div class="logo">IpcCloud</div>
      <el-menu
        :default-active="route.fullPath" background-color="#1d2129" text-color="#a3a6ad"
        active-text-color="#409eff" router class="side-menu"
      >
        <template v-for="m in menu" :key="m.label">
          <el-sub-menu v-if="m.children" :index="m.label">
            <template #title>
              <el-icon><component :is="m.icon" /></el-icon>{{ m.label }}
            </template>
            <el-menu-item v-for="s in m.children" :key="s.path" :index="s.path">
              {{ s.label }}
            </el-menu-item>
          </el-sub-menu>
          <el-menu-item v-else :index="m.path">
            <el-icon><component :is="m.icon" /></el-icon>{{ m.label }}
          </el-menu-item>
        </template>
      </el-menu>
    </el-aside>

    <el-container>
      <el-header class="topbar" height="52px">
        <div class="left">
          <el-select
            v-if="currentProject" :model-value="currentProject.id" size="default"
            style="width: 180px" @change="switchProject(projects.find((p: any) => p.id === $event))"
          >
            <el-option v-for="p in projects" :key="p.id" :label="p.name" :value="p.id" />
          </el-select>
        </div>
        <div class="right">
          <el-badge :value="unread" :hidden="!unread" class="msg-badge">
            <el-icon size="18" style="cursor: pointer" @click="navigateTo('/alarms')"><Bell /></el-icon>
          </el-badge>
          <el-dropdown>
            <span class="user-name">{{ user?.name || user?.username || '用户' }}</span>
            <template #dropdown>
              <el-dropdown-menu>
                <el-dropdown-item @click="navigateTo('/account')">个人中心</el-dropdown-item>
                <el-dropdown-item divided @click="logout">退出登录</el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
        </div>
      </el-header>
      <el-main class="main">
        <slot />
      </el-main>
    </el-container>
  </el-container>
</template>

<style scoped>
.app-shell { height: 100vh; }
.sidebar { background: #1d2129; }
.logo { color: #fff; font-weight: 700; font-size: 18px; padding: 16px 20px; letter-spacing: 1px; }
.side-menu { border-right: none; }
.topbar {
  display: flex; align-items: center; justify-content: space-between;
  border-bottom: 1px solid #e4e7ed; background: #fff;
}
.right { display: flex; align-items: center; gap: 20px; }
.user-name { cursor: pointer; color: #333; }
.msg-badge { margin-top: 4px; }
.main { background: #f5f7fa; padding: 16px; overflow: auto; }
</style>
