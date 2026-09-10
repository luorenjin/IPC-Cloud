// 任务中心（P-18：发现/导入/升级/下载长任务统一入口，进度经 WS 更新，可离开页面）
export interface TaskItem {
  id: string
  type: string
  title: string
  status: 'pending' | 'running' | 'success' | 'failed' | 'partial'
  progress: number
  detail?: string
  createdAt: number
}

/** 后端 models.Task 的 result 是自由 JSONB，标题/详情按约定字段取，缺失时回落到类型名 */
function fromServer(t: any): TaskItem {
  const r = t?.result || {}
  return {
    id: t.id,
    type: t.type || 'generic',
    title: r.title || TASK_TYPE_MAP[t.type] || t.type || t.id,
    status: t.status || 'running',
    progress: Number(t.progress) || 0,
    detail: r.detail || r.note || undefined,
    createdAt: Number(t.createdAt) || Date.now()
  }
}

export function useTasks() {
  const api = useApi()
  const tasks = useState<TaskItem[]>('tasks', () => [])
  const open = useState('taskDrawer', () => false)
  const loading = useState('taskLoading', () => false)
  const loaded = useState('taskLoaded', () => false)

  function upsert(t: Partial<TaskItem> & { id: string }) {
    const i = tasks.value.findIndex((x) => x.id === t.id)
    if (i >= 0) tasks.value[i] = { ...tasks.value[i], ...t } as TaskItem
    else tasks.value.unshift({ type: 'generic', title: t.id, status: 'running', progress: 0, createdAt: Date.now(), ...t } as TaskItem)
    if (tasks.value.length > 50) tasks.value.splice(50)
  }

  /** 拉取服务端任务列表。抽屉打开时调用，避免常驻轮询。 */
  async function load() {
    loading.value = true
    try {
      const res: any = await api.get('/tasks')
      tasks.value = (res?.items || []).map(fromServer)
      loaded.value = true
    } catch (e: any) {
      // 任务中心是辅助功能，失败给出可见反馈但不打断当前操作
      toastApiError(e, '加载任务列表失败')
    } finally {
      loading.value = false
    }
  }

  /**
   * 处理 WS task.progress 事件。
   * 后端广播的字段是 taskId（见 engine.taskProgress 与 records.go），
   * 不是 id——早期前端读 d.id 导致事件永远落空。
   */
  function applyEvent(d: any) {
    const id = d?.taskId || d?.id
    if (!id) return
    upsert({
      id,
      type: d.type || 'generic',
      title: d.title || TASK_TYPE_MAP[d.type] || id,
      status: d.status,
      progress: Number(d.progress) || 0,
      detail: d.detail || undefined
    })
  }

  function running() {
    return tasks.value.filter((t) => t.status === 'running' || t.status === 'pending').length
  }

  return { tasks, open, loading, loaded, upsert, load, applyEvent, running }
}
