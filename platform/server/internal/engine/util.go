package engine

import (
	"os"
	"path/filepath"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/timeutil"
)

func storeWriteFile(path string, data []byte) error {
	_ = os.MkdirAll(filepath.Dir(path), 0o755)
	return os.WriteFile(path, data, 0o644)
}

func baseName(p string) string { return filepath.Base(p) }

func adapterGet(source string) adapter.Adapter { return adapter.Get(source) }

func mapStopOptions(channelID, stream string) adapter.StopOptions {
	return adapter.StopOptions{ChannelID: channelID, App: "live", Stream: stream, Reason: "none_reader"}
}

var _ = errs.ENotFound
var _ = timeutil.NowMilli
