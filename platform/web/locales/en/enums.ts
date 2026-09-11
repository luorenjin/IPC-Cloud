// Display names for discriminator values and enums.
// utils/enums.ts is a plain module (cannot call useI18n), so it returns keys only;
// pages resolve them through t() at render time. Keep both sides in sync.
export default {
  // ---- Protocol sources (PRD §9.1 fixed colour semantics; wording only is translatable) ----
  'enum.source.idp': 'Native',
  'enum.source.idp.long': 'Native device',
  'enum.source.gb28181': 'GB28181',
  'enum.source.gb28181.long': 'GB/T 28181',
  'enum.source.onvif': 'ONVIF',
  'enum.source.onvif.long': 'ONVIF',
  'enum.source.rtsp': 'RTSP',
  'enum.source.rtsp.long': 'RTSP direct',

  // ---- Alarm event kinds (ALM-02) ----
  'enum.alarmKind.motion': 'Motion detection',
  'enum.alarmKind.humanoid': 'Human detection',
  'enum.alarmKind.intrusion': 'Intrusion',
  'enum.alarmKind.linecross': 'Line crossing',
  'enum.alarmKind.tamper': 'Tampering',
  'enum.alarmKind.io': 'IO alarm',
  'enum.alarmKind.tf_error': 'SD card error',
  'enum.alarmKind.device_offline': 'Device offline',
  'enum.alarmKind.node_offline': 'Media node offline',
  'enum.alarmKind.stream_lost': 'Stream lost',
  'enum.alarmKind.disk_full': 'Storage almost full',

  // ---- Alarm levels ----
  'enum.alarmLevel.error': 'Critical',
  'enum.alarmLevel.warn': 'Warning',
  'enum.alarmLevel.info': 'Info',

  // ---- Device status ----
  'enum.deviceStatus.online': 'Online',
  'enum.deviceStatus.offline': 'Offline',
  'enum.deviceStatus.pending': 'Pending approval',
  'enum.deviceStatus.error': 'Error',

  // ---- Media node status ----
  'enum.nodeStatus.online': 'Online',
  'enum.nodeStatus.offline': 'Offline',
  'enum.nodeStatus.disabled': 'Disabled',

  // ---- Channel streaming status ----
  'enum.streamStatus.live': 'Streaming',
  'enum.streamStatus.idle': 'Not streaming',
  'enum.streamStatus.starting': 'Starting',
  'enum.streamStatus.error': 'Stream error',

  // ---- Operation result (audit log) ----
  'enum.result.success': 'Success',
  'enum.result.fail': 'Failed',

  // ---- Weekdays (schedule.days uses 1–7 for Mon–Sun) ----
  'enum.day.1': 'Mon',
  'enum.day.2': 'Tue',
  'enum.day.3': 'Wed',
  'enum.day.4': 'Thu',
  'enum.day.5': 'Fri',
  'enum.day.6': 'Sat',
  'enum.day.7': 'Sun',

  // ---- Device capabilities (access spec §3.4) ----
  'enum.cap.live.main': 'Main stream preview',
  'enum.cap.live.sub': 'Sub stream preview',
  'enum.cap.live.h265': 'Main stream H.265',
  'enum.cap.snapshot': 'Snapshot',
  'enum.cap.ptz': 'PTZ control',
  'enum.cap.ptz.preset': 'PTZ presets',
  'enum.cap.focus': 'Focus',
  'enum.cap.audio.talk': 'Two-way audio',
  'enum.cap.event.motion': 'Motion events',
  'enum.cap.record.device.query': 'On-device recording search',
  'enum.cap.record.device.play': 'On-device playback',
  'enum.cap.record.device.speed': 'Playback speed',
  'enum.cap.record.device.seek': 'Playback seek',
  'enum.cap.record.platform': 'Platform recording',
  'enum.cap.status.metrics': 'Runtime status reporting',
  'enum.cap.config.remote': 'Remote configuration',
  'enum.cap.ota': 'Firmware update',
  'enum.cap.reboot': 'Remote reboot',

  // ---- Audit log actions (ACC-08) ----
  // Mirrors auditVerbs in platform/server/internal/api/audit.go, plus the create/update/delete
  // HTTP-method fallbacks and the login events; keep both sides in sync
  'enum.audit.create': 'Create',
  'enum.audit.update': 'Update',
  'enum.audit.delete': 'Delete',
  'enum.audit.config': 'Push config',
  'enum.audit.diag': 'Diagnostics',
  'enum.audit.reboot': 'Remote reboot',
  'enum.audit.sync': 'Sync',
  'enum.audit.transfer': 'Move to group',
  'enum.audit.bind': 'Bind',
  'enum.audit.preadd': 'Pre-add',
  'enum.audit.activate': 'Activate',
  'enum.audit.confirm': 'Approve',
  'enum.audit.reject': 'Reject',
  'enum.audit.discover': 'Discovery',
  'enum.audit.whitelist': 'Allowlist',
  'enum.audit.batch': 'Batch operation',
  'enum.audit.move-devices': 'Batch move',
  'enum.audit.play': 'Start stream',
  'enum.audit.stop': 'Stop stream',
  'enum.audit.snapshot': 'Snapshot',
  'enum.audit.cover': 'Refresh cover',
  'enum.audit.ptz': 'PTZ control',
  'enum.audit.presets': 'PTZ presets',
  'enum.audit.goto': 'Go to preset',
  'enum.audit.playback': 'Playback',
  'enum.audit.download': 'Download',
  'enum.audit.selfcheck': 'Self-check',
  'enum.audit.kick': 'Drop stream',
  'enum.audit.read': 'Mark as read',
  'enum.audit.read-all': 'Mark all as read',
  'enum.audit.reset-password': 'Reset password',
  'enum.audit.password': 'Change password',
  'enum.audit.crl': 'CRL update',
  'enum.audit.login': 'Log in',
  'enum.audit.logout': 'Log out'
}
