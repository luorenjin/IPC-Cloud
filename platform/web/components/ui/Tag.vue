<script setup lang="ts">
// 标签（PRD §9.1：设备来源四色固定；状态标签语义色）
const props = withDefaults(defineProps<{
  color?: 'default' | 'primary' | 'success' | 'warning' | 'danger' | 'info' | 'idp' | 'gb' | 'onvif' | 'rtsp'
  plain?: boolean
  dot?: boolean
}>(), { color: 'default', plain: false, dot: false })

const map: Record<string, string> = {
  default: 'bg-zone text-muted border-line',
  primary: 'bg-primary-soft text-primary border-[#bcdcf7]',
  success: 'bg-success-soft text-success border-[#bfe7d6]',
  warning: 'bg-warning-soft text-warning border-[#f5dfba]',
  danger: 'bg-danger-soft text-danger border-[#f7c8c4]',
  info: 'bg-zone text-info border-line',
  idp: 'bg-primary-soft text-src-idp border-[#bcdcf7]',
  gb: 'bg-success-soft text-src-gb border-[#bfe7d6]',
  onvif: 'bg-warning-soft text-src-onvif border-[#f5dfba]',
  rtsp: 'bg-zone text-src-rtsp border-line'
}
const cls = computed(() => props.plain ? `border bg-transparent ${map[props.color]}` : map[props.color])
const dotColor: Record<string, string> = {
  default: 'bg-info', primary: 'bg-primary', success: 'bg-success', warning: 'bg-warning',
  danger: 'bg-danger', info: 'bg-info', idp: 'bg-src-idp', gb: 'bg-src-gb', onvif: 'bg-src-onvif', rtsp: 'bg-src-rtsp'
}
</script>

<template>
  <span class="inline-flex items-center gap-1 rounded border px-1.5 py-px text-xs leading-5 whitespace-nowrap" :class="cls">
    <span v-if="dot" class="h-1.5 w-1.5 rounded-full" :class="dotColor[props.color]" />
    <slot />
  </span>
</template>
