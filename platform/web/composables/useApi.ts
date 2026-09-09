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

  // 401 统一兜底：刷新成功则由调用方 retry() 重试原请求；刷新失败则清 token 并跳转登录（带并发去重）。
  // 返回 true 表示已重试且拿到结果（写入 outResult.value），false 表示未处理（调用方按原样抛出原错误）。
  async function handleUnauthorized<T>(
    e: any,
    retry: () => Promise<T>,
    outResult: { value?: T }
  ): Promise<boolean> {
    if (!((e?.status === 401 || e?.response?.status === 401) && !redirecting)) return false
    if (await doRefresh()) {
      outResult.value = await retry()
      return true
    }
    if (import.meta.client) {
      redirecting = true
      navigateTo('/login?redirect=' + encodeURIComponent(useRoute().fullPath))
      setTimeout(() => { redirecting = false }, 3000)
    }
    return false
  }

  async function request<T = any>(url: string, opts: any = {}): Promise<T> {
    const headers: Record<string, string> = { ...(opts.headers || {}) }
    if (token().value) headers.Authorization = `Bearer ${token().value}`
    if (project().value) headers['X-Project-Id'] = project().value
    try {
      return await $fetch<T>('/api/v1' + url, { ...opts, headers, retry: 0 })
    } catch (e: any) {
      const out: { value?: T } = {}
      if (await handleUnauthorized(e, () => request<T>(url, opts), out)) return out.value as T
      const body = e?.data || {}
      const err: ApiError = {
        code: body.code || 'E5000',
        msg: body.msg || e?.message || '网络错误',
        suggest: body.suggest
      }
      throw err
    }
  }

  // 鉴权下载：带 Authorization/X-Project-Id 取 Blob，触发保存后释放 URL；文件名优先取响应头 Content-Disposition。
  async function download(path: string, params?: any, filename?: string): Promise<void> {
    async function doFetch(): Promise<Blob> {
      const headers: Record<string, string> = {}
      if (token().value) headers.Authorization = `Bearer ${token().value}`
      if (project().value) headers['X-Project-Id'] = project().value
      return $fetch<Blob>('/api/v1' + path, {
        params,
        headers,
        responseType: 'blob',
        retry: 0,
        onResponse({ response }) {
          disposition = response.headers.get('content-disposition') || ''
        }
      })
    }
    let disposition = ''
    let blob: Blob
    try {
      blob = await doFetch()
    } catch (e: any) {
      const out: { value?: Blob } = {}
      if (await handleUnauthorized(e, doFetch, out)) {
        blob = out.value as Blob
      } else {
        const body = e?.data || {}
        const err: ApiError = {
          code: body.code || 'E5000',
          msg: body.msg || e?.message || '网络错误',
          suggest: body.suggest
        }
        throw err
      }
    }
    const match = /filename\*?=(?:UTF-8'')?"?([^";]+)"?/i.exec(disposition)
    const name = filename || (match ? decodeURIComponent(match[1]) : 'download')
    const url = URL.createObjectURL(blob)
    try {
      const a = document.createElement('a')
      a.href = url
      a.download = name
      document.body.appendChild(a)
      a.click()
      a.remove()
    } finally {
      URL.revokeObjectURL(url)
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
    download,
    project: project
  }
}
