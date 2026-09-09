<script setup lang="ts">
// 登录页认证布局：深色基底 + 极简信号网格背景，替代原消费级科技蓝渐变卡片
</script>

<template>
  <div class="relative flex min-h-screen flex-col overflow-hidden bg-sidebar">
    <!-- 信号网格背景：细网格线 + 静态信号点，暗示"多设备接入一张网"，不做动画 -->
    <div
      class="pointer-events-none absolute inset-0 opacity-60"
      style="background-image: linear-gradient(var(--color-line) 1px, transparent 1px), linear-gradient(90deg, var(--color-line) 1px, transparent 1px); background-size: 48px 48px;"
    />
    <div
      class="pointer-events-none absolute inset-0 opacity-70"
      style="background-image: radial-gradient(circle, rgba(0,184,201,0.35) 1.5px, transparent 1.5px); background-size: 48px 48px;"
    />
    <div class="pointer-events-none absolute inset-0" style="background: radial-gradient(ellipse at center 38%, transparent 0%, var(--color-sidebar) 78%);" />

    <!-- 顶栏：左上角品牌名称 -->
    <header class="relative z-10 flex h-16 shrink-0 items-center px-8">
      <div class="flex items-center gap-2">
        <span class="flex h-7 w-7 items-center justify-center rounded-signal bg-primary text-sidebar">
          <Icon name="video" :size="16" />
        </span>
        <span class="text-[17px] font-bold tracking-wide text-ink">IpcCloud</span>
      </div>
    </header>

    <!-- 中间主内容区 -->
    <main class="relative z-10 flex flex-1 items-center justify-center p-4">
      <slot />
    </main>

    <!-- 底部版权 -->
    <footer class="relative z-10 flex items-center justify-center px-10 py-4 text-xs text-sidebar-text">
      Copyright © 2019-2026 IpcCloud. All rights reserved.
    </footer>
  </div>
</template>
