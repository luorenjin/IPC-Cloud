// Package timeutil 时间工具。
package timeutil

import "time"

func NowMilli() int64 { return time.Now().UnixMilli() }
