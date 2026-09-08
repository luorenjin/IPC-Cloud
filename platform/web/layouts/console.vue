<script setup lang="ts">
// 控制台专属顶层布局（无侧栏，严格对齐图 2：白底顶栏 + 辅助链接 + 用户菜单 + 全宽工作区）
const { user, logout, loadMe } = useAuth()
const { t, locale, setLocale } = useI18n()

onMounted(async () => {
  await loadMe()
  if (!user.value) navigateTo('/login')
})

const userMenu = [
  { label: t('user.profile'), value: 'profile' },
  { label: t('user.language') + '：' + (locale.value === 'zh-CN' ? '中文' : 'EN'), value: 'lang' },
  { label: t('user.logout'), value: 'logout', danger: true, divided: true }
]

function onUserMenu(v: string) {
  if (v === 'profile') navigateTo('/account')
  else if (v === 'lang') setLocale(locale.value === 'zh-CN' ? 'en' : 'zh-CN')
  else if (v === 'logout') logout()
}
</script>

<template>
  <div class="flex min-h-screen flex-col bg-[#f0f3f7]">
    <!-- 顶栏（100% 严格对齐图 2 顶栏） -->
    <header class="flex h-13 shrink-0 items-center justify-between border-b border-[#e5e6eb] bg-white px-6" style="height: 52px">
      <!-- 左侧：品牌 Logo -->
      <div class="flex items-center gap-2.5">
        <span class="flex h-7 w-7 items-center justify-center rounded bg-[#1785E6] text-white">
          <Icon name="video" :size="16" />
        </span>
        <span class="text-base font-bold text-[#1f2329]">IpcCloud <span class="font-normal text-[#86909c]">| 视频管理平台</span></span>
      </div>

      <!-- 右侧辅助链接群与用户信息（严格对齐图 2） -->
      <div class="flex items-center gap-6 text-xs text-[#4e5969]">
        <button class="flex items-center gap-1 hover:text-[#1785E6] transition-colors">
          <Icon name="star" :size="13" class="text-[#86909c]" />合作伙伴
        </button>
        <button class="flex items-center gap-1 hover:text-[#1785E6] transition-colors">
          <Icon name="cloud" :size="13" class="text-[#86909c]" />云端官网
        </button>
        <button class="flex items-center gap-1 hover:text-[#1785E6] transition-colors">
          <Icon name="tag" :size="13" class="text-[#86909c]" />服务及价格
        </button>
        <button class="flex items-center gap-1 hover:text-[#1785E6] transition-colors">
          <Icon name="help-circle" :size="13" class="text-[#86909c]" />咨询与帮助
        </button>

        <span class="h-3.5 w-px bg-[#e5e6eb]" />

        <!-- 用户头像与下拉 -->
        <UiDropdown :items="userMenu" @select="onUserMenu">
          <div class="flex cursor-pointer items-center gap-1.5 text-xs text-[#1f2329] hover:text-[#1785E6]">
            <span class="flex h-6 w-6 items-center justify-center rounded-full bg-[#ebf5ff] text-[#1785E6]">
              <Icon name="user" :size="13" />
            </span>
            <span class="font-medium">{{ user?.name || user?.username || 'Administrator' }}</span>
            <Icon name="chevron-down" :size="12" class="text-[#86909c]" />
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
