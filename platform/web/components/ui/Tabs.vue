<script setup lang="ts">
// 下划线式 Tabs（PRD 基线：激活项品牌蓝 + 蓝色下划线）
import { TabsRoot, TabsList, TabsTrigger, TabsContent } from 'reka-ui'

const props = withDefaults(defineProps<{
  modelValue: string
  items?: { label: string; value: string; badge?: number | string }[]
}>(), { items: () => [] })

const emit = defineEmits<{ 'update:modelValue': [v: string] }>()
</script>

<template>
  <TabsRoot :model-value="modelValue" @update:model-value="emit('update:modelValue', String($event))">
    <TabsList class="flex items-center gap-1 border-b border-line-soft">
      <TabsTrigger
        v-for="it in items" :key="it.value" :value="it.value"
        class="flex items-center gap-1.5 border-b-2 border-transparent px-3.5 pb-2.5 pt-2 text-sm text-muted outline-none ipc-focus-ring transition-colors hover:text-ink data-[state=active]:border-primary data-[state=active]:font-medium data-[state=active]:text-primary"
      >
        {{ it.label }}
        <span v-if="it.badge" class="rounded-full bg-danger px-1.5 text-[10px] leading-4 text-white">{{ it.badge }}</span>
      </TabsTrigger>
      <slot name="extra" />
    </TabsList>
    <slot />
  </TabsRoot>
</template>
