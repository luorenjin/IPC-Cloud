<script setup lang="ts">
const { t } = useI18n()
// 通用数据表格（替代 el-table，PRD 基线：浅灰表头、行悬停、细边框、支持选择列/固定操作列/空状态）
import { CheckboxRoot, CheckboxIndicator } from 'reka-ui'

export interface Column {
  key: string
  label: string
  width?: string
  align?: 'left' | 'center' | 'right'
  fixed?: boolean
  ellipsis?: boolean
}

const props = withDefaults(defineProps<{
  columns: Column[]
  rows: any[]
  rowKey?: string
  loading?: boolean
  selectable?: boolean
  selection?: any[]
  empty?: string
  dense?: boolean
  /** 行的可读名称字段，用于复选框的可访问名称（读屏会念"选择 摄像头A"） */
  rowLabelKey?: string
  /** 自定义行可读名称（优先于 rowLabelKey）：行里没有现成的名称字段时用它拼一个（如"移动侦测 09:21"） */
  rowLabel?: (row: any) => string
  /** 需要高亮的行主键（深链定位用），命中行加高亮底色；null/空串表示不高亮 */
  highlight?: string | number | null
}>(), { rowKey: 'id', loading: false, selectable: false, empty: '', dense: false, rowLabelKey: 'name', highlight: null })

const emit = defineEmits<{ 'update:selection': [v: any[]] }>()

/** 行名称：优先自定义函数，其次 rowLabelKey 指定字段，缺失时退回主键，保证复选框始终有名称 */
function rowLabel(row: any): string {
  if (props.rowLabel) return props.rowLabel(row)
  return String(row?.[props.rowLabelKey] ?? row?.[props.rowKey] ?? t('table.thisRow'))
}

const selected = computed(() => props.selection || [])
const allChecked = computed(() => props.rows.length > 0 && selected.value.length === props.rows.length)
const someChecked = computed(() => selected.value.length > 0 && !allChecked.value)

function keyOf(row: any) { return row[props.rowKey] ?? row.id ?? row.name }
function isChecked(row: any) { return selected.value.some((k) => k === keyOf(row)) }
function toggleRow(row: any) {
  const k = keyOf(row)
  emit('update:selection', isChecked(row) ? selected.value.filter((x) => x !== k) : [...selected.value, k])
}
function toggleAll() {
  emit('update:selection', allChecked.value ? [] : props.rows.map(keyOf))
}
</script>

<template>
  <div class="relative w-full overflow-auto rounded-signal border border-line bg-surface">
    <table class="w-full border-collapse text-sm">
      <thead>
        <tr class="bg-zone text-left">
          <th v-if="selectable" class="w-10 border-b border-line px-3 py-2.5">
            <CheckboxRoot
              :model-value="allChecked ? true : someChecked ? 'indeterminate' : false"
              :aria-label="allChecked ? t('table.deselectAll') : t('table.selectAllPage')"
              class="flex h-4 w-4 items-center justify-center rounded-sm border border-line bg-surface outline-none ipc-focus-ring transition-colors data-[state=checked]:border-primary data-[state=checked]:bg-primary data-[state=indeterminate]:border-primary"
              @update:model-value="toggleAll"
            >
              <CheckboxIndicator class="text-white">
                <Icon v-if="allChecked" name="check" :size="11" :stroke="3" />
                <span v-else class="block h-0.5 w-2.5 rounded bg-white" />
              </CheckboxIndicator>
            </CheckboxRoot>
          </th>
          <!-- 表头内边距必须与下方 td 的 px-3 保持一致：th 少这一层 padding 会让所有左对齐列
               的表头文字比单元格内容左偏 12px（此前即如此）。纵向再补一点，避免表头比数据行还局促。 -->
          <th
            v-for="c in columns" :key="c.key"
            class="border-b border-line px-3 py-1.5 font-medium text-muted"
            :class="[c.align === 'center' ? 'text-center' : c.align === 'right' ? 'text-right' : '', c.fixed ? 'sticky right-0 z-10 bg-zone' : '']"
            :style="c.width ? { width: c.width, minWidth: c.width } : {}"
          >{{ c.label }}</th>
        </tr>
      </thead>
      <tbody>
        <tr
          v-for="(row, i) in rows" :key="keyOf(row)"
          :data-row-key="String(keyOf(row))"
          class="group/row transition-colors hover:bg-primary-softer"
          :class="highlight !== null && highlight !== '' && String(keyOf(row)) === String(highlight)
            ? 'bg-warning-soft ring-1 ring-inset ring-warning/40' : ''"
        >
          <td v-if="selectable" class="border-b border-line-soft px-3" :class="dense ? 'py-1.5' : 'py-2.5'">
            <CheckboxRoot
              :model-value="isChecked(row)"
              :aria-label="t('table.selectRow', { name: rowLabel(row) })"
              class="flex h-4 w-4 items-center justify-center rounded-sm border border-line bg-surface outline-none ipc-focus-ring transition-colors data-[state=checked]:border-primary data-[state=checked]:bg-primary"
              @update:model-value="toggleRow(row)"
            >
              <CheckboxIndicator class="text-white"><Icon name="check" :size="11" :stroke="3" /></CheckboxIndicator>
            </CheckboxRoot>
          </td>
          <td
            v-for="c in columns" :key="c.key"
            class="border-b border-line-soft px-3 text-body"
            :class="[c.align === 'center' ? 'text-center' : c.align === 'right' ? 'text-right' : '', c.fixed ? 'sticky right-0 z-[1] bg-surface group-hover/row:bg-primary-softer' : '']"
          >
            <div v-if="c.ellipsis !== false" class="truncate" :title="typeof row[c.key] === 'string' ? row[c.key] : ''">
              <slot :name="c.key" :row="row" :index="i">{{ row[c.key] ?? '—' }}</slot>
            </div>
            <slot v-else :name="c.key" :row="row" :index="i">{{ row[c.key] ?? '—' }}</slot>
          </td>
        </tr>
      </tbody>
    </table>

    <div v-if="loading" class="absolute inset-0 flex items-center justify-center bg-surface/60">
      <Icon name="refresh" :size="22" class="ipc-spin text-primary" />
    </div>
    <div v-else-if="!rows.length" class="flex flex-col items-center justify-center gap-2 py-12 text-placeholder">
      <Icon name="box" :size="34" :stroke="1.4" />
      <span class="text-sm">{{ empty }}</span>
      <slot name="empty-action" />
    </div>
  </div>
</template>
