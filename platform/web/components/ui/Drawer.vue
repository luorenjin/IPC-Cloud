<script setup lang="ts">
// 右侧抽屉（Reka Dialog，用于消息详情 / 任务中心）
import { DialogRoot, DialogPortal, DialogOverlay, DialogContent, DialogTitle, DialogClose } from 'reka-ui'

const props = withDefaults(defineProps<{ open: boolean; title?: string; width?: string }>(), { width: 'max-w-md' })
const emit = defineEmits<{ 'update:open': [v: boolean] }>()
// Esc 关闭：见 composables/useEscClose.ts
useEscClose(computed(() => props.open), () => emit('update:open', false))
</script>

<template>
  <DialogRoot :open="open" @update:open="emit('update:open', $event)">
    <DialogPortal>
      <DialogOverlay class="fixed inset-0 z-50 bg-black/40 ipc-anim-fade-in" />
      <DialogContent
        class="fixed right-0 top-0 z-50 flex h-full w-[calc(100vw-48px)] flex-col border-l border-line bg-surface shadow-pop ipc-anim-slide-right focus:outline-none"
        :class="width"
      >
        <div class="flex items-center justify-between border-b border-line-soft px-5 py-3.5">
          <DialogTitle class="text-[15px] font-semibold text-ink">{{ title }}</DialogTitle>
          <DialogClose class="rounded p-1 text-placeholder hover:bg-zone hover:text-body">
            <Icon name="x" :size="16" />
          </DialogClose>
        </div>
        <div class="min-h-0 flex-1 overflow-y-auto"><slot /></div>
        <div v-if="$slots.footer" class="border-t border-line-soft px-5 py-3"><slot name="footer" /></div>
      </DialogContent>
    </DialogPortal>
  </DialogRoot>
</template>
