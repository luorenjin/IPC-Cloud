#!/usr/bin/env python3
"""GK7205V200 真机自动化：串口 + 内置 TFTP，免断电部署与冒烟测试。

子命令：
  deploy-app      只替换 /usr/bin/ipc_app 并重启服务（保留 MAC/配置/激活状态）
  reflash-rootfs  reboot → 抢停 U-Boot → tftp + sf erase/write 整个 rootfs 分区 → reset
  reflash-kernel  同上，但烧的是 kernel 分区（0x100000+5M，含 DTS）
  smoke           串口/HTTP 冒烟检查，拉回 /var/log/ipc_app.log
  shell           在板子 shell 执行一条命令并打印输出
  push            经 TFTP 把本地文件推到板子（调试用：内核模块/自检程序）

依赖：pyserial（pip install pyserial）。串口被 BurnTool / 终端占用时无法使用。
"""
import argparse
import datetime
import glob
import http.client
import ipaddress
import json
import os
import re
import socket
import struct
import sys
import threading
import time

import serial

FW_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG_DIR = os.path.join(FW_DIR, "docker", "out", "gk7205v200")
APP_BIN = os.path.join(IMG_DIR, "app", "ipc_app")
ROOTFS_IMG = os.path.join(IMG_DIR, "spi_image", "rootfs-console.64k.jffs2")
LOG_DIR = os.path.join(FW_DIR, "tools", "logs")

ROOTFS_OFF = 0x600000
ROOTFS_LEN = 0xA00000
# bootargs 分区（mtd1）：内核内存切分（mem=）就在这里。mparts 是
# sfc:512K(boot),512K(bootargs),5M(kernel),10M(rootfs) —— bootargs 在 0x80000。
# MIN(24M)：内核只用低 24MB，剩余 40MB 全给 osal 的媒体内存池（1080p 多通道
# + ISP 内部缓冲靠它）。改这里必须同步改 S85mpp 的 MMZ_START/MMZ_SIZE。
BOOTARGS_OFF = 0x80000
BOOTARGS_LEN = 0x80000
BOOTARGS_IMG = os.path.join(IMG_DIR, "spi_image", "bootargs.bin")
KERNEL_IMG = os.path.join(IMG_DIR, "spi_image", "uImage_gk7205v200")
KERNEL_OFF = 0x100000
KERNEL_LEN = 0x500000
LOAD_ADDR = 0x42000000
PROMPT_RE = re.compile(rb"[#$] $")
UBOOT_PROMPT = b"# "
ADDR_RE = re.compile(rb"\[udhcpc\] eth0 got address (\d+\.\d+\.\d+\.\d+)")
CRASH_RE = re.compile(rb"Kernel panic|Unable to handle kernel|Segmentation fault|"
                      rb"VFS: Cannot open root|Oops:|jffs2: .*[Ee]rror")


def log(msg):
    print(f"[board] {msg}", flush=True)


class Board:
    """串口会话。所有收到的字节同时写入 transcript，便于事后排查。"""

    def __init__(self, port, baud=115200, transcript=None):
        try:
            self.ser = serial.Serial(port, baud, timeout=0.1)
        except serial.SerialException as e:
            sys.exit(f"打不开串口 {port}：{e}\n先断开 MobaXterm/PuTTY/BurnTool 等占用该串口的程序")
        self.transcript = transcript
        self.buf = b""

    def close(self):
        self.ser.close()

    def _read(self):
        data = self.ser.read(4096)
        if data:
            self.buf += data
            if self.transcript:
                self.transcript.write(data)
                self.transcript.flush()
        return data

    def write(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.ser.write(data)

    def expect(self, pattern, timeout, fail=CRASH_RE):
        """等待 pattern（bytes 或已编译正则）出现，返回 match；超时或命中 fail 抛异常。"""
        rx = pattern if hasattr(pattern, "search") else re.compile(re.escape(pattern))
        end = time.time() + timeout
        while time.time() < end:
            self._read()
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            if fail is not None:
                bad = fail.search(self.buf)
                if bad:
                    raise RuntimeError(f"串口出现异常输出: {bad.group(0).decode(errors='replace')}")
        tail = self.buf[-300:].decode(errors="replace")
        raise TimeoutError(f"等待 {rx.pattern!r} 超时（{timeout}s），串口尾部：\n{tail}")

    def shell_prompt(self, timeout=10):
        """确保处于 Linux shell 提示符（必要时回车唤醒 getty 自动登录）。"""
        end = time.time() + timeout
        while time.time() < end:
            self.buf = b""
            self.write("\n")
            try:
                self.expect(PROMPT_RE, 2, fail=None)
                return
            except TimeoutError:
                continue
        raise TimeoutError("拿不到 shell 提示符：检查串口是否被占用、板子是否已启动")

    def run(self, cmd, timeout=30):
        """执行 shell 命令，返回 (输出, 退出码)。用哨兵行精确截取输出。"""
        tag = f"__RC{int(time.time() * 1000) % 100000}__"
        self.buf = b""
        self.write(f"{cmd}; echo {tag}$?\n")
        m = self.expect(re.compile(rf"{tag}(\d+)\r?\n".encode()), timeout, fail=None)
        text = self.buf_before(m)
        return text, int(m.group(1))

    def buf_before(self, m):
        out = m.string[:m.start()].decode(errors="replace")
        lines = out.replace("\r", "").split("\n")
        return "\n".join(lines[1:]).rstrip()  # 去掉回显的命令行

    def uboot(self, cmd, timeout=10, ok=UBOOT_PROMPT):
        self.buf = b""
        self.write(cmd + "\n")
        return self.expect(ok, timeout, fail=None)


class TftpServer:
    """最小只读 TFTP 服务（RFC 1350 + blksize 选项），只提供白名单文件。"""

    def __init__(self, bind_ip, files):
        self.files = files  # {name: path}
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.sock.bind((bind_ip, 69))
        except OSError as e:
            raise RuntimeError(f"TFTP 绑定 {bind_ip}:69 失败（被 Tftpd64/BurnTool 占用或无权限）：{e}")
        self.sock.settimeout(0.5)
        self.stop_evt = threading.Event()
        self.thread = threading.Thread(target=self._loop, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *a):
        self.stop_evt.set()
        self.thread.join(2)
        self.sock.close()

    def _loop(self):
        while not self.stop_evt.is_set():
            try:
                pkt, peer = self.sock.recvfrom(1024)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(pkt) < 4 or struct.unpack("!H", pkt[:2])[0] != 1:  # 只接受 RRQ
                continue
            parts = pkt[2:].split(b"\0")
            name = os.path.basename(parts[0].decode(errors="replace"))
            opts = {parts[i].decode().lower(): parts[i + 1].decode()
                    for i in range(2, len(parts) - 1, 2) if parts[i]}
            threading.Thread(target=self._send, args=(peer, name, opts), daemon=True).start()

    def _send(self, peer, name, opts):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(1.0)
        try:
            path = self.files.get(name)
            if not path:
                s.sendto(struct.pack("!HH", 5, 1) + b"file not found\0", peer)
                log(f"TFTP 拒绝未授权文件: {name}")
                return
            data = open(path, "rb").read()
            blksize = 512
            if "blksize" in opts:
                blksize = max(8, min(int(opts["blksize"]), 1468))
                oack = struct.pack("!H", 6) + b"blksize\0" + str(blksize).encode() + b"\0"
                if not self._xfer(s, peer, oack, 0):
                    return
            nblocks = len(data) // blksize + 1
            for blk in range(1, nblocks + 1):
                chunk = data[(blk - 1) * blksize: blk * blksize]
                pkt = struct.pack("!HH", 3, blk & 0xFFFF) + chunk
                if not self._xfer(s, peer, pkt, blk & 0xFFFF):
                    log(f"TFTP 传输 {name} 在第 {blk} 块中断")
                    return
            log(f"TFTP 已发送 {name}（{len(data)} 字节）")
        finally:
            s.close()

    @staticmethod
    def _xfer(s, peer, pkt, blk):
        for _ in range(8):
            s.sendto(pkt, peer)
            try:
                while True:
                    ack, _ = s.recvfrom(516)
                    op, n = struct.unpack("!HH", ack[:4])
                    if op == 4 and n == blk:
                        return True
                    if op == 5:
                        return False
            except socket.timeout:
                continue
        return False


def open_transcript(tag):
    os.makedirs(LOG_DIR, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(LOG_DIR, f"{ts}-{tag}.serial.log")
    log(f"串口记录: {path}")
    return open(path, "wb")


def pick_host_ip(board_ip):
    """选与板子同网段的本机地址（避开 vEthernet/WSL 等虚拟网卡）。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((board_ip, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def board_ip_from_shell(b):
    out, _ = b.run("ifconfig eth0 | grep 'inet addr'")
    m = re.search(r"inet addr:(\d+\.\d+\.\d+\.\d+)", out)
    if not m:
        raise RuntimeError(f"eth0 没有 IPv4 地址：{out}")
    return m.group(1)


MAC_RE = re.compile(r"^([0-9a-f]{2}:){5}[0-9a-f]{2}$")


def read_saved_mac(b):
    out, _ = b.run("cat /etc/ipc/mac 2>/dev/null")
    mac = out.strip().lower()
    return mac if MAC_RE.match(mac) and mac != "00:00:00:00:00:00" else None


def restore_mac(b, mac, want_ip=None):
    """重烧会擦掉 /etc/ipc/mac，新系统首次开机已用随机 MAC 拿了租约。
    写回旧 MAC、释放那份多余租约后按旧 MAC 重新 DHCP，并用 -r 请求原 IP。"""
    if read_saved_mac(b) == mac:
        return board_ip_from_shell(b)
    log(f"写回 MAC {mac}，重新 DHCP" + (f"，请求 {want_ip}" if want_ip else ""))
    b.run(f"echo {mac} > /etc/ipc/mac.tmp && mv /etc/ipc/mac.tmp /etc/ipc/mac && sync")
    # SIGUSR2 让 udhcpc 发 DHCPRELEASE，路由器立即回收随机 MAC 占的地址
    b.run("kill -USR2 $(cat /var/run/udhcpc.eth0.pid) 2>/dev/null; sleep 1; "
          "kill $(cat /var/run/udhcpc.eth0.pid) 2>/dev/null; sleep 1")
    b.run(f"ifconfig eth0 down && ifconfig eth0 hw ether {mac} && ifconfig eth0 up")
    b.buf = b""
    req = f"-r {want_ip} " if want_ip else ""
    b.write(f"udhcpc -b {req}-i eth0 -s /usr/share/udhcpc/default.script -p /var/run/udhcpc.eth0.pid\n")
    m = b.expect(ADDR_RE, 60, fail=None)
    bip = m.group(1).decode()
    if read_saved_mac(b) != mac:
        raise RuntimeError("写回 MAC 后 /etc/ipc/mac 仍不一致")
    # ipc_app 已监听 0.0.0.0，换地址不需要重启；重启只为让冒烟测试检查干净的新进程
    b.run("killall ipc_app; sleep 1; /etc/init.d/S90ipcapp")
    log(f"MAC 已保留，地址 {bip}")
    return bip


def md5_file(path):
    import hashlib
    return hashlib.md5(open(path, "rb").read()).hexdigest()


# ---------------------------------------------------------------- 子命令

def cmd_deploy_app(a):
    if not os.path.isfile(a.app):
        sys.exit(f"找不到 {a.app}，先跑 make -f Makefile fw-build")
    with open_transcript("deploy-app") as tr:
        b = Board(a.port, transcript=tr)
        try:
            b.shell_prompt()
            bip = a.board_ip or board_ip_from_shell(b)
            hip = a.host_ip or pick_host_ip(bip)
            log(f"板子 {bip}，本机 {hip}")
            with TftpServer(hip, {"ipc_app": a.app}):
                b.run("killall ipc_app 2>/dev/null; sleep 1")
                out, rc = b.run(f"tftp -b 1468 -g -r ipc_app -l /tmp/ipc_app.new {hip}", timeout=60)
                if rc != 0:
                    raise RuntimeError(f"板子 tftp 下载失败：{out}")
            out, _ = b.run("md5sum /tmp/ipc_app.new")
            want = md5_file(a.app)
            if want not in out:
                raise RuntimeError(f"md5 不一致：本地 {want}，板子 {out}")
            b.run("chmod 755 /tmp/ipc_app.new && mv /tmp/ipc_app.new /usr/bin/ipc_app && sync")
            b.run("/etc/init.d/S90ipcapp")
            log(f"ipc_app 已替换并启动（md5 {want}）")
            return smoke(b, bip, a)
        finally:
            b.close()


def _reflash_partition(a, off, length, what, image):
    """抢停 U-Boot → TFTP 拉镜像 → sf erase/write 指定分区 → reset 并冒烟检查。

    rootfs 与 kernel 共用这条路径，只有分区偏移/长度/镜像不同。
    """
    if off == 0:
        sys.exit("拒绝擦写 0x0：那是 U-Boot 分区，写坏会变砖")
    if not os.path.isfile(image):
        sys.exit(f"找不到 {image}")
    size = os.path.getsize(image)
    if size > length:
        sys.exit(f"镜像 {size} 字节超过 {what} 分区 {length:#x}")
    with open_transcript(f"reflash-{what}") as tr:
        b = Board(a.port, transcript=tr)
        try:
            b.shell_prompt()
            bip = a.board_ip or board_ip_from_shell(b)
            # 只有 reflash-rootfs 带 --mac；kernel 分区不动 rootfs，MAC 不会变
            mac = read_saved_mac(b) if getattr(a, "mac", None) is None else a.mac
            if mac and not MAC_RE.match(mac):
                sys.exit(f"--mac 格式不对：{mac}")
            bip_before = bip
            hip = a.host_ip or pick_host_ip(bip)
            log(f"本机 {hip}，U-Boot 临时地址 {bip}，保留 MAC {mac or '（无，将生成新 MAC）'}")

            b.buf = b""
            b.write("\nreboot\n")
            log("等待 U-Boot，持续发送按键抢停 autoboot…")
            end = time.time() + 60
            while time.time() < end:
                b.write(b"\x03 ")
                b._read()
                if b"Hit any key" in b.buf or b"U-Boot" in b.buf:
                    break
                time.sleep(0.02)
            else:
                raise TimeoutError("60s 内没看到 U-Boot 启动")
            end = time.time() + 5
            while time.time() < end:
                b.write(b"\x03 ")
                b._read()
                if b"Starting kernel" in b.buf:
                    raise RuntimeError("没抢停 autoboot（bootdelay=0），重跑本命令即可")
                if re.search(rb"\n[^\n]*# $", b.buf):
                    break
                time.sleep(0.02)
            b.uboot("", 3)
            log("已停在 U-Boot")

            b.uboot(f"setenv ipaddr {bip}")
            b.uboot("setenv netmask 255.255.255.0")
            b.uboot(f"setenv serverip {hip}")
            b.buf = b""
            b.write("printenv ethaddr\n")
            m = b.expect(UBOOT_PROMPT, 3, fail=None)
            if b"ethaddr=" not in m.string or b"00:00:00:00:00:00" in m.string:
                b.uboot("setenv ethaddr 02:00:00:00:00:01")

            with TftpServer(hip, {os.path.basename(image): image}):
                b.uboot(f"mw.b {LOAD_ADDR:#x} 0xff {length:#x}", 10)
                b.buf = b""
                b.write(f"tftp {LOAD_ADDR:#x} {os.path.basename(image)}\n")
                m = b.expect(re.compile(rb"Bytes transferred = (\d+)|TFTP error|Retry count exceeded|"
                                        rb"ARP Retry count exceeded"), 120, fail=None)
                if not m.group(1) or int(m.group(1)) != size:
                    raise RuntimeError(f"U-Boot tftp 失败：{m.group(0).decode(errors='replace')}")
                b.expect(UBOOT_PROMPT, 5, fail=None)
            log(f"已传输 {size} 字节，开始擦写 {what}（{off:#x}+{length:#x}）")
            b.uboot("sf probe 0", 10)
            b.uboot(f"sf erase {off:#x} {length:#x}", 180,
                    ok=re.compile(rb"Erased: OK[\s\S]*# |OK[\s\S]*# "))
            b.uboot(f"sf write {LOAD_ADDR:#x} {off:#x} {length:#x}", 300,
                    ok=re.compile(rb"Written: OK[\s\S]*# |OK[\s\S]*# "))
            log("写入完成，reset")
            b.buf = b""
            b.write("reset\n")
            m = b.expect(ADDR_RE, 120)
            bip = m.group(1).decode()
            log(f"新 rootfs 启动，DHCP 地址 {bip}")
            b.expect(b"[S90ipcapp] starting console", 30)
            time.sleep(2)
            b.shell_prompt(20)
            if mac:
                old_ip = bip_before
                bip = restore_mac(b, mac, old_ip)
                if bip != old_ip:
                    log(f"警告：地址由 {old_ip} 变为 {bip}（路由器未按 MAC 续回原租约）")
            return smoke(b, bip, a)
        finally:
            b.close()


def cmd_reflash_rootfs(a):
    return _reflash_partition(a, ROOTFS_OFF, ROOTFS_LEN, "rootfs", a.image)


def cmd_reflash_kernel(a):
    return _reflash_partition(a, KERNEL_OFF, KERNEL_LEN, "kernel", a.image)


def cmd_reflash_bootargs(a):
    """重烧 bootargs 分区（含 mem= 内存切分）。

    bootargs 改成 mem=24M 后内核只用低 24MB，剩余 40MB 全给 MPP 的 MMZ——
    1080p 多通道出流必需（见 firmware/docker/rootfs-overlay/etc/init.d/S85mpp
    里的 MMZ_START/MMZ_SIZE，**两者必须同步改**）。
    不动 rootfs，所以 MAC / 激活凭据 / 配置都不受影响。
    """
    return _reflash_partition(a, BOOTARGS_OFF, BOOTARGS_LEN, "bootargs", a.image)


def http_get(ip, port, path, timeout=5):
    import gzip
    c = http.client.HTTPConnection(ip, port, timeout=timeout)
    try:
        c.request("GET", path, headers={"Accept-Encoding": "gzip"})
        r = c.getresponse()
        body = r.read()
        if r.getheader("Content-Encoding") == "gzip":
            body = gzip.decompress(body)
        return r.status, body
    finally:
        c.close()


def smoke(b, bip, a):
    results = []

    def check(name, fn):
        try:
            detail = fn()
            results.append((name, True, detail or ""))
        except Exception as e:  # noqa: BLE001  冒烟汇总所有失败，不在第一个就停
            results.append((name, False, str(e)))

    def proc():
        out, _ = b.run("pidof ipc_app")
        if not out.strip().isdigit():
            raise RuntimeError("ipc_app 进程不存在")
        return f"pid {out.strip()}"

    def mac_fixed():
        out, _ = b.run("cat /etc/ipc/mac; ifconfig eth0 | grep HWaddr")
        lines = out.split("\n")
        saved = lines[0].strip().lower()
        if saved not in out.lower().split("hwaddr")[-1]:
            raise RuntimeError(f"eth0 MAC 与 /etc/ipc/mac 不一致：{out}")
        return saved

    def http_root():
        st, body = http_get(bip, a.http_port, "/")
        if st != 200 or b"<html" not in body.lower():
            raise RuntimeError(f"GET / → {st}")
        return f"{len(body)} 字节"

    def http_state():
        st, body = http_get(bip, a.http_port, "/api/v1/auth/state")
        j = json.loads(body)
        if st != 200 or j.get("code") != 0 or "activated" not in j:
            raise RuntimeError(f"GET /api/v1/auth/state → {st} {body[:120]!r}")
        return f"activated={j['activated']}"

    def http_protected():
        st, _ = http_get(bip, a.http_port, "/api/v1/system/status")
        if st not in (401, 403):
            raise RuntimeError(f"未登录访问 /api/v1/system/status 返回 {st}，应为 401/403")
        return f"{st}"

    def app_log():
        out, _ = b.run("tail -n 200 /var/log/ipc_app.log", timeout=10)
        os.makedirs(LOG_DIR, exist_ok=True)
        p = os.path.join(LOG_DIR, datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "-ipc_app.log")
        open(p, "w", encoding="utf-8").write(out)
        if re.search(r"\[E\]|ERROR|失败", out) and "启动完成" not in out:
            raise RuntimeError(f"应用日志含错误且未启动完成，见 {p}")
        return p

    for _ in range(10):  # 服务刚起，给 HTTP 监听一点时间
        try:
            http_get(bip, a.http_port, "/api/v1/auth/state", timeout=2)
            break
        except OSError:
            time.sleep(1)

    check("ipc_app 进程", proc)
    check("MAC 固定", mac_fixed)
    check("GET /", http_root)
    check("GET /api/v1/auth/state", http_state)
    check("未登录拦截", http_protected)
    check("应用日志", app_log)

    ok = all(r[1] for r in results)
    print()
    for name, passed, detail in results:
        print(f"  {'PASS' if passed else 'FAIL'}  {name:<24} {detail}")
    print(f"\nSMOKE {'PASS' if ok else 'FAIL'}  board={bip}")
    return 0 if ok else 1


def cmd_smoke(a):
    with open_transcript("smoke") as tr:
        b = Board(a.port, transcript=tr)
        try:
            b.shell_prompt()
            bip = a.board_ip or board_ip_from_shell(b)
            return smoke(b, bip, a)
        finally:
            b.close()


def cmd_shell(a):
    b = Board(a.port)
    try:
        b.shell_prompt()
        out, rc = b.run(a.command, timeout=a.timeout)
        print(out)
        return rc
    finally:
        b.close()


def cmd_push(a):
    """把本地文件经 TFTP 推到板子。

    调试期专用：内核模块、自检程序这类需要反复替换的文件，走这条路不必重烧
    rootfs（重烧会清掉 /etc/ipc/sec 里的激活凭据，还得手工备份回写）。
    推送后逐个 md5 比对，避免“传过去但内容不全”这种最难查的故障。

    （板子端 tftp 客户端由 busybox 提供，deploy-app 用的就是同一条路径。）
    """
    files = {}
    # 通配符自己展开：PowerShell 把 *.ko 原样当参数传给外部程序，不会先在 shell 里展开
    paths = []
    for pat in a.files:
        hit = glob.glob(pat)
        if not hit:
            sys.exit(f"找不到本地文件：{pat}")
        paths.extend(hit)
    for p in paths:
        if not os.path.isfile(p):
            sys.exit(f"不是文件：{p}")
        files[os.path.basename(p)] = p

    with open_transcript("push") as tr:
        b = Board(a.port, transcript=tr)
        try:
            b.shell_prompt()
            bip = a.board_ip or board_ip_from_shell(b)
            hip = a.host_ip or pick_host_ip(bip)
            log(f"板子 {bip}，本机 {hip}，目标 {a.to}")
            b.run(f"mkdir -p {a.to}", timeout=20)
            with TftpServer(hip, files):
                for name in files:
                    out, rc = b.run(f"tftp -b 1468 -g -r {name} -l {a.to}/{name} {hip}", timeout=180)
                    if rc != 0:
                        raise RuntimeError(f"下载 {name} 失败（rc={rc}）：{out}")
                    if a.exec_bit:
                        b.run(f"chmod 755 {a.to}/{name}", timeout=20)
            bad = []
            for name, path in files.items():
                out, _ = b.run(f"md5sum {a.to}/{name}", timeout=20)
                if md5_file(path) not in out:
                    bad.append(name)
            if bad:
                raise RuntimeError(f"md5 不一致：{bad}")
            out, _ = b.run(f"ls -l {a.to}", timeout=20)
            log(f"已推送 {len(files)} 个文件并校验通过")
            print(out)
            return 0
        finally:
            b.close()


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default=os.environ.get("IPC_BOARD_PORT", "COM6"), help="串口（默认 COM6 或 $IPC_BOARD_PORT）")
    p.add_argument("--board-ip", help="板子 IP（缺省从串口 ifconfig 读取）")
    p.add_argument("--host-ip", help="本机 TFTP 地址（缺省自动选与板子同网段的网卡）")
    p.add_argument("--http-port", type=int, default=8080)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("deploy-app", help="只替换 ipc_app 并重启服务")
    s.add_argument("--app", default=APP_BIN)
    s.set_defaults(fn=cmd_deploy_app)

    s = sub.add_parser("reflash-rootfs", help="经 U-Boot 重烧整个 rootfs 分区（不断电）")
    s.add_argument("--image", default=ROOTFS_IMG)
    s.add_argument("--mac", type=str.lower, help="重烧后写回的 MAC（缺省读取板子当前 /etc/ipc/mac）")
    s.add_argument("--new-mac", dest="mac", action="store_const", const="",
                   help="不保留 MAC，模拟全新设备首次开机")
    s.set_defaults(fn=cmd_reflash_rootfs)

    s = sub.add_parser("reflash-kernel",
                       help="经 U-Boot 重烧 kernel 分区（0x100000+5M，含 DTS）（不断电）")
    s.add_argument("--image", default=KERNEL_IMG)
    s.set_defaults(fn=cmd_reflash_kernel)

    s = sub.add_parser("reflash-bootargs",
                       help="经 U-Boot 重烧 bootargs 分区（0x80000+512K，含 mem= 内存切分）")
    s.add_argument("--image", default=BOOTARGS_IMG)
    s.set_defaults(fn=cmd_reflash_bootargs)

    s = sub.add_parser("smoke", help="冒烟测试")
    s.set_defaults(fn=cmd_smoke)

    s = sub.add_parser("shell", help="在板子上执行一条命令")
    s.add_argument("command")
    s.add_argument("--timeout", type=float, default=30)
    s.set_defaults(fn=cmd_shell)

    s = sub.add_parser("push", help="经 TFTP 把本地文件推到板子（调试用）")
    s.add_argument("files", nargs="+", help="本地文件路径（推到 --to 目录，保持文件名）")
    s.add_argument("--to", default="/tmp", help="板子上的目标目录（默认 /tmp）")
    s.add_argument("--exec-bit", dest="exec_bit", action="store_true",
                   help="推送后 chmod 755（推可执行文件时用）")
    s.set_defaults(fn=cmd_push)

    a = p.parse_args()
    if a.board_ip:
        ipaddress.ip_address(a.board_ip)
    sys.exit(a.fn(a))


if __name__ == "__main__":
    main()
