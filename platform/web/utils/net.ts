/**
 * 网络字段的格式校验工具。
 *
 * 只做「字面量是否像 IPv4」，不做网段/可达性推断：这类字段最终由设备网络栈解释，
 * 平台侧越俎代庖地"猜"网段只会给出错误的确认感。
 */

/**
 * 是否是合法的 IPv4 字面量：点分四段、每段 0–255 的十进制、不接受前导零。
 * 拒绝 `01.2.3.4` 这类写法是有意的——它在下发后与 `1.2.3.4` 是不同的字符串，
 * 某些网络栈会当成八进制解析，属于"保存成功但地址不是你以为的那个"。
 */
export function isIPv4(s: string): boolean {
  const parts = String(s ?? '').trim().split('.')
  if (parts.length !== 4) return false
  return parts.every((p) => /^\d{1,3}$/.test(p) && !(p.length > 1 && p[0] === '0') && Number(p) <= 255)
}
