<script setup lang="ts">
// 气泡浮层（Reka Popover，用于筛选面板、时间编辑浮层等）
import { PopoverRoot, PopoverTrigger, PopoverPortal, PopoverContent, PopoverArrow } from 'reka-ui'

const props = withDefaults(defineProps<{ open?: boolean; align?: 'start' | 'center' | 'end'; side?: 'top' | 'right' | 'bottom' | 'left'; width?: string }>(), { align: 'start', side: 'bottom', width: '' })
const emit = defineEmits<{ 'update:open': [v: boolean] }>()
</script>

<template>
  <PopoverRoot :open="open" @update:open="emit('update:open', $event)">
    <PopoverTrigger as-child><slot name="trigger" /></PopoverTrigger>
    <PopoverPortal>
      <PopoverContent
        :side="side" :align="align" :side-offset="6"
        class="z-50 rounded-chrome border border-line bg-surface-2 p-3 shadow-pop ipc-anim-pop-in outline-none"
        :class="width"
      >
        <PopoverArrow v-if="false" />
        <slot />
      </PopoverContent>
    </PopoverPortal>
  </PopoverRoot>
</template>
