/**
 * 枚举与判别值映射 —— 全站唯一权威来源。
 *
 * 根 CLAUDE.md 点名的四个协议来源字面判别值（idp / gb28181 / onvif / rtsp）
 * 此前在 3 个页面各写了一份形态不一的映射，告警类型映射也在 2 处靠注释「人工同步」。
 * 一律从这里引入，不要就地再写。
 *
 * 颜色语义固定于 PRD §9.1（自有=信号青 / 国标=成功绿 / ONVIF=紫 / RTSP=中性灰），
 * 改动前先确认 assets/css/main.css 的 src-* 令牌。
 *
 * i18n：本模块是纯函数模块，不能调用 useI18n（那是 composable，依赖 Nuxt 上下文）。
 * 因此每个映射同时给出 labelKey（词条键）与 label（中文兜底）：
 *   - 页面渲染一律用 `t(info.labelKey)`；
 *   - label 仅作为词条缺失时的可读兜底，不要直接渲染到界面。
 * 词条见 locales/<locale>/enums.ts，改键名时两处同步。
 */
import { EMPTY } from './format'

/** UiTag 的 color 取值 */
export type TagColor = 'default' | 'primary' | 'success' | 'warning' | 'danger' | 'info' | 'idp' | 'gb' | 'onvif' | 'rtsp'

// ---------- 协议来源（PRD §9.1） ----------

/** 后端 device.source 的四个字面判别值；顺序即界面展示顺序 */
export const SOURCES = ['idp', 'gb28181', 'onvif', 'rtsp'] as const
export type Source = (typeof SOURCES)[number]

export interface SourceMeta {
  /** 表格/标签里的短名（中文兜底） */
  label: string
  /** 短名词条键 */
  labelKey: string
  /** 仪表盘等处的完整名（中文兜底） */
  longLabel: string
  /** 完整名词条键 */
  longLabelKey: string
  /** UiTag color */
  color: TagColor
  /** CSS 变量，用于非 UiTag 场景（图表、分布条、状态灯） */
  cssVar: string
}

function src(k: Source, label: string, longLabel: string, color: TagColor, cssVar: string): SourceMeta {
  return { label, labelKey: `enum.source.${k}`, longLabel, longLabelKey: `enum.source.${k}.long`, color, cssVar }
}

export const SOURCE_MAP: Record<Source, SourceMeta> = {
  idp: src('idp', '自有', '自有设备', 'idp', 'var(--color-src-idp)'),
  gb28181: src('gb28181', '国标', '国标 GB/T 28181', 'gb', 'var(--color-src-gb)'),
  onvif: src('onvif', 'ONVIF', 'ONVIF', 'onvif', 'var(--color-src-onvif)'),
  rtsp: src('rtsp', 'RTSP', 'RTSP 直连', 'rtsp', 'var(--color-src-rtsp)')
}

/** 未知来源不编造展示名，回落为原始值（便于排查后端新增来源未同步前端） */
export function sourceInfo(s?: string): SourceMeta {
  const hit = SOURCE_MAP[s as Source]
  if (hit) return hit
  const raw = s || EMPTY
  // 未知来源没有词条，labelKey 直接给原始值：t() 缺键时原样返回
  return { label: raw, labelKey: raw, longLabel: raw, longLabelKey: raw, color: 'default', cssVar: 'var(--color-info)' }
}

/** 下拉筛选项（label 为词条键，调用方 t() 后展示） */
export const SOURCE_OPTIONS = SOURCES.map((v) => ({ value: v, labelKey: SOURCE_MAP[v].labelKey }))

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

/** 告警类型词条键；未知类型回落为原始值 */
export function alarmKindKey(k?: string): string {
  return k && ALARM_KIND_MAP[k] ? `enum.alarmKind.${k}` : (k || EMPTY)
}

/** 中文兜底名（不经 i18n）。界面渲染请用 t(alarmKindKey(k))。 */
export function alarmKindName(k?: string): string {
  return ALARM_KIND_MAP[k || ''] || k || EMPTY
}

/** 告警规则页可选的事件类型（仅设备侧六类，ALM-02） */
export const ALARM_KIND_OPTIONS = ['motion', 'humanoid', 'intrusion', 'linecross', 'tamper', 'io']
  .map((v) => ({ value: v, labelKey: `enum.alarmKind.${v}` }))

/** 告警级别 */
export const ALARM_LEVEL_MAP: Record<string, { label: string; labelKey: string; color: TagColor; cssVar: string }> = {
  error: { label: '严重', labelKey: 'enum.alarmLevel.error', color: 'danger', cssVar: 'var(--color-danger)' },
  warn: { label: '警告', labelKey: 'enum.alarmLevel.warn', color: 'warning', cssVar: 'var(--color-warning)' },
  info: { label: '提示', labelKey: 'enum.alarmLevel.info', color: 'info', cssVar: 'var(--color-info)' }
}

export function alarmLevelInfo(l?: string) {
  return ALARM_LEVEL_MAP[l || ''] || ALARM_LEVEL_MAP.info
}

// ---------- 状态 ----------

export interface StatusMeta {
  /** 中文兜底 */
  label: string
  /** 词条键；界面渲染用 t(labelKey) */
  labelKey: string
  color: TagColor
}

/**
 * 设备状态。注意与节点状态刻意不同：设备在线用 success（绿），
 * 因为设备列表里"在线"是常态、需与主色区分；节点在线用 primary（信号青），
 * 呼应媒体节点是信号链路本身。两者不要合并。
 */
export const DEVICE_STATUS_MAP: Record<string, StatusMeta> = {
  online: { label: '在线', labelKey: 'enum.deviceStatus.online', color: 'success' },
  offline: { label: '离线', labelKey: 'enum.deviceStatus.offline', color: 'info' },
  pending: { label: '待确认', labelKey: 'enum.deviceStatus.pending', color: 'warning' },
  error: { label: '错误', labelKey: 'enum.deviceStatus.error', color: 'danger' }
}

export function deviceStatusInfo(s?: string): StatusMeta {
  return DEVICE_STATUS_MAP[s || ''] || DEVICE_STATUS_MAP.offline
}

/** 媒体节点状态（见 DEVICE_STATUS_MAP 注释说明为何与设备不同） */
export const NODE_STATUS_MAP: Record<string, StatusMeta> = {
  online: { label: '在线', labelKey: 'enum.nodeStatus.online', color: 'primary' },
  offline: { label: '离线', labelKey: 'enum.nodeStatus.offline', color: 'danger' },
  disabled: { label: '已禁用', labelKey: 'enum.nodeStatus.disabled', color: 'info' }
}

export function nodeStatusInfo(s?: string): StatusMeta {
  // 未知状态回落为原始值：labelKey 给原始串，t() 缺键时原样返回
  return NODE_STATUS_MAP[s || ''] || { label: s || '未知', labelKey: s || 'common.unknown', color: 'warning' }
}

/**
 * 通道推流状态。
 * 取值来自 `models.Channel.StreamState`（engine 落库 + WS 推送）：idle/starting/streaming/error。
 * 旧实现只认 'online'/'live'，而后端从不发这两个值，导致 'idle'/'streaming' 都落到
 * “原样输出 + warning”分支，详情页「码流状态」因此恒为未推流。
 */
export function streamStatusInfo(s?: string): StatusMeta {
  switch (String(s || '').toLowerCase()) {
    case 'streaming':
    case 'live':
    case 'online':
      return { label: '推流中', labelKey: 'enum.streamStatus.live', color: 'success' }
    case 'starting':
      return { label: '启动中', labelKey: 'enum.streamStatus.starting', color: 'warning' }
    case 'error':
      return { label: '流错误', labelKey: 'enum.streamStatus.error', color: 'danger' }
    default:
      return { label: '未推流', labelKey: 'enum.streamStatus.idle', color: 'info' }
  }
}

/** 操作结果（审计日志） */
export function resultInfo(r?: string): StatusMeta {
  if (r === 'success') return { label: '成功', labelKey: 'enum.result.success', color: 'success' }
  if (r === 'fail') return { label: '失败', labelKey: 'enum.result.fail', color: 'danger' }
  return { label: r || EMPTY, labelKey: r || EMPTY, color: 'info' }
}

// ---------- 任务中心（P-18） ----------

export const TASK_STATUS_MAP: Record<string, StatusMeta> = {
  pending: { label: '等待中', labelKey: 'task.status.pending', color: 'info' },
  running: { label: '进行中', labelKey: 'task.status.running', color: 'primary' },
  success: { label: '已完成', labelKey: 'task.status.success', color: 'success' },
  partial: { label: '部分成功', labelKey: 'task.status.partial', color: 'warning' },
  failed: { label: '失败', labelKey: 'task.status.failed', color: 'danger' }
}

export function taskStatusInfo(s?: string): StatusMeta {
  return TASK_STATUS_MAP[s || ''] || TASK_STATUS_MAP.pending
}

/** 任务类型词条键 */
export const TASK_TYPE_KEY: Record<string, string> = {
  discover: 'task.type.discover',
  import: 'task.type.import',
  ota: 'task.type.ota',
  download: 'task.type.download'
}

/** 中文兜底名（useTasks 在无 i18n 上下文时使用） */
export const TASK_TYPE_MAP: Record<string, string> = {
  discover: '设备发现',
  import: '批量导入',
  ota: '固件升级',
  download: '录像下载'
}
