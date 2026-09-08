<script lang="ts">
// 树（替代 el-tree；递归组件，支持展开/选中/自定义节点/右键）
export interface TreeNode {
  label: string
  value: string
  children?: TreeNode[]
  meta?: any
}
</script>

<script setup lang="ts">
import { CollapsibleRoot, CollapsibleContent } from 'reka-ui'

const props = withDefaults(defineProps<{
  nodes: TreeNode[]
  selected?: string | null
  expanded?: string[]
  depth?: number
  search?: string
}>(), { depth: 0, selected: null, expanded: () => [], search: '' })

const emit = defineEmits<{ select: [node: TreeNode]; contextmenu: [event: MouseEvent, node: TreeNode] }>()

const openMap = reactive<Record<string, boolean>>({})
function isOpen(v: string) {
  if (v in openMap) return openMap[v]
  return props.depth === 0
}
function matchSearch(n: TreeNode): boolean {
  const q = props.search.trim().toLowerCase()
  if (!q) return true
  if (n.label.toLowerCase().includes(q)) return true
  return (n.children || []).some(matchSearch)
}
</script>

<template>
  <ul class="space-y-px">
    <template v-for="node in nodes" :key="node.value">
      <li v-if="matchSearch(node)">
        <CollapsibleRoot :open="isOpen(node.value)" @update:open="openMap[node.value] = $event">
          <div
            class="group/node flex cursor-pointer items-center gap-1 rounded py-1.5 pr-2 text-sm transition-colors"
            :class="selected === node.value ? 'bg-primary-soft text-primary font-medium' : 'text-body hover:bg-zone'"
            :style="{ paddingLeft: depth * 16 + 4 + 'px' }"
            @click="emit('select', node)"
            @contextmenu.prevent="emit('contextmenu', $event, node)"
          >
            <button
              v-if="node.children && node.children.length" type="button"
              class="flex h-4 w-4 shrink-0 items-center justify-center rounded text-placeholder transition-transform hover:text-body"
              :class="isOpen(node.value) ? 'rotate-90' : ''"
              @click.stop="openMap[node.value] = !isOpen(node.value)"
            ><Icon name="chevron-right" :size="12" /></button>
            <span v-else class="w-4 shrink-0" />
            <slot name="node" :node="node" :depth="depth">
              <span class="truncate">{{ node.label }}</span>
            </slot>
            <slot name="node-extra" :node="node" />
          </div>
          <CollapsibleContent v-if="node.children && node.children.length">
            <UiTree
              :nodes="node.children" :selected="selected" :depth="depth + 1" :search="search"
              @select="emit('select', $event)" @contextmenu="(e, n) => emit('contextmenu', e, n)"
            >
              <template #node="slotProps"><slot name="node" v-bind="slotProps" /></template>
              <template #node-extra="slotProps"><slot name="node-extra" v-bind="slotProps" /></template>
            </UiTree>
          </CollapsibleContent>
        </CollapsibleRoot>
      </li>
    </template>
  </ul>
</template>
