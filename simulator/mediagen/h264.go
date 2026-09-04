// Package mediagen 测试码流生成：Annex-B 解析、H264 帧循环读取器与 RTP/PS/FLV 封装。
package mediagen

import (
	"bytes"
	"fmt"
	"os"
	"sync"
	"time"
)

// NALU 类型。
const (
	NALSlice = 1
	NALIDR   = 5
	NALSEI   = 6
	NALSPS   = 7
	NALPPS   = 8
)

// Frame 一帧视频：NALU 列表（Annex-B 起始码已剥离）与时间戳。
type Frame struct {
	NALUs [][]byte
	Key   bool
	PTS   int64 // 90kHz
	DTS   int64
}

// ParseAnnexB 解析 Annex-B 码流为帧列表（每帧=一个 access unit，1 slice/帧）。
func ParseAnnexB(data []byte) ([]*Frame, [][]byte, error) {
	type sc struct {
		start, end int // 起始码区间 [start,end) 与 NALU 起点
	}
	var marks []sc
	for i := 0; i+3 <= len(data); {
		if data[i] == 0 && data[i+1] == 0 {
			if data[i+2] == 1 {
				marks = append(marks, sc{i, i + 3})
				i += 3
				continue
			}
			if i+4 <= len(data) && data[i+2] == 0 && data[i+3] == 1 {
				marks = append(marks, sc{i, i + 4})
				i += 4
				continue
			}
		}
		i++
	}
	if len(marks) < 2 {
		return nil, nil, fmt.Errorf("no annexb start codes")
	}
	nalus := make([][]byte, len(marks))
	for k, m := range marks {
		end := len(data)
		if k+1 < len(marks) {
			end = marks[k+1].start
		}
		nalu := bytes.TrimRight(data[m.end:end], "\x00")
		if len(nalu) > 0 {
			nalus[k] = nalu
		}
	}

	var frames []*Frame
	var sps, pps []byte
	var cur *Frame
	flush := func() {
		if cur != nil && len(cur.NALUs) > 0 {
			frames = append(frames, cur)
			cur = nil
		}
	}
	for _, n := range nalus {
		switch t := int(n[0] & 0x1f); t {
		case NALSPS:
			sps = n
		case NALPPS:
			pps = n
		case NALSEI:
			if cur != nil {
				cur.NALUs = append(cur.NALUs, n)
			}
		case NALSlice, NALIDR:
			flush()
			cur = &Frame{Key: t == NALIDR, NALUs: [][]byte{n}}
		default:
			// AUD/Filler 等忽略
		}
	}
	flush()
	if sps == nil || pps == nil {
		return nil, nil, fmt.Errorf("sps/pps not found in stream")
	}
	return frames, [][]byte{sps, pps}, nil
}

// Source 循环码流源：从 H264 资产按固定 fps 持续产出帧（多推流共享只读帧数据）。
type Source struct {
	mu      sync.Mutex
	frames  []*Frame
	spspps  [][]byte
	tick    int64 // 90kHz tick 每帧
	seq     int64
	startAt time.Time
}

// NewSource 加载 H264 资产构建循环源。
func NewSource(path string, fps int) (*Source, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	frames, spspps, err := ParseAnnexB(data)
	if err != nil {
		return nil, err
	}
	if fps <= 0 {
		fps = 25
	}
	return &Source{frames: frames, spspps: spspps, tick: 90000 / int64(fps), startAt: time.Now()}, nil
}

// SPS 返回 SPS。
func (s *Source) SPS() []byte { return s.spspps[0] }

// PPS 返回 PPS。
func (s *Source) PPS() []byte { return s.spspps[1] }

// Clone 派生独立时钟的循环源（多路推流各自节奏）。
func (s *Source) Clone() *Source {
	return &Source{frames: s.frames, spspps: s.spspps, tick: s.tick, startAt: time.Now()}
}

// FrameCount 帧总数。
func (s *Source) FrameCount() int { return len(s.frames) }

// Next 取下一帧（循环），时间戳按已取帧数单调推进。
func (s *Source) Next() *Frame {
	s.mu.Lock()
	n := len(s.frames)
	i := s.seq % int64(n)
	base := s.seq / int64(n) * int64(n) * s.tick
	s.seq++
	s.mu.Unlock()
	src := s.frames[i]
	return &Frame{NALUs: src.NALUs, Key: src.Key, PTS: base + i*s.tick, DTS: base + i*s.tick}
}

// WaitNext 按 fps 节奏取帧（阻塞到该帧的应发送时刻）。
func (s *Source) WaitNext() *Frame {
	f := s.Next()
	// DTS 为 90kHz tick：秒数 = DTS / 90000，即 Duration = DTS * 1s / 90000
	due := time.Duration(f.DTS) * time.Second / 90000
	if wait := due - time.Since(s.startAt); wait > 0 {
		time.Sleep(wait)
	}
	return f
}
