<script setup lang="ts">
// 单选组（Reka RadioGroup）
import { RadioGroupRoot, RadioGroupItem, RadioGroupIndicator } from 'reka-ui'

const props = withDefaults(defineProps<{
  modelValue: string
  items: { label: string; value: string }[]
  inline?: boolean
  disabled?: boolean
}>(), { inline: true, disabled: false })

const emit = defineEmits<{ 'update:modelValue': [v: string] }>()
</script>

<template>
  <RadioGroupRoot :model-value="modelValue" :disabled="disabled" :class="inline ? 'flex items-center gap-4' : 'flex flex-col gap-2'" @update:model-value="emit('update:modelValue', String($event))">
    <label v-for="it in items" :key="it.value" class="inline-flex cursor-pointer select-none items-center gap-1.5 text-sm text-body">
      <RadioGroupItem :value="it.value" class="flex h-4 w-4 items-center justify-center rounded-full border border-line bg-surface outline-none ipc-focus-ring transition-colors data-[state=checked]:border-primary">
        <RadioGroupIndicator class="block h-2 w-2 rounded-full bg-primary" />
      </RadioGroupItem>
      {{ it.label }}
    </label>
  </RadioGroupRoot>
</template>
