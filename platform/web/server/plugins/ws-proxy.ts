// WebSocket 透传插件：Nitro 的 fetch 代理无法承载 WS upgrade（会剥离 Upgrade 头），
// 因此在 node http server 上挂 upgrade 监听，将 /ws/** 请求透传到后端（GVA-37）。
// 目标地址优先级：WS_UPSTREAM（运行时显式指定）> API_ORIGIN（构建期已烘进镜像的 API 源）
// > http://localhost:8080（本机裸跑默认值）。
// 为什么要有第二档回落：容器里必须把上游指向 server:8080，而它和构建期的 API_ORIGIN 是两处配置，
// 漏配任一处就会回落到容器自身的 localhost:8080 —— 没有监听者，握手被立刻掐断，
// 前端只看到反复重连 + “Connection closed before receiving a handshake response”，日志里毫无线索。
import http from 'node:http'
import httpProxy from 'http-proxy'

const upstream = process.env.WS_UPSTREAM || process.env.API_ORIGIN || 'http://localhost:8080'

// 捕获开发/生产环境中因上游后端未启动或连接断开触发的常见网络重置异常，防止抛出未处理拒绝 (unhandledRejection)
process.on('unhandledRejection', (err: any) => {
  const code = err?.code || err?.cause?.code
  if (code === 'ECONNRESET' || code === 'ECONNREFUSED' || code === 'EPIPE' || String(err?.message || '').includes('ECONNRESET')) {
    return // 忽略上游连接未就绪或浏览器切页重置引起的偶发网络抖动
  }
  console.error('[unhandledRejection]', err)
})

export default defineNitroPlugin(() => {
  if (import.meta.dev) return // dev 走 devProxy（ws: true）

  // 启动即宣告上游地址：排查「WS 连不上」时第一眼就能确认它是不是指错了容器。
  console.log(`[ws-proxy] WS 上游 = ${upstream}`)

  const proxy = httpProxy.createProxyServer({ target: upstream, ws: true })

  // 原先只对「非 ECONNRESET / ECONNREFUSED」打日志，等于把最常见的故障（上游地址配错、
  // 上游没起来 → ECONNREFUSED）静默掉了：容器日志一片干净，前端却每 30s 重连一次。
  // 现在任何转发失败都记一条（含上游地址），按 30s 限流防止刷屏。
  let lastWarnAt = 0
  proxy.on('error', (err, _req, socket) => {
    const now = Date.now()
    if (now - lastWarnAt > 30000) {
      lastWarnAt = now
      const code = (err as any)?.code || ''
      console.warn(`[ws-proxy] 转发至 ${upstream} 失败：${code || err.message}`)
    }
    try { (socket as any)?.destroy?.() } catch { /* ignore */ }
  })

  // Nitro 入口在 useNitroApp() 之后才创建 server 并 listen，
  // 因此通过一次性 patch listen 捕获 server 实例并注册 upgrade 监听。
  const origListen = http.Server.prototype.listen as any
  http.Server.prototype.listen = function (this: http.Server, ...args: any[]) {
    const ret = origListen.apply(this, args)
    this.on('upgrade', (req, socket, head) => {
      if (req.url?.startsWith('/ws/')) {
        proxy.ws(req, socket as any, head)
      }
    })
    http.Server.prototype.listen = origListen
    return ret
  }
})
