# firmware/tools

## board.py：真机免断电部署与冒烟测试

依赖：`pip install pyserial`。串口默认 `COM6`（或设置环境变量 `IPC_BOARD_PORT` / 传 `--port`）。

**运行前断开占用串口的程序**（MobaXterm、PuTTY、BurnTool）；Tftpd64 等占 UDP 69 的程序也要关掉。首次运行时 Windows 防火墙若弹窗，放行 Python 的专用网络访问。

| 命令 | 用途 | 耗时 |
|---|---|---|
| `python firmware/tools/board.py deploy-app` | 只替换 `/usr/bin/ipc_app` 并重启服务，保留 MAC、配置和激活状态；改了 `ipc_app` 或前端时用 | ~10s |
| `python firmware/tools/board.py reflash-rootfs` | `reboot` → 抢停 U-Boot → TFTP 下载 → 擦写整个 rootfs 分区 → `reset`；改了 rootfs 内容时用 | ~1–2min |
| `python firmware/tools/board.py smoke` | 冒烟测试：进程存在、MAC 固定、`GET /`、`/api/v1/auth/state`、未登录拦截、拉回应用日志 | ~5s |
| `python firmware/tools/board.py shell "<命令>"` | 在板子上执行一条命令 | — |

`deploy-app`、`reflash-rootfs` 完成后都会自动跑 `smoke`，退出码 0 表示通过，可以直接接到 CI 或其他脚本里。

典型流程：

```bash
cd firmware/docker && make -f Makefile fw-build && cd ../..
python firmware/tools/board.py deploy-app

# rootfs 有改动时
cd firmware/docker && make -f Makefile fw-all && cd ../..
python firmware/tools/board.py reflash-rootfs
```

串口全文与应用日志保存在 `firmware/tools/logs/`（已加入 `.gitignore`）。

## console_e2e.py：控制台真机浏览器验收

依赖：`pip install playwright`；使用系统 Chrome（`channel="chrome"`），不需要 `playwright install`。

```bash
python firmware/tools/console_e2e.py                  # 默认 http://172.16.1.185:8080/，口令 Admin@12345
python firmware/tools/console_e2e.py --skip-reboot --skip-reset   # 快速回归，不重启不出厂
```

覆盖 13 个用例：激活、错误口令被拒、只显示可用菜单、设备信息真实值、设备名持久化、与计算机时间同步、系统日志导出、修改密码、会话失效回登录页、连续错误口令锁定、重启、恢复出厂（结束后自动用同一口令重新激活）。退出码 0 = 全过；截图在 `firmware/tools/logs/e2e-<时间>/`。

- 锁定用例会让本机 IP 被设备锁定 60 秒起（重复触发翻倍，最长 15 分钟），脚本会轮询直到解锁。
- 「改为静态 IP」不在自动化里：会把测试机与设备的连接改断。需要时手工在「网络设置 → 连接」验证。

### 说明

- **reflash-rootfs 会擦掉整个 10MB 分区**，但会**保留 MAC 和 IP**：烧写前读出 `/etc/ipc/mac`，新系统启动后写回，释放首次开机随机 MAC 占用的租约，再用 `udhcpc -r` 请求原 IP，所以自动化测试的地址不变。`/etc/ipc/config.json` 仍会被清掉，控制台回到未激活状态。
  - 指定 MAC：`--mac 02:xx:xx:xx:xx:xx`；模拟全新设备（生成新 MAC）：`--new-mac`。
  - 用 BurnTool 手动烧录不经过本脚本，MAC 会重新生成，IP 也会跟着变。
  - 想完全固定地址，可以在路由器上为该 MAC 做 IP 保留。
- 板子 U-Boot 的 `bootdelay=0`，脚本在 `reboot` 后持续发送按键抢停；偶尔没抢到会报「没抢停 autoboot」，这时板子会正常启动，重跑即可。
- U-Boot 阶段复用板子当前的 DHCP 地址作为临时静态 IP，`ethaddr` 无效时临时设置为 `02:00:00:00:00:01`，都不执行 `saveenv`。
- 板子起不来、串口进不了 U-Boot 时，只能断电后用 BurnTool 恢复（见 `docker/flash/烧录指南.md`）。以后想全自动，可以加一个 USB 继电器控制电源。
