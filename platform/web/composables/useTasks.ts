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

export function useTasks() {
  const tasks = useState<TaskItem[]>('tasks', () => [])
  const open = useState('taskDrawer', () => false)

  function upsert(t: Partial<TaskItem> & { id: string }) {
    const i = tasks.value.findIndex((x) => x.id === t.id)
    if (i >= 0) tasks.value[i] = { ...tasks.value[i], ...t } as TaskItem
    else tasks.value.unshift({ type: 'generic', title: t.id, status: 'running', progress: 0, createdAt: Date.now(), ...t } as TaskItem)
    if (tasks.value.length > 50) tasks.value.splice(50)
  }

  function running() {
    return tasks.value.filter((t) => t.status === 'running' || t.status === 'pending').length
  }

  return { tasks, open, upsert, running }
}
