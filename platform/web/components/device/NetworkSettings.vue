<script setup lang="ts">
/**
 * 网络设置区块（net.* 五键）。
 *
 * 与页面其它配置分开展示与保存，原因是后果量级不同：改错亮度最多是画面难看，
 * 改错 IP/掩码/网关会让设备从平台上失联，且这五个键在固件规则表里都是 reboot_required，
 * 「保存」与「生效」还隔着一次重启。因此这里用独立保存按钮 + 危险确认，
 * 而不是混在「保存并下发」里一次性提交。
 */
const props = withDefaults(defineProps<{
  /** 设备支持的 net.* 键（来自后端 supported ∩ 平台白名单） */
  keys: string[]
  /** 当前值（页面持有，本组件只读） */
  data: Record<string, any>
  /** 上一次成功回读的基准值，用于判断本区块是否有未保存修改 */
  baseline: Record<string, any>
  /** 需重启生效的键（后端静态镜像） */
  rebootRequired: string[]
  /** 保存中：锁住输入与按钮 */
  saving?: boolean
}>(), { saving: false })

const emit = defineEmits<{ change: [key: string, value: any]; save: [] }>()
const { t } = useI18n()
const confirmBox = useConfirm()

/** 静态地址四件套的固定顺序与是否必填：关闭 DHCP 时缺 IP/掩码等于把设备置于连不上的配置 */
const STATIC_FIELDS = [
  { key: 'net.ip', labelKey: 'device.config.ip', required: true },
  { key: 'net.mask', labelKey: 'device.config.mask', required: true },
  { key: 'net.gw', labelKey: 'device.config.gateway', required: false },
  { key: 'net.dns', labelKey: 'device.config.dns', required: false }
]

function display(v: unknown) {
  return v === undefined || v === null ? '' : String(v)
}

const hasDhcp = computed(() => props.keys.includes('net.dhcp'))
const fields = computed(() => STATIC_FIELDS.filter((f) => props.keys.includes(f.key)))
/** DHCP 开启时静态地址处于禁用态：显示它们"由 DHCP 管理"比留一堆可编辑空框更准确 */
const dhcpOn = computed(() => hasDhcp.value && Boolean(props.data['net.dhcp']))
const hasRebootKey = computed(() => props.keys.some((k) => props.rebootRequired.includes(k)))

const errors = computed(() => {
  const m: Record<string, string> = {}
  if (dhcpOn.value) return m
  for (const f of fields.value) {
    const label = t(f.labelKey)
    const raw = display(props.data[f.key]).trim()
    if (!raw) {
      if (f.required) m[f.key] = t('device.config.netRequired', { field: label })
      continue
    }
    if (!isIPv4(raw)) m[f.key] = t('device.config.netInvalid', { field: label })
  }
  return m
})
const hasError = computed(() => Object.keys(errors.value).length > 0)

/**
 * 本区块的未保存判定只看这五个键。
 * 页面的整体脏值判定（离开拦截用）仍然覆盖全部配置，两处口径不同是有意的：
 * 按钮要的是"这次点了会下发什么"，离开拦截要的是"走了会不会丢东西"。
 */
const netDirty = computed(() => props.keys.some((k) => display(props.data[k]) !== display(props.baseline[k])))

async function askSave() {
  const ok = await confirmBox.ask({
    title: t('device.config.netSaveConfirmTitle'),
    message: t('device.config.netSaveConfirmMsg'),
    detail: t('device.config.netSaveConfirmDetail'),
    danger: true,
    confirmText: t('device.config.netSaveConfirmOk')
  })
  if (ok) emit('save')
}
</script>

<template>
  <section class="rounded-signal border border-line">
    <header class="flex items-center justify-between border-b border-line-soft px-3 py-2">
      <span class="text-sm font-medium text-ink">{{ t('device.config.group.network') }}</span>
      <UiTag v-if="hasRebootKey" color="warning" plain>⚡ {{ t('device.config.rebootRequiredBadge') }}</UiTag>
    </header>

    <div class="space-y-3 p-3">
      <p class="text-xs text-placeholder">{{ t('device.config.networkHint') }}</p>

      <div v-if="hasDhcp" class="flex items-center gap-3">
        <label class="w-24 shrink-0 text-right text-sm text-muted">{{ t('device.config.dhcp') }}</label>
        <UiSwitch
          :model-value="Boolean(data['net.dhcp'])" :disabled="saving"
          :aria-label="t('device.config.dhcp')"
          @update:model-value="emit('change', 'net.dhcp', $event)"
        />
        <span class="text-xs text-placeholder">
          {{ Boolean(data['net.dhcp']) ? t('device.config.netDhcpOn') : t('device.config.netDhcpOff') }}
        </span>
      </div>

      <div v-for="f in fields" :key="f.key" class="flex items-center gap-3">
        <label class="w-24 shrink-0 text-right text-sm text-muted">{{ t(f.labelKey) }}</label>
        <UiInput
          size="sm" width="w-48" :disabled="dhcpOn || saving" :invalid="!!errors[f.key]"
          :model-value="display(data[f.key])"
          @update:model-value="emit('change', f.key, $event)"
        />
        <span v-if="errors[f.key]" class="text-xs text-danger">{{ errors[f.key] }}</span>
        <span v-else-if="dhcpOn" class="text-xs text-placeholder">{{ t('device.config.netDhcpManaged') }}</span>
        <span v-else-if="f.required" class="text-xs text-placeholder">{{ t('device.config.netRequiredField') }}</span>
      </div>

      <p v-if="!dhcpOn" class="text-xs text-placeholder">{{ t('device.config.netStaticHint') }}</p>
    </div>

    <div class="flex flex-wrap items-center gap-3 border-t border-line-soft px-3 py-2">
      <UiButton
        variant="primary" size="sm" :loading="saving"
        :disabled="saving || !netDirty || hasError"
        @click="askSave"
      >{{ t('device.config.netSave') }}</UiButton>
      <span v-if="hasError" class="text-xs text-danger">{{ t('device.config.netFixFirst') }}</span>
      <span v-else-if="netDirty" class="text-xs text-placeholder">{{ t('device.config.netUnsaved') }}</span>
    </div>
  </section>
</template>
