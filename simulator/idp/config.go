package idp

import (
	"crypto/sha256"
	"encoding/hex"
	"math"
)

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
	// cfgJSON 对应固件的 CFG_T_JSON（子树以 JSON 文本存取），当前只有移动侦测区域在用
	cfgJSON
	// cfgSecret 只写键（localUser.password）：可下发、可校验，但**永不回显**。
	// cfg.get 对这类键只回空串，设备侧也只存哈希不存明文——
	// 密码不该在网络里来回传，更不该留在页面内存与日志里（接入规范 §11 日志与导出必须脱敏）。
	// 与固件同名规则的 write_only 标记一一对应。
	cfgSecret
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
	// 区域：元组数组 [[x,y,w,h], …]，归一化 0–1（对齐接入规范事件 payload 的先例）
	"alarm.motion.regions": {t: cfgJSON},

	// 时间同步
	"time.ntp.enable": {t: cfgBool},
	"time.ntp.server": {t: cfgStr},
	"time.timezone":   {t: cfgStr},

	// 网络（固件规则表四项均为 reboot_required=true)
	"net.dhcp": {t: cfgBool},
	"net.ip":   {t: cfgStr},
	"net.mask": {t: cfgStr},
	"net.gw":   {t: cfgStr},
	"net.dns":  {t: cfgStr},

	// 本地设置
	"localUser.name": {t: cfgStr},
	// 只写键：min/max 在此是**字符串长度**上下限，与固件 console 的口令策略（8..63）
	// 及 PRD §159「≤8 位含字母数字」一致的下限取 8
	"localUser.password": {t: cfgSecret, min: 8, max: 63},
	"led.enable":         {t: cfgBool},
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
		// 出厂无侦测区域：空数组而不是 null，前端拿到就能直接当地址列表用
		"alarm.motion.regions": []any{},

		"time.ntp.enable": true,
		"time.ntp.server": "pool.ntp.org",
		"time.timezone":   "Asia/Shanghai",

		// net.* 全为占位内网地址，仅用于模拟配置读写，不代表真实网络环境。
		// 注意：cfgDefaults 必须覆盖 cfgRules 的每一个键——平台侧的 supported 是
		// “平台白名单 ∩ 设备回包”，少了默认值的键会直接从配置面板上消失。
		"net.dhcp": true,
		"net.ip":   "192.168.1.64",
		"net.mask": "255.255.255.0",
		"net.gw":   "192.168.1.1",
		"net.dns":  "223.5.5.5",

		"localUser.name": "admin",
		// 只写键的占位值：它只用于“本键受支持”这一个用途（平台 supported = 白名单 ∩ 设备回包），
		// 真正的口令不会落在 d.cfg 里（改密后只存哈希），所以这里永远是空串
		"localUser.password": "",
		"led.enable":         true,
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
		// 只写键：不落进 d.cfg（那里会被 cfgGet 读出、也会被 %v 打进日志），
		// 只存哈希并把 hello.localUserChanged 置 true——改完默认密码就不该再标黄了
		if rule.t == cfgSecret {
			sum := sha256.Sum256([]byte(val.(string)))
			d.pwdHash = hex.EncodeToString(sum[:])
			d.pwdChanged = true
			continue
		}
		d.cfg[k] = val
	}
	return rejected
}

// cfgReset 恢复出厂设置：清空当前配置并重置为出厂默认值，
// 对齐固件 cfg_reset(keep_keys, n) 的语义（firmware/core/src/config.c:594-614）。
// 同时清掉改密痕迹：恢复出厂就是回到出厂默认口令，`localUserChanged` 应重新变回 false，
// 否则平台上的「仍在用默认密码」标黄提示会在恢复出厂后错误地消失。
func (d *Device) cfgReset() {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.cfg = cfgDefaults()
	d.pwdHash = ""
	d.pwdChanged = false
}

// pwdChangedNow 当前是否已修改过默认口令（hello.localUserChanged，接入规范 §5.5.2）。
func (d *Device) pwdChangedNow() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.pwdChanged
}

// redactCmdData 日志脱敏：cfg.set 的 values 里可能带明文口令，不可直接 %v 进日志。
// 不改动原 map（它还要交给 cfgSet 处理），只返回一个供打印的浅拷贝。
func redactCmdData(data map[string]any) map[string]any {
	out := make(map[string]any, len(data))
	for k, v := range data {
		out[k] = v
	}
	values, ok := out["values"].(map[string]any)
	if !ok {
		return out
	}
	masked := make(map[string]any, len(values))
	for k, v := range values {
		if r, ok := cfgRules[k]; ok && r.t == cfgSecret {
			masked[k] = "***"
			continue
		}
		masked[k] = v
	}
	out["values"] = masked
	return out
}

// cfgTouchedSecret 本次 cfg.set 是否成功改写了只写键（口令）。
// 只看未被拒绝的键：写失败却对外声称“已改密”比不声称更糟。
func cfgTouchedSecret(values map[string]any, rejected []string) bool {
	if len(values) == 0 {
		return false
	}
	for k := range values {
		r, ok := cfgRules[k]
		if !ok || r.t != cfgSecret {
			continue
		}
		bad := false
		for _, rk := range rejected {
			if rk == k {
				bad = true
				break
			}
		}
		if !bad {
			return true
		}
	}
	return false
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
	case cfgInt:
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
	case cfgJSON:
		return coerceRegions(v)
	case cfgSecret:
		// 只写键：只接受字符串，长度卡在规则给定的上下限（密码策略的唯一真相是固件/平台，
		// 模拟器只负责“不合法就拒绝”，与其它键“类型不符直接拒绝”的语义一致）
		s, ok := v.(string)
		if !ok || len(s) < rule.min || len(s) > rule.max {
			return nil, false
		}
		return s, true
	default:
		return nil, false
	}
}

// cfgRegionsMax 与 firmware/profiles/*.json 的 ivs.max_regions 对齐（hal_ivs.h 的硬上限是 8，
// 而两个 profile 都只给 4）——超限直接拒绝，不做截断：静默丢掉用户画的框比报错更难排查。
const cfgRegionsMax = 4

// coerceRegions 校验移动侦测区域：元组数组 [[x,y,w,h], …]，归一化 0–1，最多 cfgRegionsMax 个。
// 与固件“类型不符直接拒绝、不做就地转换”的语义一致：任一区域不合法就整个键拒绝，
// 不存一个“部分生效”的区域集合。
func coerceRegions(v any) (any, bool) {
	arr, ok := v.([]any)
	if !ok || len(arr) > cfgRegionsMax {
		return nil, false
	}
	out := make([]any, 0, len(arr))
	for _, item := range arr {
		rect, ok := item.([]any)
		if !ok || len(rect) != 4 {
			return nil, false
		}
		one := make([]any, 0, 4)
		for _, c := range rect {
			f, ok := c.(float64)
			if !ok || f < 0 || f > 1 {
				return nil, false
			}
			// 保留 3 位小数：界面拖拽出来的坐标就是这个精度，多余位数只会让回读值与
			// 界面显示对不上，看上去像“保存把值改了”
			one = append(one, math.Round(f*1000)/1000)
		}
		out = append(out, one)
	}
	return out, true
}
