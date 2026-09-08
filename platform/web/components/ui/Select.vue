<script setup lang="ts">
// 下拉选择（Reka Select，PRD 基线：小圆角描边、激活项浅蓝）
import {
  SelectRoot, SelectTrigger, SelectValue, SelectIcon, SelectPortal,
  SelectContent, SelectViewport, SelectItem, SelectItemText, SelectItemIndicator
} from 'reka-ui'

const props = withDefaults(defineProps<{
  modelValue?: string | number | null
  options: { label: string; value: any; disabled?: boolean }[]
  placeholder?: string
  size?: 'sm' | 'md'
  disabled?: boolean
  width?: string
  align?: 'start' | 'center' | 'end'
}>(), { placeholder: '请选择', size: 'md', disabled: false, width: 'w-full', align: 'start' })

const emit = defineEmits<{ 'update:modelValue': [v: any] }>()
const open = ref(false)
const heights = { sm: 'h-7 text-xs', md: 'h-8 text-sm' }

// Reka SelectItem 不允许空字符串 value：用哨兵值映射（PRD 中"全部/未分组"等选项 value 为 ''）
const EMPTY = '__ipc_empty__'
function enc(v: any) { return v === '' || v == null ? EMPTY : String(v) }
function dec(v: any) { return v === EMPTY ? '' : v }
const inner = computed(() => enc(props.modelValue))
const opts = computed(() => (props.options || []).map((o) => ({ ...o, _v: enc(o.value) })))
function onModel(v: any) { emit('update:modelValue', dec(v)) }
</script>

<template>
  <SelectRoot :model-value="inner" :disabled="disabled" :open="open" @update:open="open = $event" @update:model-value="onModel">
    <SelectTrigger
      class="flex items-center gap-1 rounded border bg-surface px-2.5 text-left outline-none transition-colors data-[state=open]:border-primary data-[state=open]:ring-1 data-[state=open]:ring-primary/25 disabled:bg-zone disabled:cursor-not-allowed disabled:opacity-60"
      :class="[width, heights[size], open ? 'border-primary' : 'border-line hover:border-placeholder']"
    >
      <SelectValue class="truncate text-body data-[placeholder]:text-placeholder" :placeholder="placeholder" />
      <span class="ml-auto shrink-0 text-placeholder"><Icon name="chevron-down" :size="14" /></span>
    </SelectTrigger>
    <SelectPortal>
      <SelectContent
        position="popper" side="bottom" :side-offset="4" align="start"
        class="z-50 min-w-[var(--reka-select-trigger-width)] overflow-hidden rounded border border-line bg-surface shadow-pop ipc-anim-pop-in"
      >
        <SelectViewport class="p-1 max-h-72 overflow-y-auto">
          <SelectItem
            v-for="o in opts" :key="o._v" :value="o._v" :disabled="o.disabled"
            class="relative flex cursor-pointer select-none items-center rounded py-1.5 pl-7 pr-2 text-sm text-body outline-none data-[highlighted]:bg-primary-soft data-[highlighted]:text-primary data-[disabled]:opacity-45 data-[disabled]:cursor-not-allowed"
          >
            <span class="absolute left-1.5 flex w-4 items-center text-primary">
              <SelectItemIndicator><Icon name="check" :size="13" /></SelectItemIndicator>
            </span>
            <SelectItemText>{{ o.label }}</SelectItemText>
          </SelectItem>
          <div v-if="!options.length" class="px-3 py-4 text-center text-xs text-placeholder">暂无数据</div>
        </SelectViewport>
      </SelectContent>
    </SelectPortal>
  </SelectRoot>
</template>
