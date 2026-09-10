/**
 * 轻量表单校验 —— 字段级内联错误。
 *
 * 此前全站校验只有 toast：提交后弹一条「名称不能为空」，用户看不出是哪个字段错了，
 * 且失焦时没有即时反馈。useForm 提供字段级错误状态，配合 <UiField> 渲染在字段下方。
 *
 * 用法：
 *   const form = useForm({ name: '', port: 80 }, {
 *     name: [required('请输入节点名称'), maxLen(64)],
 *     port: [required(), intRange(1, 65535, '端口须为 1–65535 的整数')]
 *   })
 *   // 模板：<UiField label="名称" :error="form.errors.name">
 *   //         <UiInput v-model="form.values.name" @blur="form.validateField('name')" />
 *   //       </UiField>
 *   if (!form.validate()) return   // 校验失败：错误已就地呈现，不再弹 toast
 *   await api.post('/x', form.values)
 */

/** 校验规则：通过返回 null/空串，失败返回错误文案 */
export type Rule<T = any> = (value: T, all: Record<string, any>) => string | null | undefined

// ---------- 常用规则 ----------

export function required(msg = '此项不能为空'): Rule {
  return (v) => {
    if (v == null) return msg
    if (typeof v === 'string' && v.trim() === '') return msg
    if (Array.isArray(v) && v.length === 0) return msg
    return null
  }
}

export function maxLen(n: number, msg?: string): Rule<string> {
  return (v) => (v && String(v).length > n ? msg || `不能超过 ${n} 个字符` : null)
}

export function minLen(n: number, msg?: string): Rule<string> {
  return (v) => (v && String(v).length < n ? msg || `至少需要 ${n} 个字符` : null)
}

/** 整数区间校验：非整数或越界都算失败，不静默改写用户输入 */
export function intRange(min: number, max: number, msg?: string): Rule {
  return (v) => {
    if (v === '' || v == null) return null // 是否必填交给 required
    const n = Number(v)
    if (!Number.isInteger(n) || n < min || n > max) return msg || `须为 ${min}–${max} 之间的整数`
    return null
  }
}

export function pattern(re: RegExp, msg: string): Rule<string> {
  return (v) => (v && !re.test(String(v)) ? msg : null)
}

/** 与另一字段相等（改密码的「确认密码」） */
export function sameAs(field: string, msg = '两次输入不一致'): Rule {
  return (v, all) => (v !== all[field] ? msg : null)
}

// ---------- 主体 ----------

export function useForm<T extends Record<string, any>>(
  initial: T,
  rules: Partial<Record<keyof T, Rule[]>> = {}
) {
  const values = reactive({ ...initial }) as T
  const errors = reactive({} as Record<keyof T, string>)
  /** 已经交互过的字段：未交互的字段不显示错误，避免打开表单就一片红 */
  const touched = reactive({} as Record<keyof T, boolean>)
  const submitting = ref(false)

  function runRules(key: keyof T): string {
    const list = rules[key]
    if (!list) return ''
    for (const rule of list) {
      const msg = rule((values as any)[key], values)
      if (msg) return msg
    }
    return ''
  }

  /** 校验单个字段并记录错误；通常绑在 @blur */
  function validateField(key: keyof T): boolean {
    touched[key] = true
    const msg = runRules(key)
    if (msg) errors[key] = msg
    else delete errors[key]
    return !msg
  }

  /** 全量校验；失败时所有出错字段都会呈现错误 */
  function validate(): boolean {
    let firstBad: keyof T | null = null
    for (const key of Object.keys(rules) as (keyof T)[]) {
      touched[key] = true
      const msg = runRules(key)
      if (msg) {
        errors[key] = msg
        if (firstBad === null) firstBad = key
      } else {
        delete errors[key]
      }
    }
    return firstBad === null
  }

  /** 用户重新输入时清掉该字段的错误，不必等失焦 */
  function clearError(key: keyof T) {
    delete errors[key]
  }

  function reset(next?: Partial<T>) {
    Object.assign(values, initial, next || {})
    for (const k of Object.keys(errors)) delete (errors as any)[k]
    for (const k of Object.keys(touched)) delete (touched as any)[k]
    submitting.value = false
  }

  /** 设置服务端返回的字段错误（后端 422 带 fields 时） */
  function setErrors(map: Partial<Record<keyof T, string>>) {
    for (const [k, v] of Object.entries(map)) {
      if (v) {
        errors[k as keyof T] = v as string
        touched[k as keyof T] = true
      }
    }
  }

  const valid = computed(() => Object.keys(errors).length === 0)

  /**
   * 包装提交：先校验，再置 submitting 防重复提交。
   * 校验失败返回 false 且不调用 fn。
   */
  async function submit(fn: () => Promise<void> | void): Promise<boolean> {
    if (submitting.value) return false
    if (!validate()) return false
    submitting.value = true
    try {
      await fn()
      return true
    } finally {
      submitting.value = false
    }
  }

  return { values, errors, touched, submitting, valid, validate, validateField, clearError, reset, setErrors, submit }
}
