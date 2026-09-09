// 会话与项目状态。
export function useAuth() {
  const api = useApi()
  const user = useState<any>('user', () => null)
  const projects = useState<any[]>('projects', () => [])
  const currentProject = useState<any>('currentProject', () => null)
  const setupDone = useState<boolean>('setupDone', () => true)

  async function loadMe() {
    try {
      const res: any = await api.get('/me')
      user.value = res.user
      projects.value = res.projects || []
      const pid = api.project().value
      currentProject.value =
        projects.value.find((p: any) => p.id === pid) || projects.value[0] || null
      if (currentProject.value) api.project().value = currentProject.value.id
      setupDone.value = !!res.setupDone
    } catch {
      // 未登录
    }
  }

  async function login(username: string, password: string, remember: boolean) {
    const res: any = await api.post('/auth/login', { username, password, remember })
    useCookie('ipc_token', { maxAge: 60 * 60 * 2 }).value = res.accessToken
    useCookie('ipc_refresh', { maxAge: remember ? 60 * 60 * 24 * 30 : 60 * 60 * 24 * 7 }).value =
      res.refreshToken
    await loadMe()
  }

  function switchProject(p: any) {
    currentProject.value = p
    api.project().value = p.id
  }

  async function logout() {
    try { await api.post('/auth/logout') } catch {}
    useCookie('ipc_token').value = null
    useCookie('ipc_refresh').value = null
    user.value = null
    navigateTo('/login')
  }

  // ipc_token / ipc_project 的唯一读取点，供 useApi、useWs 等复用。
  function getToken(): string | null | undefined {
    return useCookie('ipc_token').value
  }
  function getProjectId(): string | null | undefined {
    return useCookie('ipc_project').value
  }

  return {
    user, projects, currentProject, setupDone,
    loadMe, login, switchProject, logout,
    getToken, getProjectId
  }
}
