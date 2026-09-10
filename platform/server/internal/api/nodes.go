package api

import (
	"context"

	"github.com/gin-gonic/gin"

	"github.com/jetscam/ipccloud/server/internal/crypto"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// ---------- 媒体节点 SYS-01/02 ----------

func handleListNodes(c *gin.Context) {
	var items []models.MediaNode
	store.DB.Find(&items)
	ok(c, gin.H{"items": items})
}

type nodeReq struct {
	Name       string `json:"name" binding:"required"`
	APIURL     string `json:"apiUrl" binding:"required"`
	Secret     string `json:"secret" binding:"required"`
	PublicHost string `json:"publicHost" binding:"required"`
	RTMPPort   int    `json:"rtmpPort"`
	HTTPPort   int    `json:"httpPort"`
	HTTPSPort  int    `json:"httpsPort"`
	RTPRange   string `json:"rtpRange"`
	MaxStreams int    `json:"maxStreams"`
	Weight     int    `json:"weight"`
}

func handleCreateNode(c *gin.Context) {
	var req nodeReq
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	n := models.MediaNode{ID: "mn_" + models.NewID(), Name: req.Name, APIURL: req.APIURL,
		SecretEnc: crypto.Enc(req.Secret), PublicHost: req.PublicHost,
		RTMPPort: intOr(req.RTMPPort, 1936), HTTPPort: intOr(req.HTTPPort, 80), HTTPSPort: intOr(req.HTTPSPort, 443),
		RTPRange: defStr(req.RTPRange, "30000-30100"), MaxStreams: intOr(req.MaxStreams, 200),
		Weight: intOr(req.Weight, 100), Status: "offline", CreatedAt: models.NowMilli()}
	store.DB.Create(&n)
	// 自检
	go selfCheckNode(n.ID)
	ok(c, n)
}

func handleUpdateNode(c *gin.Context) {
	id := c.Param("id")
	var req struct {
		Name       string `json:"name"`
		APIURL     string `json:"apiUrl"`
		Secret     string `json:"secret"`
		PublicHost string `json:"publicHost"`
		MaxStreams *int   `json:"maxStreams"`
		Weight     *int   `json:"weight"`
		Disabled   *bool  `json:"disabled"` // SYS-01 禁用（不参与调度）
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		fail(c, errs.EBadRequest)
		return
	}
	updates := map[string]any{}
	if req.Name != "" {
		updates["name"] = req.Name
	}
	if req.APIURL != "" {
		updates["api_url"] = req.APIURL
	}
	if req.Secret != "" {
		updates["secret_enc"] = crypto.Enc(req.Secret)
	}
	if req.PublicHost != "" {
		updates["public_host"] = req.PublicHost
	}
	if req.MaxStreams != nil {
		updates["max_streams"] = *req.MaxStreams
	}
	if req.Weight != nil {
		updates["weight"] = *req.Weight
	}
	if req.Disabled != nil {
		updates["disabled"] = *req.Disabled
	}
	store.DB.Model(&models.MediaNode{}).Where("id = ?", id).Updates(updates)
	var n models.MediaNode
	store.DB.First(&n, "id = ?", id)
	ok(c, n)
}

func handleDeleteNode(c *gin.Context) {
	id := c.Param("id")
	var n models.MediaNode
	if store.DB.First(&n, "id = ?", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	if n.Streams > 0 {
		fail(c, errs.EBadRequest.WithMsg("节点存在活跃流"))
		return
	}
	store.DB.Delete(&n)
	ok(c, nil)
}

// handleSelfCheckNode 节点自检（getMediaList 可达性）。
func handleSelfCheckNode(c *gin.Context) {
	id := c.Param("id")
	go selfCheckNode(id)
	ok(c, gin.H{"started": true})
}

// handleNodeStreams SYS-02 节点当前流列表：实时拉取 ZLM getMediaList，
// 关联 DB 会话补充来源通道；id 为流名（kick 端点以流名定位）。
func handleNodeStreams(c *gin.Context) {
	id := c.Param("id")
	var n models.MediaNode
	if store.DB.First(&n, "id = ?", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	var sessions []models.StreamSession
	store.DB.Where("node_id = ? AND ended_at = 0", id).Find(&sessions)
	byStream := map[string]models.StreamSession{}
	for _, s := range sessions {
		byStream[s.App+"/"+s.Stream] = s
	}
	items := []gin.H{}
	list, err := mediaForNode(&n).GetMediaList(context.Background())
	if err != nil {
		// 节点不可达：退化为 DB 会话视图
		for _, s := range sessions {
			items = append(items, gin.H{"id": s.Stream, "app": s.App, "stream": s.Stream,
				"channel": s.ChannelID, "viewers": 0, "bitrate": 0, "stale": true})
		}
		ok(c, gin.H{"items": items})
		return
	}
	for _, m := range list {
		app, _ := m["app"].(string)
		stream, _ := m["stream"].(string)
		viewers := int(zf(m["readerCount"]))
		bitrate := int64(zf(m["bytesSpeed"])) * 8 // B/s → bit/s
		ch := ""
		if s, okk := byStream[app+"/"+stream]; okk {
			ch = s.ChannelID
		}
		items = append(items, gin.H{"id": stream, "app": app, "stream": stream,
			"channel": ch, "viewers": viewers, "bitrate": bitrate,
			"originType": m["originTypeStr"], "aliveSecond": m["aliveSecond"]})
	}
	ok(c, gin.H{"items": items})
}

// handleKickStream 踢流：以流名定位（:sid=stream），经 ZLM close_streams 强制断源，
// 并结束对应 DB 会话。
func handleKickStream(c *gin.Context) {
	nid := c.Param("id")
	sid := c.Param("sid")
	var n models.MediaNode
	if store.DB.First(&n, "id = ?", nid).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	var sessions []models.StreamSession
	store.DB.Where("node_id = ? AND stream = ? AND ended_at = 0", nid, sid).Find(&sessions)
	app := ""
	if len(sessions) > 0 {
		app = sessions[0].App
	} else {
		app = "live" // 无会话记录时按常见 app 尝试；close_streams 对不存在的流幂等成功
	}
	if err := mediaForNode(&n).CloseStreams(app, sid); err != nil {
		fail(c, errs.EUnreachable.WithMsg("踢流失败: "+err.Error()))
		return
	}
	now := models.NowMilli()
	for _, s := range sessions {
		store.DB.Model(&s).Update("ended_at", now)
	}
	ok(c, nil)
}

func zf(v any) float64 {
	if f, ok := v.(float64); ok {
		return f
	}
	return 0
}

func intOr(v, def int) int {
	if v == 0 {
		return def
	}
	return v
}

func defStr(v, def string) string {
	if v == "" {
		return def
	}
	return v
}

// selfCheckNode 调用 getMediaList 检查可达性并更新状态。
func selfCheckNode(id string) {
	var n models.MediaNode
	if store.DB.First(&n, "id = ?", id).Error != nil {
		return
	}
	zlm := mediaForNode(&n)
	_, err := zlm.GetMediaList(context.Background())
	status, reason := "online", ""
	if err != nil {
		// ACC-02：保留具体原因（连接被拒 / 超时 / secret 错误），前端据此给出可操作提示。
		status, reason = "offline", err.Error()
	}
	store.DB.Model(&n).Updates(map[string]any{
		"status": status, "status_reason": reason, "last_keepalive": models.NowMilli()})
	enginePublishNodeStatus(id, status)
}
