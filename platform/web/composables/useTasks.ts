// 任务中心（P-18：发现/导入/升级/下载长任务统一入口，进度经 WS 更新，可离开页面）
// 数据以服务端为准：抽屉打开时拉取列表，WS 仅做增量合并。
export interface TaskFile {
  start: number
  end: number
  size: number
  url: string
}

export interface TaskItem {
  id: string
  type: string
  title: string
  status: 'pending' | 'running' | 'success' | 'failed' | 'partial' | 'canceled'
  progress: number
  detail?: string
  result?: Record<string, any>
  createdAt: number
  updatedAt?: number
}

/** 任务是否已结束（终态可删除，非终态可取消） */
export function isTaskFinal(status?: string) {
  return status === 'success' || status === 'failed' || status === 'partial' || status === 'canceled'
}

/** 状态对应的标签颜色 */
export function taskTagColor(status?: string) {
  if (status === 'success') return 'success'
  if (status === 'failed') return 'danger'
  if (status === 'partial') return 'warning'
  if (status === 'canceled') return 'default'
  return 'primary'
}

/** 状态中文文案 */
export function taskStatusText(status?: string) {
  return (
    {
      pending: '等待中',
      running: '进行中',
      success: '已完成',
      partial: '部分成功',
      failed: '失败',
      canceled: '已取消'
    }[status || ''] || status || '-'
  )
}

/** 任务类型中文文案 */
export function taskTypeText(type?: string) {
  return (
    {
      download: '录像下载',
      discover: '设备发现',
      import: '批量导入',
      upgrade: '固件升级',
      export: '数据导出',
      generic: '常规任务'
    }[type || ''] || type || '-'
  )
}

export function useTasks() {
  const tasks = useState<TaskItem[]>('tasks', () => [])
  const open = useState('taskDrawer', () => false)
  const loading = useState('taskLoading', () => false)
  // 服务端统计的进行中任务数（抽屉只装前 N 条，红点不能只数本地列表）
  const runningCount = useState('taskRunning', () => 0)
  const api = useApi()

  function syncRunning() {
    runningCount.value = tasks.value.filter((t) => t.status === 'running' || t.status === 'pending').length
  }

  function upsert(t: Partial<TaskItem> & { id: string }) {
    const i = tasks.value.findIndex((x) => x.id === t.id)
    if (i >= 0) tasks.value[i] = { ...tasks.value[i], ...t } as TaskItem
    else
      tasks.value.unshift({
        type: 'generic',
        title: t.id,
        status: 'running',
        progress: 0,
        createdAt: Date.now(),
        ...t
      } as TaskItem)
    syncRunning()
  }

  /** 本地移除（删除/清理后同步视图，避免等待重新拉取） */
  function dropLocal(ids: string[]) {
    tasks.value = tasks.value.filter((t) => !ids.includes(t.id))
    syncRunning()
  }

  /** 拉取任务列表（抽屉用，默认最近 20 条） */
  async function load(params: Record<string, any> = {}) {
    loading.value = true
    try {
      const res: any = await api.get('/tasks', { pageSize: 20, ...params })
      tasks.value = res?.items || []
      runningCount.value = Number(res?.running || 0)
      return res
    } finally {
      loading.value = false
    }
  }

  /** 删除单条任务（仅终态可删） */
  async function remove(id: string) {
    await api.del(`/tasks/${id}`)
    dropLocal([id])
  }

  /**
   * 批量清理。scope: finished 已完成 | failed 失败与已取消 | all 全部终态；
   * 进行中的任务永远不会被清理，需先取消。
   */
  async function clear(scope: 'finished' | 'failed' | 'all' = 'all', ids?: string[]) {
    const res: any = await api.post('/tasks/clear', { scope, ids })
    await load()
    return Number(res?.deleted || 0)
  }

  /** 取消进行中的任务 */
  async function cancel(id: string) {
    await api.post(`/tasks/${id}/cancel`, {})
    upsert({ id, status: 'canceled', detail: '已被用户取消' })
  }

  /** 进行中的任务数（顶栏红点） */
  function running() {
    return runningCount.value
  }

  return { tasks, open, loading, runningCount, upsert, dropLocal, load, remove, clear, cancel, running }
}
