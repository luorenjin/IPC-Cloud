<script setup lang="ts">
const { t } = useI18n()
// 分页
const props = withDefaults(defineProps<{
  page: number
  pageSize: number
  total: number
  pageSizes?: number[]
}>(), { pageSizes: () => [10, 20, 50, 100] })

const emit = defineEmits<{ 'update:page': [v: number]; 'update:pageSize': [v: number] }>()

const pages = computed(() => Math.max(1, Math.ceil(props.total / props.pageSize)))
const list = computed(() => {
  const p = props.page, n = pages.value, out: (number | string)[] = []
  if (n <= 7) { for (let i = 1; i <= n; i++) out.push(i); return out }
  out.push(1)
  if (p > 4) out.push('…')
  for (let i = Math.max(2, p - 2); i <= Math.min(n - 1, p + 2); i++) out.push(i)
  if (p < n - 3) out.push('…')
  out.push(n)
  return out
})
function go(p: number) {
  if (p >= 1 && p <= pages.value && p !== props.page) emit('update:page', p)
}
</script>

<template>
  <div class="flex flex-wrap items-center justify-end gap-3 pt-3 text-sm text-muted">
    <span>{{ t('page.totalCount', { n: total }) }}</span>
    <UiSelect
      :model-value="String(pageSize)" :options="pageSizes.map((s) => ({ label: t('page.perPage', { n: s }), value: String(s) }))"
      width="w-28" size="sm" @update:model-value="emit('update:pageSize', Number($event)); emit('update:page', 1)"
    />
    <div class="flex items-center gap-1">
      <button type="button" :aria-label="t('page.prev')" class="flex h-7 w-7 items-center justify-center rounded-chrome border border-line bg-surface text-muted transition-colors hover:border-primary hover:text-primary disabled:cursor-not-allowed disabled:opacity-40" :disabled="page <= 1" @click="go(page - 1)">
        <Icon name="chevron-left" :size="14" />
      </button>
      <template v-for="(p, i) in list" :key="i">
        <span v-if="p === '…'" class="px-1 text-placeholder">…</span>
        <button
          v-else
          type="button"
          class="h-7 min-w-7 rounded-chrome border px-1.5 text-sm transition-colors"
          :class="p === page ? 'border-primary bg-primary text-white' : 'border-line bg-surface text-muted hover:border-primary hover:text-primary'"
          :aria-label="t('page.nth', { n: p })"
          :aria-current="p === page ? 'page' : undefined"
          @click="go(Number(p))"
        >{{ p }}</button>
      </template>
      <button type="button" :aria-label="t('page.next')" class="flex h-7 w-7 items-center justify-center rounded-chrome border border-line bg-surface text-muted transition-colors hover:border-primary hover:text-primary disabled:cursor-not-allowed disabled:opacity-40" :disabled="page >= pages" @click="go(page + 1)">
        <Icon name="chevron-right" :size="14" />
      </button>
    </div>
  </div>
</template>
