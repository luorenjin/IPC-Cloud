<script setup lang="ts">
// 周 × 24h 网格编辑器（REC-05/ALM-01 通用）：拖选生成时间段。
const props = defineProps<{ modelValue: { days: number[]; ranges: string[][] } }>()
const emit = defineEmits(['update:modelValue'])

const days = ['周一', '周二', '周三', '周四', '周五', '周六', '周日']
const grid = computed(() => {
  // grid[day][minute/30] = true
  const g: boolean[][] = Array.from({ length: 7 }, () => Array(48).fill(false))
  const val = props.modelValue || { days: [], ranges: [] }
  for (const d of val.days || []) {
    if (d < 1 || d > 7) continue
    for (const r of val.ranges || []) {
      const [from, to] = r
      const s = hm2slot(from), e = hm2slot(to)
      for (let i = s; i < e; i++) g[d - 1][i] = true
    }
  }
  return g
})

function hm2slot(hm: string): number {
  const [h, m] = hm.split(':').map(Number)
  return Math.min(48, h * 2 + (m >= 30 ? 1 : 0))
}

function slot2hm(i: number): string {
  const h = Math.floor(i / 2), m = i % 2 === 1 ? '30' : '00'
  return `${String(h).padStart(2, '0')}:${m}`
}

let painting = ref(false)
let paintVal = ref(true)

function paint(day: number, slot: number) {
  grid.value[day][slot] = paintVal.value
}

function onMouseDown(day: number, slot: number) {
  painting.value = true
  paintVal.value = !grid.value[day][slot]
  paint(day, slot)
}

function onMouseOver(day: number, slot: number) {
  if (painting.value) paint(day, slot)
}

function commit() {
  const ranges = ['00:00', '24:00']
  const out: { days: number[]; ranges: string[][] } = { days: [], ranges: [] }
  for (let d = 0; d < 7; d++) {
    let start = -1
    for (let i = 0; i <= 48; i++) {
      const on = i < 48 && grid.value[d][i]
      if (on && start < 0) start = i
      if (!on && start >= 0) {
        out.days.push(d + 1)
        out.ranges.push([slot2hm(start), slot2hm(i)])
        start = -1
      }
    }
  }
  void ranges
  emit('update:modelValue', out)
}

onMouseUpHandler()

function onMouseUpHandler() {
  if (typeof window !== 'undefined') {
    window.addEventListener('mouseup', () => {
      if (painting.value) {
        painting.value = false
        commit()
      }
    })
  }
}
</script>

<template>
  <div class="sched">
    <div class="head" />
    <div v-for="i in 48" :key="i" class="head">{{ (i - 1) % 2 === 0 ? Math.floor((i - 1) / 2) : '' }}</div>

    <template v-for="(d, di) in days" :key="d">
      <div class="day">{{ d }}</div>
      <div
        v-for="s in 48" :key="s" class="cell"
        :class="{ on: grid[di][s - 1] }"
        @mousedown="onMouseDown(di, s - 1)" @mouseover="onMouseOver(di, s - 1)"
      />
    </template>
  </div>
</template>

<style scoped>
.sched {
  display: grid;
  grid-template-columns: 48px repeat(48, 1fr);
  gap: 1px; user-select: none; overflow-x: auto;
  min-width: 432px;
}
.head { font-size: 9px; color: var(--color-placeholder); text-align: center; height: 16px; }
.day { font-size: 12px; color: var(--color-muted); height: 16px; line-height: 16px; }
.cell { height: 16px; background: var(--color-zone); cursor: pointer; }
.cell:hover { outline: 1px solid var(--color-primary); outline-offset: -1px; }
.cell.on { background: var(--color-rec-timer); }
</style>
