<script setup lang="ts">
// 下拉菜单（Reka DropdownMenu）
import {
  DropdownMenuRoot, DropdownMenuTrigger, DropdownMenuPortal,
  DropdownMenuContent, DropdownMenuItem, DropdownMenuSeparator, DropdownMenuLabel
} from 'reka-ui'

const props = withDefaults(defineProps<{ items: { label: string; value: string; danger?: boolean; disabled?: boolean; divided?: boolean }[]; align?: 'start' | 'end' }>(), { align: 'end' })
const emit = defineEmits<{ select: [v: string] }>()
const open = ref(false)
// Esc 关闭：见 composables/useEscClose.ts
useEscClose(open, () => { open.value = false })
</script>

<template>
  <DropdownMenuRoot v-model:open="open">
    <DropdownMenuTrigger as-child>
      <slot :open="open"><span class="cursor-pointer"><Icon name="more" :size="16" /></span></slot>
    </DropdownMenuTrigger>
    <DropdownMenuPortal>
      <DropdownMenuContent
        side="bottom" :align="align" :side-offset="4"
        class="z-50 min-w-36 rounded-chrome border border-line bg-surface-2 p-1 shadow-pop ipc-anim-pop-in outline-none"
      >
        <template v-for="it in items" :key="it.value">
          <DropdownMenuSeparator v-if="it.divided" class="my-1 h-px bg-line-soft" />
          <DropdownMenuItem
            :disabled="it.disabled"
            class="flex cursor-pointer items-center gap-2 rounded px-2.5 py-1.5 text-sm outline-none ipc-focus-inset transition-colors data-[disabled]:cursor-not-allowed data-[disabled]:opacity-45"
            :class="it.danger ? 'text-danger data-[highlighted]:bg-danger-soft' : 'text-body data-[highlighted]:bg-primary-soft data-[highlighted]:text-primary'"
            @select="emit('select', it.value)"
          >{{ it.label }}</DropdownMenuItem>
        </template>
      </DropdownMenuContent>
    </DropdownMenuPortal>
  </DropdownMenuRoot>
</template>
