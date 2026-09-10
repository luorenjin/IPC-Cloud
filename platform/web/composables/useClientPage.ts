/**
 * 客户端分页。
 *
 * 角色/成员/节点/录像计划/计划模板/告警规则等列表接口目前一次性返回全部数据
 * （后端无分页参数），页面此前把它们全量渲染，条目一多就卡。
 * 这里在前端切片，只解决"渲染多少行"的问题——真正的服务端分页需后端配合，
 * 属于后续工作，不在本阶段范围。
 *
 * 用法：
 *   const pg = useClientPage(items)          // items 为 Ref<any[]>
 *   <UiTable :rows="pg.pageItems.value" />
 *   <UiPagination v-model:page="pg.page.value" v-model:page-size="pg.pageSize.value" :total="pg.total.value" />
 */
export function useClientPage<T>(source: Ref<T[]>, defaultSize = 20) {
  const page = ref(1)
  const pageSize = ref(defaultSize)

  const total = computed(() => source.value?.length || 0)
  const pages = computed(() => Math.max(1, Math.ceil(total.value / pageSize.value)))

  const pageItems = computed(() => {
    const start = (page.value - 1) * pageSize.value
    return (source.value || []).slice(start, start + pageSize.value)
  })

  // 数据变短（删除/筛选）后当前页可能越界，回落到最后一页
  watch([total, pageSize], () => {
    if (page.value > pages.value) page.value = pages.value
  })

  /** 数据源整体替换时（重新加载/切换筛选）回到第一页 */
  function reset() {
    page.value = 1
  }

  return { page, pageSize, total, pages, pageItems, reset }
}
