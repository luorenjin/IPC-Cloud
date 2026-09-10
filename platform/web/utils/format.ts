/**
 * 通用格式化工具 —— 全站唯一实现。
 *
 * 建立此模块前，fmtTime 在 5 个页面、ago 在 3 个页面、fmtSchedule 在 3 个页面
 * 各自复制了一份，空值兜底还不一致（有的 '-' 有的 '—'）。新增页面一律从这里引入，
 * 不要再就地实现。
 *
 * 空值统一返回 EMPTY（全角破折号 '—'），不使用 '-' / '默认' / '---' 等兜底。
 */

/** 空值占位符：全站统一，不要就地写字面量 */
export const EMPTY = '—'

/** 后端时间戳既有毫秒数也有 ISO 字符串，统一归一化为毫秒数；无法解析返回 null */
function toMillis(ts: unknown): number | null {
  if (ts == null || ts === '' || ts === 0) return null
  const n = typeof ts === 'string' ? Date.parse(ts) : Number(ts)
  return Number.isFinite(n) ? n : null
}

/** 空值兜底：null/undefined/空字符串 → '—' */
export function dash(v: unknown): string {
  if (v == null) return EMPTY
  const s = String(v).trim()
  return s === '' ? EMPTY : s
}

/** 绝对时间：本地化日期时间，如 2026/9/10 15:04:05 */
export function fmtTime(ts: unknown): string {
  const ms = toMillis(ts)
  return ms == null ? EMPTY : new Date(ms).toLocaleString()
}

/** 仅时:分（24 小时制），用于告警流等紧凑场景 */
export function fmtHm(ts: unknown): string {
  const ms = toMillis(ts)
  if (ms == null) return EMPTY
  return new Date(ms).toLocaleTimeString('zh-CN', { hour12: false, hour: '2-digit', minute: '2-digit' })
}

/**
 * 相对时间：刚刚 / N 分钟前 / N 小时前 / N 天前。
 * 未来时间（设备时钟偏快导致的负数差值）按「刚刚」处理而非显示负数。
 */
export function ago(ts: unknown): string {
  const ms = toMillis(ts)
  if (ms == null) return EMPTY
  const s = Math.floor((Date.now() - ms) / 1000)
  if (!Number.isFinite(s)) return EMPTY
  if (s < 60) return '刚刚'
  if (s < 3600) return Math.floor(s / 60) + ' 分钟前'
  if (s < 86400) return Math.floor(s / 3600) + ' 小时前'
  return Math.floor(s / 86400) + ' 天前'
}

/** 星期名：schedule.days 用 1–7 表示周一至周日 */
export const DAY_NAMES = ['周一', '周二', '周三', '周四', '周五', '周六', '周日'] as const

export interface Schedule {
  days?: number[]
  ranges?: string[][]
}

/**
 * 布防/录像时段摘要：{days:[1,2],ranges:[['00:00','24:00']]} → '周一、周二 00:00-24:00'。
 * empty 由调用方给出——告警规则页说「未布防」，模板页说「未设置」，语义不同。
 */
export function fmtSchedule(s: Schedule | null | undefined, empty = '未设置'): string {
  if (!s || !s.days?.length) return empty
  const days = [...s.days]
    .sort((a, b) => a - b)
    .map((d) => DAY_NAMES[d - 1] || d)
    .join('、')
  const ranges = (s.ranges || []).map((r) => `${r[0]}-${r[1]}`).join('、')
  return ranges ? `${days} ${ranges}` : days
}

/** 字节数 → 人类可读（用于存储用量 REC-07） */
export function fmtBytes(n: unknown): string {
  // 注意 Number(null) === 0、Number('') === 0：必须先挡空值，
  // 否则"无数据"会被渲染成 "0 B"，属于假数据兜底。
  if (n == null || n === '') return EMPTY
  const v = Number(n)
  if (!Number.isFinite(v) || v < 0) return EMPTY
  if (v === 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB']
  const i = Math.min(units.length - 1, Math.floor(Math.log(v) / Math.log(1024)))
  const val = v / Math.pow(1024, i)
  return `${val >= 100 || i === 0 ? Math.round(val) : val.toFixed(1)} ${units[i]}`
}

/** 带宽速率 B/s → 人类可读（媒体节点出入带宽）。与 fmtBytes 的区别仅在单位后缀 */
export function fmtRate(bytesPerSec: unknown): string {
  if (bytesPerSec == null || bytesPerSec === '') return EMPTY // 同 fmtBytes：空值不得显示为 0 B/s
  const v = Number(bytesPerSec)
  if (!Number.isFinite(v) || v < 0) return EMPTY
  if (v === 0) return '0 B/s'
  const units = ['B/s', 'KB/s', 'MB/s', 'GB/s']
  const i = Math.min(units.length - 1, Math.floor(Math.log(v) / Math.log(1024)))
  const val = v / Math.pow(1024, i)
  return `${i === 0 ? Math.round(val) : val.toFixed(1)} ${units[i]}`
}

/** 码率 bps → Mbps/Kbps（直播工具条 LIVE-01） */
export function fmtBitrate(bps: unknown): string {
  const v = Number(bps)
  if (!Number.isFinite(v) || v <= 0) return EMPTY
  if (v >= 1_000_000) return (v / 1_000_000).toFixed(1) + ' Mbps'
  if (v >= 1000) return Math.round(v / 1000) + ' Kbps'
  return Math.round(v) + ' bps'
}

/** 秒数 → 时长 00:12:34（回放进度、状态持续时长） */
export function fmtDuration(sec: unknown): string {
  if (sec == null || sec === '') return EMPTY // Number(null)===0，空值不得显示为 00:00:00
  const v = Number(sec)
  if (!Number.isFinite(v) || v < 0) return EMPTY
  const s = Math.floor(v)
  const hh = Math.floor(s / 3600)
  const mm = Math.floor((s % 3600) / 60)
  const ss = s % 60
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${pad(hh)}:${pad(mm)}:${pad(ss)}`
}

/** 百分比：附加 % 号，非数值返回 '—' */
export function fmtPercent(v: unknown): string {
  if (v == null) return EMPTY
  const n = typeof v === 'number' ? v : parseFloat(String(v))
  return Number.isFinite(n) ? n + '%' : dash(v)
}

/** 温度：附加 ℃，非数值返回 '—' */
export function fmtTemp(v: unknown): string {
  if (v == null) return EMPTY
  const n = typeof v === 'number' ? v : parseFloat(String(v))
  return Number.isFinite(n) ? n + '℃' : dash(v)
}
