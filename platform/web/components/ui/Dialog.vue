<script setup lang="ts">
// 通用弹窗（Reka Dialog）
import { DialogRoot, DialogPortal, DialogOverlay, DialogContent, DialogTitle, DialogDescription, DialogClose } from 'reka-ui'

const props = withDefaults(defineProps<{
  open: boolean
  title?: string
  width?: string
  description?: string
  closable?: boolean
}>(), { width: 'max-w-lg', closable: true })

const emit = defineEmits<{ 'update:open': [v: boolean] }>()
</script>

<template>
  <DialogRoot :open="open" @update:open="emit('update:open', $event)">
    <DialogPortal>
      <DialogOverlay class="fixed inset-0 z-50 bg-black/45 ipc-anim-fade-in" />
      <DialogContent
        class="fixed left-1/2 top-1/2 z-50 flex max-h-[86vh] w-[calc(100vw-32px)] -translate-x-1/2 -translate-y-1/2 flex-col rounded-md border border-line bg-surface shadow-pop ipc-anim-pop-in focus:outline-none"
        :class="width"
      >
        <div v-if="title || $slots.header" class="flex items-center justify-between border-b border-line-soft px-5 py-3.5">
          <slot name="header">
            <DialogTitle class="text-[15px] font-semibold text-ink">{{ title }}</DialogTitle>
          </slot>
          <DialogClose v-if="closable" class="rounded p-1 text-placeholder transition-colors hover:bg-zone hover:text-body">
            <Icon name="x" :size="16" />
          </DialogClose>
        </div>
        <DialogDescription v-if="description" class="sr-only">{{ description }}</DialogDescription>
        <div class="min-h-0 flex-1 overflow-y-auto px-5 py-4"><slot /></div>
        <div v-if="$slots.footer" class="flex items-center justify-end gap-2 border-t border-line-soft px-5 py-3">
          <slot name="footer" />
        </div>
      </DialogContent>
    </DialogPortal>
  </DialogRoot>
</template>
