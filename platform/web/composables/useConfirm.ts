// 全局确认框（替代 ElMessageBox.confirm；支持删除设备输入名称 MGR-11）
// 用法：if (await useConfirm().ask({ title: '删除设备', message: '...', danger: true, inputConfirm: row.name })) { ... }
// 需在 app.vue 挂载一次 <UiConfirmDialog />
export interface ConfirmOptions {
  title?: string
  message?: string
  detail?: string
  confirmText?: string
  cancelText?: string
  danger?: boolean
  inputConfirm?: string
  inputPlaceholder?: string
}

// 模块级单例（ssr:false，无跨请求状态泄漏风险）
const state = ref({
  title: '', message: '', detail: '', confirmText: '确定', cancelText: '取消',
  danger: false, inputConfirm: '', inputPlaceholder: ''
})
const open = ref(false)
let resolver: ((v: boolean) => void) | null = null

export function useConfirm() {
  function ask(opts: ConfirmOptions = {}): Promise<boolean> {
    state.value = {
      title: opts.title || '操作确认',
      message: opts.message || '确定执行该操作吗？',
      detail: opts.detail || '',
      confirmText: opts.confirmText || '确定',
      cancelText: opts.cancelText || '取消',
      danger: !!opts.danger,
      inputConfirm: opts.inputConfirm || '',
      inputPlaceholder: opts.inputPlaceholder || ''
    }
    open.value = true
    return new Promise((resolve) => { resolver = resolve })
  }
  function settle(v: boolean) {
    open.value = false
    if (resolver) { resolver(v); resolver = null }
  }
  return { state, open, ask, settle }
}
