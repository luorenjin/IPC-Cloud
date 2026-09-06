# CLAUDE.md（simulator/）

本文件为 Claude Code 在 `simulator/` 目录（IPC 终端多协议模拟器）下工作时提供指导。根目录总览见 `../CLAUDE.md`。

## 应答语言

必须使用中文应答用户（代码、标识符、命令行等技术内容保持原样）。

## 用途

Go 模块，一键拉起 **IDP / GB28181 / ONVIF / RTSP** 四种协议的模拟 IPC 终端，配合 `../platform` 完成"接入-绑定-在线-预览-回放-告警"管理闭环的端到端联调，无需真实摄像头硬件。修改 `platform/server/internal/adapter/*` 下任一协议适配器后，应优先用本模拟器对应协议验证行为，而不是仅凭代码走读判断正确性。

模块：`github.com/jetscam/ipccloud/simulator`（Go 1.25，依赖 `paho.mqtt.golang`、`gorilla/websocket`）。

## 目录结构

- `main.go` — CLI 入口，解析 `-mode`/`-broker`/`-platform`/`-sip-*`/`-adv-host`/`-assets` 等参数（均有 `SIM_*` 环境变量兜底默认值，见 `envOr`/`envOrInt`）。
- `idp/` — IDP 私有协议模拟设备：`device.go`（设备状态机/生命周期）、`rtmp.go`（推流）。
- `gb/`（`simulator/gb`） — GB28181/SIP 模拟设备。
- `onvif/` — ONVIF Device Service 模拟（发现、XAddrs 通告）。
- `rtsp/` — RTSP 服务模拟。
- `mediagen/` — 合成媒体数据：`aac.go`（音频）等，配合 `assets/testsrc.h264` 生成可推拉的测试流。
- `assets/` — 测试用媒体资源（如 `testsrc.h264`）。

## 运行

```bash
cd simulator && go run . -mode=all \
  -broker=tcp://127.0.0.1:1883 \
  -platform=http://127.0.0.1:8080 \
  -sip-host=127.0.0.1 -sip-port=5060 \
  -adv-host=192.168.1.10 \
  -assets=./assets
```

常用参数：
- `-mode`：`all|idp|gb|onvif|rtsp`，选择只启动某一种协议还是全部。
- `-broker`：IDP 控制面用的 MQTT broker 地址。
- `-platform`：平台 HTTP 地址（用于快照上传等）。
- `-sip-host`/`-sip-port`：GB28181 SIP 服务器地址。
- `-adv-host`：对外通告地址（ONVIF `StreamUri`/`XAddrs`），不填则自动探测本机对外 IP（容器内为容器 IP）。
- `-idp-id`（17 位）、`-gb-id`（20 位）/`-gb-pwd`：模拟设备标识与鉴权。
- `-onvif-port`/`-rtsp-port`、`-onvif-user`/`-onvif-pass`：对应协议服务端口与鉴权。
- `-alarm-sec`：模拟告警上报周期（秒），`0` 关闭。

在 `platform/docker-compose.yml` 中，`simulator` 服务会自动以 `-mode=all` 跟随完整技术栈启动（依赖 `emqx`、`zlm`、`server`），无需手动运行即可联调。

## 跨领域注意事项

- 代码注释为中文；保持一致。
- 修改某协议的模拟行为时，注意与 `platform/server/internal/adapter/<同名协议>/` 的假设保持同步（如 IDP 设备 ID 位数、GB28181 编号规则、流命名等），两侧一旦不一致会导致联调"看起来正常但生产环境失败"。
