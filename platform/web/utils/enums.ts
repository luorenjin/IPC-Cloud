/**
 * 枚举与判别值映射 —— 全站唯一权威来源。
 *
 * 根 CLAUDE.md 点名的四个协议来源字面判别值（idp / gb28181 / onvif / rtsp）
 * 此前在 3 个页面各写了一份形态不一的映射，告警类型映射也在 2 处靠注释「人工同步」。
 * 一律从这里引入，不要就地再写。
 *
 * 颜色语义固定于 PRD §9.1（自有=信号青 / 国标=成功绿 / ONVIF=紫 / RTSP=中性灰），
 * 改动前先确认 assets/css/main.css 的 src-* 令牌。
 */
import { EMPTY } from './format'

/** UiTag 的 color 取值 */
export type TagColor = 'default' | 'primary' | 'success' | 'warning' | 'danger' | 'info' | 'idp' | 'gb' | 'onvif' | 'rtsp'

// ---------- 协议来源（PRD §9.1） ----------

/** 后端 device.source 的四个字面判别值；顺序即界面展示顺序 */
export const SOURCES = ['idp', 'gb28181', 'onvif', 'rtsp'] as const
export type Source = (typeof SOURCES)[number]

export interface SourceMeta {
  /** 表格/标签里的短名 */
  label: string
  /** 仪表盘等处的完整名 */
  longLabel: string
  /** UiTag color */
  color: TagColor
  /** CSS 变量，用于非 UiTag 场景（图表、分布条、状态灯） */
  cssVar: string
}

export const SOURCE_MAP: Record<Source, SourceMeta> = {
  idp: { label: '自有', longLabel: '自有设备', color: 'idp', cssVar: 'var(--color-src-idp)' },
  gb28181: { label: '国标', longLabel: '国标 GB/T 28181', color: 'gb', cssVar: 'var(--color-src-gb)' },
  onvif: { label: 'ONVIF', longLabel: 'ONVIF', color: 'onvif', cssVar: 'var(--color-src-onvif)' },
  rtsp: { label: 'RTSP', longLabel: 'RTSP 直连', color: 'rtsp', cssVar: 'var(--color-src-rtsp)' }
}

/** 未知来源不编造展示名，回落为原始值（便于排查后端新增来源未同步前端） */
export function sourceInfo(s?: string): SourceMeta {
  return SOURCE_MAP[s as Source] || { label: s || EMPTY, longLabel: s || EMPTY, color: 'default', cssVar: 'var(--color-info)' }
}

/** 下拉筛选项（含「全部」由调用方自行前置） */
export const SOURCE_OPTIONS = SOURCES.map((v) => ({ value: v, label: SOURCE_MAP[v].label }))

// ---------- 告警事件类型（ALM-02） ----------

/** 设备侧智能事件：受通道规则与布防时段约束 */
export const DEVICE_ALARM_KINDS = ['motion', 'humanoid', 'intrusion', 'linecross', 'tamper', 'io', 'tf_error'] as const
/** 平台侧事件：与通道无关，由平台自身产生 */
export const PLATFORM_ALARM_KINDS = ['device_offline', 'node_offline', 'stream_lost', 'disk_full'] as const

export const ALARM_KIND_MAP: Record<string, string> = {
  motion: '移动侦测',
  humanoid: '人形侦测',
  intrusion: '区域入侵',
  linecross: '越界侦测',
  tamper: '视频遮挡',
  io: 'IO报警',
  tf_error: 'TF卡异常',
  device_offline: '设备离线',
  node_offline: '节点离线',
  stream_lost: '流中断',
  disk_full: '存储不足'
}

export function alarmKindName(k?: string): string {
  return ALARM_KIND_MAP[k || ''] || k || EMPTY
}

/** 告警规则页可选的事件类型（仅设备侧六类，ALM-02） */
export const ALARM_KIND_OPTIONS = ['motion', 'humanoid', 'intrusion', 'linecross', 'tamper', 'io']
  .map((v) => ({ value: v, label: ALARM_KIND_MAP[v] }))

/** 告警级别 */
export const ALARM_LEVEL_MAP: Record<string, { label: string; color: TagColor; cssVar: string }> = {
  error: { label: '严重', color: 'danger', cssVar: 'var(--color-danger)' },
  warn: { label: '警告', color: 'warning', cssVar: 'var(--color-warning)' },
  info: { label: '提示', color: 'info', cssVar: 'var(--color-info)' }
}

export function alarmLevelInfo(l?: string) {
  return ALARM_LEVEL_MAP[l || ''] || ALARM_LEVEL_MAP.info
}

// ---------- 状态 ----------

export interface StatusMeta {
  label: string
  color: TagColor
}

/**
 * 设备状态。注意与节点状态刻意不同：设备在线用 success（绿），
 * 因为设备列表里"在线"是常态、需与主色区分；节点在线用 primary（信号青），
 * 呼应媒体节点是信号链路本身。两者不要合并。
 */
export const DEVICE_STATUS_MAP: Record<string, StatusMeta> = {
  online: { label: '在线', color: 'success' },
  offline: { label: '离线', color: 'info' },
  pending: { label: '待确认', color: 'warning' },
  error: { label: '错误', color: 'danger' }
}

export function deviceStatusInfo(s?: string): StatusMeta {
  return DEVICE_STATUS_MAP[s || ''] || DEVICE_STATUS_MAP.offline
}

/** 媒体节点状态（见 DEVICE_STATUS_MAP 注释说明为何与设备不同） */
export const NODE_STATUS_MAP: Record<string, StatusMeta> = {
  online: { label: '在线', color: 'primary' },
  offline: { label: '离线', color: 'danger' },
  disabled: { label: '已禁用', color: 'info' }
}

export function nodeStatusInfo(s?: string): StatusMeta {
  return NODE_STATUS_MAP[s || ''] || { label: s || '未知', color: 'warning' }
}

/** 通道推流状态 */
export function streamStatusInfo(s?: string): StatusMeta {
  if (s === 'online' || s === 'live') return { label: '推流中', color: 'success' }
  if (!s || s === 'offline') return { label: '未推流', color: 'info' }
  return { label: String(s), color: 'warning' }
}

/** 操作结果（审计日志） */
export function resultInfo(r?: string): StatusMeta {
  if (r === 'success') return { label: '成功', color: 'success' }
  if (r === 'fail') return { label: '失败', color: 'danger' }
  return { label: r || EMPTY, color: 'info' }
}

// ---------- 任务中心（P-18） ----------

export const TASK_STATUS_MAP: Record<string, StatusMeta> = {
  pending: { label: '等待中', color: 'info' },
  running: { label: '进行中', color: 'primary' },
  success: { label: '已完成', color: 'success' },
  partial: { label: '部分成功', color: 'warning' },
  failed: { label: '失败', color: 'danger' }
}

export function taskStatusInfo(s?: string): StatusMeta {
  return TASK_STATUS_MAP[s || ''] || TASK_STATUS_MAP.pending
}

export const TASK_TYPE_MAP: Record<string, string> = {
  discover: '设备发现',
  import: '批量导入',
  ota: '固件升级',
  download: '录像下载'
}
