<script lang="ts">
/**
 * 依赖声明：字段的上游总开关。声明了 dependsOn 的字段，在总开关关闭时只是
 * 「当前不生效」——它不是无效值，设备侧该键仍然存在，因此只禁用交互、不清空、不隐藏。
 */
export interface CfgDepends {
  /** 上游键名（如 alarm.motion.enable） */
  key: string
  /** 上游达到该值时本字段才可用 */
  equals: unknown
  /** 禁用原因文案键：禁用必须给出原因，否则用户不知道是页面坏了还是自己没开开关 */
  hintKey: string
}

/**
 * 配置字段声明，键名 / 类型 / 取值范围与固件 `firmware/core/src/config.c` 的规则表逐字对齐。
 * 类型定义随组件一起放在这里，是因为「一个字段怎么渲染」已经收敛到本组件，
 * 页面只负责声明数据与提供状态。
 */
export type CfgField =
  | { key: string; labelKey: string; type: 'int'; min: number; max: number; dependsOn?: CfgDepends }
  | { key: string; labelKey: string; type: 'bool'; dependsOn?: CfgDepends }
  | { key: string; labelKey: string; type: 'enum'; options: { label: string; value: string }[]; dependsOn?: CfgDepends }
  | { key: string; labelKey: string; type: 'str'; dependsOn?: CfgDepends }
  | { key: string; labelKey: string; type: 'tz'; dependsOn?: CfgDepends }
</script>

<script setup lang="ts">
/**
 * 一行配置字段：标签 + 控件 + 提示区。
 *
 * 历史上同一段「按 type 渲染控件 + 三种提示」在页面里重复了三处（主网格 / 高级参数折叠区 /
 * 画面滑杆行），于是每新增一条规则（依赖禁用、需重启徽标…）都要改三遍，且极易漏改其中一处。
 * 收敛到这里之后，规则只实现一次；结构不同的场合（画面滑杆与数字框联动）用 #control 插槽
 * 覆盖控件部分，仍然复用同一套提示优先级。
 */
const props = withDefaults(defineProps<{
  /** 已 t() 过的标签文本 */
  label: string
  /** 有 field 时渲染内置控件；用 #control 插槽时不传 */
  field?: CfgField
  modelValue?: unknown
  disabled?: boolean
  /** 禁用原因，与 disabled 一起使用 */
  disabledHint?: string
  invalid?: boolean
  /** 越界文案，默认按 boundText 生成 */
  invalidText?: string
  rejected?: boolean
  rebootRequired?: boolean
  /** 合法区间文案，空串表示不显示 */
  boundText?: string
  /** 附加说明（如锐度不模拟的告知），优先级低于拒绝/越界/禁用 */
  hint?: string
  labelWidth?: string
}>(), {
  modelValue: undefined,
  disabled: false,
  disabledHint: '',
  invalid: false,
  invalidText: '',
  rejected: false,
  rebootRequired: false,
  boundText: '',
  hint: '',
  labelWidth: 'w-24'
})

const emit = defineEmits<{ 'update:modelValue': [v: any] }>()
const { t } = useI18n()

/**
 * 输入框显示值：原样回显。
 * 不用 cfgNum()（数值归一化）是为了不把用户正在编辑的状态改写掉——
 * 归一化会让"清空数字框"立刻变成 0，用户既清不掉也看不出自己填过什么。
 */
function display(v: unknown) {
  return v === undefined || v === null ? '' : String(v)
}

/**
 * 常见时区。做成下拉是为了避免自由文本写错：固件侧 time.timezone 是自由字符串，
 * 写错不会报错，只会静默生效成一个错的时区——属于最难发现的那类问题。
 */
const TZ_OPTIONS = [
  'Asia/Shanghai', 'Asia/Hong_Kong', 'Asia/Taipei', 'Asia/Tokyo', 'Asia/Seoul', 'Asia/Singapore',
  'Asia/Bangkok', 'Asia/Jakarta', 'Asia/Kolkata', 'Asia/Dubai',
  'Australia/Sydney', 'Europe/London', 'Europe/Paris', 'Europe/Berlin', 'Europe/Moscow',
  'America/New_York', 'America/Chicago', 'America/Denver', 'America/Los_Angeles', 'America/Sao_Paulo',
  'UTC'
]
const tzOptions = computed(() => {
  const cur = display(props.modelValue)
  const list = TZ_OPTIONS.map((v) => ({ label: v, value: v }))
  // 设备上报了列表外的时区时必须原样保留：把它显示成空白会诱使用户"顺手改一个"，
  // 而那恰恰会覆盖设备上原本正确的值
  if (cur && !TZ_OPTIONS.includes(cur)) list.unshift({ label: cur, value: cur })
  return list
})
</script>

<template>
  <div class="flex items-center gap-3">
    <label class="shrink-0 text-right text-sm text-muted" :class="labelWidth">{{ label }}</label>

    <!-- 内置控件按声明类型渲染；画面滑杆行等结构不同的场合用 #control 插槽覆盖 -->
    <slot>
      <UiSwitch
        v-if="field?.type === 'bool'" size="sm" :model-value="Boolean(modelValue)"
        :disabled="disabled" :aria-label="label"
        @update:model-value="emit('update:modelValue', $event)"
      />
      <UiSelect
        v-else-if="field?.type === 'enum'" :model-value="display(modelValue)" :options="field.options"
        size="sm" width="w-32" :disabled="disabled"
        @update:model-value="emit('update:modelValue', $event)"
      />
      <UiSelect
        v-else-if="field?.type === 'tz'" :model-value="display(modelValue)" :options="tzOptions"
        size="sm" width="w-48" :disabled="disabled"
        @update:model-value="emit('update:modelValue', $event)"
      />
      <UiInput
        v-else-if="field?.type === 'int'" type="number" size="sm" width="w-24"
        :invalid="invalid" :disabled="disabled" :model-value="display(modelValue)"
        @update:model-value="emit('update:modelValue', $event)"
      />
      <UiInput
        v-else size="sm" width="w-48" :disabled="disabled" :model-value="display(modelValue)"
        @update:model-value="emit('update:modelValue', $event)"
      />
    </slot>

    <!-- 提示区优先级：需重启徽标恒显；其后的文字提示同一位置只出一条，避免一行挤三段不同颜色的文案 -->
    <UiTag v-if="rebootRequired" color="warning" plain>⚡ {{ t('device.config.rebootRequiredBadge') }}</UiTag>
    <span v-if="rejected" class="text-xs text-danger">{{ t('device.config.rejected') }}</span>
    <span v-else-if="invalid" class="text-xs text-danger">
      {{ invalidText || t('device.config.outOfRange', { range: boundText }) }}
    </span>
    <span v-else-if="disabled && disabledHint" class="text-xs text-placeholder">{{ disabledHint }}</span>
    <span v-else-if="hint" class="text-xs text-placeholder">{{ hint }}</span>
    <span v-else-if="boundText" class="text-xs text-placeholder">{{ boundText }}</span>
  </div>
</template>
