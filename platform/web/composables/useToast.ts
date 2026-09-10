// 全局消息提示（替代 ElMessage/ElNotification，基于 Reka Toast 状态）
export interface ToastItem {
  id: number
  type: 'success' | 'error' | 'warning' | 'info'
  title: string
  description?: string
  duration: number
}

// 模块级单例（ssr:false，无跨请求泄漏；避免在模块顶层调用 useState 导致启动崩溃）
const items = ref<ToastItem[]>([])
let seq = 0

function push(type: ToastItem['type'], msg: any, duration = 3500): number {
  const id = ++seq
  const item: ToastItem = {
    id,
    type,
    title: typeof msg === 'string' ? msg : msg?.title || '',
    description: typeof msg === 'string' ? msg?.suggest : msg?.description,
    duration
  }
  items.value.push(item)
  if (duration > 0) setTimeout(() => dismiss(id), duration)
  return id
}

export function dismiss(id: number) {
  const i = items.value.findIndex((t) => t.id === id)
  if (i >= 0) items.value.splice(i, 1)
}

export function useToast() {
  return {
    items,
    success: (m: any) => push('success', m),
    error: (m: any, d = 5000) => push('error', m, d),
    warning: (m: any) => push('warning', m),
    info: (m: any) => push('info', m, 4000),
    dismiss
  }
}

// 统一错误呈现（MGR-15：错误码 + 原因 + 建议）
// fallback 由调用方传入具体语境文案（如「加载设备列表失败」）；
// 这里不再内置中文默认值，缺省时用通用词条。
export function toastApiError(e: any, fallback?: string) {
  const { t } = useI18n()
  const msg = e?.msg || e?.message || fallback || t('common.loadFailed')
  const title = e?.code ? `${e.code}：${msg}` : msg
  useToast().error({ title, suggest: e?.suggest })
}
