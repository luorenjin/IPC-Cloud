<script setup lang="ts">
// 标签（PRD §9.1：设备来源四色固定；状态标签语义色）
const props = withDefaults(defineProps<{
  color?: 'default' | 'primary' | 'success' | 'warning' | 'danger' | 'info' | 'idp' | 'gb' | 'onvif' | 'rtsp'
  plain?: boolean
  dot?: boolean
}>(), { color: 'default', plain: false, dot: false })

const map: Record<string, string> = {
  default: 'bg-zone text-muted border-line',
  primary: 'bg-primary-soft text-primary border-primary/30',
  success: 'bg-success-soft text-success border-success/30',
  warning: 'bg-warning-soft text-warning border-warning/30',
  danger: 'bg-danger-soft text-danger border-danger/30',
  info: 'bg-zone text-info border-line',
  idp: 'bg-primary-soft text-src-idp border-primary/30',
  gb: 'bg-success-soft text-src-gb border-success/30',
  onvif: 'bg-accent-onvif-soft text-src-onvif border-accent-onvif/30',
  rtsp: 'bg-zone text-src-rtsp border-line'
}
const cls = computed(() => props.plain ? `border bg-transparent ${map[props.color]}` : map[props.color])
const dotColor: Record<string, string> = {
  default: 'bg-info', primary: 'bg-primary', success: 'bg-success', warning: 'bg-warning',
  danger: 'bg-danger', info: 'bg-info', idp: 'bg-src-idp', gb: 'bg-src-gb', onvif: 'bg-src-onvif', rtsp: 'bg-src-rtsp'
}
</script>

<template>
  <span class="inline-flex items-center gap-1 rounded-signal border px-1.5 py-px text-xs leading-5 whitespace-nowrap" :class="cls">
    <span v-if="dot" class="h-1.5 w-1.5 rounded-full" :class="dotColor[props.color]" />
    <slot />
  </span>
</template>
