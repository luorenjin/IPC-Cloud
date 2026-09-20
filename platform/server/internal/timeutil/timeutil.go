// Package timeutil 时间工具。
package timeutil

import "time"

func NowMilli() int64 { return time.Now().UnixMilli() }

// DefaultTZ 项目时区缺失或非法时的回退时区，与 models.Project.TZ 的默认值保持一致。
const DefaultTZ = "Asia/Shanghai"

// LoadLocation 解析 IANA 时区名；空值或解析失败回退 DefaultTZ，再失败回退 UTC。
//
// 容器内通常没有 /etc/localtime 指向业务时区，time.Local 往往是 UTC，
// 因此业务上"今天/零点"一律走本函数取项目时区，不要用 time.Local。
func LoadLocation(tz string) *time.Location {
	if tz != "" {
		if loc, err := time.LoadLocation(tz); err == nil {
			return loc
		}
	}
	if loc, err := time.LoadLocation(DefaultTZ); err == nil {
		return loc
	}
	return time.UTC
}

// DayStart 返回 t 在 loc 时区下当日零点的时刻。
//
// 不能用 t.Truncate(24*time.Hour)：Truncate 以 UTC 纪元为基准对齐，
// 在非 UTC 时区会把"今天"整体偏移若干小时（UTC+8 下会退到前一天 08:00）。
func DayStart(t time.Time, loc *time.Location) time.Time {
	l := t.In(loc)
	return time.Date(l.Year(), l.Month(), l.Day(), 0, 0, 0, 0, loc)
}

// DayStartMillis 返回 t 在 loc 时区下当日零点的毫秒时间戳。
func DayStartMillis(t time.Time, loc *time.Location) int64 {
	return DayStart(t, loc).UnixMilli()
}

// DayKey 返回 t 在 loc 时区下的 YYYY-MM-DD 日期键（录像按天索引等场景使用）。
func DayKey(t time.Time, loc *time.Location) string {
	return t.In(loc).Format("2006-01-02")
}

// NextDayStart 返回 loc 时区下 t 之后一天的零点，用 AddDate 推进以正确跨 DST/月/年。
func NextDayStart(t time.Time, loc *time.Location) time.Time {
	return DayStart(t, loc).AddDate(0, 0, 1)
}
