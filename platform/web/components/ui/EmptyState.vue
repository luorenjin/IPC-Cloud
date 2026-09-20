<script setup lang="ts">
const { t } = useI18n()
// 空状态（PRD §9.1：插画 + 主按钮 + 帮助链接）
// hint：空状态应告诉用户下一步能做什么，而不只是"暂无数据"
const props = withDefaults(defineProps<{ text?: string; hint?: string; icon?: string }>(), { text: '', icon: 'box' })
</script>

<template>
  <div class="flex flex-col items-center justify-center gap-3 py-12 text-center">
    <svg viewBox="0 0 120 90" class="h-20 w-28 text-line" fill="none" stroke="currentColor" stroke-width="2">
      <path d="M20 62h80l-8-34H28z" stroke-linejoin="round" />
      <path d="M28 28l10-14h44l10 14" stroke-linejoin="round" />
      <path d="M44 46h32M50 54h20" stroke-linecap="round" opacity=".5" />
      <circle cx="60" cy="20" r="4" opacity=".5" />
    </svg>
    <p class="text-sm text-placeholder">{{ text || t('common.noData') }}</p>
    <p v-if="hint" class="-mt-1 max-w-xs text-xs text-placeholder/80">{{ hint }}</p>
    <div v-if="$slots.action" class="flex items-center gap-3"><slot name="action" /></div>
  </div>
</template>
