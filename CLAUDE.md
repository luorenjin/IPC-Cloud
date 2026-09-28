# CLAUDE.md

本文件为 Claude Code 在本仓库工作时的入口指针。

**根级规范正文在 [`AGENTS.md`](AGENTS.md)**（语言、PRD 溯源、架构边界、命令、协议/前端/固件/模拟器硬约束均以该文件为准）。请先完整阅读 `AGENTS.md`。

进入子目录前再读对应详细文档：

- [`platform/CLAUDE.md`](platform/CLAUDE.md) — 云平台（服务端 + Web），日常开发主战场
- [`firmware/CLAUDE.md`](firmware/CLAUDE.md) — 固件通用层（HAL + core + console）
- [`firmware/web/AGENTS.md`](firmware/web/AGENTS.md) — 本机控制台前端与 `gen_assets.py` 嵌入流程
- [`simulator/CLAUDE.md`](simulator/CLAUDE.md) — 多协议终端模拟器
