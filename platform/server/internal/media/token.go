package media

import (
	"context"
	"fmt"
	"time"

	"github.com/jetscam/ipccloud/server/internal/auth"
	"github.com/jetscam/ipccloud/server/internal/store"
)

// PlayToken 播放 token（≤10min，绑定用户与通道，§8.4）。
func PlayToken(userID, channelID, app, stream string, ttlMin int) (string, error) {
	return auth.Sign("play", userID, "", fmt.Sprintf("%s|%s|%s|%s", channelID, app, stream, channelID), time.Duration(ttlMin)*time.Minute)
}

// PushToken 推流 token（≤60s，一次性）。
func PushToken(nodeID, app, stream string) (string, error) {
	return auth.Sign("push", "", "", fmt.Sprintf("%s|%s|%s", nodeID, app, stream), 60*time.Second)
}

// VerifyPlay 校验播放 token。
func VerifyPlay(token, channelID, app, stream string) error {
	c, err := auth.Parse(token)
	if err != nil || c.Kind != "play" {
		return fmt.Errorf("invalid play token")
	}
	want := fmt.Sprintf("%s|%s|%s|%s", channelID, app, stream, channelID)
	if c.Extra != want {
		return fmt.Errorf("play token mismatch")
	}
	return nil
}

// VerifyPush 校验一次性推流 token；成功即标记已用。
func VerifyPush(token, nodeID, app, stream string) error {
	c, err := auth.Parse(token)
	if err != nil || c.Kind != "push" {
		return fmt.Errorf("invalid push token")
	}
	want := fmt.Sprintf("%s|%s|%s", nodeID, app, stream)
	if c.Extra != want {
		return fmt.Errorf("push token mismatch")
	}
	ctx := context.Background()
	key := "used:push:" + c.ID
	if _, err := store.KVImpl.Get(ctx, key); err == nil {
		return fmt.Errorf("push token already used")
	}
	return store.KVImpl.Set(ctx, key, "1", 5*time.Minute)
}
