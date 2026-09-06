// WebSocket 透传插件：Nitro 的 fetch 代理无法承载 WS upgrade（会剥离 Upgrade 头），
// 因此在 node http server 上挂 upgrade 监听，将 /ws/** 请求透传到后端（GVA-37）。
// 目标地址：构建/运行时环境变量 WS_UPSTREAM（默认 http://localhost:8080）。
import http from 'node:http'
import httpProxy from 'http-proxy'

const upstream = process.env.WS_UPSTREAM || 'http://localhost:8080'

export default defineNitroPlugin(() => {
  if (import.meta.dev) return // dev 走 devProxy（ws: true）

  const proxy = httpProxy.createProxyServer({ target: upstream, ws: true })
  proxy.on('error', (err, _req, socket) => {
    console.error('[ws-proxy]', err.message)
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
