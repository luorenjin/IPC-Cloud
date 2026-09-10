<script setup lang="ts">
const { t } = useI18n()
// 全局消息提示容器（挂载于 app.vue；通过 useToast().success/error 调用）
import { ToastProvider, ToastViewport, ToastRoot, ToastTitle, ToastDescription, ToastClose } from 'reka-ui'

const { items, dismiss } = useToast()
const style: Record<string, { icon: string; cls: string }> = {
  success: { icon: 'check-circle', cls: 'text-success' },
  error: { icon: 'x-circle', cls: 'text-danger' },
  warning: { icon: 'alert-triangle', cls: 'text-warning' },
  info: { icon: 'info', cls: 'text-primary' }
}
</script>

<template>
  <ToastProvider :duration="4000" :label="t('common.notifications')">
    <ToastViewport class="fixed left-1/2 top-4 z-[70] flex w-full max-w-md -translate-x-1/2 flex-col items-center gap-2 outline-none">
      <ToastRoot
        v-for="t in items" :key="t.id" :default-open="true"
        @update:open="(v) => { if (!v) dismiss(t.id) }"
        class="pointer-events-auto flex w-full items-start gap-2.5 rounded-chrome border border-line bg-surface-2 px-4 py-3 shadow-pop ipc-anim-toast-in"
      >
        <span :class="style[t.type].cls" class="mt-0.5"><Icon :name="style[t.type].icon" :size="17" /></span>
        <div class="min-w-0 flex-1">
          <ToastTitle class="text-sm font-medium leading-5 text-ink">{{ t.title }}</ToastTitle>
          <ToastDescription v-if="t.description" class="mt-0.5 text-xs leading-5 text-muted">{{ t.description }}</ToastDescription>
        </div>
        <ToastClose class="rounded p-0.5 text-placeholder transition-colors hover:bg-zone hover:text-body">
          <Icon name="x" :size="14" />
        </ToastClose>
      </ToastRoot>
    </ToastViewport>
  </ToastProvider>
</template>
