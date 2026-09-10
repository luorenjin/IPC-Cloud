<script setup lang="ts">
// 控制台专属顶层布局（无侧栏，值守台深色基调：项目入口前的极简顶栏，不含营销导航）
const { user, logout, loadMe } = useAuth()
const { t, locale, setLocale } = useI18n()

onMounted(async () => {
  await loadMe()
  if (!user.value) navigateTo('/login')
})

// computed：语言切换后菜单文案与当前语言标记要跟着变
const userMenu = computed(() => [
  { label: t('user.profile'), value: 'profile' },
  { label: t('user.language') + t('common.colon') + (locale.value === 'zh-CN' ? t('lang.zh') : t('lang.en')), value: 'lang' },
  { label: t('user.logout'), value: 'logout', danger: true, divided: true }
])

function onUserMenu(v: string) {
  if (v === 'profile') navigateTo('/account')
  else if (v === 'lang') setLocale(locale.value === 'zh-CN' ? 'en' : 'zh-CN')
  else if (v === 'logout') logout()
}
</script>

<template>
  <div class="flex min-h-screen flex-col bg-canvas">
    <!-- 顶栏：仅保留品牌标识与用户菜单，私有化运维工具不需要营销导航 -->
    <header class="flex h-bar shrink-0 items-center justify-between border-b border-line bg-surface px-6">
      <div class="flex items-center gap-2.5">
        <span class="flex h-7 w-7 items-center justify-center rounded-signal bg-primary text-sidebar">
          <Icon name="video" :size="16" />
        </span>
        <span class="text-base font-bold text-ink">IpcCloud <span class="font-normal text-muted">| {{ t('app.subtitle') }}</span></span>
      </div>

      <div class="flex items-center gap-4 text-xs text-body">
        <!-- 用户头像与下拉 -->
        <UiDropdown :items="userMenu" @select="onUserMenu">
          <div class="flex cursor-pointer items-center gap-1.5 text-xs text-body hover:text-primary">
            <span class="flex h-6 w-6 items-center justify-center rounded-full bg-primary-soft text-primary">
              <Icon name="user" :size="13" />
            </span>
            <span class="font-medium">{{ user?.name || user?.username || 'Administrator' }}</span>
            <Icon name="chevron-down" :size="12" class="text-placeholder" />
          </div>
        </UiDropdown>
      </div>
    </header>

    <!-- 主体全屏内容区（无侧栏） -->
    <main class="flex-1 overflow-auto">
      <slot />
    </main>
  </div>
</template>
