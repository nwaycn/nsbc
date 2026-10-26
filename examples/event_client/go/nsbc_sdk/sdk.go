// Package nsbc_sdk provides a unified Go client library for NSBC
// (Next-Generation Session Border Controller) realtime event/CDR
// subscription + originate.
//
// Pure stdlib — no third-party deps. Zero external imports beyond
// Go 1.21 stdlib.
//
// API contract (same surface as Python nsbc_client):
//
//   // Connection
//   uds := nsbc_sdk.NewClientUds("/var/run/nsbc/control.sock")
//   err := uds.Connect()
//   defer uds.Close()
//   uds.Subscribe("events,cdr")
//   go uds.IterFrames(frameCh)   // pushes *Frame into channel
//
//   // Originate (dual-path: UDS preferred → HTTP fallback)
//   callUrl := nsbc_sdk.BuildCallUrl(nsbc_sdk.OriginateOpts{
//       OrigTo: "7gja30rf",
//       App:    "echo",
//       Audio:  []string{"PCMU", "PCMA", "G722", "OPUS"},
//       Video:  []string{"H264", "VP8"},
//   })
//   ok, payload, via := nsbc_sdk.Originate(callUrl, "auto", udsPath, apiBase, apiToken)
//
//   // Frame parsing / correlation / webhook
//   frame := nsbc_sdk.ParseFrame(line)
//   correlator := nsbc_sdk.NewCallCorrelator(timelineCb)
//   correlator.Feed(frame)
//   webhook := nsbc_sdk.NewWebhookSink(url, 5, 2)
//   webhook.Send(frame)
package nsbc_sdk

import (
	"bufio"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"
)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const (
	DefaultUds      = "/var/run/nsbc/control.sock"
	DefaultApiBase  = "https://127.0.0.1:443"
	DefaultSseUrl   = "https://127.0.0.1:443/api/v1/calls/stream"
	DefaultTopics   = "events,cdr"
	DefaultTimeoutS = 30
)

const (
	prompt          = "nsbc> "
	bannerPrefix    = "NSBC "
	defaultAudio    = "PCMU,PCMA,G722,OPUS"
	defaultVideo    = "H264,VP8"
)

// Frame header: YYYY-MM-DDTHH:MM:SS.mmm DOMAIN body
var frameRe = regexp.MustCompile(`^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3})\s+(\S+)\s+(.*)$`)
var kvRe = regexp.MustCompile(`(\w+)=(\S+)`)

// sslVerify global switch — set InsecureSkipVerify on HTTP path when false.
var sslVerify = true

// SetSSLVerify flips TLS cert verification for HTTP paths (nsbc-web API + SSE).
func SetSSLVerify(verify bool) { sslVerify = verify }

func newHTTPClient(timeout time.Duration) *http.Client {
	t := &http.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: !sslVerify},
	}
	return &http.Client{Transport: t, Timeout: timeout}
}

// ---------------------------------------------------------------------------
// Frame type
// ---------------------------------------------------------------------------

// Frame is the canonical parsed obs_sub push frame.
// For CDR domain, Fields holds the full JSON record.
// For events, Fields is a map of k=v pairs.
type Frame struct {
	Ts     string                 `json:"ts"`     // "2026-10-26T15:30:01.123"
	Domain string                 `json:"domain"` // CALL_INCOMING / CALL_END / CDR ...
	Kind   string                 `json:"kind"`   // call / cdr / register / cmd / other
	Fields map[string]interface{} `json:"fields"` // per-domain payload
	Raw    string                 `json:"raw"`    // original line (rstripped)
	Source string                 `json:"source"` // "uds" | "sse"
}

// ---------------------------------------------------------------------------
// UDSClient — nsbcd Unix Domain Socket control interface
// ---------------------------------------------------------------------------

// Client is a thin wrapper around nsbcd's Unix Domain Socket control interface.
type Client struct {
	udsPath string
	conn    net.Conn
	banner  string
	mu      sync.Mutex // protects SendCommand
	reader  *bufio.Reader
}

// NewClientUds creates an unconnected UDS client.
func NewClientUds(udsPath string) *Client {
	return &Client{udsPath: udsPath}
}

// Connect opens the Unix socket and reads the initial banner/prompt.
func (c *Client) Connect() error {
	fi, err := os.Stat(c.udsPath)
	if err != nil || fi != nil && fi.Mode()&os.ModeSocket == 0 {
		return fmt.Errorf("UDS socket not found: %s", c.udsPath)
	}
	conn, err := net.DialTimeout("unix", c.udsPath, 5*time.Second)
	if err != nil {
		return fmt.Errorf("dial %s: %w", c.udsPath, err)
	}
	c.conn = conn
	c.reader = bufio.NewReaderSize(conn, 65536)
	// Probe banner (non-blocking)
	_ = conn.SetReadDeadline(time.Now().Add(1 * time.Second))
	b, err := io.ReadAll(io.LimitReader(c.reader, 4096))
	if err == nil {
		c.banner = string(b)
	}
	_ = conn.SetReadDeadline(time.Time{})
	return nil
}

// Banner returns the initial nsbcd banner string (call after Connect).
func (c *Client) Banner() string { return c.banner }

// Close shuts down the connection.
func (c *Client) Close() error {
	if c.conn != nil {
		return c.conn.Close()
	}
	return nil
}

// SendCommand sends a single \n-terminated command and reads up to the next nsbc> prompt.
func (c *Client) SendCommand(cmd string, timeout time.Duration) (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	_, err := c.conn.Write([]byte(strings.TrimRight(cmd, "\n") + "\n"))
	if err != nil {
		return "", err
	}

	_ = c.conn.SetReadDeadline(time.Now().Add(timeout))
	var sb strings.Builder
	for time.Now().Before(time.Now().Add(0)) { // deadline already set above
		line, rerr := c.reader.ReadString('\n')
		if line != "" {
			sb.WriteString(line)
			if strings.Contains(line, prompt) {
				break
			}
		}
		if rerr != nil {
			break
		}
	}
	_ = c.conn.SetReadDeadline(time.Time{})

	out := sb.String()
	if idx := strings.LastIndex(out, prompt); idx >= 0 {
		out = out[:idx]
	}
	return strings.TrimRight(out, "\r\n"), nil
}

// Subscribe sends "sub <topics>" and returns the server response.
func (c *Client) Subscribe(topics string) (string, error) {
	return c.SendCommand("sub "+topics, 2*time.Second)
}

// Unsubscribe sends "unsub [topics]".
func (c *Client) Unsubscribe(topics string) (string, error) {
	arg := topics
	if arg == "" {
		return c.SendCommand("unsub", 2*time.Second)
	}
	return c.SendCommand("unsub "+arg, 2*time.Second)
}

// IterFrames blocks and pushes parsed *Frame into ch until the connection closes.
func (c *Client) IterFrames(ch chan<- *Frame) {
	defer close(ch)
	for {
		line, err := c.reader.ReadString('\n')
		if err != nil {
			return
		}
		f := ParseFrame(strings.TrimRight(line, "\r\n"))
		if f != nil {
			f.Source = "uds"
			ch <- f
		}
	}
}

// ---------------------------------------------------------------------------
// Originate: call_url builder + dual-path dispatch
// ---------------------------------------------------------------------------

// OriginateOpts parameterizes BuildCallUrl.
type OriginateOpts struct {
	OrigTo     string   // destination extension/user (default "1000")
	App        string   // post-originate app: "echo" | "park" | "hangup" (default "echo")
	Audio      []string // audio codecs; nil/empty → skipped
	Video      []string // video codecs; nil/empty → skipped
	Timeout    int      // ringing timeout seconds; 0 → skipped
	CallerID   string   // origination caller ID number
	CallerName string   // origination caller ID name
	CallUrl    string   // explicit call_url (bypasses builder)
}

// BuildCallUrl assembles a FreeSWITCH-style originate call_url with brace-var codec block.
func BuildCallUrl(opts OriginateOpts) string {
	if opts.CallUrl != "" {
		return opts.CallUrl
	}
	ext := opts.OrigTo
	if ext == "" {
		ext = "1000"
	}
	app := opts.App
	if app == "" {
		app = "echo"
	}

	var parts []string
	if opts.CallerID != "" {
		parts = append(parts, "origination_caller_id_number="+opts.CallerID)
	}
	if opts.CallerName != "" {
		parts = append(parts, "origination_caller_id_name="+opts.CallerName)
	}
	if opts.Timeout > 0 {
		parts = append(parts, fmt.Sprintf("originate_timeout=%d", opts.Timeout))
	}
	if len(opts.Audio) > 0 {
		parts = append(parts, "audio_codecs="+strings.Join(opts.Audio, ","))
	}
	if len(opts.Video) > 0 {
		parts = append(parts, "video_codecs="+strings.Join(opts.Video, ","))
	}

	vars := ""
	if len(parts) > 0 {
		vars = "{" + strings.Join(parts, ",") + "}"
	}
	return fmt.Sprintf("%suser/%s &%s", vars, ext, app)
}

// OriginateResult is the return tuple of Originate (Go doesn't have named tuple
// return values on the call site — prefer a struct when callers need all 3).
type OriginateResult struct {
	OK      bool
	Payload interface{} // string on UDS; map[string]any on HTTP; map{"uds":...,"web":...} on both-fail
	Via     string // "uds" | "web" | "both_failed"
}

// Originate fires an originate via UDS or HTTP, with auto-fallback in "auto" mode.
func Originate(callUrl, via, udsPath, apiBase, apiToken string) OriginateResult {
	switch via {
	case "uds":
		ok, p := originateViaUds(udsPath, callUrl, 10*time.Second)
		return OriginateResult{OK: ok, Payload: p, Via: "uds"}
	case "web":
		ok, p := originateViaWeb(apiBase, callUrl, 10*time.Second, apiToken)
		return OriginateResult{OK: ok, Payload: p, Via: "web"}
	}

	// auto: UDS first → HTTP fallback
	ok, p := originateViaUds(udsPath, callUrl, 10*time.Second)
	if ok {
		return OriginateResult{OK: true, Payload: p, Via: "uds"}
	}
	ok2, p2 := originateViaWeb(apiBase, callUrl, 10*time.Second, apiToken)
	if ok2 {
		return OriginateResult{OK: true, Payload: p2, Via: "web"}
	}
	return OriginateResult{
		OK:      false,
		Payload: map[string]interface{}{"uds": p, "web": p2},
		Via:     "both_failed",
	}
}

func originateViaUds(udsPath, callUrl string, timeout time.Duration) (bool, string) {
	fi, err := os.Stat(udsPath)
	if err != nil || fi != nil && fi.Mode()&os.ModeSocket == 0 {
		return false, fmt.Sprintf("UDS socket not found: %s", udsPath)
	}

	c, err := net.DialTimeout("unix", udsPath, 5*time.Second)
	if err != nil {
		return false, fmt.Sprintf("dial: %v", err)
	}
	defer c.Close()

	_ = c.SetDeadline(time.Now().Add(timeout))
	_, _ = c.Write([]byte("originate " + callUrl + "\n"))

	var sb strings.Builder
	buf := make([]byte, 65536)
	for {
		n, rerr := c.Read(buf)
		if n > 0 {
			sb.Write(buf[:n])
			text := sb.String()
			if (strings.Contains(text, "OK ") || strings.Contains(text, "ERR ")) &&
				(strings.HasSuffix(strings.TrimRight(text, "\r\n"), ">") || strings.Contains(text, ">")) {
				break
			}
		}
		if rerr != nil {
			break
		}
		if sb.Len() > 1_000_000 {
			break
		}
	}
	_ = c.SetDeadline(time.Time{})

	text := strings.TrimSpace(sb.String())
	text = strings.TrimRight(text, ">")
	text = strings.TrimSpace(text)

	if strings.HasPrefix(text, "OK ") || strings.Contains(text, "\nOK ") || text == "OK" {
		return true, text
	}
	if strings.HasPrefix(text, "ERR") || strings.Contains(text, "ERR ") {
		return false, text
	}
	if text == "" {
		return true, "dispatched (async)"
	}
	return true, text
}

func originateViaWeb(apiBase, callUrl string, timeout time.Duration, apiToken string) (bool, interface{}) {
	url := strings.TrimRight(apiBase, "/") + "/ext/v1/originate"
	body, _ := json.Marshal(map[string]string{"call_url": callUrl})

	req, err := http.NewRequest("POST", url, strings.NewReader(string(body)))
	if err != nil {
		return false, map[string]interface{}{"network_error": err.Error()}
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	if apiToken != "" {
		req.Header.Set("Authorization", "Bearer "+apiToken)
	}

	hc := newHTTPClient(timeout)
	resp, err := hc.Do(req)
	if err != nil {
		return false, map[string]interface{}{"network_error": err.Error()}
	}
	defer resp.Body.Close()

	data, _ := io.ReadAll(resp.Body)
	if resp.StatusCode >= 400 {
		return false, map[string]interface{}{
			"http_error": resp.StatusCode,
			"detail":     string(data),
		}
	}

	var parsed interface{}
	if jerr := json.Unmarshal(data, &parsed); jerr == nil {
		return true, parsed
	}
	return true, string(data)
}

// ---------------------------------------------------------------------------
// Frame parsing
// ---------------------------------------------------------------------------

// ParseFrame converts one obs_sub push line → *Frame, or nil.
func ParseFrame(line string) *Frame {
	line = strings.TrimRight(line, "\r\n")
	if line == "" {
		return nil
	}
	m := frameRe.FindStringSubmatch(line)
	if m == nil {
		return nil
	}
	ts, domain, body := m[1], m[2], m[3]

	f := &Frame{
		Ts:     ts,
		Domain: domain,
		Kind:   classifyDomain(domain),
		Raw:    line,
		Fields: map[string]interface{}{},
	}

	if domain == "CDR" {
		_ = json.Unmarshal([]byte(body), &f.Fields)
	} else {
		for _, km := range kvRe.FindAllStringSubmatch(body, -1) {
			f.Fields[km[1]] = km[2]
		}
	}
	return f
}

func classifyDomain(domain string) string {
	switch {
	case strings.HasPrefix(domain, "CALL_") || domain == "CALL_OUTGOING":
		return "call"
	case domain == "CDR":
		return "cdr"
	case domain == "REGISTER":
		return "register"
	case domain == "OK" || domain == "ERR" || strings.HasPrefix(domain, "subscribed:"):
		return "cmd"
	}
	return "other"
}

// ---------------------------------------------------------------------------
// CallCorrelator: stitch call lifecycle by call_id
// ---------------------------------------------------------------------------

// CallCorrelator tracks CALL_INCOMING → CALL_ANSWERED → CALL_END → CDR per call_id.
type CallCorrelator struct {
	mu         sync.Mutex
	calls      map[string]map[string]interface{}
	onComplete func(map[string]interface{})
}

// NewCallCorrelator creates a correlator. onComplete fires each time a CDR arrives.
func NewCallCorrelator(onComplete func(map[string]interface{})) *CallCorrelator {
	return &CallCorrelator{
		calls:      map[string]map[string]interface{}{},
		onComplete: onComplete,
	}
}

// Feed a parsed frame into the correlator. Returns the complete timeline if
// a CDR just arrived, nil otherwise. Thread-safe.
func (cc *CallCorrelator) Feed(frame *Frame) map[string]interface{} {
	if frame == nil {
		return nil
	}
	callID, _ := frame.Fields["call_id"].(string)
	if callID == "" {
		callID, _ = frame.Fields["callid"].(string)
	}
	if callID == "" {
		return nil
	}

	cc.mu.Lock()
	defer cc.mu.Unlock()

	tl, ok := cc.calls[callID]
	if !ok {
		tl = map[string]interface{}{
			"call_id": callID,
			"events":  []interface{}{},
			"cdr":     nil,
			"start_ts": frame.Ts,
		}
		cc.calls[callID] = tl
	}

	ev := map[string]interface{}{
		"ts":     frame.Ts,
		"domain": frame.Domain,
		"fields": frame.Fields,
	}
	tl["events"] = append(tl["events"].([]interface{}), ev)

	if frame.Domain == "CDR" {
		tl["cdr"] = frame.Fields
		tl["end_ts"] = frame.Ts
		if cc.onComplete != nil {
			cc.onComplete(tl)
		}
		return tl
	}
	return nil
}

// Snapshot returns a copy of all tracked timelines.
func (cc *CallCorrelator) Snapshot() map[string]map[string]interface{} {
	cc.mu.Lock()
	defer cc.mu.Unlock()
	out := make(map[string]map[string]interface{}, len(cc.calls))
	for k, v := range cc.calls {
		out[k] = v
	}
	return out
}

// ---------------------------------------------------------------------------
// WebhookSink: fire-and-forget POST per event
// ---------------------------------------------------------------------------

// WebhookSink forwards every parsed frame as JSON to an external URL.
type WebhookSink struct {
	url       string
	timeout   time.Duration
	retries   int
	mu        sync.Mutex
	ok, fail  int
	hc        *http.Client
}

// NewWebhookSink creates a sink. timeout is per-attempt seconds; retries
// is number of additional attempts after the first fails.
func NewWebhookSink(url string, timeoutSecs, retries int) *WebhookSink {
	return &WebhookSink{
		url:     url,
		timeout: time.Duration(timeoutSecs) * time.Second,
		retries: retries,
		hc:      newHTTPClient(time.Duration(timeoutSecs) * time.Second),
	}
}

// Send spawns a daemon goroutine to POST the frame (non-blocking).
func (w *WebhookSink) Send(frame *Frame) {
	fCopy := *frame
	go w.doPost(&fCopy)
}

func (w *WebhookSink) doPost(f *Frame) {
	body, _ := json.Marshal(f)
	for attempt := 0; attempt <= w.retries; attempt++ {
		req, _ := http.NewRequest("POST", w.url, strings.NewReader(string(body)))
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("X-NSBC-Event", f.Domain)
		resp, err := w.hc.Do(req)
		if err == nil && resp.StatusCode >= 200 && resp.StatusCode < 300 {
			resp.Body.Close()
			w.mu.Lock()
			w.ok++
			w.mu.Unlock()
			return
		}
		if resp != nil {
			resp.Body.Close()
		}
		if attempt < w.retries {
			time.Sleep(time.Duration(attempt+1) * 500 * time.Millisecond)
		}
	}
	w.mu.Lock()
	w.fail++
	w.mu.Unlock()
}

// Stats returns cumulative POST success/fail counters.
func (w *WebhookSink) Stats() (ok, fail int) {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.ok, w.fail
}

// ---------------------------------------------------------------------------
// SSEClient — Server-Sent Events over HTTP (remote deployments)
// ---------------------------------------------------------------------------

// SseEvent is a raw parsed SSE event before Frame mapping.
type SseEvent struct {
	Event string
	Data  string
	Retry int // 0 = not set by server
}

// SseClient consumes nsbc-web /calls/stream over SSE.
type SseClient struct {
	url      string
	timeout  time.Duration
	headers  map[string]string
	hc       *http.Client
	resp     *http.Response
	reader   *bufio.Reader
}

// NewSseClient creates an unconnected SSE client.
func NewSseClient(url string, timeoutSecs int, headers map[string]string) *SseClient {
	if headers == nil {
		headers = map[string]string{}
	}
	return &SseClient{
		url:     url,
		timeout: time.Duration(timeoutSecs) * time.Second,
		headers: headers,
		hc:      newHTTPClient(time.Duration(timeoutSecs) * time.Second),
	}
}

// Connect opens the GET request and prepares the line reader.
func (s *SseClient) Connect() error {
	req, err := http.NewRequest("GET", s.url, nil)
	if err != nil {
		return fmt.Errorf("build SSE request: %w", err)
	}
	req.Header.Set("Accept", "text/event-stream")
	req.Header.Set("Cache-Control", "no-cache")
	for k, v := range s.headers {
		req.Header.Set(k, v)
	}
	resp, err := s.hc.Do(req)
	if err != nil {
		return fmt.Errorf("SSE connect: %w", err)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		resp.Body.Close()
		return fmt.Errorf("SSE connect: HTTP %d", resp.StatusCode)
	}
	s.resp = resp
	s.reader = bufio.NewReaderSize(resp.Body, 65536)
	return nil
}

// Close shuts down the HTTP connection.
func (s *SseClient) Close() {
	if s.resp != nil {
		_ = s.resp.Body.Close()
		s.resp = nil
	}
}

// IterEvents blocks, reads the SSE stream, and pushes parsed SseEvents
// into the channel. Closes ch on disconnect. Auto-reconnect on EOF.
// Start in a goroutine like: go client.IterEvents(eventCh).
func (s *SseClient) IterEvents(ch chan<- SseEvent) {
	defer close(ch)
	for {
		if err := s.readStream(ch); err != nil {
			time.Sleep(3 * time.Second)
		}
		s.Close()
		if err := s.Connect(); err != nil {
			time.Sleep(3 * time.Second)
			continue
		}
	}
}

func (s *SseClient) readStream(ch chan<- SseEvent) error {
	eventName := "message"
	var dataLines []string
	var retryMs int
	retrySet := false

	for {
		line, err := s.reader.ReadString('\n')
		if err != nil {
			return err
		}
		line = strings.TrimRight(line, "\n\r")

		if line == "" {
			if len(dataLines) > 0 || eventName != "message" {
				ev := SseEvent{Event: eventName, Data: strings.Join(dataLines, "\n")}
				if retrySet {
					ev.Retry = retryMs
				}
				select {
				case ch <- ev:
				default: // drop if consumer lagging
				}
			}
			eventName = "message"
			dataLines = nil
			retrySet = false
			retryMs = 0
			continue
		}

		if strings.HasPrefix(line, ":") {
			continue // SSE comment
		}
		if strings.HasPrefix(line, "event:") {
			v := strings.TrimSpace(line[6:])
			if v == "" {
				v = "message"
			}
			eventName = v
		} else if strings.HasPrefix(line, "data:") {
			v := line[5:]
			if strings.HasPrefix(v, " ") {
				v = v[1:]
			}
			dataLines = append(dataLines, v)
		} else if strings.HasPrefix(line, "retry:") {
			if n, err := fmt.Sscanf(strings.TrimSpace(line[6:]), "%d", &retryMs); n == 1 && err == nil {
				retrySet = true
			}
		}
	}
}

// SseEventToFrame converts a raw SseEvent into the canonical *Frame.
// Returns nil if the SSE payload is not valid JSON.
//
// nsbc-web /calls/stream emits SSE event=session where payload.event is
// "ring"/"answer"/"hangup"/"originate" — we map these to the same
// uppercase Domain tokens that UDS frames use.
func SseEventToFrame(ev SseEvent) *Frame {
	var blob map[string]interface{}
	if err := json.Unmarshal([]byte(ev.Data), &blob); err != nil {
		return nil
	}
	kindOuter, _ := blob["kind"].(string)
	payload, _ := blob["payload"].(map[string]interface{})
	if payload == nil {
		// Some deployments emit payload directly at top level
		payload = blob
	}

	SSEToDomain := map[string]string{
		"ring":     "CALL_INCOMING",
		"answer":   "CALL_ANSWERED",
		"hangup":   "CALL_END",
		"originate": "CALL_OUTGOING",
	}
	sseEvt, _ := payload["event"].(string)
	sseEvt = strings.ToLower(sseEvt)
	domain := SSEToDomain[sseEvt]
	if domain == "" {
		if sseEvt != "" {
			domain = strings.ToUpper(sseEvt)
		} else if kindOuter != "" {
			domain = strings.ToUpper(kindOuter)
		} else {
			domain = "EVENT"
		}
	}

	ts := ""
	if v, ok := payload["ts"].(string); ok {
		ts = v
	} else if v, ok := payload["timestamp"].(string); ok {
		ts = v
	}
	if ts == "" {
		ts = time.Now().Format("2006-01-02T15:04:05.000")
	}

	raw, _ := json.Marshal(blob)
	return &Frame{
		Ts:     ts,
		Domain: domain,
		Kind:   classifyDomain(domain),
		Fields: payload,
		Raw:    string(raw),
		Source: "sse",
	}
}
