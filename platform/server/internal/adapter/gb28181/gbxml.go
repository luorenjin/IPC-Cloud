package gb28181

import (
	"encoding/xml"
	"fmt"
	"io"
	"strings"

	"golang.org/x/text/encoding/simplifiedchinese"
	"golang.org/x/text/encoding/unicode"
)

// CatalogItem 目录项。
type CatalogItem struct {
	DeviceID     string `xml:"DeviceID"`
	Name         string `xml:"Name"`
	Manufacturer string `xml:"Manufacturer"`
	Model        string `xml:"Model"`
	Owner        string `xml:"Owner"`
	Address      string `xml:"Address"`
	ParentID     string `xml:"ParentID"`
	Status       string `xml:"Status"`
}

// RecordItem 录像段。
type RecordItem struct {
	DeviceID  string `xml:"DeviceID"`
	Name      string `xml:"Name"`
	StartTime string `xml:"StartTime"`
	EndTime   string `xml:"EndTime"`
	Secrecy   string `xml:"Secrecy"`
	Type      string `xml:"Type"`
	FileSize  string `xml:"FileSize"`
}

// MANSCDP MANSCDP+xml 报文（Notify/Query/Response/Control 根元素统一解析）。
type MANSCDP struct {
	XMLName  xml.Name
	CmdType  string `xml:"CmdType"`
	SN       int64  `xml:"SN"`
	DeviceID string `xml:"DeviceID"`
	Status   string `xml:"Status"`
	SumNum   int    `xml:"SumNum"`
	DeviceList struct {
		Num   int           `xml:"Num,attr"`
		Items []CatalogItem `xml:"Item"`
	} `xml:"DeviceList"`
	RecordList struct {
		Num   int          `xml:"Num,attr"`
		Items []RecordItem `xml:"Item"`
	} `xml:"RecordList"`
	AlarmType    string `xml:"AlarmType"`
	AlarmTime    string `xml:"AlarmTime"`
	AlarmPriority string `xml:"AlarmPriority"`
	AlarmMethod  string `xml:"AlarmMethod"`
	Manufacturer string `xml:"Manufacturer"`
	Model        string `xml:"Model"`
	Firmware     string `xml:"Firmware"`
}

// DecodeBody 解析 MANSCDP（兼容 GB2312/GBK 与 UTF-8）。
func DecodeBody(body string) (*MANSCDP, error) {
	b := []byte(body)
	if i := strings.Index(body, `encoding="`); i >= 0 {
		enc := body[i+10:]
		if j := strings.Index(enc, `"`); j >= 0 {
			enc = strings.ToLower(enc[:j])
		}
		switch enc {
		case "gb2312", "gbk", "gb18030":
			if dec, err := simplifiedchinese.GB18030.NewDecoder().Bytes(b); err == nil {
				b = dec
			}
		default:
			// utf-8 等
		}
	}
	// BOM
	b = []byte(strings.TrimPrefix(string(b), "\ufeff"))
	var m MANSCDP
	if err := xml.Unmarshal(b, &m); err != nil {
		return nil, fmt.Errorf("bad MANSCDP: %w", err)
	}
	return &m, nil
}

func xmlEncodeUTF8(v any) string {
	var sb strings.Builder
	sb.WriteString(`<?xml version="1.0" encoding="utf-8"?>` + "\r\n")
	enc := unicode.UTF8.NewEncoder()
	w := enc.Writer(writerFunc(func(p []byte) (int, error) {
		sb.Write(p)
		return len(p), nil
	}))
	e := xml.NewEncoder(w)
	e.Encode(v)
	e.Flush()
	return sb.String()
}

type writerFunc func([]byte) (int, error)

func (f writerFunc) Write(p []byte) (int, error) { return f(p) }

var _ io.Writer = writerFunc(nil)

// gbQuery 通用查询体（Catalog/DeviceInfo/RecordInfo）。
type gbQuery struct {
	XMLName   xml.Name `xml:"Query"`
	CmdType   string   `xml:"CmdType"`
	SN        int64    `xml:"SN"`
	DeviceID  string   `xml:"DeviceID"`
	StartTime string   `xml:"StartTime,omitempty"`
	EndTime   string   `xml:"EndTime,omitempty"`
	Type      string   `xml:"Type,omitempty"`
}

// queryCatalogXML 构造 Catalog 查询。
func queryCatalogXML(deviceID string, sn int64) string {
	return xmlEncodeUTF8(gbQuery{CmdType: "Catalog", SN: sn, DeviceID: deviceID})
}

func queryDeviceInfoXML(deviceID string, sn int64) string {
	return xmlEncodeUTF8(gbQuery{CmdType: "DeviceInfo", SN: sn, DeviceID: deviceID})
}

func queryRecordInfoXML(deviceID string, sn int64, start, end string, recType string) string {
	return xmlEncodeUTF8(gbQuery{CmdType: "RecordInfo", SN: sn, DeviceID: deviceID,
		StartTime: start, EndTime: end, Type: recType})
}
