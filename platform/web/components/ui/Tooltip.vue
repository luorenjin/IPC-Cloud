<script setup lang="ts">
// 文字提示（Reka Tooltip）
import { TooltipRoot, TooltipTrigger, TooltipPortal, TooltipContent, TooltipProvider } from 'reka-ui'

const props = withDefaults(defineProps<{ label: string; side?: 'top' | 'right' | 'bottom' | 'left' }>(), { side: 'top' })
</script>

<template>
  <TooltipProvider :delay-duration="300">
    <TooltipRoot>
      <TooltipTrigger as-child><slot /></TooltipTrigger>
      <TooltipPortal>
        <TooltipContent
          :side="side" :side-offset="4"
          class="z-50 rounded-chrome border border-line bg-surface-2 px-2 py-1 text-xs text-ink shadow-pop ipc-anim-fade-in"
        >
          {{ label }}
          <span class="absolute left-1/2 top-full -translate-x-1/2 border-4 border-transparent border-t-[var(--color-surface-2)]" />
        </TooltipContent>
      </TooltipPortal>
    </TooltipRoot>
  </TooltipProvider>
</template>
