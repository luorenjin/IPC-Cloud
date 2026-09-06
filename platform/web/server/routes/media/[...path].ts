// ZLM 媒体文件同源代理（/media/** → MEDIA_ORIGIN）。
// 平台页面带 COEP: require-corp（多线程 WASM 解码硬性要求，PRD §6.4），
// 跨源 <video> 加载 ZLM MP4 会被浏览器阻断（ERR_BLOCKED_BY_RESPONSE…Coep），
// 而 ZLM 无法追加 CORP 响应头，故由平台同源代理；Range 头透传以支持点播拖动。
import http from 'node:http'
import { defineEventHandler, getRequestURL } from 'h3'

const upstream = process.env.MEDIA_ORIGIN || 'http://127.0.0.1:8081'

export default defineEventHandler((event) => {
  const req = event.node.req
  const res = event.node.res
  // 仅代理静态点播路径，避免成为任意内网代理
  const url = getRequestURL(event)
  if (!url.pathname.startsWith('/media/record/')) {
    res.statusCode = 404
    res.end('not found')
    return
  }
  const target = new URL(upstream.replace(/\/$/, '') + url.pathname.slice('/media'.length) + url.search)

  return new Promise<void>((resolve) => {
    const ureq = http.request(
      {
        hostname: target.hostname,
        port: target.port || 80,
        path: target.pathname + target.search,
        method: req.method,
        headers: req.headers.range ? { Range: req.headers.range } : {}
      },
      (ures) => {
        const headers: Record<string, string> = {}
        for (const h of ['content-type', 'content-length', 'content-range', 'accept-ranges']) {
          const v = ures.headers[h]
          if (typeof v === 'string') headers[h] = v
        }
        res.writeHead(ures.statusCode || 502, headers)
        ures.pipe(res)
        ures.on('end', () => resolve())
        ures.on('error', () => resolve())
      }
    )
    ureq.on('error', () => {
      if (!res.headersSent) {
        res.statusCode = 502
        res.setHeader('content-type', 'text/plain; charset=utf-8')
        res.end('media upstream error')
      } else {
        res.end()
      }
      resolve()
    })
    ureq.end()
  })
})
