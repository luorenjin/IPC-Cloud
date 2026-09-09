<script setup lang="ts">
// 输入框（PRD 基线：小圆角、浅灰边框、聚焦品牌蓝）
const props = withDefaults(defineProps<{
  modelValue?: string | number | null
  type?: string
  placeholder?: string
  size?: 'sm' | 'md' | 'lg'
  disabled?: boolean
  readonly?: boolean
  clearable?: boolean
  maxlength?: number
  width?: string
  autocomplete?: string
}>(), { type: 'text', size: 'md', disabled: false, clearable: false, width: 'w-full' })

const emit = defineEmits<{ 'update:modelValue': [v: string]; enter: []; clear: [] }>()
const focused = ref(false)
const heights = { sm: 'h-7 text-xs', md: 'h-8 text-sm', lg: 'h-10 text-sm' }
</script>

<template>
  <div
    class="group/ipc flex items-center rounded-chrome border bg-surface px-2.5 transition-colors"
    :class="[
      width, heights[size],
      focused ? 'border-primary ring-1 ring-primary/25' : 'border-line hover:border-placeholder',
      disabled ? 'bg-zone cursor-not-allowed opacity-60' : ''
    ]"
  >
    <slot name="prefix" />
    <input
      class="w-full min-w-0 bg-transparent outline-none text-body placeholder:text-placeholder disabled:cursor-not-allowed"
      :type="type" :value="modelValue" :placeholder="placeholder" :disabled="disabled" :readonly="readonly" :maxlength="maxlength" :autocomplete="autocomplete"
      @input="emit('update:modelValue', ($event.target as HTMLInputElement).value)"
      @focus="focused = true" @blur="focused = false"
      @keyup.enter="emit('enter')"
    />
    <button
      v-if="clearable && modelValue !== '' && modelValue != null" type="button"
      class="ml-1 hidden text-placeholder hover:text-body group-hover/ipc:block"
      @click="emit('update:modelValue', ''); emit('clear')"
    >
      <Icon name="x" :size="13" />
    </button>
    <slot name="suffix" />
  </div>
</template>
