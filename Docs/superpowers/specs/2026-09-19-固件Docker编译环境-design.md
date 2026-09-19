# 固件 Docker 编译环境（GOKE IPC Linux SDK，四款芯片：GK7202V300/GK7205V200/GK7205V300/GK7605V100）

| 项目 | 内容 |
|---|---|
| 日期 | 2026-09-19 |
| 关联目录 | `firmware/`（本仓库通用层）+ 国科微 GOKE SDK（仓库外部，见 §2） |
| 状态 | 设计中 |

---

## 1. 背景与目标

`firmware/` 目前只有 `platform/mock`（x86 模拟平台），CLAUDE.md 明确要求**芯片选型定稿前不得向 core/modules 添加芯片相关假设**。决策记录中具体芯片型号仍属待定，但硬件团队已将候选范围锁定为 GOKE SDK 同一份代码树下的四款配置——**GK7202V300 / GK7205V200 / GK7205V300 / GK7605V100**——并已备有原厂 SDK、原理图与开发板资料（`D:/JetsCam/Source/国科微/` 下）。

本次目标**不是**给 `firmware/` 接入某一款芯片的平台实现（那属于芯片最终定稿后的工作），而是**先把 GOKE 官方 SDK 本身的编译环境用 Docker 固化下来，并让这四款候选芯片都能各自编译出镜像**：任何人在任何一台 Windows 机器上，不装 Ubuntu、不碰 WSL 里的全局环境，就能可重复地把 SDK 编译出 uboot/kernel/rootfs 镜像，为后续硬件选型对比、调试和固件预研做准备。

### 非目标

- 不实现 `firmware/platform/<soc>/`（具体芯片未终选，CLAUDE.md 禁止；四款候选中最终只会保留一款进入量产）。
- 不把 `firmware/` 通用层接入这套交叉编译环境（留作芯片定稿后的独立任务）。
- 不支持四款候选之外的芯片型号——若后续候选范围变化，按 §3 的 `CHIP` 变量模式扩展 `configs/` 映射即可，不在本次实现范围内预先设计。
- SDK 源码包（.tar.gz/.zip，数百 MB～GB 级）不进 git，仅通过只读挂载喂给容器。

---

## 2. 已确认的环境事实

在 `D:/JetsCam/Source/国科微/GK7205V200_7205V300_7605V100/GKIPCLinuxV100R001C00SPC030/` 下逐层探查后确认：

- **基线 SDK**：`.../Software/GKIPCLinuxV100R001C00SPC030.tar.gz`（718MB，gzip 内容 1.9GB，80757 个文件，其中 **566 个符号链接**——必须在 Linux 文件系统里解压，NTFS bind mount 会打断这些链接）。
- **累积补丁**：`国科SDK/patch/GKIPCLinuxV100R001C00SPC031.zip`（双层嵌套 zip，解开后含 `patch_install.sh`，LF 换行、标准 POSIX shell）。SPC031 的 `README.md` 自述"累积补丁版本，已合入 SPC030CP001、SPC030CP002"，所以**只需打这一个补丁**，不必按 PDF 里旧版本的说明逐个打 CP001→CP002。
- **交叉工具链**：内置于 SDK 包 `tools/toolchains/arm-gcc6.3-linux-uclibceabi/`，宿主侧二进制（`host_bin/arm-linux-uclibceabi-gcc`）是 **x86-64 ELF 动态链接可执行文件**，不需要 32 位兼容库。
- **多芯片支持**：SPC030 的 `configs/` 下有 `gk7202v300/ gk7205v200/ gk7205v300/ gk7605v100/` 四套配置，与硬件团队锁定的候选范围一一对应，本次四套全部纳入验证范围（见 §1 目标）。
- **构建入口**：`build/env.sh`（环境变量）；顶层 `Makefile -> build/root.mk` **确实是符号链接**（`tar tvf` 校验为 `lrwxrwxrwx`），与 PDF 描述一致——此前一次探查误判为"普通文件"，已用 `tar tvf` 复核纠正。这进一步印证 §4.1 的命名卷决策：若直接在 Windows/NTFS 路径解包，这个顶层入口链接本身就会解不出来。
- **本机 Docker**：Docker Desktop（WSL2 后端），容器内实测 20 核 / 7.7GB 内存可用，`ubuntu:18.04` 镜像的 apt 源（archive.ubuntu.com）**仍然可用**，`make gcc bison flex fakeroot gettext` 全部装得上，`glibc 2.27` 精确匹配 SDK 工具链下限要求。
- **磁盘**：D 盘剩余 90GB；源码卷 ~2GB，单款芯片编译产物数 GB，四款全部编译累计预计十余 GB（具体以实测为准），仍在可用空间内——若吃紧可在切换 `CHIP` 前用 `sdk-clean` 清理上一款的中间产物。

---

## 3. 架构

```
firmware/docker/
├── Dockerfile          # ubuntu:18.04 构建机，装 PDF 要求的包 + menuconfig/打包依赖
├── Makefile            # sdk-init / sdk-build / sdk-shell / sdk-export / sdk-clean 等入口
├── entrypoint.sh        # 容器内非 root 用户、UID 对齐
└── out/                 # sdk-export 产物落地目录（Windows 可见，供烧录工具使用），.gitignore
```

- **镜像**：`ubuntu:18.04` + `make gcc bison flex fakeroot gettext`（PDF 列出的最小集）+ 实测还需要的 `bc libncurses5-dev`（`make menuconfig` 依赖）、`tar gzip unzip file rsync python3`（SDK 内部脚本常用）。装完清 apt 缓存，镜像控制在几百 MB。**不把 SDK 打进镜像**——SDK 体积大且会频繁改动（打补丁、改配置），放进镜像层会导致每次重建都要重新处理 GB 级数据。
- **SDK 存放**：Docker **命名卷** `goke-sdk-src`，挂载到容器内固定路径 `/sdk`（`tar xzf` 解开后即 `/sdk/GKIPCLinuxV100R001C00SPC030/`）。SDK 源包本身通过**只读 bind mount** 挂进容器（`D:\JetsCam\Source\国科微\...\Software:/sdk-src:ro`，补丁包同理挂 `/sdk-patch:ro`），`make sdk-init` 时在卷内 `tar xzf` 解包、二层解开 SPC031 补丁 zip（该补丁包是双层嵌套 zip，见 §2）、执行 `./patch_install.sh ../GKIPCLinuxV100R001C00SPC030` 打补丁——全程在 Linux 文件系统里操作，566 个符号链接与所有可执行位原样保留，IO 走 ext4 全速。
- **非 root 构建用户**：容器内建一个与宿主 UID 对齐的普通用户执行编译（`fakeroot` 制作 rootfs 时才提权模拟 root），避免卷内产物全部归 root、且更贴近 SDK 原厂预期的非 root 构建环境。
- **产物导出**：`make build` 的产物（`out/$(CHIP)/image/` 下的 uboot/kernel/rootfs 镜像）留在卷里，`make sdk-export` 显式把这些文件 `cp` 到 bind mount 至 Windows 侧的 `firmware/docker/out/$(CHIP)/`（按芯片分子目录，四款候选各自编译的产物互不覆盖），供后续用 PC 端烧录工具或 boot 命令行使用。这一步把"容器内产物"和"宿主可见文件"的边界明确分开，避免误以为容器里所有文件都能直接在资源管理器里看到。

### 命令面

Makefile 目标（在 `firmware/docker/` 下执行 `make <target>`）：

| 目标 | 作用 |
|---|---|
| `sdk-init` | 首次初始化：卷 `/sdk` 内解压 SDK tar.gz，打 SPC031 累积补丁。已初始化（`/sdk/GKIPCLinuxV100R001C00SPC030/Makefile` 存在）则跳过（幂等，除非 `FORCE=1`） |
| `sdk-menuconfig` | 容器内 `/sdk/GKIPCLinuxV100R001C00SPC030` 下交互式 `make menuconfig`（需要 TTY，`docker run -it`） |
| `sdk-build` | 容器内 `cd /sdk/GKIPCLinuxV100R001C00SPC030 && source build/env.sh && cp configs/$(CHIP)/$(CHIP)_def_cfg.mk cfg.mk && make build -j$(NPROC)` |
| `sdk-shell` | 进容器交互 shell（默认工作目录 `/sdk/GKIPCLinuxV100R001C00SPC030`），手动跑 `make uboot` / `make linux` / `make rootfs` 等部分编译命令 |
| `sdk-export` | 把卷内 `out/$(CHIP)/image/` 拷到 bind mount 的 `/host-out/$(CHIP)/`（对应宿主 `firmware/docker/out/$(CHIP)/`） |
| `sdk-clean` | 容器内 `make <target>_clean`；`sdk-clean-all` 额外删卷（需二次确认） |

`CHIP` 默认 `gk7205v200`（硬件团队当前打样最多的一款），可覆盖为其余三款之一（`gk7202v300`/`gk7205v300`/`gk7605v100`，如 `make sdk-build CHIP=gk7205v300`）——不在 Makefile/Dockerfile 里硬编码单一芯片型号，呼应 CLAUDE.md「芯片选型未定稿」的约束；本次四款均需端到端验证一遍（见 §6 验收标准）。

---

## 4. 关键设计决策

### 4.1 为什么命名卷而非 bind mount 宿主目录

Windows/NTFS 无法表示 Unix 符号链接与可执行权限位；SDK 内有 566 个符号链接（工具链的多个别名指向 `host_bin/` 下同一个二进制），bind mount 到 Windows 路径会导致这些链接在容器里显示为悬空文件，`make build` 大概率在链接工具或运行工具链某个别名时失败。命名卷由 Docker 存储在 Linux 侧（WSL2 后端的 ext4），完全规避该问题，副作用是**卷内容对 Windows 资源管理器不可见**，需要 `sdk-shell` 才能直接查看/编辑源码——这是本次的已知取舍，接受。

### 4.2 为什么不把业务层 firmware/ 一起编译

`firmware/` 通用层的 CMake 门禁明确禁止 core/modules 出现平台宏；`platform/<soc>/` 目录本身还不存在（四款候选均未定稿），写它需要芯片选型最终定稿。本次范围收窄为"把 GOKE 原厂 SDK 编译环境跑通，覆盖四款候选芯片"，为后续硬件选型对比与调试提供确定性环境，firmware/ 与该环境的对接是芯片定稿后的下一个独立任务。

### 4.3 补丁策略

SPC031 是自述的"累积补丁"（合入 CP001+CP002+2021/8-10 兼容性器件补丁+两款 sensor 驱动更新），`sdk-init` 只打这一个补丁，不逐层打 CP001→CP002→CP030 本身没有的更多补丁。若未来国科微发布 SPC031 之后的新补丁，按同一模式（新增只读挂载 + `patch_install.sh`）追加即可，本次不预先设计多补丁链路（YAGNI）。

### 4.4 产物导出是显式步骤，不是自动

`sdk-build` 完成后产物留在卷里，必须显式跑 `sdk-export` 才能在 Windows 侧看到镜像文件。这是有意为之：卷是构建的工作区（含大量中间 .o/.ko 文件，没必要都倒腾到 Windows），`out/` 只放真正要用的最终镜像，边界清晰。

### 4.5 为什么四款芯片都要验证，而不是只打通一款

`Docs/PRD/决策记录与待定事项.md` 里芯片型号仍是"待定"；硬件团队目前把候选范围锁定在 GOKE SDK 自带的这四套配置上，但尚未从中选定最终量产型号。如果编译环境只验证 GK7205V200 一款，一旦后续选型改为其余三款之一，还要重新确认环境是否真的支持——而 SDK 本身已经把四套 `configs/` 都带全了，多花的只是编译时间和磁盘（见 §2 磁盘评估），没有理由不在环境搭建这一步就把四条路径全部跑通，避免选型阶段被"环境支不支持"这种本可提前排除的因素卡住。

---

## 5. 错误处理与边界情况

- **未挂载 SDK 包**：`sdk-init` 若在 `/sdk-src` 找不到 tar.gz，报错退出并提示需要在 `docker run`/`docker compose` 命令中传入正确的宿主路径（SDK 实际路径含中文字符 `D:/JetsCam/Source/国科微/...`，Makefile 传路径给 `docker run -v` 时需原样传递、不做 ASCII 转写或裁剪）。
- **重复 init**：卷内已有 `Makefile` 视为已初始化，`sdk-init` 默认跳过并打印提示；`FORCE=1 make sdk-init` 先清空卷再重新解包+打补丁。
- **编译失败**：`make build` 保留 GOKE 原生的失败退出码与输出，不做额外包装掩盖错误；`firmware/docker/README`（实现阶段补充）记录常见失败（如 `make menuconfig` 需要 `-it` 交互式 TTY，非交互式 `docker compose run` 默认不带）。

---

## 6. 测试与验收

因为这是"搭建编译环境"而非"写业务代码"，验收标准是**端到端把 SDK 编译出镜像**，而非单元测试：

1. `docker build` 成功，镜像内 `gcc --version` 显示 7.x，`ldd --version` 显示 2.27。
2. `make sdk-init` 后卷内可见完整 SDK 目录树（`ls /sdk` 有 `build configs open_source source tools Makefile`），且补丁生效（`patch_install.sh` 退出码 0）。
3. 对**四款芯片逐一**执行 `make sdk-build CHIP=<chip>`（`gk7202v300`/`gk7205v200`/`gk7205v300`/`gk7605v100`），均无 `FATAL`/`Error 1` 类致命错误，各自产出 `out/<chip>/image/` 下的 uboot/kernel/rootfs 镜像文件。
4. 对四款芯片逐一 `make sdk-export CHIP=<chip>` 后，`firmware/docker/out/<chip>/` 在 Windows 侧均可见对应镜像文件，文件大小非零。
5. 任选其一芯片（如 `gk7205v200`）重跑 `make sdk-build`（不删卷）走增量编译，不报错——增量编译只需验证一条路径，不必四款全跑。

---

## 7. 已知限制 / 后续工作

- 四款候选（`gk7202v300`/`gk7205v200`/`gk7205v300`/`gk7605v100`）均做端到端编译验证，但没有实际开发板可对四款逐一烧录验证镜像能否真正跑通；本次止步于"编译产出 image 文件"。
- 若后续硬件团队引入四款之外的新候选（如更换 SDK 基线带来新增芯片配置），需要新增对应的 `configs/<chip>/` 验证，不在本次范围内预先设计。
- `firmware/` 通用层接入 `platform/<soc>/` 是明确的后续工作，需等待 `Docs/PRD/决策记录与待定事项.md` 中芯片选型定稿（届时四款中大概率只保留一款）。
- SDK 版本后续升级（新的 SPCxxx 基线或补丁）需要更新 Makefile 里的版本号变量，本次先硬编码 `GKIPCLinuxV100R001C00SPC030` + `SPC031` 补丁，不做版本参数化抽象（YAGNI，等真的出现第二个版本再抽象）。
