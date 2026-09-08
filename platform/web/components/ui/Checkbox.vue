<script setup lang="ts">
// 复选框（Reka Checkbox）
import { CheckboxRoot, CheckboxIndicator } from 'reka-ui'

const props = withDefaults(defineProps<{ modelValue: boolean; label?: string; disabled?: boolean }>(), { disabled: false })
const emit = defineEmits<{ 'update:modelValue': [v: boolean] }>()
</script>

<template>
  <label class="inline-flex cursor-pointer select-none items-center gap-1.5 text-sm text-body" :class="disabled ? 'cursor-not-allowed opacity-50' : ''">
    <CheckboxRoot
      :model-value="modelValue" :disabled="disabled"
      class="flex h-4 w-4 shrink-0 items-center justify-center rounded-sm border border-line bg-surface outline-none transition-colors data-[state=checked]:border-primary data-[state=checked]:bg-primary"
      @update:model-value="emit('update:modelValue', Boolean($event))"
    >
      <CheckboxIndicator class="text-white"><Icon name="check" :size="11" :stroke="3" /></CheckboxIndicator>
    </CheckboxRoot>
    <slot>{{ label }}</slot>
  </label>
</template>
