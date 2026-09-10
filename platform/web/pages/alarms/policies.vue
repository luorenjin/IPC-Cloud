<script setup lang="ts">
// 告警策略（ALM-05）：项目级「哪些事件要产生告警、怎么提醒」的总开关。
// 后端 engine.platformEvent 第一层判定就读这份策略（policyEnabled），
// 关闭某类型后该类事件不再落库为告警——这里是它唯一的配置入口。
const api = useApi()
const toast = useToast()

/** 单类事件的策略；ring 目前仅前端提示音，不产生外部通知（PRD §2.2 外部通知 MVP 不做） */
interface Policy {
  enabled: boolean
  web: boolean
  ring: boolean
}

// 分组呈现：设备侧事件由摄像机上报、受布防时段与通道规则约束；
// 平台侧事件由平台自身产生，与通道无关。两类的排障路径完全不同，故分开。
const GROUPS: { title: string; hint: string; kinds: string[] }[] = [
  {
    title: '设备侧事件',
    hint: '由摄像机上报，还需通过通道告警规则与布防时段判定（见告警规则页）',
    kinds: [...DEVICE_ALARM_KINDS]
  },
  {
    title: '平台侧事件',
    hint: '由平台自身产生，与通道无关，关闭后将不再记录该类告警',
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
    toastApiError(e, '加载告警策略失败')
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
    toast.success('告警策略已保存')
  } catch (e: any) {
    toastApiError(e, '保存告警策略失败')
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
        <h1 class="text-base font-semibold text-ink">告警策略</h1>
        <p class="mt-1 text-xs text-placeholder">
          控制本项目内哪些事件会被记录为告警。关闭后该类事件不再产生告警记录，也不会出现在消息中心。
        </p>
      </div>
      <div class="flex shrink-0 items-center gap-2">
        <UiButton v-if="dirty" size="sm" @click="reset">放弃修改</UiButton>
        <UiButton variant="primary" size="sm" :loading="saving" :disabled="!dirty" @click="save">
          保存
        </UiButton>
      </div>
    </div>

    <UiLoading :loading="loading" class="min-h-40">
      <section v-for="g in GROUPS" :key="g.title" class="mb-4 rounded-signal border border-line bg-surface">
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
            {{ groupState(g.kinds).on === g.kinds.length ? '全部关闭' : '全部开启' }}
          </button>
        </header>

        <table class="w-full text-sm">
          <thead>
            <tr class="text-left text-xs text-muted">
              <th class="px-4 py-2 font-medium">事件类型</th>
              <th class="w-24 px-3 py-2 text-center font-medium">产生告警</th>
              <th class="w-24 px-3 py-2 text-center font-medium">站内提醒</th>
              <th class="w-24 px-3 py-2 text-center font-medium">声音提示</th>
            </tr>
          </thead>
          <tbody>
            <tr
              v-for="k in g.kinds" :key="k"
              class="border-t border-line-soft transition-colors hover:bg-primary-softer"
            >
              <td class="px-4 py-2.5 text-body">{{ alarmKindName(k) }}</td>
              <td class="px-3 py-2.5 text-center">
                <UiSwitch
                  v-if="policies[k]" v-model="policies[k].enabled" size="sm"
                  :aria-label="`产生${alarmKindName(k)}告警`"
                />
              </td>
              <td class="px-3 py-2.5 text-center">
                <UiSwitch
                  v-if="policies[k]" v-model="policies[k].web" size="sm" :disabled="!policies[k].enabled"
                  :aria-label="`${alarmKindName(k)}站内提醒`"
                />
              </td>
              <td class="px-3 py-2.5 text-center">
                <UiSwitch
                  v-if="policies[k]" v-model="policies[k].ring" size="sm" :disabled="!policies[k].enabled"
                  :aria-label="`${alarmKindName(k)}声音提示`"
                />
              </td>
            </tr>
          </tbody>
        </table>
      </section>

      <p class="text-xs text-placeholder">
        提示：设备侧事件还需在「告警规则」中为具体通道启用并设置布防时段；此处关闭则该类型对所有通道一律不产生告警。
      </p>
    </UiLoading>
  </div>
</template>
