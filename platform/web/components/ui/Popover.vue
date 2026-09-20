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
// Esc 关闭改走 composables/useEscClose.ts 的全局栈（原来是模板上的 @keydown.esc）。
// 换成全局栈多解决两件事：焦点在**触发器**上时 Esc 也能关（旧实现只覆盖焦点在弹层内的场景），
// 以及「弹窗里开下拉」时 Esc 只关最上面那层而不是把弹窗一起关掉。
useEscClose(rootOpen, () => { rootOpen.value = false })
</script>

<template>
  <PopoverRoot :open="rootOpen" @update:open="rootOpen = $event">
    <PopoverTrigger as-child><slot name="trigger" /></PopoverTrigger>
    <PopoverPortal>
      <PopoverContent
        :side="side" :align="align" :side-offset="6"
        class="z-50 rounded-chrome border border-line bg-surface-2 p-3 shadow-pop ipc-anim-pop-in outline-none"
        :class="width"
      >
        <PopoverArrow v-if="false" />
        <slot />
      </PopoverContent>
    </PopoverPortal>
  </PopoverRoot>
</template>
