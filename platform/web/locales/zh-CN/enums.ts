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
  'enum.day.7': '周日',

  // ---- 设备能力集（接入规范 §3.4） ----
  'enum.cap.live.main': '主码流预览',
  'enum.cap.live.sub': '子码流预览',
  'enum.cap.live.h265': '主码流 H.265',
  'enum.cap.snapshot': '抓图',
  'enum.cap.ptz': '云台控制',
  'enum.cap.ptz.preset': '预置位',
  'enum.cap.focus': '对焦',
  'enum.cap.audio.talk': '语音对讲',
  'enum.cap.event.motion': '移动侦测事件',
  'enum.cap.record.device.query': '设备端录像检索',
  'enum.cap.record.device.play': '设备端录像回放',
  'enum.cap.record.device.speed': '回放倍速',
  'enum.cap.record.device.seek': '回放定位',
  'enum.cap.record.platform': '平台侧录像',
  'enum.cap.status.metrics': '运行状态上报',
  'enum.cap.config.remote': '远程配置',
  'enum.cap.ota': '固件升级',
  'enum.cap.reboot': '远程重启',

  // ---- 操作日志动作（ACC-08） ----
  // 动词表镜像 platform/server/internal/api/audit.go 的 auditVerbs + 按 HTTP 方法
  // 回落的 create/update/delete + 登录事件；后端新增动词时两处同步
  'enum.audit.create': '新增',
  'enum.audit.update': '修改',
  'enum.audit.delete': '删除',
  'enum.audit.config': '下发配置',
  'enum.audit.diag': '一键诊断',
  'enum.audit.reboot': '远程重启',
  'enum.audit.sync': '同步',
  'enum.audit.transfer': '转移分组',
  'enum.audit.bind': '绑定',
  'enum.audit.preadd': '预添加',
  'enum.audit.activate': '激活',
  'enum.audit.confirm': '确认',
  'enum.audit.reject': '驳回',
  'enum.audit.discover': '设备发现',
  'enum.audit.whitelist': '白名单',
  'enum.audit.batch': '批量操作',
  'enum.audit.move-devices': '批量转移',
  'enum.audit.play': '起播',
  'enum.audit.stop': '停播',
  'enum.audit.snapshot': '抓图',
  'enum.audit.cover': '刷新封面',
  'enum.audit.ptz': '云台控制',
  'enum.audit.presets': '预置位',
  'enum.audit.goto': '预置位调用',
  'enum.audit.playback': '回放',
  'enum.audit.download': '下载',
  'enum.audit.selfcheck': '自检',
  'enum.audit.kick': '踢流',
  'enum.audit.read': '标记已读',
  'enum.audit.read-all': '全部已读',
  'enum.audit.reset-password': '重置密码',
  'enum.audit.password': '修改密码',
  'enum.audit.crl': 'CRL 更新',
  'enum.audit.login': '登录',
  'enum.audit.logout': '退出登录'
}
