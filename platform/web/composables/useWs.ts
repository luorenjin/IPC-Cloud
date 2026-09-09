// WebSocket 实时事件（设备状态/告警/任务进度）。
export function useWs(onEvent: (ev: any) => void) {
  const { getToken, getProjectId } = useAuth()
  let ws: WebSocket | null = null
  let closed = false
  let retryMs = 3000
  let failCount = 0

  function connect() {
    const token = getToken()
    const projectId = getProjectId() || ''
    if (!token || closed) return
    const proto = location.protocol === 'https:' ? 'wss' : 'ws'
    try {
      ws = new WebSocket(`${proto}://${location.host}/ws/v1/events?token=${token}&projectId=${projectId}`)
      ws.onmessage = (m) => {
        try {
          const ev = JSON.parse(m.data)
          if (ev.type !== 'connected') onEvent(ev)
        } catch {}
      }
      ws.onerror = () => {
        // 静默捕获避免打扰控制台
      }
      ws.onclose = () => {
        if (!closed) {
          failCount++
          const delay = failCount > 2 ? 30000 : retryMs
          setTimeout(connect, delay)
          retryMs = Math.min(retryMs * 1.5, 15000)
        }
      }
      ws.onopen = () => {
        retryMs = 3000
        failCount = 0
      }
    } catch {}
  }

  onMounted(connect)
  onBeforeUnmount(() => {
    closed = true
    try { ws?.close() } catch {}
  })
}
