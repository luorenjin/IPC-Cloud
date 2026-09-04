package mediagen

// PSMuxer H264 → MPEG-PS（GB28181 PS/PS 封装）。
type PSMuxer struct {
	scrBase int64 // 90kHz
}

// NewPSMuxer 构造 PS 封装器。
func NewPSMuxer() *PSMuxer { return &PSMuxer{} }

// psPackHeader 生成 PS pack header（00 00 01 BA + SCR）。
func (m *PSMuxer) psPackHeader(scr90k int64) []byte {
	scr := uint64(scr90k) * 300 // 27MHz
	b := make([]byte, 14)
	b[0], b[1], b[2] = 0x00, 0x00, 0x01
	b[3] = 0xBA
	// SCR 字段（33bit）+ extension（9bit）
	scrBase := scr / 300
	scrExt := scr % 300
	b[4] = 0x44 | byte(scrBase>>27)&0x38 | 0x04
	b[5] = byte(scrBase >> 20)
	b[6] = byte(scrBase>>15)&0xF8 | 0x04
	b[7] = byte(scrBase >> 7)
	b[8] = byte(scrBase<<1)&0xFE | 0x04
	b[9] = byte(scrExt<<3)&0xF8 | 0x04 | 1
	// mux rate + marker
	b[10] = 0x01
	b[11] = 0x89
	b[12] = 0xC3
	b[13] = 0xF8
	return b
}

var crcTab [256]uint32

func init() {
	for i := 0; i < 256; i++ {
		c := uint32(i) << 24
		for j := 0; j < 8; j++ {
			if c&0x80000000 != 0 {
				c = (c << 1) ^ 0x04C11DB7
			} else {
				c <<= 1
			}
		}
		crcTab[i] = c
	}
}

func crc32mpeg(b []byte) uint32 {
	crc := uint32(0xFFFFFFFF)
	for _, v := range b {
		crc = (crc << 8) ^ crcTab[byte(crc>>24)^v]
	}
	return crc
}

// psm 生成 Program Stream Map（ISO 13818-1 §2.5.4，声明 H264/0x1B → 0xE0）。
func psm() []byte {
	// elementary_stream_map 每项固定 4 字节：
	// stream_type(1B) + elementary_stream_id(1B) + elementary_stream_info_length(2B)。
	// libmpeg(ZLM) 按每项 ≥4B 解析，缺 info_length 会导致整个 map 被跳过。
	es := []byte{0x1B, 0xE0, 0x00, 0x00}
	// body: current_next/version(1) + reserved/marker(1) + program_stream_info_length(2)
	//       + elementary_stream_map_length(2) + es_map
	body := append([]byte{0xE0, 0xFF, 0x00, 0x00, 0x00, byte(len(es))}, es...)
	bodyLen := len(body) + 4 // length 字段计至 CRC 结束
	// CRC32/MPEG-2 覆盖 start code 之后的 length 与 body（不含 CRC 本身）
	crcIn := append([]byte{byte(bodyLen >> 8), byte(bodyLen)}, body...)
	crc := crc32mpeg(crcIn)
	body = append(body, byte(crc>>24), byte(crc>>16), byte(crc>>8), byte(crc))
	hdr := []byte{0x00, 0x00, 0x01, 0xBC, byte(bodyLen >> 8), byte(bodyLen)}
	return append(hdr, body...)
}

// encTS 编码 90kHz 时间戳为 5 字节 PTS/DTS 字段（prefix: '0010'DTS / '0011'PTS / '0001'仅PTS）。
func encTS(ts int64, prefix byte) []byte {
	b := make([]byte, 5)
	b[0] = prefix<<4 | byte(ts>>29)&0x0E | 1
	b[1] = byte(ts >> 22)
	b[2] = byte(ts>>14)&0xFE | 1
	b[3] = byte(ts >> 7)
	b[4] = byte(ts<<1)&0xFE | 1
	return b
}

// pesHeader 生成 PES 头（stream 0xE0，PTS=DTS 时仅写 PTS）。
func pesHeader(pts90k, dts90k int64, payloadLen int) []byte {
	var hdr []byte
	flags := byte(0x80) // '10' + PTS only
	if dts90k != pts90k {
		flags = 0xC0 // PTS+DTS
		hdr = append(hdr, encTS(dts90k, 0x2)...) // '0010'
		hdr = append(hdr, encTS(pts90k, 0x3)...) // '0011'
	} else {
		hdr = append(hdr, encTS(pts90k, 0x2)...) // '0010'
	}
	pesLen := 3 + len(hdr) + payloadLen
	out := []byte{0x00, 0x00, 0x01, 0xE0}
	out = append(out, byte(pesLen>>8), byte(pesLen))
	out = append(out, flags, 0x00, byte(len(hdr)))
	return append(out, hdr...)
}

// Pack 将一帧封装为一个 PS 包（pack header + [PSM] + PES(frame)）。
func (m *PSMuxer) Pack(f *Frame, withPSM bool) []byte {
	var es []byte
	for _, n := range f.NALUs {
		es = append(es, 0x00, 0x00, 0x00, 0x01)
		es = append(es, n...)
	}
	out := m.psPackHeader(f.DTS)
	if withPSM {
		out = append(out, psm()...)
	}
	out = append(out, pesHeader(f.PTS, f.DTS, len(es))...)
	out = append(out, es...)
	return out
}
