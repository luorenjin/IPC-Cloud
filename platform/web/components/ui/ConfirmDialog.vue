<script setup lang="ts">
const { t } = useI18n()
// 确认对话框（全局单例，挂载于 app.vue；通过 useConfirm().ask() 调用）
import {
  AlertDialogRoot, AlertDialogPortal, AlertDialogOverlay, AlertDialogContent,
  AlertDialogTitle, AlertDialogDescription, AlertDialogCancel
} from 'reka-ui'

const { state, open, settle } = useConfirm()
const inputValue = ref('')
const canConfirm = computed(() => !state.value.inputConfirm || inputValue.value.trim() === state.value.inputConfirm)

watch(open, (v) => { if (!v) inputValue.value = '' })
</script>

<template>
  <AlertDialogRoot :open="open" @update:open="(v) => { if (!v) settle(false) }">
    <AlertDialogPortal>
      <AlertDialogOverlay class="fixed inset-0 z-[60] bg-black/45 ipc-anim-fade-in" />
      <AlertDialogContent class="fixed left-1/2 top-1/2 z-[60] w-[calc(100vw-48px)] max-w-sm -translate-x-1/2 -translate-y-1/2 rounded-chrome border border-line bg-surface-2 p-5 shadow-pop ipc-anim-pop-in outline-none">
        <div class="flex gap-3">
          <span class="mt-0.5 flex h-8 w-8 shrink-0 items-center justify-center rounded-full" :class="state.danger ? 'bg-danger-soft text-danger' : 'bg-warning-soft text-warning'">
            <Icon :name="state.danger ? 'alert-triangle' : 'alert-circle'" :size="18" />
          </span>
          <div class="min-w-0 flex-1">
            <AlertDialogTitle class="text-[15px] font-semibold text-ink">{{ state.title }}</AlertDialogTitle>
            <AlertDialogDescription class="mt-1 text-sm leading-6 text-muted">
              {{ state.message }}
              <template v-if="state.detail"><br /><span class="text-xs text-placeholder">{{ state.detail }}</span></template>
            </AlertDialogDescription>
            <div v-if="state.inputConfirm" class="mt-3">
              <p class="mb-1 text-xs text-muted">{{ t('confirm.typeToConfirm') }} <b class="text-danger">{{ state.inputConfirm }}</b></p>
              <UiInput v-model="inputValue" :placeholder="state.inputPlaceholder || t('confirm.namePlaceholder')" size="sm" />
            </div>
          </div>
        </div>
        <div class="mt-5 flex justify-end gap-2">
          <button class="h-8 rounded-chrome border border-line bg-surface px-3.5 text-sm text-body transition-colors hover:border-placeholder" @click="settle(false)">
            {{ state.cancelText }}
          </button>
          <button
            class="h-8 rounded-chrome px-3.5 text-sm text-white transition-colors disabled:cursor-not-allowed disabled:opacity-45"
            :class="state.danger ? 'bg-danger hover:opacity-85' : 'bg-primary hover:bg-primary-deep'"
            :disabled="!canConfirm" @click="settle(true)"
          >{{ state.confirmText }}</button>
        </div>
      </AlertDialogContent>
    </AlertDialogPortal>
  </AlertDialogRoot>
</template>
