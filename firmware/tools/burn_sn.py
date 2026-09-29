#!/usr/bin/env python3
"""模组序列号工厂化烧录工具（burn_sn）：产线写入 DeviceID + 出厂验证码。

规范：Docs/PRD/IpcCloud设备序列号生成规则_v1.0.md（单码体系：17 位 DeviceID 即序列码）。
码值生成复用 gen_sn.py —— 与规范同一张 HW_CODE 配置码登记表、同一套校验（Luhn mod32），
避免「生成工具」与「烧录工具」两套口径。

用法：
  ① 只生成（MES 预占号 / 打印标贴，不连设备）
       python firmware/tools/burn_sn.py gen --model-code SPRA --seq 1
  ② 烧录到模组（串口），写入后**回读逐字节校验**
       python firmware/tools/burn_sn.py burn --port COM6 --model-code SPRA --seq 1
  ③ 复检已烧录模组（回读 + 校验位验真）
       python firmware/tools/burn_sn.py verify --port COM6
  ④ 开发/演练
       python firmware/tools/burn_sn.py burn --target mock --model-code SPRA --seq 1
       python firmware/tools/burn_sn.py burn --dry-run --model-code SPRA --seq 1

产线要点（工厂化的硬约束）：
  * 未烧录才允许写：目标已有 device_id → 拒绝，重烧必须显式 --force；
  * 写入用 `printf '%s'`（无换行、无 shell 展开）+ chmod 600 + sync，落盘路径与
    固件 c_write 完全一致（/etc/ipc/sec/<key>），固件按需即时读到，无需重启；
  * 回读后逐字节比对，并用 Luhn mod32 复验 DeviceID；任一不符 → FAIL、退出码 1；
  * 退出码 0/1 可直接接 MES；`--json` 输出机器可读结果；
  * 年周缺省取**当前 ISO 年周**；流水 `--seq` 由 MES 下发（重号责任在 MES）。
"""
import argparse
import datetime
import json
import os
import re
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_sn import HW_CODE, devid_valid, gen, hw_text  # noqa: E402  （同目录工具）

SEC_DIR = "/etc/ipc/sec"
# 与 gen_sn.py 生成器同表：无 I/O 与 0/1，避免标签抄读混淆
VERIFY_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
MOCK_DIR = os.path.join(os.path.dirname(HERE), "mock_state")   # firmware/mock_state


def log(tag, msg):
    print(f"[sn:{tag}] {msg}", flush=True)


def label_groups(device_id):
    """标贴分组：MMM-TTTT-YYWW-SSSSS-C"""
    return "-".join(device_id[i:j] for i, j in ((0, 3), (3, 7), (7, 11), (11, 16), (16, 17)))


def build_code(a):
    """生成 DeviceID + 验证码；参数或登记表不合法直接抛错（不产出错码）。"""
    iso = datetime.date.today().isocalendar()
    try:
        yv = int(a.year) if a.year is not None else iso[0] % 100
        wv = int(a.week) if a.week is not None else iso[1]
    except ValueError:
        raise ValueError(f"年周不合法：year={a.year} week={a.week}")
    if not 0 <= yv <= 99 or not 1 <= wv <= 53:
        raise ValueError(f"年周不合法：year={a.year} week={a.week}")
    year, week = f"{yv:02d}", f"{wv:02d}"
    verify = None
    supplied = getattr(a, "verify", None)      # gen 子命令没有 --verify
    if supplied:
        verify = supplied.upper()
        if len(verify) != 6 or any(c not in VERIFY_ALPHABET for c in verify):
            raise ValueError(f"验证码须为 6 位（字符集去 I/O/0/1）：{supplied}")
    r = gen(a.vendor, a.model_code, year, week, a.seq, verify=verify)
    r.update({"model_code": a.model_code.upper(), "year": year, "week": week,
              "qr": f"IPC1:{r['device_id']}:{r['verify_code']}:{getattr(a, 'model', 'SP-R1-02')}"})
    return r


def announce(r):
    log("gen", f"DeviceID    : {r['device_id']}   （分组 {label_groups(r['device_id'])}）")
    log("gen", f"配置码 {r['model_code']}    : {hw_text(r['model_code']) or '未登记！'}")
    log("gen", f"生产年周    : {r['year']} 年第 {int(r['week'])} 周；序列 {r['seq_text']}")
    log("gen", f"VerifyCode  : {r['verify_code']}")
    log("gen", f"QR 文本     : {r['qr']}")


# ---------- 本地 mock 安全存储（开发调试，不连设备） ----------

def _mock_path(key):
    return os.path.join(MOCK_DIR, f"secure_{key}.bin")


def _mock_read(key):
    try:
        with open(_mock_path(key), "rb") as f:
            return f.read().decode("ascii", "replace")
    except FileNotFoundError:
        return None


def _mock_write(key, value):
    os.makedirs(MOCK_DIR, exist_ok=True)
    with open(_mock_path(key), "wb") as f:
        f.write(value.encode("ascii"))
    try:
        os.chmod(_mock_path(key), 0o600)
    except OSError:
        pass


# ---------- 串口（真机模组） ----------

def open_board(port):
    import board as board_mod          # 同目录：复用串口会话/命令执行
    b = board_mod.Board(port)
    b.shell_prompt()
    return b


def _serial_read(board, key):
    """读回一个安全存储键：哨兵包裹内容，正则提取（回显行已被 run() 去掉）。"""
    out, rc = board.run(f"printf '\\n<'; cat {SEC_DIR}/{key}; printf '>\\n'", timeout=15)
    if rc != 0:
        return None
    m = re.search(r"<([A-Za-z0-9]{1,120})>", out)
    return m.group(1) if m else None


def _serial_write(board, key, value):
    cmd = (f"mkdir -p {SEC_DIR} && printf '%s' '{value}' > {SEC_DIR}/{key} && "
           f"chmod 600 {SEC_DIR}/{key} && sync")
    _, rc = board.run(cmd, timeout=20)
    return rc == 0


def _serial_has_device_id(board):
    _, rc = board.run(f"ls {SEC_DIR}/device_id", timeout=15)
    return rc == 0


# ---------- 校验 ----------

def check_pair(device_id, verify):
    """回读结果的硬校验：字符集 + Luhn mod32 + 验证码口径。返回错误列表。"""
    errs = []
    if device_id is None:
        errs.append("device_id 读不到（未烧录或存储不可读）")
    else:
        if len(device_id) != 17:
            errs.append(f"device_id 长度 {len(device_id)} ≠ 17")
        elif not devid_valid(device_id):
            errs.append("device_id 校验位不通过（Luhn mod32）")
    if verify is None:
        errs.append("verify_code 读不到")
    else:
        if len(verify) != 6 or any(c not in VERIFY_ALPHABET for c in verify):
            errs.append(f"verify_code 口径不符：{verify!r}")
    return errs


def dump_json(result):
    print(json.dumps(result, ensure_ascii=False), flush=True)


# ---------- 子命令 ----------

def cmd_gen(a):
    try:
        r = build_code(a)
    except ValueError as e:
        log("FAIL", str(e))
        return 1
    r["seq_text"] = f"{a.seq:05d}"
    announce(r)
    result = {**{k: r[k] for k in ("device_id", "verify_code", "model_code", "qr", "year", "week")},
              "hw": hw_text(r["model_code"]), "result": "generated"}
    if a.json:
        dump_json(result)
    log("PASS", "码值已生成（未写入任何设备）")
    return 0


def cmd_burn(a):
    try:
        r = build_code(a)
    except ValueError as e:
        log("FAIL", str(e))
        return 1
    r["seq_text"] = f"{a.seq:05d}"
    announce(r)
    values = {"device_id": r["device_id"], "verify_code": r["verify_code"]}
    result = {**{k: r[k] for k in ("device_id", "verify_code", "model_code", "qr", "year", "week")},
              "hw": hw_text(r["model_code"]), "target": a.target, "result": "fail"}

    if a.dry_run:
        log("PASS", "演练完成（--dry-run，未连接设备、未写入）")
        result["result"] = "dry-run"
        if a.json:
            dump_json(result)
        return 0

    if a.target == "mock":
        if _mock_read("device_id") and not a.force:
            log("FAIL", f"mock 安全存储已有 device_id（{_mock_read('device_id')}），重烧请加 --force")
            if a.json:
                dump_json(result)
            return 1
        for k, v in values.items():
            _mock_write(k, v)
        back = {k: _mock_read(k) for k in values}
    else:
        board = open_board(a.port)
        try:
            if _serial_has_device_id(board) and not a.force:
                old = _serial_read(board, "device_id")
                log("FAIL", f"模组已烧录过 DeviceID（{old or '存在但读不回'}），重烧请加 --force")
                if a.json:
                    dump_json(result)
                return 1
            for k, v in values.items():
                if not _serial_write(board, k, v):
                    log("FAIL", f"写入 {k} 失败（shell 返回非 0）")
                    if a.json:
                        dump_json(result)
                    return 1
            back = {k: _serial_read(board, k) for k in values}
        finally:
            board.close()

    errs = check_pair(back.get("device_id"), back.get("verify_code"))
    same = (back.get("device_id") == values["device_id"] and
            back.get("verify_code") == values["verify_code"])
    if not same:
        errs.append("回读内容与写入值不一致")
    result["readback"] = back
    result["result"] = "pass" if not errs else "fail"
    if a.json:
        dump_json(result)
    if errs:
        for e in errs:
            log("FAIL", e)
        return 1
    log("PASS", f"烧录并回读一致：{values['device_id']} / {values['verify_code']}（{a.target}）")
    log("info", "固件按需读取安全存储，控制台/二维码立即生效，无需重启")
    return 0


def cmd_verify(a):
    result = {"target": a.target}
    if a.target == "mock":
        back = {k: _mock_read(k) for k in ("device_id", "verify_code")}
    else:
        board = open_board(a.port)
        try:
            back = {k: _serial_read(board, k) for k in ("device_id", "verify_code")}
        finally:
            board.close()
    errs = check_pair(back.get("device_id"), back.get("verify_code"))
    result.update({"device_id": back.get("device_id"), "verify_code": back.get("verify_code"),
                   "hw": hw_text((back.get("device_id") or "")[3:7]) if back.get("device_id") else None,
                   "result": "pass" if not errs else "fail"})
    if back.get("device_id"):
        log("info", f"DeviceID {back['device_id']}（分组 {label_groups(back['device_id'])}）")
        log("info", f"硬件配置 {result['hw'] or '配置码未登记'}")
    if a.json:
        dump_json(result)
    if errs:
        for e in errs:
            log("FAIL", e)
        return 1
    log("PASS", "回读校验通过（长度/字符集/Luhn mod32/验证码口径）")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_port(sp):
        sp.add_argument("--port", default=os.environ.get("IPC_BOARD_PORT", "COM6"),
                        help="串口（默认 COM6 或 $IPC_BOARD_PORT）")

    def common(sp):
        sp.add_argument("--vendor", default="JPC", help="厂商码 3 位（缺省 JPC = IpcCloud/JetsCam）")
        sp.add_argument("--model-code", required=True, help="配置码 4 位（须已在 HW_CODE 登记）")
        sp.add_argument("--year", default=None, help="生产年 2 位（缺省当前 ISO 年）")
        sp.add_argument("--week", default=None, help="生产周 2 位（缺省当前 ISO 周）")
        sp.add_argument("--seq", type=int, required=True, help="产线流水 0-99999（由 MES 下发）")
        sp.add_argument("--json", action="store_true", help="额外输出一行 JSON 结果（接 MES）")

    g = sub.add_parser("gen", help="只生成码值（MES 预占号 / 打印标贴）")
    common(g)
    g.set_defaults(fn=cmd_gen)

    b = sub.add_parser("burn", help="生成并写入模组，回读校验")
    add_port(b)
    common(b)
    b.add_argument("--verify", default=None, help="出厂验证码 6 位（缺省随机，字符集去 I/O/0/1）")
    b.add_argument("--model", default="SP-R1-02", help="型号（仅用于 QR 文本）")
    b.add_argument("--target", choices=("serial", "mock"), default="serial", help="写入目标（缺省串口真机）")
    b.add_argument("--force", action="store_true", help="允许覆盖已烧录的 device_id")
    b.add_argument("--dry-run", action="store_true", help="只生成与打印，不连接设备")
    b.set_defaults(fn=cmd_burn)

    v = sub.add_parser("verify", help="回读并校验已烧录的模组")
    add_port(v)
    v.add_argument("--target", choices=("serial", "mock"), default="serial")
    v.add_argument("--json", action="store_true")
    v.set_defaults(fn=cmd_verify)

    a = p.parse_args()
    try:
        return a.fn(a)
    except (ValueError, KeyError) as e:
        log("FAIL", str(e))
        return 1
    except Exception as e:                     # 串口占用/超时等产线故障
        traceback.print_exc()
        log("FAIL", f"{type(e).__name__}: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
