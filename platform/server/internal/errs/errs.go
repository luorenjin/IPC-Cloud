// Package errs 统一错误码（接入规范 §10）与 HTTP 错误响应。
package errs

import (
	"fmt"
)

// AppError 业务错误：Code 为平台错误码（如 E7002），Msg 为一句原因，Suggest 为一句建议。
type AppError struct {
	Code    string `json:"code"`
	Msg     string `json:"msg"`
	Suggest string `json:"suggest,omitempty"`
	HTTP    int    `json:"-"`
}

func (e *AppError) Error() string { return fmt.Sprintf("%s %s", e.Code, e.Msg) }

var registry = map[string]*AppError{}

func reg(code, msg, suggest string, httpStatus int) *AppError {
	e := &AppError{Code: code, Msg: msg, Suggest: suggest, HTTP: httpStatus}
	registry[code] = e
	return e
}

// 按接入规范 §10 注册错误码；提示文案与规范一致。
var (
	EBadRequest      = reg("E0400", "参数错误", "", 400)
	EForbid          = reg("E0403", "无权限或设备不支持该操作", "", 403)
	EUnreachable     = reg("E1001", "设备/地址不可达", "检查网线、IP、防火墙", 502)
	EDeviceNotOnline = reg("E1002", "设备未上线", "确认设备已通电联网", 409)
	EAuthFailed      = reg("E2001", "用户名/密码错误", "萤石设备请用 6 位验证码作为密码", 401)
	EOnvifDisabled   = reg("E2002", "ONVIF 未开启或不兼容", "在设备 Web 开启 ONVIF，或改用 RTSP", 409)
	EProtocolError   = reg("E3001", "RTSP/ONVIF/SIP 协议错误", "核对地址与协议", 502)
	ECodecUnsupport  = reg("E3002", "编码不支持", "将设备编码改为 H.264/H.265", 409)
	ENodeOffline     = reg("E4001", "无可用媒体节点", "联系管理员检查流媒体服务", 503)
	EStreamTimeout   = reg("E4002", "起流超时", "检查设备带宽，尝试 TCP", 504)
	EStreamLimit     = reg("E4003", "超出并发/带宽上限", "稍后重试", 429)
	EDiskFull        = reg("E5001", "平台录像磁盘不足", "清理或扩容", 507)
	EGBAuthFailed    = reg("E6001", "国标注册密码错误", "核对平台密码", 401)
	EGBCatalogEmpty  = reg("E6002", "目录为空", "检查设备通道配置", 409)
	EGBRecordUnsup   = reg("E6003", "不支持录像检索/回放", "该型号不支持设备端回放", 409)
	EAlreadyBound    = reg("E7001", "设备已绑定其他项目", "请原项目转移/删除，或提交解绑申请", 409)
	EVerifyCode      = reg("E7002", "验证码错误", "核对标贴验证码", 401)
	EDeviceTimeout   = reg("E7003", "设备未应答", "稍后重试", 504)
	EPreadExpired    = reg("E7004", "预添加过期", "重新激活", 409)
	ENotBound        = reg("E7005", "设备未绑定", "先完成绑定", 409)
	ERateLimited     = reg("E7006", "绑定尝试过多", "1 小时后重试", 429)
	EOtaIncompatible = reg("E8001", "固件与设备不匹配", "选择正确固件", 409)
	EOtaDownloadFail = reg("E8002", "下载/校验失败", "检查网络后重试", 502)
	EOtaRollback     = reg("E8003", "新固件启动失败已回滚", "联系支持", 500)

	ENotFound       = reg("E0404", "资源不存在", "", 404)
	EUnauthorized   = reg("E4010", "未登录或会话过期", "请重新登录", 401)
	EAccountLocked  = reg("E4011", "账号已锁定", "稍后再试", 423)
	EServerInternal = reg("E5000", "服务器内部错误", "请联系管理员", 500)
)

// Get 返回已注册错误码副本（用于运行时构造）。
func Get(code string) *AppError {
	if e, ok := registry[code]; ok {
		cp := *e
		return &cp
	}
	return &AppError{Code: code, Msg: "未知错误", HTTP: 500}
}

// WithMsg 覆盖原因文案。
func (e *AppError) WithMsg(m string) *AppError {
	cp := *e
	cp.Msg = m
	return &cp
}
