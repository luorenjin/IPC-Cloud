package mediagen

// AAC 静音轨（AAC-LC，44.1kHz，立体声，1024 samples/帧 ≈ 23.2ms）。
// 供 IDP RTMP 推流封装 AAC audio tag：模拟器无音频时纯视频流经转协议
// 会导致播放器死等音频初始化（详见 idp/device.go）。
// 采用最通用的采样率/声道组合以最大化浏览器 MSE 兼容性。

// AACSilenceASC AudioSpecificConfig（AOT=2 LC, srIdx=4(44.1kHz), ch=2）。
var AACSilenceASC = []byte{0x12, 0x10}

// AACSilenceFrame AAC raw 静音帧（可循环发送）。
var AACSilenceFrame = []byte{0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c}

// AACSilenceFrameDurMs 单帧时长（毫秒，取整）。
const AACSilenceFrameDurMs = 23
