// 临时诊断工具：导出 2 秒 PS 流到文件供 ffprobe 验证。
package main

import (
	"fmt"
	"os"

	"github.com/jetscam/ipccloud/simulator/mediagen"
)

func main() {
	src, err := mediagen.NewSource("/assets/testsrc.h264", 25)
	if err != nil {
		panic(err)
	}
	src = src.Clone()
	mux := mediagen.NewPSMuxer()
	out, _ := os.Create("/tmp/out.ps")
	defer out.Close()
	for i := 0; i < 50; i++ { // 2 秒 @25fps
		f := src.Next()
		withPSM := f.Key || i == 0
		if withPSM {
			f.NALUs = append([][]byte{src.SPS(), src.PPS()}, f.NALUs...)
		}
		if _, err := out.Write(mux.Pack(f, withPSM)); err != nil {
			panic(err)
		}
	}
	fmt.Println("done, size:", func() int { st, _ := out.Stat(); return int(st.Size()) }())
}
