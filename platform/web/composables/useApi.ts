// 统一 API 客户端：JWT 自动附带、401 刷新、错误码结构化（MGR-15）。
export interface ApiError {
  code: string
  msg: string
  suggest?: string
}

let refreshing: Promise<boolean> | null = null
let redirecting = false

export function useApi() {
  const token = () => useCookie('ipc_token', { maxAge: 60 * 60 * 2 })
  const refresh = () => useCookie('ipc_refresh', { maxAge: 60 * 60 * 24 * 7 })
  const project = () => useCookie('ipc_project', { maxAge: 60 * 60 * 24 * 30 })

  async function doRefresh(): Promise<boolean> {
    if (!refreshing) {
      refreshing = (async () => {
        try {
          const res: any = await $fetch('/api/v1/auth/refresh', {
            method: 'POST',
            body: { refreshToken: refresh().value }
          })
          token().value = res.accessToken
          refresh().value = res.refreshToken
          return true
        } catch {
          token().value = null
          refresh().value = null
          return false
        } finally {
          refreshing = null
        }
      })()
    }
    return refreshing
  }

  async function request<T = any>(url: string, opts: any = {}): Promise<T> {
    const headers: Record<string, string> = { ...(opts.headers || {}) }
    if (token().value) headers.Authorization = `Bearer ${token().value}`
    if (project().value) headers['X-Project-Id'] = project().value
    try {
      return await $fetch<T>('/api/v1' + url, { ...opts, headers, retry: 0 })
    } catch (e: any) {
      if ((e?.status === 401 || e?.response?.status === 401) && !redirecting) {
        if (await doRefresh()) return request<T>(url, opts)
        if (import.meta.client) {
          redirecting = true
          navigateTo('/login?redirect=' + encodeURIComponent(useRoute().fullPath))
          setTimeout(() => { redirecting = false }, 3000)
        }
      }
      const body = e?.data || {}
      const err: ApiError = {
        code: body.code || 'E5000',
        msg: body.msg || e?.message || '网络错误',
        suggest: body.suggest
      }
      throw err
    }
  }

  return {
    request,
    get: <T = any>(url: string, params?: any) =>
      request<T>(url, { method: 'GET', params }),
    post: <T = any>(url: string, body?: any) =>
      request<T>(url, { method: 'POST', body }),
    put: <T = any>(url: string, body?: any) =>
      request<T>(url, { method: 'PUT', body }),
    del: <T = any>(url: string) => request<T>(url, { method: 'DELETE' }),
    project: project
  }
}
