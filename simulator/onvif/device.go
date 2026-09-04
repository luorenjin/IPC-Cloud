// Package onvif ONVIF 设备模拟：WS-Discovery 发现 + Device/Media Service SOAP。
// 支持 GetDeviceInformation / GetProfiles / GetStreamUri / SystemReboot。
package onvif

import (
	"bytes"
	"crypto/sha1"
	"encoding/base64"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"strings"
)

// Device ONVIF 模拟设备。
type Device struct {
	LocalIP    string // 对外 IP（StreamUri 用）
	HTTPPort   int    // Device Service 端口
	RTSPPort   int    // RTSP server 端口
	User       string
	Password   string
	Serial     string
	OnLog      func(format string, args ...any)
}

// Start 启动 HTTP 服务与 WS-Discovery。
func (d *Device) Start() error {
	mux := http.NewServeMux()
	mux.HandleFunc("/onvif/device_service", d.handleSOAP)
	mux.HandleFunc("/onvif/", d.handleSOAP)
	srv := &http.Server{Addr: fmt.Sprintf(":%d", d.HTTPPort), Handler: mux}
	go func() {
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			d.logf("http: %v", err)
		}
	}()
	go d.discoveryLoop()
	d.logf("onvif service on http://%s:%d/onvif/device_service (ws-discovery active)", d.LocalIP, d.HTTPPort)
	return nil
}

// Stop 无后台资源需要释放。
func (d *Device) Stop() {}

func (d *Device) logf(format string, args ...any) {
	if d.OnLog != nil {
		d.OnLog("[onvif] "+format, args...)
	} else {
		log.Printf("[onvif] "+format, args...)
	}
}

// ---------- SOAP 处理 ----------

func (d *Device) handleSOAP(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	req := string(body)
	d.logf("%s %s", soapActionOf(r.Header.Get("Content-Type"), req), r.RemoteAddr)

	// UsernameToken 认证（若请求带 Security 且配置了凭据则校验）
	if d.User != "" && strings.Contains(req, "UsernameToken") {
		if !d.verifyUserToken(req) {
			soapFault(w, "Sender", "Not Authorized")
			return
		}
	}

	switch {
	case strings.Contains(req, "GetDeviceInformation"):
		respXML(w, fmt.Sprintf(`<tds:GetDeviceInformationResponse>
<tds:Manufacturer>JetsCam</tds:Manufacturer>
<tds:Model>SIM-ONVIF</tds:Model>
<tds:FirmwareVersion>1.0.0-sim</tds:FirmwareVersion>
<tds:SerialNumber>%s</tds:SerialNumber>
<tds:HardwareId>GK7205V200</tds:HardwareId>
</tds:GetDeviceInformationResponse>`, d.Serial))
	case strings.Contains(req, "GetProfiles"):
		respXML(w, `<trt:GetProfilesResponse>
<trt:Profiles token="main" fixed="true">
<trt:Name>main</trt:Name>
<trt:VideoEncoderConfiguration token="main">
<trt:Name>main</trt:Name>
<trt:Resolution><tt:Width>640</tt:Width><tt:Height>360</tt:Height></trt:Resolution>
</trt:VideoEncoderConfiguration>
</trt:Profiles>
<trt:Profiles token="sub" fixed="true">
<trt:Name>sub</trt:Name>
<trt:VideoEncoderConfiguration token="sub">
<trt:Name>sub</trt:Name>
<trt:Resolution><tt:Width>320</tt:Width><tt:Height>180</tt:Height></trt:Resolution>
</trt:VideoEncoderConfiguration>
</trt:Profiles>
</trt:GetProfilesResponse>`)
	case strings.Contains(req, "GetStreamUri"):
		token := xmlAttr(req, "ProfileToken", "token")
		path := "onvif1"
		if token == "sub" {
			path = "onvif2"
		}
		respXML(w, fmt.Sprintf(`<trt:GetStreamUriResponse>
<trt:MediaUri><tt:Uri>rtsp://%s:%d/%s</tt:Uri>
<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>
<tt:Timeout>PT60S</tt:Timeout></trt:MediaUri>
</trt:GetStreamUriResponse>`, d.LocalIP, d.RTSPPort, path))
	case strings.Contains(req, "SystemReboot"):
		respXML(w, `<tds:SystemRebootResponse><tds:Message>Rebooting</tds:Message></tds:SystemRebootResponse>`)
	case strings.Contains(req, "GetCapabilities"):
		respXML(w, fmt.Sprintf(`<tt:GetCapabilitiesResponse>
<tt:Capabilities><tt:Device><tt:XAddr>http://%s:%d/onvif/device_service</tt:XAddr></tt:Device>
<tt:Media><tt:XAddr>http://%s:%d/onvif/device_service</tt:XAddr></tt:Media></tt:Capabilities>
</tt:GetCapabilitiesResponse>`, d.LocalIP, d.HTTPPort, d.LocalIP, d.HTTPPort))
	case strings.Contains(req, "GetServices"):
		respXML(w, fmt.Sprintf(`<tds:GetServicesResponse>
<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>
<tds:XAddr>http://%s:%d/onvif/device_service</tds:XAddr></tds:Service>
<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>
<tds:XAddr>http://%s:%d/onvif/device_service</tds:XAddr></tds:Service>
</tds:GetServicesResponse>`, d.LocalIP, d.HTTPPort, d.LocalIP, d.HTTPPort))
	default:
		soapFault(w, "Sender", "Unknown action")
	}
}

func respXML(w http.ResponseWriter, body string) {
	out := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
 xmlns:tds="http://www.onvif.org/ver10/device/wsdl"
 xmlns:trt="http://www.onvif.org/ver10/media/wsdl"
 xmlns:tt="http://www.onvif.org/ver10/schema">
<s:Body>%s</s:Body></s:Envelope>`, body)
	w.Header().Set("Content-Type", "application/soap+xml; charset=utf-8")
	_, _ = w.Write([]byte(out))
}

func soapFault(w http.ResponseWriter, code, msg string) {
	out := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">
<s:Body><s:Fault><s:Code><s:Value>s:%s</s:Value></s:Code>
<s:Reason><s:Text>%s</s:Text></s:Reason></s:Fault></s:Body></s:Envelope>`, code, msg)
	w.Header().Set("Content-Type", "application/soap+xml; charset=utf-8")
	w.WriteHeader(http.StatusBadRequest)
	_, _ = w.Write([]byte(out))
}

// verifyUserToken 校验 WS-UsernameToken PasswordDigest。
func (d *Device) verifyUserToken(req string) bool {
	user := xmlText(req, "Username")
	nonceB64 := xmlText(req, "Nonce")
	created := xmlText(req, "Created")
	digest := xmlText(req, "Password")
	if user != d.User {
		return false
	}
	nonce, err := base64.StdEncoding.DecodeString(nonceB64)
	if err != nil {
		return false
	}
	h := sha1.Sum(append(append(nonce, []byte(created)...), []byte(d.Password)...))
	return base64.StdEncoding.EncodeToString(h[:]) == digest
}

func soapActionOf(ct, body string) string {
	if i := strings.Index(ct, "action="); i >= 0 {
		a := strings.Trim(strings.TrimPrefix(ct[i+7:], `"`), `"`)
		if j := strings.LastIndex(a, "/"); j >= 0 {
			a = a[j+1:]
		}
		return a
	}
	return "unknown"
}

func xmlText(doc, tag string) string {
	for _, t := range []string{tag, "tt:" + tag} {
		// 无属性形式 <tag>…</tag>
		if i := strings.Index(doc, "<"+t+">"); i >= 0 {
			rest := doc[i+len(t)+2:]
			j := strings.Index(rest, "</"+t+">")
			if j >= 0 {
				return strings.TrimSpace(rest[:j])
			}
		}
		// 带属性形式 <tag attr="…">…</tag>（如 Password/Nonce 的 WS-Security 元素）
		if i := strings.Index(doc, "<"+t+" "); i >= 0 {
			gt := strings.Index(doc[i:], ">")
			if gt < 0 {
				continue
			}
			start := i + gt + 1
			j := strings.Index(doc[start:], "</"+t+">")
			if j >= 0 {
				return strings.TrimSpace(doc[start : start+j])
			}
		}
	}
	return ""
}

func xmlAttr(doc, elem, attr string) string {
	i := strings.Index(doc, "<"+elem)
	if i < 0 {
		return ""
	}
	seg := doc[i:min(len(doc), i+512)]
	j := strings.Index(seg, attr+`="`)
	if j < 0 {
		return ""
	}
	rest := seg[j+len(attr)+3:]
	if k := strings.Index(rest, `"`); k >= 0 {
		return rest[:k]
	}
	return ""
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// ---------- WS-Discovery ----------

const probeSOAPNeedle = "Probe"

// discoveryLoop 加入组播组响应 Probe。
func (d *Device) discoveryLoop() {
	ifaceIP := net.ParseIP(d.LocalIP)
	pc, err := net.ListenMulticastUDP("udp4", nil, &net.UDPAddr{IP: net.ParseIP("239.255.255.250"), Port: 3702})
	if err != nil {
		d.logf("discovery join: %v", err)
		return
	}
	defer pc.Close()
	buf := make([]byte, 65535)
	for {
		n, from, err := pc.ReadFromUDP(buf)
		if err != nil {
			return
		}
		req := string(buf[:n])
		if !strings.Contains(req, probeSOAPNeedle) {
			continue
		}
		xaddr := fmt.Sprintf("http://%s:%d/onvif/device_service", d.LocalIP, d.HTTPPort)
		msgID := xmlText(req, "MessageID")
		if msgID == "" {
			msgID = "urn:sim"
		}
		resp := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
 xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing"
 xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"
 xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
<s:Header>
<w:MessageID>%s</w:MessageID>
<w:RelatesTo>%s</w:RelatesTo>
<w:To>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:To>
<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</w:Action>
</s:Header>
<s:Body>
<d:ProbeMatches>
<d:ProbeMatch>
<w:EndpointReference><w:Address>urn:uuid:sim-onvif-%s</w:Address></w:EndpointReference>
<d:Types>dn:NetworkVideoTransmitter</d:Types>
<d:Scopes>onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/name/SIM-ONVIF</d:Scopes>
<d:XAddrs>%s</d:XAddrs>
<d:MetadataVersion>1</d:MetadataVersion>
</d:ProbeMatch>
</d:ProbeMatches>
</s:Body>
</s:Envelope>`, msgID, msgID, d.Serial, xaddr)
		if _, err := pc.WriteToUDP([]byte(resp), from); err != nil {
			if ifaceIP == nil {
				return
			}
		}
		_ = bytes.MinRead
	}
}
