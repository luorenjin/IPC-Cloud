package timeutil

import (
	"testing"
	"time"
)

// 北京时间 2026-09-11 00:45。服务端容器 time.Local 通常是 UTC，这一时刻在 UTC 下
// 仍属 09-10；若用 time.Local/Truncate(24h) 取零点会落到"昨天 08:00"，
// 「今日告警」等按天统计随之整体偏移 8 小时。本用例锁死按项目时区取值的行为。
func TestDayStartUsesGivenLocation(t *testing.T) {
	loc := LoadLocation("Asia/Shanghai")
	if loc.String() != "Asia/Shanghai" {
		t.Fatalf("LoadLocation 未返回 Asia/Shanghai：%v", loc)
	}
	at := time.Date(2026, 9, 11, 0, 45, 37, 0, loc)

	want := time.Date(2026, 9, 11, 0, 0, 0, 0, loc).UnixMilli()
	if got := DayStartMillis(at, loc); got != want {
		t.Fatalf("DayStartMillis = %d，期望 %d", got, want)
	}
	// 固化回归点：旧的 Truncate(24h) 写法必须与本地零点不同，否则本用例失去意义
	if old := at.Truncate(24 * time.Hour).UnixMilli(); old == want {
		t.Fatalf("Truncate(24h) 居然等于本地零点（%d），用例前提已失效", old)
	}
	if got := DayKey(at, loc); got != "2026-09-11" {
		t.Fatalf("DayKey(项目时区) = %s，期望 2026-09-11", got)
	}
	// 同一时刻在 UTC 下属于前一天，说明"日期归属"确实随时区变化
	if got := DayKey(at, time.UTC); got != "2026-09-10" {
		t.Fatalf("DayKey(UTC) = %s，期望 2026-09-10", got)
	}
}

// NextDayStart 用于录像"按天展开"，必须用 AddDate 推进以正确跨天。
func TestNextDayStart(t *testing.T) {
	loc := LoadLocation("Asia/Shanghai")
	at := time.Date(2026, 9, 11, 23, 59, 0, 0, loc)
	want := time.Date(2026, 9, 12, 0, 0, 0, 0, loc)
	if got := NextDayStart(at, loc); !got.Equal(want) {
		t.Fatalf("NextDayStart = %v，期望 %v", got, want)
	}
}

// 空值/非法时区必须回退，绝不返回 nil 或让调用方崩在时区上。
func TestLoadLocationFallback(t *testing.T) {
	for _, tz := range []string{"", "Not/AZone", "Asia/Shanghai"} {
		got := LoadLocation(tz)
		if got == nil {
			t.Fatalf("LoadLocation(%q) 返回 nil", tz)
		}
	}
	if got := LoadLocation("Not/AZone").String(); got != DefaultTZ {
		t.Fatalf("非法时区应回退 %s，得到 %s", DefaultTZ, got)
	}
	if got := LoadLocation("").String(); got != DefaultTZ {
		t.Fatalf("空时区应回退 %s，得到 %s", DefaultTZ, got)
	}
}
