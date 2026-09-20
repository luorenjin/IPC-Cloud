<script setup lang="ts">
// 滑块（Reka Slider，用于 PTZ 速度等）
import { SliderRoot, SliderTrack, SliderRange, SliderThumb } from 'reka-ui'

const props = withDefaults(defineProps<{
  modelValue: number
  min?: number
  max?: number
  step?: number
  disabled?: boolean
}>(), { min: 0, max: 100, step: 1, disabled: false })

const emit = defineEmits<{ 'update:modelValue': [v: number] }>()
</script>

<template>
  <SliderRoot
    :model-value="[modelValue]" :min="min" :max="max" :step="step" :disabled="disabled"
    class="relative flex h-5 w-full touch-none select-none items-center"
    @update:model-value="emit('update:modelValue', Number(($event as number[])[0]))"
  >
    <SliderTrack class="relative h-1 flex-1 grow rounded-full bg-line">
      <SliderRange class="absolute h-full rounded-full bg-primary" />
    </SliderTrack>
    <SliderThumb
      class="block h-3.5 w-3.5 rounded-full border-2 border-primary bg-surface shadow outline-none ipc-focus-ring transition-transform hover:scale-110"
    />
  </SliderRoot>
</template>
