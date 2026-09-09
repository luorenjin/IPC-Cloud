// 生产代理目标：构建时由 API_ORIGIN 指定（本地默认 8080，compose 内为 server:8080）
import tailwindcss from '@tailwindcss/vite'

const apiOrigin = process.env.API_ORIGIN || 'http://localhost:8080'

export default defineNuxtConfig({
  compatibilityDate: '2026-09-07',
  ssr: false,
  experimental: {
    appManifest: false
  },
  devtools: { enabled: false },
  modules: [],
  components: [
    { path: '~/components/ui', prefix: 'ui' },
    { path: '~/components/ui', pathPrefix: false },
    '~/components'
  ],
  css: ['~/assets/css/main.css'],
  vite: {
    plugins: [tailwindcss()]
  },
  app: {
    head: {
      title: 'IpcCloud 视频管理平台',
      meta: [
        { charset: 'utf-8' },
        { name: 'viewport', content: 'width=device-width, initial-scale=1' }
      ],
      link: [
        { rel: 'icon', type: 'image/x-icon', href: '/favicon.ico' },
        { rel: 'icon', type: 'image/svg+xml', href: '/favicon.svg' },
        { rel: 'apple-touch-icon', href: '/apple-touch-icon.png' }
      ],
      script: [
        // h265web.js 由部署时放置于 /vendor/h265web.js（含模块与 wasm 资源）
        { src: '/vendor/h265web.js', async: true, tagPosition: 'head' }
      ]
    }
  },
  // COOP/COEP：多线程 WASM 解码要求（PRD §6.4），不可删除
  nitro: {
    routeRules: {
      '/**': {
        headers: {
          'Cross-Origin-Opener-Policy': 'same-origin',
          'Cross-Origin-Embedder-Policy': 'require-corp'
        }
      },
      '/api/v1/**': { proxy: `${apiOrigin}/api/v1/**` },
      '/ws/v1/**': { proxy: `${apiOrigin}/ws/v1/**`, ws: true },
      '/hooks/zlm/**': { proxy: `${apiOrigin}/hooks/zlm/**` }
    },
    devProxy: {
      '/api/v1': { target: 'http://localhost:8080/api/v1', changeOrigin: true },
      '/ws/v1': { target: 'ws://localhost:8080/ws/v1', ws: true },
      '/hooks/zlm': { target: 'http://localhost:8080/hooks/zlm', changeOrigin: true }
    }
  }
})
