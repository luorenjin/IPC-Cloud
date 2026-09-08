<script setup lang="ts">
// 加载遮罩（替代 v-loading 指令）
const props = withDefaults(defineProps<{ loading?: boolean; text?: string }>(), { loading: false })
</script>

<template>
  <div class="relative">
    <slot />
    <div v-if="loading" class="absolute inset-0 z-10 flex flex-col items-center justify-center gap-2 bg-surface/70">
      <Icon name="refresh" :size="24" class="ipc-spin text-primary" />
      <span v-if="text" class="text-xs text-placeholder">{{ text }}</span>
    </div>
  </div>
</template>
