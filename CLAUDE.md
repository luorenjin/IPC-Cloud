# CLAUDE.md

本文件为 Claude Code（claude.ai/code）在本仓库中工作时提供指导。

## 应答语言

**必须使用中文应答用户**（代码、标识符、命令行等技术内容保持原样即可）。此要求对所有子目录同样生效。

## 仓库结构

本仓库包含三个共享同一份 PRD（位于 `Docs/PRD/`）的独立产品，每个都有自己的 CLAUDE.md，进入对应目录工作前请先读它：

- **[`platform/CLAUDE.md`](platform/CLAUDE.md)** — IpcCloud 云平台（服务端 + Web）。日常开发主要在这里。
- **[`firmware/CLAUDE.md`](firmware/CLAUDE.md)** — 厂商无关的 IPC 固件通用层（HAL + core 服务），C11 编写，无需真实硬件即可在 x86 上构建。
- **[`simulator/CLAUDE.md`](simulator/CLAUDE.md)** — 基于 Go 的多协议 IPC 终端模拟器，用于在没有真实摄像头的情况下端到端联调平台。

三者的典型协作方式：修改 `platform/server/internal/adapter/*` 的协议行为后，用 `simulator` 对应协议模拟设备联调验证；`firmware` 目前独立于另外两者演进（无真实硬件依赖），只在协议/接入规范层面与 `platform` 共享设计文档。

## 关键设计文档

做架构改动前请先阅读（位于 `Docs/PRD/` 下）：
- `决策记录与待定事项.md` — 决策记录；说明哪些已冻结、哪些仍待定（芯片/传感器/Flash 选型明确**尚未决定**——不要硬编码相关假设）。
- `IpcCloud平台PRD_v1.0.md` — 平台 PRD（代码注释与提交记录中大量引用的功能编号，如 LIVE-02、MGR-05、ALM-03）。
- `IpcCloud设备接入规范_v1.0.md` — 设备接入规范（协议行为、统一设备模型、流命名规则 §8.3）。
- `IPC固件平台化架构_HAL适配方案.md` — 固件 HAL 分层设计依据。

**注意**：`Docs/PRD/README.md` 以及 `Docs/PRD/用户手册/`、`问题指南/` 下的全部文件 **不是本项目的规格文档**；它是竞品（TP-LINK）云平台帮助中心的爬取存档，仅作功能对比参考（详见 `Docs/PRD/TP-LINK商云分析报告_IPC平台PRD参考.md`）。查阅"PRD"时不要误用这批文件。

## 跨领域注意事项

- 代码注释、PRD 引用与错误信息均为中文；新增内容保持这一惯例。
- 协议/设备来源字符串（`idp`、`gb28181`、`onvif`、`rtsp`）作为字面判别值贯穿 `platform` 与 `simulator`——追踪某个协议的端到端行为时，应搜索该来源字符串本身，而不仅仅是对应的类型定义。
