package idp

// 模拟设备的本地配置。
//
// ⚠️ 这是与平台 side 的**双向契约**：键名、类型、取值范围三项必须与固件
// `firmware/core/src/config.c` 的规则表逐字对齐（AGENTS.md §5「双向契约一致性」）。
// 键名写错时设备侧不会报错，只会把该键丢进 cfg.set 的 rejected 列表——
// 平台侧看起来就是「保存成功但没生效」的假成功，最难查。
//
// 编码键在固件里是按 pattern 展开的：video.<ch>.<name>.<field>，
// 0/main 为主码流。这里只声明平台会读写的子集。

type cfgType int

const (
	cfgInt cfgType = iota
	cfgBool
	cfgStr
)

type cfgRule struct {
	t        cfgType
	min, max int
	enum     []string
}

// cfgRules 与固件 register_common_rules() + video 规则一致。
var cfgRules = map[string]cfgRule{
	// 画面（0–100 归一化；flip/mirror 是 0/1 开关）
	"image.brightness": {t: cfgInt, min: 0, max: 100},
	"image.contrast":   {t: cfgInt, min: 0, max: 100},
	"image.saturation": {t: cfgInt, min: 0, max: 100},
	"image.sharpness":  {t: cfgInt, min: 0, max: 100},
	"image.flip":       {t: cfgInt, min: 0, max: 1},
	"image.mirror":     {t: cfgInt, min: 0, max: 1},

	// 编码（主码流；w/h 在固件里 reboot_required）
	"video.0.main.codec": {t: cfgStr, enum: []string{"h265", "h264", "mjpeg"}},
	"video.0.main.w":     {t: cfgInt, min: 64, max: 1920},
	"video.0.main.h":     {t: cfgInt, min: 64, max: 1080},
	"video.0.main.fps":   {t: cfgInt, min: 1, max: 30},
	"video.0.main.kbps":  {t: cfgInt, min: 32, max: 16384},
	"video.0.main.gop":   {t: cfgInt, min: 1, max: 300},
	"video.0.main.rc":    {t: cfgStr, enum: []string{"cbr", "vbr"}},

	// OSD
	"osd.channelName.enable": {t: cfgBool},
	"osd.time.enable":        {t: cfgBool},

	// 录像
	"record.enabled":        {t: cfgBool},
	"record.mode":           {t: cfgStr, enum: []string{"continuous", "event", "schedule"}},
	"record.retention_days": {t: cfgInt, min: 1, max: 365},
	"record.channel":        {t: cfgInt, min: 0, max: 2},

	// 移动侦测
	"alarm.motion.enable":      {t: cfgBool},
	"alarm.motion.sensitivity": {t: cfgInt, min: 0, max: 100},

	// 时间同步
	"time.ntp.enable": {t: cfgBool},
	"time.ntp.server": {t: cfgStr},
	"time.timezone":   {t: cfgStr},

	// 网络（固件规则表 reboot_required=true；平台侧当前只做只读展示，见 platform/web
	// [id].vue 的 time 页签，不开放可编辑交互——避免误改导致设备/云端断连）
	"net.dhcp": {t: cfgBool},
	"net.ip":   {t: cfgStr},

	// 本地设置
	"localUser.name": {t: cfgStr},
	"led.enable":     {t: cfgBool},
}

// cfgDefaults 出厂默认值。
// 编码项取自固件自带 profile `firmware/profiles/mock-x86.json` 的 channel 0 default；
// 画面项取 0–100 的中点 50（中位观感，厂商面板的出厂值同为 50/50/50）。
func cfgDefaults() map[string]any {
	return map[string]any{
		"image.brightness": 50,
		"image.contrast":   50,
		"image.saturation": 50,
		"image.sharpness":  50,
		"image.flip":       0,
		"image.mirror":     0,

		"video.0.main.codec": "h265",
		"video.0.main.w":     1920,
		"video.0.main.h":     1080,
		"video.0.main.fps":   25,
		"video.0.main.kbps":  2048,
		"video.0.main.gop":   50,
		"video.0.main.rc":    "vbr",

		"osd.channelName.enable": true,
		"osd.time.enable":        true,

		"record.enabled":        true,
		"record.mode":           "continuous",
		"record.retention_days": 30,
		"record.channel":        0,

		"alarm.motion.enable":      true,
		"alarm.motion.sensitivity": 50,

		"time.ntp.enable": true,
		"time.ntp.server": "pool.ntp.org",
		"time.timezone":   "Asia/Shanghai",

		// net.ip 为任意占位内网地址，仅用于模拟只读展示，不代表真实网络配置
		"net.dhcp": true,
		"net.ip":   "192.168.1.64",

		"localUser.name": "admin",
		"led.enable":     true,
	}
}

// cfgGet 按请求的 keys 返回当前值；只返回设备确实拥有的键（与固件按 pattern 匹配一致）。
func (d *Device) cfgGet(keys []string) map[string]any {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.cfg == nil {
		d.cfg = cfgDefaults()
	}
	out := map[string]any{}
	for _, k := range keys {
		if _, ok := cfgRules[k]; !ok {
			continue
		}
		if v, ok := d.cfg[k]; ok {
			out[k] = v
		}
	}
	return out
}

// cfgSet 写入合法键，返回被拒绝的键列表（对齐固件 cfg_apply_json 的 rejected[] 语义：
// 未知键、类型不符、超出范围、枚举越界一律拒绝，其余照常写入——不是全有或全无）。
func (d *Device) cfgSet(values map[string]any) []string {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.cfg == nil {
		d.cfg = cfgDefaults()
	}
	rejected := []string{}
	for k, v := range values {
		rule, ok := cfgRules[k]
		if !ok {
			rejected = append(rejected, k)
			continue
		}
		val, ok := coerceCfg(rule, v)
		if !ok {
			rejected = append(rejected, k)
			continue
		}
		d.cfg[k] = val
	}
	return rejected
}

// coerceCfg 按规则做类型/范围校验并归一化取值。
// 固件对类型不符是直接拒绝（cfg_set_bool 打 int 键返回 HAL_EINVAL），这里同样不接受"就地转换"。
func coerceCfg(rule cfgRule, v any) (any, bool) {
	switch rule.t {
	case cfgBool:
		b, ok := v.(bool)
		if !ok {
			return nil, false
		}
		return b, true
	case cfgStr:
		s, ok := v.(string)
		if !ok {
			return nil, false
		}
		if len(rule.enum) > 0 {
			for _, e := range rule.enum {
				if e == s {
					return s, true
				}
			}
			return nil, false
		}
		return s, true
	default: // cfgInt
		f, ok := v.(float64)
		if !ok {
			return nil, false
		}
		n := int(f)
		if float64(n) != f { // 1.5 这类非整数不算 int
			return nil, false
		}
		if n < rule.min || n > rule.max {
			return nil, false
		}
		return n, true
	}
}
