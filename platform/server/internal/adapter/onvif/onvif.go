// Package onvif ONVIF 适配器：WS-Discovery 发现、添加探测、状态探测（接入规范 §7）。
package onvif

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/jetscam/ipccloud/server/internal/adapter"
	"github.com/jetscam/ipccloud/server/internal/crypto"
	"github.com/jetscam/ipccloud/server/internal/errs"
	"github.com/jetscam/ipccloud/server/internal/models"
	"github.com/jetscam/ipccloud/server/internal/store"
)

func b64(b []byte) string { return base64.StdEncoding.EncodeToString(b) }

func sha1Sum(b []byte) []byte {
	h := sha1.Sum(b)
	return h[:]
}

func jsonUnmarshal(s string, v any) error { return json.Unmarshal([]byte(s), v) }

const Source = "onvif"

// Adapter ONVIF 适配器。
type Adapter struct {
	ctx  context.Context
	mu   sync.Mutex
	hc   *http.Client
	keys map[string]profileKeys
}

// profileKeys 记录探测出的 RTSP 地址。
type profileKeys struct {
	Main string
	Sub  string
}

func New() *Adapter {
	return &Adapter{hc: &http.Client{Timeout: 6 * time.Second}, keys: map[string]profileKeys{}}
}

func (a *Adapter) Source() string { return Source }

func (a *Adapter) Start(ctx context.Context) error {
	a.ctx = ctx
	go a.statusWatcher(ctx)
	return nil
}

func (a *Adapter) Stop() {}

// ---------- WS-Discovery ----------

// Discovered 发现结果。
type Discovered struct {
	IP     string `json:"ip"`
	XAddr  string `json:"xaddr"`
	Scopes string `json:"scopes"`
}

const probeSOAP = `<?xml version="1.0" encoding="UTF-8"?>
<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope"
  xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing"
  xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"
  xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <e:Header>
    <w:MessageID>uuid:%s</w:MessageID>
    <w:To e:mustUnderstand="true">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>
    <w:Action e:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>
  </e:Header>
  <e:Body>
    <d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe>
  </e:Body>
</e:Envelope>`

// Discover WS-Discovery 探测（默认 10s 超时）。
func Discover(ctx context.Context, timeout time.Duration) ([]Discovered, error) {
	pc, err := net.ListenPacket("udp4", ":0")
	if err != nil {
		return nil, err
	}
	defer pc.Close()
	dst, _ := net.ResolveUDPAddr("udp4", "239.255.255.250:3702")
	b := make([]byte, 8)
	rand.Read(b)
	msgID := base64.RawURLEncoding.EncodeToString(b)
	if _, err := pc.WriteTo([]byte(fmt.Sprintf(probeSOAP, msgID)), dst); err != nil {
		return nil, err
	}
	results := map[string]Discovered{}
	deadline := time.Now().Add(timeout)
	buf := make([]byte, 65535)
	for time.Now().Before(deadline) {
		pc.SetReadDeadline(deadline)
		n, _, err := pc.ReadFrom(buf)
		if err != nil {
			break
		}
		var env struct {
			Body struct {
				ProbeMatches struct {
					ProbeMatch []struct {
						XAddrs string `xml:"XAddrs"`
						Scopes string `xml:"Scopes"`
					} `xml:"ProbeMatch"`
				} `xml:"ProbeMatches"`
			} `xml:"Body"`
		}
		if xml.Unmarshal(buf[:n], &env) == nil {
			for _, pm := range env.Body.ProbeMatches.ProbeMatch {
				for _, x := range strings.Fields(pm.XAddrs) {
					host := hostOf(x)
					if host == "" {
						continue
					}
					if _, dup := results[host]; !dup {
						results[host] = Discovered{IP: host, XAddr: x, Scopes: pm.Scopes}
					}
				}
			}
		}
	}
	out := make([]Discovered, 0, len(results))
	for _, v := range results {
		out = append(out, v)
	}
	return out, nil
}

func hostOf(xaddr string) string {
	s := xaddr
	if i := strings.Index(s, "://"); i >= 0 {
		s = s[i+3:]
	}
	if i := strings.IndexAny(s, ":/"); i >= 0 {
		s = s[:i]
	}
	return s
}

// ---------- SOAP 工具 ----------

// soapAction 发送带 WS-UsernameToken 认证的 SOAP 请求。
func soapAction(ctx context.Context, xaddr, user, pass, action, extra string) (string, error) {
	nonce := make([]byte, 12)
	rand.Read(nonce)
	created := time.Now().UTC().Format("2006-01-02T15:04:05Z")
	digest := b64(sha1Sum(append(append(nonce, []byte(created)...), []byte(pass)...)))
	nb64 := b64(nonce)
	auth := ""
	if user != "" {
		auth = fmt.Sprintf(`<Security s:mustUnderstand="0" xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd">
<UsernameToken><Username>%s</Username><Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">%s</Password>
<Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary">%s</Nonce>
<Created xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd">%s</Created></UsernameToken></Security>`,
			xmlEscape(user), digest, nb64, created)
	}
	body := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:trt="http://www.onvif.org/ver10/media/wsdl"
 xmlns:tds="http://www.onvif.org/ver10/device/wsdl">
<s:Header>%s</s:Header><s:Body>%s</s:Body></s:Envelope>`, auth, extra)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, strings.TrimRight(xaddr, "/"), bytes.NewReader([]byte(body)))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/soap+xml; charset=utf-8; action="+action)
	resp, err := a_httpClient().Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	out := string(b)
	if resp.StatusCode != http.StatusOK {
		return out, fmt.Errorf("E2001 ONVIF http %d", resp.StatusCode)
	}
	return out, nil
}

// a_httpClient 共享 HTTP 客户端。
func a_httpClient() *http.Client { return &http.Client{Timeout: 6 * time.Second} }

// ---------- 探测 ----------

// ProbeInfo 探测结果。
type ProbeInfo struct {
	Vendor   string
	Model    string
	Firmware string
	Serial   string
	Profiles []Profile
}

// Profile 码流档位。
type Profile struct {
	Token     string
	Name      string
	Width     int
	Height    int
	StreamURI string
}

// Probe GetDeviceInformation/GetProfiles/GetStreamUri（§7.2）。
func (a *Adapter) Probe(ctx context.Context, xaddr, user, pass string) (*ProbeInfo, error) {
	info := &ProbeInfo{}
	out, err := soapAction(ctx, xaddr, user, pass,
		"http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation",
		`<tds:GetDeviceInformation/>`)
	if err != nil {
		return nil, err
	}
	info.Vendor = xmlText(out, "Manufacturer")
	info.Model = xmlText(out, "Model")
	info.Firmware = xmlText(out, "FirmwareVersion")
	info.Serial = xmlText(out, "SerialNumber")

	out, err = soapAction(ctx, xaddr, user, pass,
		"http://www.onvif.org/ver10/media/wsdl/GetProfiles", `<trt:GetProfiles/>`)
	if err != nil {
		return nil, errs.EOnvifDisabled
	}
	var pr struct {
		Profiles []struct {
			Token string `xml:"token,attr"`
			Name  string `xml:"Name"`
			Video struct {
				Resolution struct {
					Width  int `xml:"Width"`
					Height int `xml:"Height"`
				} `xml:"Resolution"`
			} `xml:"VideoEncoderConfiguration"`
		} `xml:"Profiles"`
	}
	if xml.Unmarshal([]byte(soapBodyOf(out)), &pr) == nil {
		for _, p := range pr.Profiles {
			info.Profiles = append(info.Profiles, Profile{
				Token: p.Token, Name: p.Name,
				Width: p.Video.Resolution.Width, Height: p.Video.Resolution.Height,
			})
		}
	}
	for i := range info.Profiles {
		uriSOAP := fmt.Sprintf(`<trt:GetStreamUri><trt:StreamSetup><Stream xmlns="http://www.onvif.org/ver10/schema">RTP-Unicast</Stream>
<Transport xmlns="http://www.onvif.org/ver10/schema"><Protocol>RTSP</Protocol></Transport></trt:StreamSetup>
<trt:ProfileToken>%s</trt:ProfileToken></trt:GetStreamUri>`, xmlEscape(info.Profiles[i].Token))
		out, err := soapAction(ctx, xaddr, user, pass,
			"http://www.onvif.org/ver10/media/wsdl/GetStreamUri", uriSOAP)
		if err == nil {
			info.Profiles[i].StreamURI = xmlText(out, "Uri")
		}
	}
	if len(info.Profiles) == 0 {
		return nil, errs.EOnvifDisabled
	}
	return info, nil
}

// ---------- Adapter 接口实现 ----------

func (a *Adapter) StartStream(ctx context.Context, opt adapter.StartOptions) error {
	return nil // engine 直接 addStreamProxy
}

func (a *Adapter) StopStream(opt adapter.StopOptions) {}

func (a *Adapter) Snapshot(ctx context.Context, channelID, uploadURL string) (string, error) {
	return "", errs.EForbid.WithMsg("请经 ZLM 抓图")
}

func (a *Adapter) QueryRecords(ctx context.Context, q adapter.RecordQuery) ([]map[string]any, int, bool, error) {
	return nil, 0, false, nil
}

func (a *Adapter) StartPlayback(ctx context.Context, p adapter.PlaybackStart) error {
	return errs.EGBRecordUnsup
}

func (a *Adapter) PlaybackCtrl(ctx context.Context, p adapter.PlaybackCtrl) error {
	return errs.EGBRecordUnsup
}

func (a *Adapter) StopPlayback(ctx context.Context, channelID, sessionID string) {}

func (a *Adapter) Reboot(ctx context.Context, deviceID string) error {
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", deviceID).Error; err != nil {
		return errs.ENotFound
	}
	xaddr, user, pass, err := a.creds(&dev)
	if err != nil {
		return err
	}
	_, err = soapAction(ctx, xaddr, user, pass,
		"http://www.onvif.org/ver10/device/wsdl/SystemReboot", `<tds:SystemReboot/>`)
	return err
}

func (a *Adapter) Diagnose(ctx context.Context, deviceID string) ([]map[string]any, error) {
	var dev models.Device
	if err := store.DB.First(&dev, "id = ?", deviceID).Error; err != nil {
		return nil, errs.ENotFound
	}
	xaddr, _, _, _ := a.creds(&dev)
	ok := probeTCP(xaddr, 3*time.Second)
	return []map[string]any{
		{"item": "onvif", "ok": ok, "cost": 0, "msg": map[bool]string{true: "", false: "设备不可达"}[ok]},
	}, nil
}

func (a *Adapter) Transfer(ctx context.Context, deviceID, projectID string) error { return nil }
func (a *Adapter) Unbind(ctx context.Context, deviceID string) error              { return nil }

// creds 解密凭据并取 XAddr。
func (a *Adapter) creds(dev *models.Device) (xaddr, user, pass string, err error) {
	ip, _ := dev.Identity["ip"].(string)
	port, _ := dev.Identity["port"].(string)
	if port == "" {
		port = "80"
	}
	xaddr = fmt.Sprintf("http://%s:%s/onvif/device_service", ip, port)
	raw, e := crypto.Dec(dev.CredentialsEnc)
	if e == nil {
		var c map[string]string
		if jsonUnmarshal(raw, &c) == nil {
			user, pass = c["user"], c["pass"]
		}
	}
	return
}

// statusWatcher 30s 周期探测在线状态（§3.2，连续 3 次失败判离线）。
func (a *Adapter) statusWatcher(ctx context.Context) {
	t := time.NewTicker(30 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			var devs []models.Device
			store.DB.Where("source = ? AND status IN ('online','offline') AND deleted_at = 0", Source).Find(&devs)
			for _, d := range devs {
				xaddr, _, _, _ := a.creds(&d)
				ok := probeTCP(xaddr, 4*time.Second)
				if ok && d.Status != "online" {
					store.DB.Model(&models.Device{}).Where("id = ?", d.ID).
						Updates(map[string]any{"status": "online", "last_seen_at": models.NowMilli()})
				} else if !ok && d.Status == "online" {
					n := failCount(&d) + 1
					if d.Meta == nil {
						d.Meta = models.JSONB{}
					}
					d.Meta["probeFails"] = n
					if n >= 3 {
						store.DB.Model(&models.Device{}).Where("id = ?", d.ID).
							Update("status", "offline")
					}
					store.DB.Model(&d).Update("meta", d.Meta)
				}
			}
		}
	}
}

func failCount(d *models.Device) int {
	n, _ := d.Meta["probeFails"].(float64)
	return int(n)
}

func probeTCP(xaddr string, timeout time.Duration) bool {
	host := strings.TrimPrefix(xaddr, "http://")
	host = strings.Split(host, "/")[0]
	conn, err := net.DialTimeout("tcp", host, timeout)
	if err != nil {
		return false
	}
	conn.Close()
	return true
}

func xmlEscape(s string) string {
	r := strings.NewReplacer("&", "&amp;", "<", "&lt;", ">", "&gt;", `"`, "&quot;")
	return r.Replace(s)
}

// soapBodyOf 提取 SOAP 响应 Body 的内部 XML（兼容 s:/soap: 前缀），供结构化解析。
func soapBodyOf(doc string) string {
	open := -1
	for _, n := range []string{"<s:Body>", "<soap:Body>", "<Body>"} {
		if i := strings.Index(doc, n); i >= 0 {
			open = i + len(n)
			break
		}
	}
	if open < 0 {
		return doc
	}
	close := -1
	for _, n := range []string{"</s:Body>", "</soap:Body>", "</Body>"} {
		if j := strings.LastIndex(doc, n); j >= 0 {
			close = j
			break
		}
	}
	if close < open {
		return doc
	}
	return doc[open:close]
}

// xmlText 提取元素文本，兼容无前缀 <tag> 与命名空间前缀 <prefix:tag>（真实 ONVIF 设备多用后者）。
func xmlText(doc, tag string) string {
	for _, n := range []string{"<" + tag, ":" + tag} {
		i := strings.Index(doc, n)
		if i < 0 {
			continue
		}
		gt := strings.Index(doc[i:], ">")
		if gt < 0 {
			continue
		}
		openEnd := i + gt
		// 回溯得到完整开始标签名（如 tt:Uri / Manufacturer）
		lt := strings.LastIndex(doc[:openEnd], "<")
		if lt < 0 {
			continue
		}
		name := strings.SplitN(doc[lt+1:openEnd], " ", 2)[0]
		if !strings.HasSuffix(name, tag) {
			continue
		}
		start := openEnd + 1
		j := strings.Index(doc[start:], "</"+name+">")
		if j >= 0 {
			return strings.TrimSpace(doc[start : start+j])
		}
	}
	return ""
}