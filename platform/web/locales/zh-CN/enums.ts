// 判别值与枚举的展示名。
// utils/enums.ts 是纯模块（不能调 useI18n），因此那里只返回词条键，
// 由页面在渲染时用 t() 取值。改动键名时两处要同步。
export default {
  // ---- 协议来源（PRD §9.1 四色语义固定，仅文案可译） ----
  'enum.source.idp': '自有',
  'enum.source.idp.long': '自有设备',
  'enum.source.gb28181': '国标',
  'enum.source.gb28181.long': '国标 GB/T 28181',
  'enum.source.onvif': 'ONVIF',
  'enum.source.onvif.long': 'ONVIF',
  'enum.source.rtsp': 'RTSP',
  'enum.source.rtsp.long': 'RTSP 直连',

  // ---- 告警事件类型（ALM-02） ----
  'enum.alarmKind.motion': '移动侦测',
  'enum.alarmKind.humanoid': '人形侦测',
  'enum.alarmKind.intrusion': '区域入侵',
  'enum.alarmKind.linecross': '越界侦测',
  'enum.alarmKind.tamper': '视频遮挡',
  'enum.alarmKind.io': 'IO报警',
  'enum.alarmKind.tf_error': 'TF卡异常',
  'enum.alarmKind.device_offline': '设备离线',
  'enum.alarmKind.node_offline': '节点离线',
  'enum.alarmKind.stream_lost': '流中断',
  'enum.alarmKind.disk_full': '存储不足',

  // ---- 告警级别 ----
  'enum.alarmLevel.error': '严重',
  'enum.alarmLevel.warn': '警告',
  'enum.alarmLevel.info': '提示',

  // ---- 设备状态 ----
  'enum.deviceStatus.online': '在线',
  'enum.deviceStatus.offline': '离线',
  'enum.deviceStatus.pending': '待确认',
  'enum.deviceStatus.error': '错误',

  // ---- 媒体节点状态 ----
  'enum.nodeStatus.online': '在线',
  'enum.nodeStatus.offline': '离线',
  'enum.nodeStatus.disabled': '已禁用',

  // ---- 通道推流状态 ----
  'enum.streamStatus.live': '推流中',
  'enum.streamStatus.idle': '未推流',
  'enum.streamStatus.starting': '启动中',
  'enum.streamStatus.error': '流错误',

  // ---- 操作结果（审计日志） ----
  'enum.result.success': '成功',
  'enum.result.fail': '失败',

  // ---- 星期（schedule.days 用 1–7 表示周一至周日） ----
  'enum.day.1': '周一',
  'enum.day.2': '周二',
  'enum.day.3': '周三',
  'enum.day.4': '周四',
  'enum.day.5': '周五',
  'enum.day.6': '周六',
  'enum.day.7': '周日'
}
