// 轻量 i18n（PRD §6.6：中文默认/英文；ACC-09 语言切换）。
//
// 词条按模块拆分在 locales/<locale>/*.ts 中，键约定为「页面.区块.用途」：
//   nav.*      侧栏与顶栏导航
//   common.*   跨页面复用的动词与通用名词（确定/取消/保存…）
//   device.*   设备列表与设备详情
//   alarm.*    消息中心、告警规则、布防模板、告警策略
//   record.*   录像计划与计划模板
//   live.*     实时预览与录像回放
//   system.*   项目分组、角色成员、媒体节点、系统设置、操作日志
//   account.*  登录、个人中心、扫码、向导
//
// 新增文案一律先加词条再引用，不要在页面里写死中文。
import zhCN from '~/locales/zh-CN'
import en from '~/locales/en'

export type Locale = 'zh-CN' | 'en'

const messages: Record<Locale, Record<string, string>> = {
  'zh-CN': zhCN,
  en
}

/** 占位符插值：t('device.selected', { n: 3 }) → "已选 3 台" */
function interpolate(tpl: string, params?: Record<string, any>): string {
  if (!params) return tpl
  return tpl.replace(/\{(\w+)\}/g, (m, k) => (params[k] === undefined || params[k] === null ? m : String(params[k])))
}

export function useI18n() {
  const locale = useCookie<Locale>('ipc_locale', { maxAge: 60 * 60 * 24 * 365, default: () => 'zh-CN' })

  /**
   * 取词条。缺键时按 en → zh-CN → 键名 回落，
   * 回落到键名是刻意的：界面上出现 `device.list.title` 这样的字符串
   * 比静默显示空白更容易发现漏翻。
   */
  function t(key: string, params?: Record<string, any>): string {
    const hit = messages[locale.value]?.[key] ?? messages['zh-CN'][key] ?? key
    return interpolate(hit, params)
  }

  function setLocale(l: Locale) { locale.value = l }

  return { locale, t, setLocale }
}
