<script setup lang="ts">
// 步骤条（PRD 基线：完成蓝勾、当前蓝圈数字、未到灰圈）
const props = withDefaults(defineProps<{ steps: string[]; current: number; vertical?: boolean }>(), { vertical: false })
</script>

<template>
  <div class="flex items-center" :class="vertical ? 'flex-col items-start gap-4' : ''">
    <template v-for="(s, i) in steps" :key="i">
      <div class="flex items-center gap-2">
        <span
          class="flex h-6 w-6 shrink-0 items-center justify-center rounded-full text-xs font-medium transition-colors"
          :class="i < current ? 'bg-primary text-white' : i === current ? 'border-2 border-primary bg-surface text-primary' : 'border border-line bg-zone text-placeholder'"
        >
          <Icon v-if="i < current" name="check" :size="13" :stroke="3" />
          <template v-else>{{ i + 1 }}</template>
        </span>
        <span class="whitespace-nowrap text-sm" :class="i === current ? 'font-medium text-ink' : i < current ? 'text-body' : 'text-placeholder'">{{ s }}</span>
      </div>
      <div v-if="i < steps.length - 1" class="bg-line" :class="vertical ? 'ml-3 h-6 w-px' : 'mx-3 h-px w-12'" />
    </template>
  </div>
</template>
