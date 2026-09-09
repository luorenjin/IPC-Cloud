<script setup lang="ts">
// 卡片（PRD 基线：白底、细边框、小圆角、shadow=never）
const props = withDefaults(defineProps<{ title?: string; bodyClass?: string; flat?: boolean }>(), { flat: false })
</script>

<template>
  <div class="rounded-signal border border-line bg-surface" :class="flat ? '' : 'shadow-card'">
    <div v-if="title || $slots.header" class="flex items-center justify-between border-b border-line-soft px-4 py-3">
      <slot name="header">
        <span class="text-sm font-semibold text-ink">{{ title }}</span>
      </slot>
      <div v-if="$slots.extra" class="flex items-center gap-2"><slot name="extra" /></div>
    </div>
    <div :class="bodyClass || 'p-4'"><slot /></div>
  </div>
</template>
