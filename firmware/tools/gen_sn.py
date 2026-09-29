#!/usr/bin/env python3
"""设备序列号（DeviceID）生成 / 解码 / 自检 —— 规则的可执行定义。

规范正文见 Docs/PRD/IpcCloud设备序列号生成规则_v1.0.md（与本文件同步维护）。

单码体系（2026-09-28 用户裁定合一，废除 22 位 SN）：
  DeviceID（17 位，唯一身份；接入规范已冻结的格式与字符集不变）
      MMM TTTT YYWW SSSSS C   厂商3 + 配置码4 + 生产年周4 + 序列5 + 校验1
      字符集 = 项目 32 符号表（2-9 + A-Z 去 I/O），校验位 Luhn mod 32
  硬件配置（SoC/存储/传感器/WiFi/网络/4G）由 **TTTT 配置码登记表**承载：
      一个 SKU 一个码、查表即得明细（HW_CODE），不逐台编码——同 SKU 出厂
      硬件相同，单机差异只在年周与流水。

用法：
  python firmware/tools/gen_sn.py gen --model-code SPRA --year 26 --week 38 --seq 1 --qr
  python firmware/tools/gen_sn.py decode JPCSPRA2U3822223B
  python firmware/tools/gen_sn.py selftest
"""
import argparse
import random
import sys

# ---- 配置码登记表（TTTT，4 位、32 符号表；新硬件先登记码表再出货，见规范 §2.2） ----
# 一个 SKU 一个码：硬件明细登记在码表里，不再逐台编码——同一 SKU 出厂硬件
# 完全相同，单机差异只在年周与流水；控制台「硬件配置」行解码的就是这张表。
HW_CODE = {
    "SPRA": {
        "soc": "GK7205V200",
        "storage": "64MB DDR + 16MB SPI NOR",
        "sensor": "GC2053",
        "wifi": "无",
        "net": "以太网",
        "cell": "无",
        "note": "SP-R1-02 当前基线",
    },
}

# DeviceID 用的 32 符号表：2-9 + A-Z 去 I/O（接入规范「A-Z 2-9 去除 0 O 1 I」）
A32 = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
assert len(A32) == 32


def hw_text(code):
    """配置码 → 人可读硬件描述；未登记返回 None（页面不显示该行）。"""
    h = HW_CODE.get(code)
    if not h:
        return None
    return (f"SOC {h['soc']}；{h['storage']}；传感器 {h['sensor']}；"
            f"WiFi {h['wifi']}；网络 {h['net']}；蜂窝 {h['cell']}")


def devid_check_char(body16):
    """DeviceID 校验位：Luhn mod 32（N=32，翻倍结果 >=32 时减 31）。"""
    if len(body16) != 16:
        raise ValueError(f"DeviceID 正文应为 16 位，实际 {len(body16)}")
    s = 0
    for i, c in enumerate(reversed(body16)):      # 最右位参与翻倍
        d = A32.index(c)
        if i % 2 == 1:                            # 从右往左第 2、4、6… 位翻倍
            d = 2 * d
            if d >= 32:
                d -= 31
        s += d
    return A32[(32 - s % 32) % 32]


def devid_valid(dev_id):
    dev_id = dev_id.upper()
    return (len(dev_id) == 17 and all(c in A32 for c in dev_id) and
            devid_check_char(dev_id[:16]) == dev_id[16])


def seq_to_a32(n, width=5):
    """十进制流水 → 32 符号表 base32（DeviceID 序列段）。"""
    if not 0 <= n < 32 ** width:
        raise ValueError(f"流水 {n} 超出 {width} 位 base32 容量")
    out = []
    for _ in range(width):
        out.append(A32[n % 32])
        n //= 32
    return "".join(reversed(out))


def seq_from_a32(s):
    n = 0
    for c in s:
        n = n * 32 + A32.index(c)
    return n


def yw_to_a32(year2, week2):
    """十进制年(00-99)/周(1-53) → 各 2 位 A32 base32 编码。

    接入规范的字符集禁 0/1（防 0↔O、1↔I 抄读混淆），十进制字面量写不进
    `YYWW`——例：2026 年第 40 周的 "2640" 含 0，第 11 周含 1。故年、周各按
    base32 用同一张 A32 表编码（解码 yw_from_a32），字符集与 Luhn mod32
    校验因此保持规范原样。（2026-09-28 由 burn_sn 产线试跑暴露。）
    """
    y, w = int(year2), int(week2)
    if not 0 <= y <= 99:
        raise ValueError(f"生产年应为 00-99：{year2}")
    if not 1 <= w <= 53:
        raise ValueError(f"生产周应为 1-53：{week2}")
    return A32[y // 32] + A32[y % 32] + A32[w // 32] + A32[w % 32]


def yw_from_a32(s):
    """YYWW 的 4 位 A32 编码 → (年, 周) 两位十进制字符串。"""
    def dec(a, b):
        return A32.index(a) * 32 + A32.index(b)
    return f"{dec(s[0], s[1]):02d}", f"{dec(s[2], s[3]):02d}"


def gen(vendor3, model_code4, year2, week2, seq, verify=None):
    """产出 DeviceID + 验证码（产线按批次生成）。配置码必须已在 HW_CODE 登记。"""
    for name, val, n in (("vendor", vendor3, 3), ("model", model_code4, 4)):
        if len(val) != n:
            raise ValueError(f"{name} 应为 {n} 位：{val}")
        if any(c not in A32 for c in val.upper()):
            raise ValueError(f"{name} 含 32 符号表以外的字符（禁 0/1/I/O）：{val}")
    if model_code4.upper() not in HW_CODE:
        raise ValueError(f"配置码 {model_code4} 未登记（见 HW_CODE / 规范 §2.2，先登记再出货）")
    # 年/周接受 1-2 位十进制输入（--week 1 → "01"），非法字符/越界一律拒绝
    try:
        yv, wv = int(str(year2)), int(str(week2))
    except ValueError:
        raise ValueError(f"年周不合法：{year2}{week2}")
    if not 0 <= yv <= 99 or not 1 <= wv <= 53:
        raise ValueError(f"年周不合法：{year2}{week2}")
    year2, week2 = f"{yv:02d}", f"{wv:02d}"
    seq = int(seq)
    if not 0 <= seq <= 99999:
        raise ValueError(f"流水应为 0-99999：{seq}")

    dev_body = (vendor3 + model_code4 + yw_to_a32(year2, week2) +
                seq_to_a32(seq)).upper()
    dev_id = dev_body + devid_check_char(dev_body)

    if verify is None:
        # 6 位出厂验证码：避开易混淆字符（与设备标签/手工输入场景一致）
        alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
        verify = "".join(random.choice(alphabet) for _ in range(6))
    return {"device_id": dev_id, "verify_code": verify}


def decode(code):
    """解码 DeviceID（17 位），返回人可读的字段说明。"""
    c = code.upper().replace("-", "").strip()
    out = {"输入": c, "类型": "?"}
    if len(c) == 17 and devid_valid(c):
        out["类型"] = "DeviceID（17 位，唯一身份）"
        out["厂商"] = c[0:3]
        out["配置码"] = c[3:7]
        out["硬件配置"] = hw_text(c[3:7]) or "未登记（见规范 §2.2）"
        yy, ww = yw_from_a32(c[7:11])
        out["生产年周"] = f"{yy} 年 {ww} 周（YYWW={c[7:11]}）"
        out["序列"] = f"{c[11:16]}（十进制流水 {seq_from_a32(c[11:16])}）"
        out["校验"] = f"{c[16]}（{'通过' if devid_valid(c) else '不通过'}）"
    else:
        out["类型"] = "无法识别（长度/校验不符，或不是 17 位 DeviceID）"
    return out


def selftest():
    # 1) 基线样机（厂商码 JPC = IpcCloud/JetsCam；"IPC" 含被禁的 I，不能用）
    #    2026 年第 38 周 → YYWW = base32("26")+base32("38") = "2U38"
    r = gen("JPC", "SPRA", 26, 38, 1, verify="ABC234")
    assert r["device_id"] == "JPCSPRA2U3822223B", r["device_id"]
    assert devid_valid(r["device_id"]) and len(r["device_id"]) == 17
    assert r["verify_code"] == "ABC234"

    # 2) 解码：配置码查登记表给出硬件明细
    d = decode(r["device_id"])
    assert d["生产年周"].startswith("26 年 38 周") and "流水 1）" in d["序列"], d
    assert "GK7205V200" in d["硬件配置"] and "GC2053" in d["硬件配置"], d
    assert hw_text("SPRA") is not None and hw_text("ZZZZ") is None

    # 3) 单比特篡改必被校验位逮住；含禁用字符（0/1/I/O）直接判非合法
    assert not devid_valid(r["device_id"][:16] +
                           (A32[(A32.index(r["device_id"][16]) + 1) % 32]))
    assert not devid_valid("JPCSPRA26382222I")
    assert not devid_valid("JPCSPRA263822220O")

    # 4) 流水 base32 往返
    for n in (0, 1, 31, 32, 99999):
        assert seq_from_a32(seq_to_a32(n)) == n

    # 5) 边界：序列 99999 / 第 53 周也合法
    r2 = gen("JPC", "SPRA", 26, 53, 99999)
    assert devid_valid(r2["device_id"])

    # 6) 非法输入被拒：禁用字符、未登记配置码、非法周
    for bad in (("JPC", "SP1A"), ("JPC", "SP0A"), ("IP0", "SPRA"), ("IPC", "SPRA")):
        try:
            gen(bad[0], bad[1], 26, 38, 1)
        except ValueError:
            pass
        else:
            raise AssertionError(f"应拒绝非法字符：{bad}")
    try:
        gen("JPC", "ZZZZ", 26, 38, 1)
    except ValueError:
        pass
    else:
        raise AssertionError("应拒绝未登记的配置码")
    try:
        gen("JPC", "SPRA", 26, 0, 1)
    except ValueError:
        pass
    else:
        raise AssertionError("应拒绝非法周")

    # 7) 年周十进制含 0/1（如 2030年第1周、2026年第40周）——十进制字面量写不进
    #    32 符号表，必须走 base32 编码；这是 burn_sn 产线试跑时踩到的回归点
    for yy, ww in (("30", "1"), ("26", "11"), ("26", "40"), ("1", "53")):
        r3 = gen("JPC", "SPRA", yy, ww, 1)
        assert devid_valid(r3["device_id"]), r3["device_id"]
        d3 = decode(r3["device_id"])
        assert d3["生产年周"].startswith(f"{int(yy):02d} 年 {int(ww):02d} 周"), d3
    assert yw_from_a32(yw_to_a32(26, 40)) == ("26", "40")
    assert yw_from_a32(yw_to_a32("01", 1)) == ("01", "01")
    print("selftest OK")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen", help="生成 DeviceID + 验证码（单码体系）")
    g.add_argument("--vendor", default="JPC", help="厂商码 3 位（32 符号表；IpcCloud/JetsCam = JPC）")
    g.add_argument("--model-code", default="SPRA", help="配置码 4 位（须已在 HW_CODE 登记，禁 0/1/I/O）")
    g.add_argument("--year", default="26", help="生产年 00-99（1 位也接受，自动补零）")
    g.add_argument("--week", default="38", help="生产周 1-53（1 位也接受，自动补零）")
    g.add_argument("--seq", default="1", type=int, help="产线流水 0-99999")
    g.add_argument("--verify", default=None, help="出厂验证码 6 位（缺省随机）")
    g.add_argument("--qr", action="store_true", help="同时输出二维码文本 IPC1:<DeviceID>:<VerifyCode>:<Model>")
    g.add_argument("--model", default="SP-R1-02", help="型号（仅用于二维码文本）")

    d = sub.add_parser("decode", help="解码 DeviceID（17 位）")
    d.add_argument("code")

    sub.add_parser("selftest", help="内置自检（码表/校验/边界）")

    a = p.parse_args()
    if a.cmd == "selftest":
        return selftest()
    if a.cmd == "decode":
        for k, v in decode(a.code).items():
            print(f"{k}: {v}")
        return 0
    r = gen(a.vendor, a.model_code, a.year, a.week, a.seq, verify=a.verify)
    print(f"DeviceID (17) : {r['device_id']}")
    print(f"VerifyCode(6) : {r['verify_code']}")
    print(f"配置码 {a.model_code.upper():<4}  : {hw_text(a.model_code) or '未登记'}")
    if a.qr:
        print(f"QR 文本       : IPC1:{r['device_id']}:{r['verify_code']}:{a.model}")
    # 生成即自检，杜绝把错码流出
    assert devid_valid(r["device_id"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
