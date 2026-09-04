# IpcCloud 平台

IpcCloud 摄像头接入与管理平台的云端实现（PRD v1.0）。

## 架构

```
web (Nuxt3 + Element Plus + h265web.js)
  └─ REST / WS ──▶ server (Go + Gin + GORM)
                     ├─ adapters: idp(MQTT) | gb28181(SIP) | onvif | rtsp
                     ├─ engine: 起播编排 / ZLM Hook / 告警 / 录像计划
                     ├─ media: ZLM 客户端 + 节点调度（粘性 + 最小负载）
                     ├─ PostgreSQL / Redis
                     └─ ZLMediaKit 节点 (WS-FLV / RTP / RTMP)
```

## 快速开始（Docker Compose）

```bash
cd platform
docker compose up -d --build
```

- 前端：http://localhost:3000 （默认账号 admin / Admin@12345）
- 后端 API：http://localhost:8080/api/v1
- ZLM API：http://localhost:8081
- EMQX Dashboard：http://localhost:18083

首次登录后按"首次设置向导"添加媒体节点
（apiUrl 填 http://zlm:80 ，secret 见 ZLM 容器 config.ini 的 api.secret，publicHost 填宿主机 IP）。

## 本地开发

```bash
cd server && go run ./cmd/ipccloud
cd web && npm install && npm run dev
```

## 功能覆盖（对照 PRD 附录 A）

- ACC-01~08：登录锁定/向导/项目/分组/RBAC/成员/操作日志/个人中心
- ADD-01/05/06/07/08/09：IDP 绑定、国标（参数展示/白名单/待确认）、ONVIF 发现、RTSP 手动、预添加
- MGR-01~08/11/12/15：设备列表/筛选/详情/编辑/通道/同步/诊断/重启/删除/转移/错误码
- LIVE-01~07：h265web.js WS-FLV、按需拉流、通道树、1/4 分屏、清晰度、PTZ
- REC-01~07/09：设备端录像检索/回放控制、平台录像计划、存储概览
- ALM-01~07：布防模板、规则、设备/平台事件、策略、消息中心、WS 实时提醒
- SYS-01/02：节点 CRUD/自检/流列表
- DASH-01：仪表盘
- SET-01/02：全局设置、IDP 服务配置

说明：GB28181 MVP 仅 UDP/5060；播放器 h265web.js 需部署到 web/public/vendor/h265web.js
（缺库时播放器显示错误卡片，其余功能不受影响）。
