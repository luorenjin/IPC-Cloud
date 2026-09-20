// 按当前页面协议选择播放地址：https 页面必须走 wss，否则浏览器会阻止混合内容；
// http 页面优先 ws，避免连未配置证书的 wss 端口。live.vue 与预览弹窗共用同一份逻辑。
export function pickFlv(res: any): string {
  const https = location.protocol === 'https:'
  return https ? res.wssFlv || res.wsFlv || '' : res.wsFlv || res.wssFlv || ''
}
