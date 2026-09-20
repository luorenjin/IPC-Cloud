// 全局会话守卫：未登录访问非白名单路由时跳转登录页并带上 redirect。
const WHITELIST = ['/login', '/scan']

export default defineNuxtRouteMiddleware((to) => {
  if (WHITELIST.includes(to.path)) return
  if (import.meta.server) return
  const { getToken } = useAuth()
  if (!getToken()) {
    return navigateTo('/login?redirect=' + encodeURIComponent(to.fullPath))
  }
})
