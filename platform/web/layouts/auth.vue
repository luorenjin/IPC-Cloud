<script setup lang="ts">
// 登录页认证布局（100% 严格对齐图 1：科技蓝背景 + 左上角品牌Logo + 居中卡片 + 底部Footer）
</script>

<template>
  <div class="relative flex min-h-screen flex-col overflow-hidden bg-[#0a529c]">
    <!-- 科技网格与渐变背景图层 -->
    <div
      class="absolute inset-0 pointer-events-none opacity-90"
      style="background: radial-gradient(ellipse at center 40%, #1c7cdb 0%, #0d529c 55%, #08376e 100%);"
    />
    <!-- 科技环形光圈装饰 -->
    <div
      class="absolute inset-0 pointer-events-none opacity-25"
      style="background-image: radial-gradient(circle at 50% 50%, rgba(255,255,255,0.2) 1px, transparent 1px), radial-gradient(circle at 50% 50%, rgba(255,255,255,0.15) 1px, transparent 0); background-size: 40px 40px, 80px 80px;"
    />

    <!-- 顶栏：左上角品牌名称 -->
    <header class="relative z-10 flex h-16 shrink-0 items-center px-8">
      <div class="flex items-center gap-2">
        <span class="flex h-7 w-7 items-center justify-center rounded bg-white text-[#1785E6] font-bold shadow-sm">
          <Icon name="video" :size="16" />
        </span>
        <span class="text-[17px] font-bold tracking-wide text-white">IpcCloud | 视频管理平台</span>
      </div>
    </header>

    <!-- 中间主内容区 -->
    <main class="relative z-10 flex flex-1 items-center justify-center p-4">
      <slot />
    </main>

    <!-- 底部版权备案 Footer -->
    <footer class="relative z-10 flex flex-wrap items-center justify-between px-10 py-4 text-xs text-white/60">
      <div class="mx-auto text-center">
        <span>IpcCloud 视联技术 版权所有 Copyright © 2019-2026 IpcCloud. All rights reserved.</span>
        <span class="ml-6 hover:text-white/90 cursor-pointer">粤ICP备2026048153号</span>
      </div>
    </footer>
  </div>
</template>
