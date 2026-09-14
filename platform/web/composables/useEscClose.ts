// 全局 Esc 关闭栈：一次按键只关「最上层」的浮层。
//
// 为什么不用 Reka 自带的链路：DismissableLayer 用 `index === layers.size - 1` 判「是否栈顶」，
// 但 index 是 computed、layers 又是模块级**非响应式** Set，实测在本项目里判定会失效
// （点遮罩、点关闭按钮都能关，唯独 Esc 没反应；ui/Popover.vue 早前已用 @keydown.esc 单独兜底过）。
//
// 这里改成显式栈：谁最后打开谁在栈顶，Esc 只关它。确定性来自「注册顺序」，不依赖 Reka 内部的
// 时序推断，所以「弹窗里开下拉」「下拉里再开浮层」都能一层一层退出来——
// 这也是当初不敢只给 Dialog 加 @keydown.esc 的原因（那样会一把关掉整个弹窗，甚至在下拉
// 自身也不响应 Esc 时彻底卡死）。
//
// 用捕获阶段 + stopPropagation：本栈已经决定了该关谁，不需要其它（失效的）链路再插手，
// 也避免同一次按键被内外两层各关一次。
type EscLayer = { id: symbol; close: () => void }

const stack: EscLayer[] = []
let listening = false

function onKeydown(e: KeyboardEvent) {
  if (e.key !== 'Escape' || e.defaultPrevented) return
  const top = stack[stack.length - 1]
  if (!top) return
  e.preventDefault()
  e.stopPropagation()
  top.close()
}

/**
 * 把一个「打开即应响应 Esc」的浮层登记进栈。
 *
 * @param isOpen 该浮层的打开状态（ref 或 computed 均可）
 * @param close  关闭回调；每次打开都会刷新，不必担心闭包过期
 */
export function useEscClose(isOpen: Ref<boolean> | ComputedRef<boolean>, close: () => void) {
  const entry: EscLayer = { id: Symbol('esc-layer'), close }
  const at = () => stack.indexOf(entry)
  const remove = () => {
    const i = at()
    if (i >= 0) stack.splice(i, 1)
  }
  watch(isOpen, (v) => {
    if (v) {
      entry.close = close
      if (at() < 0) stack.push(entry)
      if (!listening) {
        window.addEventListener('keydown', onKeydown, true)
        listening = true
      }
    } else {
      remove()
    }
  }, { immediate: true })
  // 组件卸载（含弹窗被 v-if 摘掉）时不能把死条目留在栈里，否则 Esc 会永远关一个不存在的层、
  // 且真正的顶层浮层再也收不到按键。
  onScopeDispose(remove)
}
