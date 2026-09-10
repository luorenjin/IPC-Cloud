// 中文词条汇总。新增模块在此登记。
import common from './common'
import enums from './enums'
import device from './device'
import alarm from './alarm'
import record from './record'
import live from './live'
import system from './system'
import account from './account'

export default {
  ...common,
  ...enums,
  ...device,
  ...alarm,
  ...record,
  ...live,
  ...system,
  ...account
} as Record<string, string>
