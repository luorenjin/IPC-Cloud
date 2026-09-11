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
  'enum.day.7': 'Sun'
}
