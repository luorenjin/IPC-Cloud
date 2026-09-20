<script setup lang="ts">
// 角标（顶栏消息未读数等）
const props = withDefaults(defineProps<{ value?: number | string; max?: number; hidden?: boolean; dot?: boolean }>(), { max: 99, hidden: false, dot: false })
const text = computed(() => {
  if (props.dot) return ''
  const v = Number(props.value || 0)
  return v > props.max ? props.max + '+' : String(v)
})
</script>

<template>
  <span class="relative inline-flex">
    <slot />
    <span
      v-if="!hidden && (dot || Number(value) > 0)"
      class="pointer-events-none absolute -right-1.5 -top-1 z-10 flex items-center justify-center rounded-full bg-danger font-medium text-white"
      :class="dot ? 'h-2 w-2' : 'min-w-[16px] px-1 text-[10px] leading-4'"
      style="border: 1.5px solid var(--color-surface); background-clip: padding-box"
    >{{ text }}</span>
  </span>
</template>
