# GOKE IPC Linux SDK Docker 编译环境

用 Docker 固化国科微（GOKE）IPC Linux SDK（基线 `GKIPCLinuxV100R001C00SPC030` + 累积补丁 `SPC031`）的官方编译环境，覆盖四款候选芯片配置：`gk7202v300`/`gk7205v200`/`gk7205v300`/`gk7605v100`。任何一台装了 Docker Desktop 的 Windows 机器都能不装 Ubuntu/WSL 全局环境、可重复地编译出 uboot/kernel/rootfs 镜像。四款芯片均已完成端到端编译验证（`make build` 全流程，产物齐全）。

设计依据：[`Docs/superpowers/specs/2026-09-19-固件Docker编译环境-design.md`](../../Docs/superpowers/specs/2026-09-19-固件Docker编译环境-design.md)。

## 前置条件

- Docker Desktop（WSL2 后端）。
- 原厂 SDK 压缩包 `GKIPCLinuxV100R001C00SPC030.tar.gz` 与累积补丁 `GKIPCLinuxV100R001C00SPC031.zip`（仓库外部资产，不进 git，需自行获取存放路径）。
- 若在 Windows 上用 GnuWin32 `make` 驱动本目录的 `Makefile`：本机环境下已确认三类与 GnuWin32 make（3.81）版本/仓库路径相关的怪癖——Makefile 文件内容不能含中文字符、裸 `make <target>` 在含特殊字符的仓库路径下可能失效、`$(CURDIR)`/`$(shell pwd)` 可能损坏路径里的特殊字符——详见下方「常见故障排查」。涉及宿主路径的变量一律通过命令行显式传入，不要写进 Makefile 默认值。也可以不装 `make`，直接照抄各目标对应的 `docker build`/`docker run` 命令手动执行。

## 快速开始

> 提示：在本机（以及其他仓库路径含不可见特殊字符或非纯 ASCII 字符的 Windows + GnuWin32 make 环境）上，不加 `-f` 参数的裸 `make <target>` 可能会因为 make 隐式查找默认 Makefile 文件名这一步的路径解析限制而报乱码化的"没有规则可以创建目标"错误，而不是真正执行目标（与 Makefile 内容本身无关，详见「常见故障排查」）。下面的命令统一加上了 `-f Makefile`，可以直接照抄；如果你的机器没有这个问题，去掉 `-f Makefile` 效果相同。

```bash
cd firmware/docker

# 1. 构建镜像
MSYS_NO_PATHCONV=1 docker build -t goke-sdk-build .

# 2. 初始化 SDK（把下面两个路径换成你机器上的实际位置）
make -f Makefile sdk-init \
  SDK_SRC_DIR="D:/path/to/GKIPCLinuxV100R001C00SPC030/Software" \
  SDK_PATCH_DIR="D:/path/to/patch"

# 3. 编译指定芯片（CHIP 默认 gk7205v200，可换成 gk7202v300/gk7205v300/gk7605v100）
make -f Makefile sdk-build CHIP=gk7205v200

# 4. 导出镜像到宿主可见目录 firmware/docker/out/<chip>/
make -f Makefile sdk-export CHIP=gk7205v200

# 5. 清理某芯片的编译中间产物
make -f Makefile sdk-clean CHIP=gk7205v200
```

## Makefile 目标一览

| 目标 | 作用 |
|---|---|
| `image` | 构建/更新 Docker 镜像（其余目标均自动依赖它，`sdk-clean-all` 除外） |
| `sdk-init` | 首次初始化：卷内解压 SDK、打 SPC031 补丁。已初始化则跳过，`FORCE=1` 强制重新初始化 |
| `sdk-build` | 按 `CHIP` 跑 `make build`（uboot+kernel+rootfs 等全部阶段） |
| `sdk-menuconfig` | 交互式 `make menuconfig`（需要 TTY，已用 `docker run -it`） |
| `sdk-shell` | 进容器交互 shell，默认工作目录为 SDK 根目录，可手动跑 `make uboot`/`make linux` 等部分编译命令 |
| `sdk-export` | 把卷内 `out/<chip>/image/` 拷到宿主 `firmware/docker/out/<chip>/` |
| `sdk-clean` | 按 `CHIP` 跑 `make <target>`（`TARGET` 默认 `clean`） |
| `sdk-clean-all` | 打印确认提示，不自动执行；需要手动 `docker volume rm goke-sdk-src` 才会真正删除卷（含全部 SDK 源码与编译产物） |

可覆盖的变量：`CHIP`（默认 `gk7205v200`）、`TARGET`（默认 `clean`）、`BUILD_UID`/`BUILD_GID`（默认均 `1000`）、`FORCE`（默认 `0`）、`SDK_SRC_DIR`/`SDK_PATCH_DIR`（无默认值，`sdk-init` 时必须显式传入）、`OUT_DIR`（`sdk-export` 专用，无显式默认值；不传入时效果上等价于执行 `make` 时所在目录下的 `out/`，即通常的 `firmware/docker/out/`——这个默认值从当前版本起改为在 `sdk-export` 的 recipe 内用 shell 的 `$(pwd)` 实时求值，而不是 Makefile 顶层的 `$(CURDIR)`，原因见下方「常见故障排查」）。

## 已知限制

- 四款候选芯片均完成端到端编译验证，但没有实际开发板可供逐一烧录验证镜像能否真正跑通；本环境止步于"编译产出 image 文件"。
- 若后续候选芯片范围变化（如更换 SDK 基线带来新增配置），需要新增对应的 `configs/<chip>/` 支持，本环境不预先设计。
- `firmware/` 通用层（HAL/core/modules）与本编译环境的对接是独立的后续工作，需等待芯片选型定稿（见根 `CLAUDE.md` 与 `Docs/PRD/决策记录与待定事项.md`）。
- SDK 版本升级（新的 SPCxxx 基线或补丁）需要同步更新 `scripts/*.sh` 里硬编码的 `SDK_NAME`/`PATCH_NAME` 变量。

## 常见故障排查

- **`docker run`/`docker build` 报 `CreateFile ...: The filename ... is incorrect` 或路径出现乱码**：多半是从 Git Bash 执行 `docker` 命令时漏加 `MSYS_NO_PATHCONV=1` 前缀，或者把含中文的路径硬编码进了 `Makefile` 文件本身（GnuWin32 `make` 无法正确解析 Makefile 文件里的中文字符）——把中文路径改成命令行 `VAR=值` 显式传入即可。
- **裸 `make sdk-xxx` 报乱码化的"没有规则可以创建目标 ... 停止"错误（退出码 2），而不是真正执行目标**：这是 GnuWin32 `make`（3.81）**隐式查找默认 Makefile 文件名**这一步本身的限制——在仓库路径含不可见特殊字符（本仓库目录名 `IpcCloud` 里嵌了一个 ZWNJ 字符）时会复现，与 Makefile 文件内容或写法无关。用 `make -f Makefile <target>` 显式指定 Makefile 文件名即可绕开，本文档「快速开始」的示例命令已统一加上，可以直接照抄。这是路径相关的问题，不是所有 Windows 机器都会触发。
- **`sdk-init`/`sdk-build` 报 `Permission denied`**：命名卷默认 root 属主，`entrypoint.sh` 已包含 `chown "$BUILD_UID:$BUILD_GID" /sdk` 修复此问题；若你修改过 `entrypoint.sh` 且重新出现该报错，检查该行是否还在，以及是否在 `gosu` 降权**之前**执行。
- **`sdk-export` 报 `docker: Error response from daemon: CreateFile ...: The filename, directory name, or volume label syntax is incorrect`，且报错路径里仓库目录名后面变成了字面的 `?`（例如 `...IpcCloud?\firmware\docker\out`）**：这是 GnuWin32 `make` 自身对 `$(CURDIR)`/`$(shell pwd)` 求值时，损坏了仓库路径里那个不可见 ZWNJ 特殊字符所致（与 Docker、bash、Makefile 写法本身无关）。当前 `sdk-export` 目标已经修复：`OUT_DIR` 不显式传入时，改为在 recipe 自己的 shell 里用 `` $$(pwd) `` 实时求值，而不是让 make 在变量替换阶段（`$(CURDIR)` 或顶层 `$(shell pwd)`）计算，因此照抄「快速开始」的命令不会触发这个问题。如果你后续给这个 Makefile 新增目标、且该目标需要用宿主路径算默认值，注意避免在 make 变量替换阶段用 `$(CURDIR)`/`$(shell pwd)`，参照 `sdk-export` 目标的写法，把路径计算放到 recipe 自己的 shell 里。
- **`make menuconfig` 卡住或报没有终端**：该目标需要交互式 TTY，必须用 `make sdk-menuconfig`（内部已用 `docker run -it`），不要用 `sdk-shell` 里再手动拼 `docker run` 且漏加 `-it`。
- **`sdk-build` 报 `CHIP=... 不是合法配置`**：`CHIP` 必须是 `gk7202v300`/`gk7205v200`/`gk7205v300`/`gk7605v100` 之一，对应 SDK 内 `configs/<chip>/<chip>_def_cfg.mk` 必须存在。
