<script setup lang="ts">
const { t } = useI18n()
// 错误卡片（MGR-15：错误码 + 原因 + 建议 + [重试][诊断]）
const props = defineProps<{ code?: string; msg?: string; suggest?: string }>()
const emit = defineEmits<{ retry: []; diagnose: [] }>()
</script>

<template>
  <div class="flex flex-col items-center justify-center gap-2 rounded-chrome bg-black/70 px-6 py-5 text-center backdrop-blur-sm">
    <span class="text-danger"><Icon name="alert-circle" :size="26" /></span>
    <p class="text-sm font-medium text-white">
      <span v-if="code" class="mr-1 font-mono text-danger">{{ code }}</span>{{ msg || t('live.player.playFailed') }}
    </p>
    <p v-if="suggest" class="max-w-sm text-xs leading-5 text-white/70">{{ suggest }}</p>
    <div class="mt-1 flex items-center gap-2">
      <button class="h-7 rounded-chrome border border-white/20 px-3 text-xs text-white transition-colors hover:border-primary hover:text-primary" @click="emit('retry')">{{ t('common.retry') }}</button>
      <button class="h-7 rounded-chrome border border-white/20 px-3 text-xs text-white transition-colors hover:border-primary hover:text-primary" @click="emit('diagnose')">{{ t('common.diagnose') }}</button>
    </div>
  </div>
</template>
