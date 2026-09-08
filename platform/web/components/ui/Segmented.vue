<script setup lang="ts">
// 分段式 Tabs（PRD 基线：蓝底白字分段，如 1/4 分屏、存储位置切换）
import { TabsRoot, TabsList, TabsTrigger, TabsContent } from 'reka-ui'

const props = withDefaults(defineProps<{
  modelValue: string
  items?: { label: string; value: string }[]
}>(), { items: () => [] })

const emit = defineEmits<{ 'update:modelValue': [v: string] }>()
</script>

<template>
  <TabsRoot :model-value="modelValue" @update:model-value="emit('update:modelValue', String($event))">
    <TabsList class="inline-flex items-center rounded border border-line bg-zone p-0.5">
      <TabsTrigger
        v-for="it in items" :key="it.value" :value="it.value"
        class="rounded px-3 py-1 text-xs text-muted outline-none transition-colors data-[state=active]:bg-primary data-[state=active]:text-white data-[state=active]:shadow-sm"
      >{{ it.label }}</TabsTrigger>
    </TabsList>
    <slot />
  </TabsRoot>
</template>
