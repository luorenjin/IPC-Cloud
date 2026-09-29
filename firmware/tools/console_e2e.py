#!/usr/bin/env python3
"""本机控制台真机浏览器验收（playwright + 系统 Chrome）。

用法：
  python firmware/tools/console_e2e.py [--url http://172.16.1.185:8080/] [--password Admin@12345]
                                       [--skip-reboot] [--skip-reset]

依赖：pip install playwright（用系统 Chrome：channel="chrome"，无需 playwright install）。
设备必须处于「未激活」或「已用 --password 激活」状态。恢复出厂用例结束后会用同一口令重新激活，
保证跑完设备状态与开始时一致（已激活）。
退出码 0 = 全部通过；截图保存在 firmware/tools/logs/e2e-<时间>/。
"""
import argparse
import datetime
import json
import os
import sys
import time
import urllib.error
import urllib.request

from playwright.sync_api import sync_playwright

LOG_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
ALT_PWD = "Admin@54321"
# 无论设备上报什么能力都**不该出现**的菜单（能力位恒 false 的那几个）。
# 注意：「摄像头」曾因图像页是占位而列在这里，图像页接线后已从名单移除
# （它的显隐由 image.basic 决定，下面 NAV_FEAT 会跟着设备实际上报走）。
HIDDEN_MENUS = ["事件侦测", "云服务", "算法赋能"]

# 与 firmware/web/js/router.js 的 NAV / TOP_FEAT 一一对应的 feature ID。
# 写在这里是为了让 E4 **按设备当时上报的能力**推导期望（而不是写死成
# “顶栏只剩设置”）：能力位会随固件演进变真（gk 视频 HAL 落地后 preview.live /
# tools.download 就从 false 变 true），写死的断言会跟着过时。
NAV_FEAT = {
    "摄像头": "image.basic", "事件侦测": "event.any", "存储": "storage.tf",
    "网络设置": "network.config", "云服务": "cloud.bind", "系统设置": None,
    "算法赋能": "event.smart",
}
TOP_FEAT = {"preview": "preview.live", "tools": "tools.download", "settings": None}


class E2E:
    def __init__(self, url, pwd, outdir):
        self.url, self.pwd, self.outdir = url, pwd, outdir
        self.results = []
        self.errors = []

    # ---- 工具 ----
    def api(self, path):
        with urllib.request.urlopen(self.url.rstrip("/") + path, timeout=5) as r:
            return json.loads(r.read())

    def page_api(self, path):
        return json.loads(self.pg.evaluate(f"fetch('{path}',{{credentials:'include'}}).then(r=>r.text())"))

    def toast(self):
        try:
            return self.pg.inner_text("#toast", timeout=500)
        except Exception:
            return ""

    def shot(self, name):
        self.pg.screenshot(path=os.path.join(self.outdir, name + ".png"))

    def go(self, grp, sub, tab=None):
        pg = self.pg
        if pg.is_visible("#f-login"):   # 上一个用例失败把会话丢了：先登录，避免连锁失败
            self.login(self.pwd)
        pg.click(".topnav button[data-top=settings]")
        pg.wait_for_timeout(200)
        if not pg.is_visible(f".side-item.sub[data-id='{sub}']"):
            pg.click(f".side-item[data-kind=p][data-id='{grp}']")
            pg.wait_for_timeout(300)
        pg.click(f".side-item.sub[data-id='{sub}']")
        pg.wait_for_timeout(600)
        if tab:
            pg.click(f"#tabs button[data-t='{tab}']")
            pg.wait_for_timeout(900)

    def login(self, pwd):
        self.pg.fill("#f-login [name=p]", pwd)
        self.pg.click("#f-login [type=submit]")
        self.pg.wait_for_timeout(2500)

    def activate(self, pwd):
        self.pg.fill("#f-activate [name=p1]", pwd)
        self.pg.fill("#f-activate [name=p2]", pwd)
        self.pg.click("#f-activate [type=submit]")
        self.pg.wait_for_timeout(3000)

    def case(self, cid, name, fn):
        try:
            detail = fn() or ""
            self.results.append((cid, name, True, detail))
        except Exception as e:  # noqa: BLE001  汇总所有失败
            self.results.append((cid, name, False, str(e)[:300]))
            try:
                self.shot(f"{cid}-fail")
            except Exception:
                pass

    @staticmethod
    def expect(cond, msg):
        if not cond:
            raise AssertionError(msg)

    def wait_unlocked(self, timeout=960):
        """设备按来源 IP 锁定错误登录；上一轮残留的锁定会让整轮用例连锁失败，开跑前先等它解除。"""
        import hashlib
        import hmac
        base = self.url.rstrip("/") + "/api/v1/auth/"

        def post(path, body):
            req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                         headers={"Content-Type": "application/json"}, method="POST")
            try:
                with urllib.request.urlopen(req, timeout=5) as r:
                    return r.status, json.loads(r.read())
            except urllib.error.HTTPError as e:
                return e.code, {}

        if not self.api("/api/v1/auth/state").get("activated"):
            return
        t0 = time.time()
        while time.time() - t0 < timeout:
            _, c = post("challenge", {"user": "admin"})
            key = hashlib.pbkdf2_hmac("sha256", self.pwd.encode(), bytes.fromhex(c["salt"]), c["iter"], 32)
            proof = hmac.new(key, c["nonce"].encode(), "sha256").hexdigest()
            status, _ = post("login", {"user": "admin", "nonce": c["nonce"], "proof": proof})
            if status == 200:
                return
            if status != 409:
                raise SystemExit(f"口令 {self.pwd} 无法登录设备（HTTP {status}），请确认 --password")
            print(f"[e2e] 设备仍处于登录锁定，已等待 {time.time() - t0:.0f}s…", flush=True)
            time.sleep(15)
        raise SystemExit("设备登录锁定超过 16 分钟未解除")

    def wait_device(self, want_activated, timeout=150):
        t0 = time.time()
        time.sleep(10)
        while time.time() - t0 < timeout:
            try:
                st = self.api("/api/v1/auth/state")
                if st.get("activated") == want_activated:
                    return time.time() - t0
            except Exception:
                pass
            time.sleep(3)
        raise AssertionError(f"{timeout}s 内设备未恢复（期望 activated={want_activated}）")

    # ---- 用例 ----
    def run(self, skip_reboot, skip_reset):
        self.wait_unlocked()
        with sync_playwright() as p:
            self.browser = p.chromium.launch(channel="chrome")
            self.ctx = self.browser.new_context(accept_downloads=True)
            self.pg = self.ctx.new_page()
            self.pg.on("pageerror", lambda e: self.errors.append(str(e)))
            self.pg.on("dialog", lambda d: d.accept("确认") if d.type == "prompt" else d.accept())
            self.pg.goto(self.url)
            self.pg.wait_for_timeout(2000)

            def e1():
                if self.pg.is_visible("#f-activate"):
                    return "未激活，显示激活表单"
                self.expect(self.pg.is_visible("#f-login"), "既不是激活页也不是登录页")
                return "已激活，显示登录表单"
            self.case("E1", "首页按激活状态显示", e1)

            def e2():
                if self.pg.is_visible("#f-activate"):
                    self.activate(self.pwd)
                    self.expect(self.api("/api/v1/auth/state")["activated"], "设备仍未激活")
                    self.expect(self.pg.is_visible("#shell"), "激活后未进入控制台")
                    self.pg.click("#btn-logout")
                    self.pg.wait_for_timeout(1000)
                    return "激活成功"
                return "已激活，跳过"
            self.case("E2", "激活", e2)

            def e3():
                self.login("WrongPass999")
                self.expect(not self.pg.is_visible("#shell"), "错误口令进入了控制台")
                return self.toast()
            self.case("E3", "错误口令被拒", e3)

            def e4():
                self.login(self.pwd)
                self.expect(self.pg.is_visible("#shell"), "正确口令未进入控制台")
                # 顶栏/侧栏的显隐由设备上报的能力决定（router.js 的 TOP_FEAT/NAV + IPC.feat()），
                # 所以期望值从设备自己上报的 features 推导，不写死。必须先等前端把 features
                # 载入：未载入时 feat() 是“全可见”兜底态，会把本来正确的设备读成失败。
                self.pg.wait_for_function(
                    "window.IPC && IPC.S && IPC.S.features !== null", timeout=5000)
                feats = self.pg.evaluate("IPC.S.features")
                tops = self.pg.eval_on_selector_all(".topnav button", "e=>e.filter(x=>!x.hidden).map(x=>x.dataset.top)")
                want = [t for t, fid in TOP_FEAT.items() if fid is None or feats.get(fid) is True]
                self.expect(tops == want, f"顶栏应为 {want}（按上报能力推导），实际：{tops}")
                self.expect("settings" in tops, "顶栏必须保留设置入口")

                # 进设置页再看侧栏：落地页可能是预览（preview.live=true 时），那时侧栏没渲染
                self.pg.click(".topnav button[data-top=settings]")
                self.pg.wait_for_timeout(600)
                menus = self.pg.eval_on_selector_all(".side-item[data-kind=p]", "e=>e.map(x=>x.dataset.id)")
                want_m = [m for m, fid in NAV_FEAT.items() if fid is None or feats.get(fid) is True]
                self.expect(menus == want_m, f"侧栏应为 {want_m}（按上报能力推导），实际：{menus}")
                bad = [m for m in HIDDEN_MENUS if m in menus]
                self.expect(not bad, f"未实现的菜单仍可见：{bad}")
                self.shot("E4-menus")
                return "顶栏：" + "、".join(tops) + "；菜单：" + "、".join(menus)
            self.case("E4", "登录且只显示可用功能", e4)

            def e5():
                self.go("系统设置", "基本设置", "设备信息")
                rows = dict(r.split("\n", 1) for r in
                            self.pg.eval_on_selector_all(".sys-info-row", "e=>e.map(x=>x.innerText)") if "\n" in r)
                info = self.page_api("/api/v1/system/info")
                st1 = self.page_api("/api/v1/system/status")
                time.sleep(2)
                st2 = self.page_api("/api/v1/system/status")
                self.expect(rows.get("设备型号") == info["model"], f"型号不符：{rows}")
                self.expect(rows.get("IP") == info["ip"] and info["ip"] in self.url, f"IP 不符：{rows}")
                self.expect(rows.get("MAC", "").lower() == info["mac"], f"MAC 不符：{rows}")
                self.expect(info["fw_version"] not in ("", "dev"), f"固件版本仍为 {info['fw_version']}")
                self.expect("-2147483648" not in json.dumps(st1), "status 含 INT32_MIN")
                self.expect(info["uptime_s"] > 0, "运行时长为 0")
                self.shot("E5-info")
                return f"{info['model']} {info['fw_version']} {info['ip']} {info['mac']} cpu={st2.get('cpu_usage_pct')}%"
            self.case("E5", "设备信息为真实值", e5)

            def e6():
                self.go("系统设置", "基本设置", "基本设置")
                old = self.pg.input_value("#dev-name")
                self.pg.fill("#dev-name", "测试相机")
                self.pg.click("#body .btn.primary")
                self.pg.wait_for_timeout(1000)
                self.expect("保存成功" in self.toast(), f"保存提示：{self.toast()}")
                self.pg.reload()
                self.pg.wait_for_timeout(3000)
                self.go("系统设置", "基本设置", "基本设置")
                v = self.pg.input_value("#dev-name")
                self.expect(v == "测试相机", f"刷新后设备名为 {v}")
                self.pg.fill("#dev-name", old or "SP-R1-02")
                self.pg.click("#body .btn.primary")
                self.pg.wait_for_timeout(800)
                return "保存并回读一致"
            self.case("E6", "设备名称持久化", e6)

            def e7():
                self.go("系统设置", "基本设置", "时间校对")
                self.pg.select_option("#tm-mode", "manual")
                self.pg.wait_for_timeout(300)
                self.pg.click("#tm-pc")
                self.pg.wait_for_timeout(1500)
                t = self.page_api("/api/v1/system/time")
                diff = abs(t["utc"] - time.time())
                self.expect(diff < 5, f"设备与本机相差 {diff:.1f}s")
                return f"相差 {diff:.1f}s"
            self.case("E7", "与计算机时间同步", e7)

            def e8():
                self.go("系统设置", "系统配置", "系统日志")
                self.pg.wait_for_timeout(1000)
                total = self.pg.inner_text("#log-total")
                self.expect(total != "共 0 条", "日志为空")
                with self.pg.expect_download() as dl:
                    self.pg.click("#log-out")
                return f"{total}，导出 {dl.value.suggested_filename}"
            self.case("E8", "系统日志查看与导出", e8)

            def change(old, new):
                self.go("系统设置", "用户管理")
                self.pg.fill("#pw-old", old)
                self.pg.fill("#pw-new", new)
                self.pg.fill("#pw-conf", new)
                self.pg.click("#pw-save")
                self.pg.wait_for_timeout(3000)
                self.expect(self.pg.is_visible("#f-login"), f"改密后未回登录页：{self.toast()}")

            def e9():
                change(self.pwd, ALT_PWD)
                self.login(self.pwd)
                self.expect(not self.pg.is_visible("#shell"), "旧口令仍能登录")
                self.login(ALT_PWD)
                self.expect(self.pg.is_visible("#shell"), "新口令不能登录")
                change(ALT_PWD, self.pwd)
                self.login(self.pwd)
                self.expect(self.pg.is_visible("#shell"), "改回后不能登录")
                return "改密 → 新口令登录 → 改回"
            self.case("E9", "修改密码", e9)

            def e11():
                self.go("系统设置", "系统配置", "系统日志")
                self.ctx.clear_cookies()
                self.pg.click("#log-go")   # 触发一次需要登录的请求
                self.pg.wait_for_timeout(1500)
                self.expect(self.pg.is_visible("#f-login"), "会话失效后未回到登录页")
                self.login(self.pwd)
                return "回到登录页"
            self.case("E11", "会话失效回登录页", e11)

            def e10():
                if self.pg.is_visible("#shell"):
                    self.pg.click("#btn-logout")
                    self.pg.wait_for_timeout(800)
                msgs = []
                for _ in range(8):
                    with self.pg.expect_response(lambda r: "/auth/login" in r.url, timeout=20000) as resp:
                        self.pg.fill("#f-login [name=p]", "WrongPass999")
                        self.pg.click("#f-login [type=submit]")
                    self.pg.wait_for_timeout(300)
                    msgs.append(f"{resp.value.status}:{self.toast()}")
                    if resp.value.status == 409:
                        break
                self.expect(any("尝试次数过多" in m for m in msgs), f"未触发锁定：{msgs}")
                # 锁定时长首次 60s，重复触发会翻倍（上限 15 分钟）：轮询直到能登录
                t0 = time.time()
                while time.time() - t0 < 960:
                    time.sleep(15)
                    self.login(self.pwd)
                    if self.pg.is_visible("#shell"):
                        break
                self.expect(self.pg.is_visible("#shell"), "锁定解除后仍不能登录")
                return f"第 {len(msgs)} 次提示尝试次数过多，{time.time() - t0:.0f}s 后恢复"
            self.case("E10", "连续错误口令锁定", e10)

            if not skip_reboot:
                def e12():
                    self.go("系统设置", "系统配置", "系统维护")
                    self.pg.click("#reboot")
                    self.pg.wait_for_timeout(1500)
                    self.expect(self.pg.is_visible("#reboot-mask"), "未显示重启遮罩")
                    cost = self.wait_device(True)
                    self.pg.wait_for_selector("#f-login", state="visible", timeout=30000)
                    self.login(self.pwd)
                    self.expect(self.pg.is_visible("#shell"), "重启后不能登录")
                    return f"约 {cost:.0f}s 恢复"
                self.case("E12", "重启设备", e12)

            if not skip_reset:
                def e13():
                    # 恢复出厂已对齐实机挪到「配置管理」（PRD LC-SYS-04：配置管理=简单/完全恢复+导出+导入），
                    # 「系统维护」只剩重启与定时重启
                    self.go("系统设置", "系统配置", "配置管理")
                    self.pg.click("#cfg-factory")
                    self.pg.wait_for_timeout(1500)
                    self.expect(self.pg.is_visible("#reboot-mask"), "未显示等待遮罩")
                    cost = self.wait_device(False)
                    self.pg.wait_for_selector("#f-activate", state="visible", timeout=30000)
                    self.activate(self.pwd)
                    self.expect(self.pg.is_visible("#shell"), "出厂后重新激活失败")
                    return f"约 {cost:.0f}s 回到未激活，已重新激活"
                self.case("E13", "恢复出厂", e13)

            self.browser.close()

    def report(self):
        ok = all(r[2] for r in self.results) and not self.errors
        print()
        for cid, name, passed, detail in self.results:
            print(f"  {'PASS' if passed else 'FAIL'}  {cid:<4} {name:<18} {detail}")
        if self.errors:
            print("  页面脚本错误：", self.errors)
        n = sum(1 for r in self.results if r[2])
        print(f"\nE2E {'PASS' if ok else 'FAIL'} {n}/{len(self.results)}  截图：{self.outdir}")
        return 0 if ok else 1


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=os.environ.get("IPC_CONSOLE_URL", "http://172.16.1.185:8080/"))
    ap.add_argument("--password", default=os.environ.get("IPC_CONSOLE_PWD", "Admin@12345"))
    ap.add_argument("--skip-reboot", action="store_true")
    ap.add_argument("--skip-reset", action="store_true")
    a = ap.parse_args()
    outdir = os.path.join(LOG_ROOT, "e2e-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    os.makedirs(outdir, exist_ok=True)
    e = E2E(a.url, a.password, outdir)
    e.run(a.skip_reboot, a.skip_reset)
    sys.exit(e.report())


if __name__ == "__main__":
    main()
