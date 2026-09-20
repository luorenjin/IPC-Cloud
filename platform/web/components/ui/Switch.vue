<script setup lang="ts">
// 开关（Reka Switch）
import { SwitchRoot, SwitchThumb } from 'reka-ui'

// ariaLabel：开关常放在表格单元格里，没有可见文字标签，读屏只会念"开关"。
// 调用方应说明这个开关控制的是什么（如"启用 摄像头A 的录像计划"）。
const props = withDefaults(defineProps<{ modelValue: boolean; disabled?: boolean; size?: 'sm' | 'md'; ariaLabel?: string }>(), { disabled: false, size: 'md' })
const emit = defineEmits<{ 'update:modelValue': [v: boolean] }>()

const dims = computed(() => props.size === 'sm'
  ? { root: 'h-4 w-7', thumb: 'h-3 w-3', on: 'translate-x-3', off: 'translate-x-0.5' }
  : { root: 'h-5 w-9', thumb: 'h-4 w-4', on: 'translate-x-4', off: 'translate-x-0.5' })
</script>

<template>
  <SwitchRoot
    :model-value="modelValue" :disabled="disabled" :aria-label="ariaLabel"
    class="relative inline-flex shrink-0 items-center rounded-full transition-colors outline-none ipc-focus-ring disabled:cursor-not-allowed disabled:opacity-45"
    :class="[dims.root, modelValue ? 'bg-primary' : 'bg-line']"
    @update:model-value="emit('update:modelValue', Boolean($event))"
  >
    <SwitchThumb class="absolute rounded-full bg-white shadow transition-transform" :class="[dims.thumb, modelValue ? dims.on : dims.off]" />
  </SwitchRoot>
</template>
