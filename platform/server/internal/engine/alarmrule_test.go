package engine

import (
	"testing"
	"time"

	"github.com/jetscam/ipccloud/server/internal/models"
)

func TestIsDeviceSideKind(t *testing.T) {
	// ALM-03 六种设备侧类型受通道规则约束
	for _, k := range []string{"motion", "humanoid", "intrusion", "linecross", "tamper", "io"} {
		if !isDeviceSideKind(k) {
			t.Errorf("%q 应为设备侧类型", k)
		}
	}
	// 平台侧与未列举类型一律不受约束（白名单语义）
	for _, k := range []string{
		"device_offline", "node_offline", "stream_lost", "record_fail", "disk_full", "auth_fail",
		"tf_error", "tf_inserted", "rebooted", "bind_state", "stream_limit", "ota_state", "",
	} {
		if isDeviceSideKind(k) {
			t.Errorf("%q 不应为设备侧类型", k)
		}
	}
}

func TestParseHHMM(t *testing.T) {
	cases := []struct {
		in   string
		want int
		ok   bool
	}{
		{"00:00", 0, true},
		{"08:30", 510, true},
		{"24:00", 1440, true},
		{" 09:05 ", 545, true},
		{"9:5", 545, true},
		{"25:00", 0, false},
		{"08:60", 0, false},
		{"0830", 0, false},
		{"", 0, false},
	}
	for _, c := range cases {
		got, ok := parseHHMM(c.in)
		if ok != c.ok || (ok && got != c.want) {
			t.Errorf("parseHHMM(%q) = (%d,%v)，期望 (%d,%v)", c.in, got, ok, c.want, c.ok)
		}
	}
}

// 2026-09-09 是周三（ISO 3）
func wed(h, m int) time.Time { return time.Date(2026, 9, 9, h, m, 0, 0, time.UTC) }

// 2026-09-12 是周六（ISO 6）
func sat(h, m int) time.Time { return time.Date(2026, 9, 12, h, m, 0, 0, time.UTC) }

// 2026-09-13 是周日（ISO 7；Go 的 Weekday() 为 0，是最易写错的转换分支）
func sun(h, m int) time.Time { return time.Date(2026, 9, 13, h, m, 0, 0, time.UTC) }

func TestScheduleActiveAt(t *testing.T) {
	workday := models.JSONB{
		"days":   []any{1.0, 2.0, 3.0, 4.0, 5.0},
		"ranges": []any{[]any{"08:00", "12:00"}, []any{"14:00", "18:00"}},
	}
	allday := models.JSONB{
		"days":   []any{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0},
		"ranges": []any{[]any{"00:00", "24:00"}},
	}

	cases := []struct {
		name string
		sch  models.JSONB
		at   time.Time
		want bool
	}{
		{"空模板视为全天候", models.JSONB{}, sat(3, 0), true},
		{"nil 模板视为全天候", nil, sat(3, 0), true},
		{"全天候-周六凌晨", allday, sat(3, 0), true},
		{"全天候-边界 23:59", allday, sat(23, 59), true},
		{"工作日-周三上午时段内", workday, wed(9, 0), true},
		{"工作日-周三午休时段外", workday, wed(13, 0), false},
		{"工作日-周三下午时段内", workday, wed(14, 0), true},
		{"工作日-起点包含", workday, wed(8, 0), true},
		{"工作日-终点不包含", workday, wed(12, 0), false},
		{"工作日-周六整天不布防", workday, sat(9, 0), false},
		{"days 缺失则不限星期", models.JSONB{"ranges": []any{[]any{"08:00", "12:00"}}}, sat(9, 0), true},
		{"ranges 缺失则全天", models.JSONB{"days": []any{6.0}}, sat(3, 0), true},
		{"ranges 为空数组则全天", models.JSONB{"days": []any{6.0}, "ranges": []any{}}, sat(3, 0), true},
		{"非法时间条目被跳过", models.JSONB{"ranges": []any{[]any{"bad", "12:00"}}}, wed(9, 0), false},
		{"全天候-周日应放行（ISO 7 转换）", allday, sun(10, 0), true},
		{"工作日-周日不布防（ISO 7 转换）", workday, sun(10, 0), false},
	}
	for _, c := range cases {
		if got := scheduleActiveAt(c.sch, c.at); got != c.want {
			t.Errorf("%s: 得到 %v，期望 %v", c.name, got, c.want)
		}
	}
}
