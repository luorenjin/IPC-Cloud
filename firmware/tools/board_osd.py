#!/usr/bin/env python3
"""板端 OSD 设置的小工具：查看 / 还原（不依赖浏览器）。

为什么需要它：`console_e2e.py` 的 E13「恢复出厂」会把用户 OSD 设置清零，跑完验收后
必须把用户值写回去；手点浏览器 UI 慢且易漏项。控制台登录是 challenge→proof
（PBKDF2-HMAC-SHA256，会话 token 走 HttpOnly Cookie），这里照搬 e2e 的姿势。

⚠️ POST 与 GET 的字段名**不对称**（`console_api.c` 的 osd_set/osd_get）：
  - 通道名：POST 收 `channel_name`，GET 回 `name`
  - 字号：POST 收 `time_font_px` / `name_font_px`（全局一项，两个一起写），GET 回 `font_px`
  - 位置：只能成对给（只给一半会被忽略）
写错键名**不会报错**，接口只当没这个字段（“保存成功但没生效”）。
所以 `set` 之后一律回读核对，对不上就报出来。

用法：
    python firmware/tools/board_osd.py show
    python firmware/tools/board_osd.py set --name 中间 --name-enable --date --week \\
        --font-px 64 --name-pos 2,2 --time-pos 2,90
"""

import argparse
import hashlib
import hmac
import http.cookiejar
import json
import sys
import urllib.error
import urllib.request

DEFAULT_URL = "http://172.16.1.185:8080"
DEFAULT_PWD = "Admin@12345"


class Board:
    def __init__(self, url, user, pwd):
        self.base = url.rstrip("/")
        self.user = user
        self.pwd = pwd.encode()
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def req(self, path, body=None, method=None):
        data = None if body is None else json.dumps(body).encode()
        r = urllib.request.Request(
            self.base + path, data=data,
            headers={"Content-Type": "application/json"}, method=method)
        with self.opener.open(r, timeout=10) as resp:
            txt = resp.read().decode("utf-8", "replace")
        return json.loads(txt) if txt.strip() else {}

    def login(self):
        st = urllib.request.urlopen(self.base + "/api/v1/auth/state", timeout=5)
        if not json.loads(st.read()).get("activated"):
            raise SystemExit("设备未激活，先在浏览器里完成激活")
        c = self.req("/api/v1/auth/challenge", {"user": self.user}, "POST")
        key = hashlib.pbkdf2_hmac("sha256", self.pwd, bytes.fromhex(c["salt"]), c["iter"], 32)
        proof = hmac.new(key, c["nonce"].encode(), "sha256").hexdigest()
        try:
            self.req("/api/v1/auth/login",
                     {"user": self.user, "nonce": c["nonce"], "proof": proof}, "POST")
        except urllib.error.HTTPError as e:
            raise SystemExit(f"登录失败 HTTP {e.code}（409 = 来源 IP 处于锁定，等一会儿再试）")

    def osd(self):
        return self.req("/api/v1/osd")

    def set_osd(self, patch):
        return self.req("/api/v1/osd", patch, "POST")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["show", "set"])
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default=DEFAULT_PWD)
    ap.add_argument("--name")
    ap.add_argument("--name-enable", dest="name_enable", action="store_true")
    ap.add_argument("--no-name-enable", dest="name_enable", action="store_false")
    ap.add_argument("--date", dest="time_date", action="store_true")
    ap.add_argument("--no-date", dest="time_date", action="store_false")
    ap.add_argument("--week", dest="time_week", action="store_true")
    ap.add_argument("--no-week", dest="time_week", action="store_false")
    ap.add_argument("--time-enable", dest="time_enable", action="store_true")
    ap.add_argument("--no-time-enable", dest="time_enable", action="store_false")
    ap.add_argument("--font-px", type=int)
    ap.add_argument("--name-pos", help="x,y（整数百分比）")
    ap.add_argument("--time-pos", help="x,y（整数百分比）")
    ap.set_defaults(name_enable=None, time_date=None, time_week=None, time_enable=None)
    a = ap.parse_args()

    b = Board(a.url, a.user, a.password)
    b.login()

    if a.action == "show":
        print(json.dumps(b.osd(), ensure_ascii=False, indent=2))
        return 0

    patch = {}
    # POST 键名 → GET 键名（用于回读校验）
    want = {}
    for k in ("name_enable", "time_date", "time_week", "time_enable"):
        v = getattr(a, k)
        if v is not None:
            patch[k] = v
            want[k] = v
    if a.name is not None:
        patch["channel_name"] = a.name
        want["name"] = a.name
    if a.font_px is not None:
        patch["time_font_px"] = patch["name_font_px"] = a.font_px
        want["font_px"] = a.font_px
    for opt, key, gk in (("name_pos", "name", "name"), ("time_pos", "time", "time")):
        s = getattr(a, opt)
        if s:
            x, y = (int(v) for v in s.split(","))
            patch[f"{key}_x"], patch[f"{key}_y"] = x, y
            want[f"{gk}_x"], want[f"{gk}_y"] = x, y
    if not patch:
        print("没有要改的项（至少给一个 -- 参数）", file=sys.stderr)
        return 2

    b.set_osd(patch)
    now = b.osd()
    print(json.dumps(now, ensure_ascii=False, indent=2))

    bad = {k: (v, now.get(k)) for k, v in want.items() if now.get(k) != v}
    if bad:
        print("\n【注意】以下项未生效（POST 键名写错？）：", file=sys.stderr)
        for k, (v, g) in bad.items():
            print(f"   {k}: 期望 {v!r} → 实际 {g!r}", file=sys.stderr)
        return 1
    # 不要用 emoji：Windows 控制台是 GBK，打印 ✅ 会抛 UnicodeEncodeError（写完其实成功了，
    # 却在最后一行报错，看着像失败）
    print("\n已按要求写入（回读一致）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
