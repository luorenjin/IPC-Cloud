/**
 * WebSocket 实时事件（设备状态 / 告警 / 任务进度）—— 单连接多订阅。
 *
 * 此前每次 useWs() 都新建一条连接，布局 + 页面同时使用即多条并存，且：
 * - 重连定时器在组件卸载后仍会触发（closed 只挡了回调注册，没清 timer）；
 * - 切换项目后连接仍带旧 projectId，收不到新项目事件。
 *
 * 现在整个应用共用一条连接：订阅者集合为空时关闭，非空时按需建立；
 * 项目变化时主动重连。调用方只需 useWs(handler)，卸载自动退订。
 */

type Handler = (ev: any) => void

const handlers = new Set<Handler>()

let ws: WebSocket | null = null
let retryTimer: ReturnType<typeof setTimeout> | null = null
let idleCloseTimer: ReturnType<typeof setTimeout> | null = null
let failCount = 0
/** 当前连接使用的 projectId，用于判断项目切换后是否需要重连 */
let connectedProject: string | null = null

function clearRetry() {
  if (retryTimer) {
    clearTimeout(retryTimer)
    retryTimer = null
  }
}

function closeSocket() {
  clearRetry()
  if (idleCloseTimer) {
    clearTimeout(idleCloseTimer)
    idleCloseTimer = null
  }
  const old = ws
  ws = null
  connectedProject = null
  if (old) {
    // 先摘掉 onclose 再关，避免触发重连逻辑
    old.onclose = null
    old.onmessage = null
    old.onerror = null
    old.onopen = null
    try { old.close() } catch {}
  }
}

function scheduleRetry() {
  clearRetry()
  if (!handlers.size) return
  failCount++
  // 前两次快速重试，之后退避到 30s，避免后端长时间不可用时刷屏
  const delay = failCount > 2 ? 30000 : Math.min(3000 * Math.pow(1.5, failCount - 1), 15000)
  retryTimer = setTimeout(connect, delay)
}

/**
 * 延迟关闭：订阅者归零后等一小段时间再真正断开。
 * 路由切换时旧页面先卸载、新页面后挂载，中间订阅者会短暂为空——
 * 若立即断开，新页面挂载时又要重连，这段空窗期的事件就丢了。
 * 若期间有新订阅者加入，关闭被取消。
 */
function scheduleIdleClose() {
  if (idleCloseTimer) clearTimeout(idleCloseTimer)
  idleCloseTimer = setTimeout(() => {
    idleCloseTimer = null
    if (!handlers.size) closeSocket()
  }, 5000)
}

/**
 * 等待会话就绪（cookie 尚未写入）时的短间隔重试。
 * 与 scheduleRetry 分开：这不是"连接失败"，不应计入退避，
 * 否则登录后要等 30s 才连上。
 */
function scheduleSessionPoll() {
  clearRetry()
  if (!handlers.size) return
  retryTimer = setTimeout(connect, 500)
}

function connect() {
  clearRetry()
  if (!handlers.size) return
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return

  const { getToken, getProjectId } = useAuth()
  const token = getToken()
  if (!token) {
    // 会话尚未就绪（登录跳转后 cookie 还没写入、或刚刷新页面）。
    // 必须排重试，否则这条连接再也不会建立——整个应用只有一条连接，
    // 不像从前每次路由切换都会由新组件重新尝试。
    scheduleSessionPoll()
    return
  }
  const projectId = getProjectId() || ''

  const proto = location.protocol === 'https:' ? 'wss' : 'ws'
  try {
    const sock = new WebSocket(`${proto}://${location.host}/ws/v1/events?token=${token}&projectId=${projectId}`)
    ws = sock
    connectedProject = projectId
    sock.onopen = () => { failCount = 0 }
    sock.onmessage = (m) => {
      let ev: any
      try {
        ev = JSON.parse(m.data)
      } catch {
        return // 非 JSON 帧忽略
      }
      if (ev?.type === 'connected') return
      // 复制一份再遍历：处理函数里可能退订，直接遍历会漏掉后续订阅者
      for (const h of [...handlers]) {
        try {
          h(ev)
        } catch (e) {
          // 单个订阅者抛错不能影响其他订阅者
          console.error('[useWs] 事件处理失败', e)
        }
      }
    }
    sock.onerror = () => {
      // 静默：紧随其后的 onclose 负责重连
    }
    sock.onclose = () => {
      if (ws === sock) {
        ws = null
        connectedProject = null
        scheduleRetry()
      }
    }
  } catch {
    scheduleRetry()
  }
}

/** 项目切换后带着旧 projectId 的连接收不到新项目事件，需重连 */
function reconnectIfProjectChanged() {
  if (!handlers.size) return
  const { getProjectId } = useAuth()
  const pid = getProjectId() || ''
  if (ws && connectedProject !== null && connectedProject !== pid) {
    closeSocket()
    connect()
  }
}

/**
 * 订阅实时事件。组件卸载时自动退订；最后一个订阅者退订后关闭连接。
 * 在组件 setup 中调用（依赖 onMounted / onBeforeUnmount）。
 */
export function useWs(onEvent: Handler) {
  let stopWatch: (() => void) | null = null

  onMounted(() => {
    // 取消上一个页面卸载时排下的延迟关闭——连接要继续复用
    if (idleCloseTimer) {
      clearTimeout(idleCloseTimer)
      idleCloseTimer = null
    }
    handlers.add(onEvent)
    failCount = 0
    connect()

    // 项目切换时重连（currentProject 由 useAuth 持有）
    const { currentProject } = useAuth()
    stopWatch = watch(currentProject, reconnectIfProjectChanged)
  })

  onBeforeUnmount(() => {
    handlers.delete(onEvent)
    stopWatch?.()
    stopWatch = null
    // 不立即关闭：路由切换时 Vue 先卸载旧页面再挂载新页面，
    // 中间这一瞬 handlers 会短暂归零。若立刻关连接，等新页面挂载时
    // 又得重连，白白丢掉这段时间的事件（表现为切页后收不到实时推送）。
    scheduleIdleClose()
  })
}
