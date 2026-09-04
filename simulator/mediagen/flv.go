package mediagen

import (
	"encoding/binary"
)

// AVCSeqHeader 生成 AVCDecoderConfigurationRecord（FLV VideoTag 内容）。
func AVCSeqHeader(sps, pps []byte) []byte {
	out := []byte{
		0x17, 0x00, // frame type 1 (key) + codec 7 (AVC)；AVCPacketType=0（sequence header）
		0x00, 0x00, 0x00, // composition time
		0x01,                   // configurationVersion
		sps[1], sps[2], sps[3], // profile/compat/level
		0xFF, // 6bit reserved + NALU length size -1 = 3
		0xE1, // 1 SPS
	}
	l := make([]byte, 2)
	binary.BigEndian.PutUint16(l, uint16(len(sps)))
	out = append(out, l...)
	out = append(out, sps...)
	out = append(out, 0x01)
	binary.BigEndian.PutUint16(l, uint16(len(pps)))
	out = append(out, l...)
	out = append(out, pps...)
	return out
}

// AVCFrame 将一帧 NALUs 打成 FLV video tag body（带 4 字节长度前缀）。
func AVCFrame(f *Frame, sps, pps []byte) []byte {
	first := byte(0x27) // inter frame
	if f.Key {
		first = 0x17
	}
	out := []byte{first, 0x01, 0x00, 0x00, 0x00} // AVC NALU + composition time 0
	if f.Key {
		// 关键帧前附 SPS/PPS
		for _, n := range [][]byte{sps, pps} {
			l := make([]byte, 4)
			binary.BigEndian.PutUint32(l, uint32(len(n)))
			out = append(out, l...)
			out = append(out, n...)
		}
	}
	for _, n := range f.NALUs {
		l := make([]byte, 4)
		binary.BigEndian.PutUint32(l, uint32(len(n)))
		out = append(out, l...)
		out = append(out, n...)
	}
	return out
}
