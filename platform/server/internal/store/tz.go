package store

import (
	"log"
	"time"

	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/timeutil"
)

// ProjectLocation 取项目时区，用于"今天/零点/按天索引"的时间判定。
//
// 必须按项目时区求值而不是服务器本地时区（time.Local）：服务端容器一般为
// UTC，直接用 time.Local 会让"今日告警"之类的统计整体偏移（UTC+8 下多算 8 小时）。
// 项目不存在或时区非法时回退 timeutil.DefaultTZ，绝不因时区问题丢数据。
func ProjectLocation(projectID string) *time.Location {
	if projectID != "" {
		var p models.Project
		if err := DB.First(&p, "id = ?", projectID).Error; err == nil && p.TZ != "" {
			if loc, err := time.LoadLocation(p.TZ); err == nil {
				return loc
			}
			log.Printf("[tz] 项目 %s 时区 %q 解析失败，回退 %s", projectID, p.TZ, timeutil.DefaultTZ)
		}
	}
	return timeutil.LoadLocation("")
}
