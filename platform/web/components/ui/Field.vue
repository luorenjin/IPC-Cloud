<script setup lang="ts">
// 表单字段外壳：标签 + 控件 + 字段级错误/说明（配合 useForm）。
// 错误文案就地呈现在字段下方，替代"提交后弹 toast 说哪里错了"的旧做法。
const props = withDefaults(defineProps<{
  label?: string
  /** 错误文案；非空即进入错误态 */
  error?: string
  /** 常态说明文字；有错误时让位给错误文案 */
  hint?: string
  required?: boolean
  /** 标签宽度，用于左右布局；不传则标签在上 */
  labelWidth?: string
}>(), { required: false })

// 错误文案与控件通过 aria-describedby 关联，读屏才能念出错误原因
const fieldId = useId()
const msgId = computed(() => `${fieldId}-msg`)
const horizontal = computed(() => !!props.labelWidth)
</script>

<template>
  <div class="mb-3" :class="horizontal ? 'flex items-start gap-3' : ''">
    <label
      v-if="label"
      :for="fieldId"
      class="block text-[13px] text-muted"
      :class="horizontal ? 'shrink-0 pt-1.5 text-right' : 'mb-1.5'"
      :style="horizontal ? { width: labelWidth } : undefined"
    >
      {{ label }}
      <span v-if="required" class="text-danger" aria-hidden="true">*</span>
    </label>

    <div class="min-w-0 flex-1">
      <!-- 控件通过 slot props 拿到 id 与无障碍属性；调用方可选择性透传 -->
      <slot :id="fieldId" :invalid="!!error" :describedby="error || hint ? msgId : undefined" />

      <p v-if="error" :id="msgId" class="mt-1 text-xs text-danger" role="alert">{{ error }}</p>
      <p v-else-if="hint" :id="msgId" class="mt-1 text-xs text-placeholder">{{ hint }}</p>
    </div>
  </div>
</template>
