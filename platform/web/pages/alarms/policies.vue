<script setup lang="ts">
// 告警策略（ALM-05）：项目级「哪些事件要产生告警、怎么提醒」的总开关。
// 后端 engine.platformEvent 第一层判定就读这份策略（policyEnabled），
// 关闭某类型后该类事件不再落库为告警——这里是它唯一的配置入口。
const api = useApi()
const toast = useToast()
const { t } = useI18n()

/** 单类事件的策略；ring 目前仅前端提示音，不产生外部通知（PRD §2.2 外部通知 MVP 不做） */
interface Policy {
  enabled: boolean
  web: boolean
  ring: boolean
}

// 分组呈现：设备侧事件由摄像机上报、受布防时段与通道规则约束；
// 平台侧事件由平台自身产生，与通道无关。两类的排障路径完全不同，故分开。
const GROUPS: { key: string; title: string; hint: string; kinds: string[] }[] = [
  {
    key: 'device',
    title: t('alarm.policies.groupDevice'),
    hint: t('alarm.policies.groupDeviceHint'),
    kinds: [...DEVICE_ALARM_KINDS]
  },
  {
    key: 'platform',
    title: t('alarm.policies.groupPlatform'),
    hint: t('alarm.policies.groupPlatformHint'),
    kinds: [...PLATFORM_ALARM_KINDS]
  }
]

const policies = reactive<Record<string, Policy>>({})
const loading = ref(false)
const saving = ref(false)
/** 保存成功后的基线，用于判断是否有未保存改动 */
const baseline = ref('')

function normalize(raw: any): Record<string, Policy> {
  const out: Record<string, Policy> = {}
  for (const g of GROUPS) {
    for (const k of g.kinds) {
      const v = raw?.[k] || {}
      // 后端缺省视为开启（handleGetAlarmPolicies 未配置时返回全开）
      out[k] = {
        enabled: v.enabled !== false,
        web: v.web !== false,
        ring: v.ring === true
      }
    }
  }
  return out
}

async function load() {
  loading.value = true
  try {
    const res: any = await api.get('/alarm-policies')
    Object.assign(policies, normalize(res))
    baseline.value = JSON.stringify(policies)
  } catch (e: any) {
    toastApiError(e, t('alarm.msg.loadPoliciesFailed'))
  } finally {
    loading.value = false
  }
}

const dirty = computed(() => baseline.value !== '' && JSON.stringify(policies) !== baseline.value)

async function save() {
  saving.value = true
  try {
    await api.put('/alarm-policies', JSON.parse(JSON.stringify(policies)))
    baseline.value = JSON.stringify(policies)
    toast.success(t('alarm.msg.policiesSaved'))
  } catch (e: any) {
    toastApiError(e, t('alarm.msg.savePoliciesFailed'))
  } finally {
    saving.value = false
  }
}

function reset() {
  if (!baseline.value) return
  Object.assign(policies, JSON.parse(baseline.value))
}

/** 整组开关：组内全开时关闭全部，否则开启全部 */
function toggleGroup(kinds: string[]) {
  const allOn = kinds.every((k) => policies[k]?.enabled)
  kinds.forEach((k) => { if (policies[k]) policies[k].enabled = !allOn })
}
function groupState(kinds: string[]) {
  const on = kinds.filter((k) => policies[k]?.enabled).length
  return { on, total: kinds.length }
}

onMounted(load)
</script>

<template>
  <div class="mx-auto max-w-3xl">
    <div class="mb-4 flex items-start justify-between gap-4">
      <div>
        <h1 class="text-base font-semibold text-ink">{{ t('alarm.policies.title') }}</h1>
        <p class="mt-1 text-xs text-placeholder">
          {{ t('alarm.policies.subtitle') }}
        </p>
      </div>
      <div class="flex shrink-0 items-center gap-2">
        <UiButton v-if="dirty" size="sm" @click="reset">{{ t('common.discard') }}</UiButton>
        <UiButton variant="primary" size="sm" :loading="saving" :disabled="!dirty" @click="save">
          {{ t('common.save') }}
        </UiButton>
      </div>
    </div>

    <UiLoading :loading="loading" class="min-h-40">
      <section v-for="g in GROUPS" :key="g.key" class="mb-4 rounded-signal border border-line bg-surface">
        <header class="flex items-center justify-between gap-3 border-b border-line-soft px-4 py-2.5">
          <div class="min-w-0">
            <h2 class="text-sm font-medium text-ink">{{ g.title }}</h2>
            <p class="mt-0.5 text-xs text-placeholder">{{ g.hint }}</p>
          </div>
          <button
            type="button"
            class="shrink-0 rounded-chrome border border-line px-2.5 py-1 text-xs text-muted transition-colors hover:border-primary hover:text-primary"
            @click="toggleGroup(g.kinds)"
          >
            {{ groupState(g.kinds).on === g.kinds.length ? t('alarm.policies.turnAllOff') : t('alarm.policies.turnAllOn') }}
          </button>
        </header>

        <table class="w-full text-sm">
          <thead>
            <tr class="text-left text-xs text-muted">
              <th class="px-4 py-2 font-medium">{{ t('alarm.policies.colKind') }}</th>
              <th class="w-24 px-3 py-2 text-center font-medium">{{ t('alarm.policies.colEnabled') }}</th>
              <th class="w-24 px-3 py-2 text-center font-medium">{{ t('alarm.policies.colWeb') }}</th>
              <th class="w-24 px-3 py-2 text-center font-medium">{{ t('alarm.policies.colRing') }}</th>
            </tr>
          </thead>
          <tbody>
            <tr
              v-for="k in g.kinds" :key="k"
              class="border-t border-line-soft transition-colors hover:bg-primary-softer"
            >
              <td class="px-4 py-2.5 text-body">{{ t(alarmKindKey(k)) }}</td>
              <td class="px-3 py-2.5 text-center">
                <UiSwitch
                  v-if="policies[k]" v-model="policies[k].enabled" size="sm"
                  :aria-label="t('alarm.policies.ariaEnabled', { kind: t(alarmKindKey(k)) })"
                />
              </td>
              <td class="px-3 py-2.5 text-center">
                <UiSwitch
                  v-if="policies[k]" v-model="policies[k].web" size="sm" :disabled="!policies[k].enabled"
                  :aria-label="t('alarm.policies.ariaWeb', { kind: t(alarmKindKey(k)) })"
                />
              </td>
              <td class="px-3 py-2.5 text-center">
                <UiSwitch
                  v-if="policies[k]" v-model="policies[k].ring" size="sm" :disabled="!policies[k].enabled"
                  :aria-label="t('alarm.policies.ariaRing', { kind: t(alarmKindKey(k)) })"
                />
              </td>
            </tr>
          </tbody>
        </table>
      </section>

      <p class="text-xs text-placeholder">
        {{ t('alarm.policies.footHint') }}
      </p>
    </UiLoading>
  </div>
</template>
