<script setup lang="ts">
// 气泡浮层（Reka Popover，用于筛选面板、时间编辑浮层等）
import { PopoverRoot, PopoverTrigger, PopoverPortal, PopoverContent, PopoverArrow } from 'reka-ui'

// open 必须显式声明默认值 undefined。
// open?: boolean 会被 Vue 当作布尔 prop：调用方未传时被强转为 false，
// Reka 便判定为「受控」且自身不再持有开合状态，点击触发器不会有任何反应。
// 只有保持 undefined，Reka 才走非受控分支（passive），由组件自己记住开合。
const props = withDefaults(
  defineProps<{ open?: boolean; align?: 'start' | 'center' | 'end'; side?: 'top' | 'right' | 'bottom' | 'left'; width?: string }>(),
  { open: undefined, align: 'start', side: 'bottom', width: '' }
)
const emit = defineEmits<{ 'update:open': [v: boolean] }>()

// 非受控：内部记状态；受控（调用方传了 open / v-model:open）：完全以 props 为准。
const innerOpen = ref(false)
const isControlled = computed(() => props.open !== undefined)
const rootOpen = computed<boolean>({
  get: () => (isControlled.value ? Boolean(props.open) : innerOpen.value),
  set: (v) => {
    innerOpen.value = v
    emit('update:open', v)
  }
})
// Esc 由模板上的 @keydown.esc 显式关闭，不依赖 Reka 内部链路：
// Reka 的 DismissableLayer 用 onKeyStroke('Escape') +「当前层是否栈顶」判定来派发
// dismiss，非模态 Popover 下实测该链路不生效（焦点已在弹层内、点外部可关，唯独 Esc
// 无反应）。WAI-ARIA APG 要求浮层支持 Esc 关闭，故在此兜底。
// 局限：仅覆盖焦点落在弹层内的场景；焦点在触发器上时 Esc 不会关闭。
</script>

<template>
  <PopoverRoot :open="rootOpen" @update:open="rootOpen = $event">
    <PopoverTrigger as-child><slot name="trigger" /></PopoverTrigger>
    <PopoverPortal>
      <PopoverContent
        :side="side" :align="align" :side-offset="6"
        class="z-50 rounded-chrome border border-line bg-surface-2 p-3 shadow-pop ipc-anim-pop-in outline-none"
        :class="width"
        @keydown.esc="rootOpen = false"
      >
        <PopoverArrow v-if="false" />
        <slot />
      </PopoverContent>
    </PopoverPortal>
  </PopoverRoot>
</template>
