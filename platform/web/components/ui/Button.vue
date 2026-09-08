<script setup lang="ts">
// 按钮（PRD 基线：主按钮品牌蓝、次按钮灰描边、文字链接蓝）
const props = withDefaults(defineProps<{
  variant?: 'primary' | 'default' | 'ghost' | 'danger' | 'dangerText' | 'text'
  size?: 'sm' | 'md' | 'lg'
  block?: boolean
}>(), { variant: 'default', size: 'md', block: false })

const base = 'inline-flex items-center justify-center gap-1.5 rounded font-medium transition-colors select-none whitespace-nowrap disabled:opacity-45 disabled:cursor-not-allowed'
const sizes = { sm: 'h-7 px-2.5 text-xs', md: 'h-8 px-3.5 text-sm', lg: 'h-10 px-5 text-sm' }
const variants = {
  primary: 'bg-primary text-white hover:bg-primary-deep active:bg-primary-deep',
  default: 'bg-surface text-body border border-line hover:border-primary hover:text-primary active:bg-primary-soft',
  ghost: 'text-body hover:bg-zone',
  danger: 'bg-danger text-white hover:opacity-85',
  dangerText: 'text-danger hover:bg-danger-soft',
  text: 'text-primary hover:bg-primary-soft px-1'
}
const cls = computed(() => [base, sizes[props.size], variants[props.variant], props.block ? 'w-full' : ''])
</script>

<template>
  <button :class="cls" type="button"><slot /></button>
</template>
