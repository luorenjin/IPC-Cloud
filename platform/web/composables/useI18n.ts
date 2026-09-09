// 轻量 i18n（PRD §6.6：中文默认/英文；ACC-09 语言切换）。
// 页面级文案在重构中逐步接入；此处提供基础设施与通用词条。
export type Locale = 'zh-CN' | 'en'

const messages: Record<Locale, Record<string, string>> = {
  'zh-CN': {
    'app.name': 'IpcCloud 视频管理平台',
    'nav.dashboard': '总览',
    'nav.devices': '设备',
    'nav.video': '视频',
    'nav.live': '实时预览',
    'nav.playback': '录像回放',
    'nav.alarms': '告警',
    'nav.messageCenter': '消息中心',
    'nav.alarmRules': '告警规则',
    'nav.alarmTemplates': '布防模板',
    'nav.plan': '计划',
    'nav.recordPlans': '录像计划',
    'nav.recordTemplates': '计划模板',
    'nav.system': '系统',
    'nav.projects': '项目与分组',
    'nav.roles': '角色与成员',
    'nav.nodes': '媒体节点',
    'nav.settings': '系统设置',
    'nav.audit': '操作日志',
    'nav.tasks': '任务中心',
    'common.search': '搜索',
    'common.add': '添加',
    'common.edit': '编辑',
    'common.delete': '删除',
    'common.save': '保存',
    'common.cancel': '取消',
    'common.confirm': '确定',
    'common.export': '导出',
    'common.sync': '同步',
    'common.retry': '重试',
    'common.diagnose': '诊断',
    'common.online': '在线',
    'common.offline': '离线',
    'common.all': '全部',
    'common.noData': '暂无数据',
    'common.total': '共',
    'common.items': '条',
    'user.profile': '个人中心',
    'user.logout': '退出登录',
    'user.language': '界面语言'
  },
  en: {
    'app.name': 'IpcCloud Video Platform',
    'nav.dashboard': 'Overview',
    'nav.devices': 'Devices',
    'nav.video': 'Video',
    'nav.live': 'Live View',
    'nav.playback': 'Playback',
    'nav.alarms': 'Alarms',
    'nav.messageCenter': 'Messages',
    'nav.alarmRules': 'Alarm Rules',
    'nav.alarmTemplates': 'Guard Templates',
    'nav.plan': 'Plans',
    'nav.recordPlans': 'Record Plans',
    'nav.recordTemplates': 'Plan Templates',
    'nav.system': 'System',
    'nav.projects': 'Projects & Groups',
    'nav.roles': 'Roles & Members',
    'nav.nodes': 'Media Nodes',
    'nav.settings': 'Settings',
    'nav.audit': 'Audit Log',
    'nav.tasks': 'Tasks',
    'common.search': 'Search',
    'common.add': 'Add',
    'common.edit': 'Edit',
    'common.delete': 'Delete',
    'common.save': 'Save',
    'common.cancel': 'Cancel',
    'common.confirm': 'Confirm',
    'common.export': 'Export',
    'common.sync': 'Sync',
    'common.retry': 'Retry',
    'common.diagnose': 'Diagnose',
    'common.online': 'Online',
    'common.offline': 'Offline',
    'common.all': 'All',
    'common.noData': 'No data',
    'common.total': 'Total',
    'common.items': 'items',
    'user.profile': 'Profile',
    'user.logout': 'Sign out',
    'user.language': 'Language'
  }
}

export function useI18n() {
  const locale = useCookie<Locale>('ipc_locale', { maxAge: 60 * 60 * 24 * 365, default: () => 'zh-CN' })
  function t(key: string): string {
    return messages[locale.value]?.[key] ?? messages['zh-CN'][key] ?? key
  }
  function setLocale(l: Locale) { locale.value = l }
  return { locale, t, setLocale }
}
