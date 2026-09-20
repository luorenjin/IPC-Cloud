<script setup lang="ts">
// 复选框（Reka Checkbox）
import { CheckboxRoot, CheckboxIndicator } from 'reka-ui'

const props = withDefaults(defineProps<{
  modelValue: boolean
  label?: string
  disabled?: boolean
  /** 无可见文字时（如仅图标场景）由调用方给出可访问名称 */
  ariaLabel?: string
}>(), { disabled: false })
const emit = defineEmits<{ 'update:modelValue': [v: boolean] }>()

// Reka 把 CheckboxRoot 渲染成 <button>，而 <label> 只能关联表单控件、无法关联 button，
// 因此外层 label 的文字不会成为它的可访问名称，必须用 aria-labelledby 显式关联。
const textId = useId()
</script>

<template>
  <label class="inline-flex cursor-pointer select-none items-center gap-1.5 text-sm text-body" :class="disabled ? 'cursor-not-allowed opacity-50' : ''">
    <CheckboxRoot
      :model-value="modelValue" :disabled="disabled"
      :aria-label="ariaLabel"
      :aria-labelledby="ariaLabel ? undefined : textId"
      class="flex h-4 w-4 shrink-0 items-center justify-center rounded-sm border border-line bg-surface outline-none transition-colors ipc-focus-ring data-[state=checked]:border-primary data-[state=checked]:bg-primary"
      @update:model-value="emit('update:modelValue', Boolean($event))"
    >
      <CheckboxIndicator class="text-white"><Icon name="check" :size="11" :stroke="3" /></CheckboxIndicator>
    </CheckboxRoot>
    <span :id="textId"><slot>{{ label }}</slot></span>
  </label>
</template>
