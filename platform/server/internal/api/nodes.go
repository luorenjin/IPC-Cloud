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

// handleNodeStreams SYS-02 节点当前流列表。
func handleNodeStreams(c *gin.Context) {
	id := c.Param("id")
	var n models.MediaNode
	if store.DB.First(&n, "id = ?", id).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	var sessions []models.StreamSession
	store.DB.Where("node_id = ? AND ended_at = 0", id).Find(&sessions)
	ok(c, gin.H{"items": sessions})
}

// handleKickStream 踢流。
func handleKickStream(c *gin.Context) {
	sid := c.Param("sid")
	var ss models.StreamSession
	if store.DB.First(&ss, "id = ?", sid).Error != nil {
		fail(c, errs.ENotFound)
		return
	}
	if node, err := devsvc_NodeByID(ss.NodeID); err == nil {
		_ = node
	}
	store.DB.Model(&ss).Update("ended_at", models.NowMilli())
	ok(c, nil)
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
	status := "offline"
	if err == nil {
		status = "online"
	}
	store.DB.Model(&n).Updates(map[string]any{"status": status, "last_keepalive": models.NowMilli()})
	enginePublishNodeStatus(id, status)
}
