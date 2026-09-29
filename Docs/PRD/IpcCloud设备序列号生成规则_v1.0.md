# IpcCloud 设备序列号生成规则 v1.0

> 状态：**已定稿（2026-09-28）——单码体系：只保留 17 位 DeviceID，废除 22 位 SN（用户裁定合一）**
> 可执行定义：[`firmware/tools/gen_sn.py`](../../firmware/tools/gen_sn.py)（`selftest` 即规则回归）
> 关联规范：《IpcCloud设备接入规范_v1.0》DeviceID 条目（**不修改**）、《IPC固件平台化架构_HAL适配方案》生产烧录

## 1. 背景与定位：单码体系（DeviceID 唯一）

接入规范已冻结平台身份 **DeviceID（17 位）**，它被平台绑定、设备证书 `CN=<DeviceID>`、MQTT ACL（`idp/v1/<DeviceID>/...`）、二维码内容共用，**本规则不改它**：

```
DeviceID = MMM TTTT YYWW SSSSS C     （厂商3 + 配置码4 + 生产年周4 + 序列5 + 校验1）
字符集 32 符号表：2-9 + A-Z 去 I/O（即规范的「A-Z 2-9 去除 0 O 1 I」）
校验位：前 16 位 Luhn mod 32
```

**2026-09-28 用户裁定：合一，不保留双码。** 早期提案里的 22 位机身序列号（SN）**已废除**，依据：

- 同一 SKU 出厂硬件完全相同——硬件信息属于「型号」而非「单机」，**登记一次即可**；
- TP-LINK 自身即如此：`TL-IPC445GP-AI4` 的型号段已表达配置，机身另一条码是**商品码（EAN）**，不是第二套设备身份；
- 单码让平台契约（17 位输入框、证书 CN、MQTT ACL、导入模板、用户手册）**零改动**。

硬件明细（SoC / 存储 / 传感器 / WiFi / 网络 / 4G）改由 `TTTT` **配置码登记表**承载（§2.2），控制台「硬件配置」行就是这张表的解码结果。

## 2. DeviceID 结构（17 位，32 符号表）

```
MMM TTTT YYWW SSSSS C
厂商3  配置码4  年周4  序列5  校验1
```

### 2.1 段位含义

| 段 | 位数 | 含义 | 规则 |
|---|---|---|---|
| `MMM` | 3 | 厂商码 | `JPC` = IpcCloud/JetsCam（`IPC` 含被禁字符 I，不可用） |
| `TTTT` | 4 | **配置码（硬件/SKU）** | 必须在 §2.2 登记表中；禁 `0/1/I/O` |
| `YYWW` | 4 | 生产年周（年、周各 2 位） | 十进制年(00–99)/周(1–53)各按 **base32 用 32 符号表编码**——字符集禁 `0/1`，十进制字面量写不进去（如 2026 年第 40 周的 "2640" 含 0、第 11 周含 1）；解码用 `gen_sn.py decode` |
| `SSSSS` | 5 | 序列段 | 十进制流水 0–99999 按 32 符号表 base32 编码（`22223` = 流水 1） |
| `C` | 1 | 校验位 | Luhn mod 32（§3） |

### 2.2 配置码登记表（TTTT → 硬件配置）

**登记制**：新硬件组合先在此表登记码值再出货。`gen_sn.py` 对未登记码**拒绝生成**；控制台对未登记码**不显示**「硬件配置」行（不编造）。三处必须同步：本表 ↔ `gen_sn.py` 的 `HW_CODE` ↔ 控制台 `firmware/web/js/views/system.js` 的 `HW_CODE`。

| 配置码 | SoC | 存储 | 传感器 | WiFi | 网络 | 蜂窝 | 说明 |
|---|---|---|---|---|---|---|---|
| `SPRA` | GK7205V200 | 64MB DDR + 16MB SPI NOR | GC2053 | 无 | 以太网 | 无 | SP-R1-02 当前基线（字母 `SPR` + 硬件版本 `A`） |

> 硬件变更（换 SoC / 传感器 / 存储 / 无线）→ **换一个配置码并登记**；同 SKU 单机差异只体现在 `YYWW` 与 `SSSSS`。

## 3. 校验算法（Luhn mod 32，接入规范既有）

```
对正文 16 位从右往左编号 i=0,1,…；i 为奇数的位 d = 2×d，若 d ≥ 32 则 d -= 31
Σ = Σ d ;  CHECK = A32[(32 − Σ mod 32) mod 32]    # A32 = 23456789ABCDEFGHJKLMNPQRSTUVWXYZ
```

算法与示例向量由 `gen_sn.py selftest` 钉住（单比特篡改必被检出、禁用字符判非法、流水与**年周** base32 往返、边界 99999/第 53 周、未登记配置码拒绝生成、含 0/1 的年周可正常生成）。

## 4. 生成与烧录（工厂化工具）

1. **产线入口：`firmware/tools/burn_sn.py`**（工厂化工具，码值生成复用 `gen_sn.py`，同一张登记表、同一套校验）：
   ```bash
   # 只生成（MES 预占号 / 打印标贴，不连设备）
   python firmware/tools/burn_sn.py gen --model-code SPRA --seq 1
   # 烧录到模组（串口），写入后回读逐字节校验 + Luhn 验真；退出码 0/1 可接 MES，--json 出机器可读结果
   python firmware/tools/burn_sn.py burn --port COM6 --model-code SPRA --seq 1
   # 复检已烧录模组 / 开发调试
   python firmware/tools/burn_sn.py verify --port COM6
   python firmware/tools/burn_sn.py burn --target mock --model-code SPRA --seq 1
   ```
   工厂化硬约束：**未烧录才允许写**（已有 device_id → 拒绝，重烧需显式 `--force`）；写入用 `printf '%s'`（无换行、无 shell 展开）+ `chmod 600` + `sync`；回读逐字节比对 + 校验位验真，不符即 FAIL。年周缺省取当前 ISO 年周，流水 `--seq` 由 MES 下发（重号责任在 MES）。
2. **写入安全存储**（`hal_crypto`，键名见 `hal_crypto.h`，路径 `/etc/ipc/sec/<key>`）：
   - `device_id` → 17 位 DeviceID（既有键，接入规范）
   - `verify_code` → 6 位验证码（既有键）
   - ~~`device_sn`~~ → **已随 22 位 SN 一并废除**（键定义已删；本地 mock 里烧过的残留文件可直接删）
   固件按需读取，写完**无需重启**，控制台/二维码立即生效。
3. **标签**：机身标贴印 17 位 DeviceID（分组 `MMM-TTTT-YYWW-SSSSS-C`）+ QR（QR 内容见 §6）。
4. **未烧录**：读不到就显示「未烧录」，**任何环节都不得编造/派生假码**（当前开发期真机即此状态）。

## 5. 读取与展示

- `GET /api/v1/system/info`：
  - `serial` = 17 位 DeviceID（未烧录为空串）；
  - `qr_content` = `IPC1:<DeviceID>:<VerifyCode>:<Model>`（三者齐全时才给，缺任一省略字段）；
  - ~~`sn`~~ 字段已随双码方案废除。
- 控制台「系统设置→基本设置→设备信息」按四段展示（对齐实机 + PRD LC-SYS-02）：
  **设备信息**（日期时间 / 设备型号 / 设备名称 / **序列号** / 固件版本）→ **网络信息**（IP / MAC）→ **码流信息**（分辨率 / 帧率）→ **设备二维码**。
- **页面只有一个码（用户 2026-09-28 两次裁定）**：合并后 **DeviceID 就是序列码**——「序列号」行显示的就是 `serial`；二维码**只出图、不显示明文**（对齐实机）。
- DeviceID 已烧录且配置码已登记时，额外显示一行「硬件配置」：把 §2.2 登记表解码成中文（SoC / 存储 / 传感器 / WiFi / 网络 / 蜂窝）；配置码未登记 → 该行不显示。

## 6. 二维码内容

`IPC1:<DeviceID>:<VerifyCode>:<Model>`（接入规范 QR 条目）。编码为 QR（byte 模式、纠错 M），前端纯 JS 绘制（固件控制台禁 npm/CDN），**页面只渲染图形、不展示明文**（对齐实机）。DeviceID 或验证码未烧录 → 不出图，显示「未烧录序列码/验证码，无法生成绑定二维码」。

## 7. 与平台/其它规范的关系

- 平台侧**只认 17 位 DeviceID**（绑定、搜索、导入模板、ACL 均不变）；合一后**设备侧也只有这一个码**，不存在第二套编号。
- 本规则**不需要**改接入规范、平台 `cfgKeys`、模拟器契约——码值是安全存储里的只读出厂值，不是 `cfg.*` 配置键。
- 恢复出厂不清除 `device_id` / `verify_code`（与凭据同属安全存储，不受 `cfg_reset` 影响）。

## 8. 示例（基线样机）

```
配置（SPRA 登记表）：GK7205V200 + 64MB DDR + 16MB SPI NOR + GC2053 + 无 WiFi + 以太网 + 无 4G
生产：2026 年第 38 周，产线流水 1

DeviceID  = JPCSPRA2U3822223B    （厂商 JPC + 配置码 SPRA + 年周 2U38 = 26年38周 + 序列 22223 + 校验 B）
VerifyCode= （产线随机 6 位）
QR        = IPC1:JPCSPRA2U3822223B:<VerifyCode>:SP-R1-02

生成：python firmware/tools/gen_sn.py gen --qr
烧录：python firmware/tools/burn_sn.py burn --model-code SPRA --seq 1
自检：python firmware/tools/gen_sn.py selftest   → selftest OK
解码：python firmware/tools/gen_sn.py decode JPCSPRA2U3822223B
```

- `YYWW=2U38`：十进制 26 年、38 周各按 base32 编码（十进制字面量含 0/1 时写不进 32 符号表，如 2026 年第 40 周）；
- `SSSSS=22223`：十进制流水 `1` 的 base32 编码。两者换算均由 `gen_sn.py` 完成。

## 9. 边界与待定

| 项 | 现状 | 处理 |
|---|---|---|
| 双码→单码合一 | 22 位 SN 已废除（键 `device_sn`、字段 `sn`、段位表均删） | 无存量设备（仅本地 mock 烧过），**零迁移**；残留 `secure_device_sn.bin` 直接删 |
| 配置码未登记 | `gen_sn.py` 拒生成、控制台不显示「硬件配置」行 | 新硬件先登记 §2.2 + 工具 + 前端三处码表 |
| DDR 容量口径 | 硬件基线按 64MB 芯片规格，实测以 `free`/`/proc/meminfo` 为准 | 登记在 `SPRA` 的“存储”列；实测口径变化改登记表 |
| 4G/5G | 本项目无蜂窝能力，profile 也无该字段 | `SPRA` 登记蜂窝=无；引入蜂窝时**新增配置码**再出货 |
| 存量开发设备 | 未烧录 | 页面如实显示「未烧录」；需要演示时用 `gen_sn.py` 生成后由产线工具烧录 |
| 序列 99999 用尽 | — | 换批继续（base32 容量 32^5 ≈ 3375 万，远大于十进制 5 位） |
| 年周含 0/1 的编码坑 | 初版把十进制年周直接拼进 `YYWW`，遇 2026 年第 40 周（含 0）即抛错 | 改为年/周各 base32（§2.1）；2026-09-28 由 `burn_sn` 产线试跑暴露，`selftest` 已加回归 |
| 烧录工具 | `firmware/tools/burn_sn.py`（gen/burn/verify 三态，防重烧、回读校验、`--json` 接 MES） | 烧录前先 `verify` 确认未烧录；重烧必须 `--force` |
