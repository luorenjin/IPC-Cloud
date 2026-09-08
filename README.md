# IpcCloud

IPC（网络摄像头）云端接入与管理平台，围绕同一份 PRD（[`Docs/PRD/`](Docs/PRD/)）演进出三个独立又协作的产品：云平台、设备固件通用层、多协议终端模拟器。

## 仓库结构

| 目录 | 说明 | 详情 |
|---|---|---|
| [`platform/`](platform/) | IpcCloud 云平台：Go + Gin 服务端、Nuxt3 Web 前端，日常开发主要在这里 | [platform/README.md](platform/README.md) · [platform/CLAUDE.md](platform/CLAUDE.md) |
| [`firmware/`](firmware/) | 厂商无关的 IPC 固件通用层（HAL v1 + L2 core），C11 编写，无需真实硬件即可在 x86 上构建测试 | [firmware/README.md](firmware/README.md) · [firmware/CLAUDE.md](firmware/CLAUDE.md) |
| [`simulator/`](simulator/) | 基于 Go 的多协议 IPC 终端模拟器（IDP/GB28181/ONVIF/RTSP），无需真实摄像头即可端到端联调平台 | [simulator/CLAUDE.md](simulator/CLAUDE.md) |

三者的典型协作方式：修改 `platform/server/internal/adapter/*` 的协议行为后，用 `simulator` 对应协议模拟设备联调验证；`firmware` 目前独立于另外两者演进（无真实硬件依赖），只在协议/接入规范层面与 `platform` 共享设计文档。

## 快速开始

日常开发主要围绕 `platform/` 展开，推荐用 Docker Compose 一键起完整技术栈（Postgres、Redis、EMQX、ZLMediaKit、server、web、simulator）：

```bash
cd platform && docker compose up -d --build
```

- Web：http://localhost:3000 （默认账号 admin / Admin@12345）
- 服务端 API：http://localhost:8080/api/v1
- ZLM API：http://localhost:8081，EMQX Dashboard：http://localhost:18083

首次登录后按设置向导添加媒体节点（apiUrl 填 `http://zlm:80`，secret 见 ZLM 容器 `config.ini` 的 `api.secret`，publicHost 填宿主机 IP）。

不使用 Docker 的本地开发、固件构建/测试、模拟器运行方式，详见各子目录的 README / CLAUDE.md。

## 关键设计文档

架构改动前建议先读（位于 [`Docs/PRD/`](Docs/PRD/) 下）：

- [`决策记录与待定事项.md`](Docs/PRD/决策记录与待定事项.md) — 决策记录，说明哪些已冻结、哪些仍待定（芯片/传感器/Flash 选型**尚未决定**）
- [`IpcCloud平台PRD_v1.0.md`](Docs/PRD/IpcCloud平台PRD_v1.0.md) — 平台 PRD（代码注释与提交记录中大量引用的功能编号，如 LIVE-02、MGR-05、ALM-03）
- [`IpcCloud设备接入规范_v1.0.md`](Docs/PRD/IpcCloud设备接入规范_v1.0.md) — 设备接入规范（协议行为、统一设备模型、流命名规则 §8.3）
- [`IPC固件平台化架构_HAL适配方案.md`](Docs/PRD/IPC固件平台化架构_HAL适配方案.md) — 固件 HAL 分层设计依据

> **注意**：`Docs/PRD/README.md` 以及 `Docs/PRD/用户手册/`、`问题指南/` 下的全部文件**不是本项目的规格文档**，而是竞品（TP-LINK）云平台帮助中心的爬取存档，仅作功能对比参考（详见 [`TP-LINK商云分析报告_IPC平台PRD参考.md`](Docs/PRD/TP-LINK商云分析报告_IPC平台PRD参考.md)）。

## 面向 AI 编码助手

进入子目录工作前请先读对应的 `CLAUDE.md`（[根目录](CLAUDE.md) → [platform](platform/CLAUDE.md) / [firmware](firmware/CLAUDE.md) / [simulator](simulator/CLAUDE.md)），其中约定了应答语言、跨领域注意事项（协议来源字面判别值等）以及各自的架构与命令速查。
