// WebSocket 实时事件（设备状态/告警/任务进度）。
export function useWs(onEvent: (ev: any) => void) {
  let ws: WebSocket | null = null
  let closed = false
  let retryMs = 2000

  function connect() {
    const token = useCookie('ipc_token').value
    const projectId = useCookie('ipc_project').value || ''
    if (!token || closed) return
    const proto = location.protocol === 'https:' ? 'wss' : 'ws'
    ws = new WebSocket(`${proto}://${location.host}/ws/v1/events?token=${token}&projectId=${projectId}`)
    ws.onmessage = (m) => {
      try {
        const ev = JSON.parse(m.data)
        if (ev.type !== 'connected') onEvent(ev)
      } catch {}
    }
    ws.onclose = () => {
      if (!closed) {
        setTimeout(connect, retryMs)
        retryMs = Math.min(retryMs * 1.5, 15000)
      }
    }
    ws.onopen = () => { retryMs = 2000 }
  }

  onMounted(connect)
  onBeforeUnmount(() => {
    closed = true
    ws?.close()
  })
}
