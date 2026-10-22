/**
 * NSBC AI dialogue engine — Go implementation (HTTP · v3)
 *
 * Architectural hard constraint: all external interaction must go through nsbc-web
 * (the Rust sidecar); the Go engine never touches UDS directly.
 *
 * Build: go mod init aiengine && go mod tidy && go build -o aiengine .
 * Run:  ./aiengine                    (default connects to http://127.0.0.1:8080)
 *       NSBC_WEB_URL=http://10.0.0.1:9000 ./aiengine   (custom)
 *
 * Interaction protocol (nsbc-web → Go):
 *   SSE  GET  /api/v1/vad/stream              → VAD_EVENT realtime stream
 *   POST /api/v1/calls/:id/mute              → uuid_mute on/off [leg]
 *   POST /api/v1/calls/:id/broadcast         → uuid_broadcast <wav_path>
 *   POST /api/v1/calls/:id/vad-start         → uuid_vad_start <threshold>
 *   POST /api/v1/calls/:id/vad-stop          → uuid_vad_stop
 *   POST /api/v1/originate                   → originate {vars}url
 *   GET  /api/v1/calls                       → show calls (pick up call-id)
 *
 * AI barge-in core loop:
 *   1. POST /originate → B leg rings → user manually answers
 *   2. POST /vad-start to start VAD
 *   3. SSE receives VAD_EVENT state=SPEECH → POST /mute on leg=1 → ASR → LLM → TTS → POST /broadcast → POST /mute off
 *   4. SSE receives VAD_EVENT state=SILENCE → (cleanup)
 *
 * ASR / LLM / TTS: stub placeholders — replace when wiring in real services.
 */

package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"regexp"
	"strings"
	"sync"
	"syscall"
	"time"
)

// =====================================================================
// Config
// =====================================================================

type Config struct {
	NSBCWebURL string // nsbc-web base URL (with /api/v1 prefix)
	Realm      string
	Timeout    time.Duration
}

func loadConfig() Config {
	base := os.Getenv("NSBC_WEB_URL")
	if base == "" {
		base = "http://127.0.0.1:8080"
	}
	// Ensure no trailing slash
	base = strings.TrimRight(base, "/")
	return Config{
		NSBCWebURL: base + "/api/v1",
		Realm:      getenv("NSBC_REALM", "nsbc.local"),
		Timeout:    10 * time.Second,
	}
}

func getenv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

// =====================================================================
// VAD_EVENT event (matches C-side vad_engine.c snprintf output)
// =====================================================================

// nsbc-web SSE payload is directly {"event":"VAD_EVENT","call_key":"0x7f...",...}
type VadEvent struct {
	Event   string `json:"event"`    // "VAD_EVENT"
	CallKey string `json:"call_key"` // "0x7f..."
	Leg     int    `json:"leg"`      // 0=user side  1=AI side
	State   string `json:"state"`    // "SPEECH" | "SILENCE"
	RMS     uint16 `json:"rms"`
	TsNs    int64  `json:"ts_ns"`
}

// =====================================================================
// CallSession — tracks a single call
// =====================================================================

type CallSession struct {
	CallKey  string    // C pointer address (unique identifier inside VAD_EVENT)
	CallID   string    // SIP Call-ID (HTTP commands require this)
	State    string    // "IDLE"/"CONFIRMED"/"SPEAKING"/"TERMINATED"
	AIMuted  bool      // whether the AI side is currently muted
	LastSeen time.Time
	mu       sync.Mutex
}

// =====================================================================
// NSBCWebClient — nsbc-web HTTP API client
// =====================================================================

type NSBCWebClient struct {
	cfg    Config
	http   *http.Client
	callIDCache sync.Map // call_key → call_id
}

func NewNSBCWebClient(cfg Config) *NSBCWebClient {
	return &NSBCWebClient{
		cfg:  cfg,
		http: &http.Client{Timeout: cfg.Timeout},
	}
}

func (c *NSBCWebClient) path(parts ...string) string {
	return c.cfg.NSBCWebURL + strings.Join(parts, "")
}

// ------ SSE subscribe ------

// SubscribeVAD subscribes to /vad/stream SSE, returns event channel
func (c *NSBCWebClient) SubscribeVAD(ctx context.Context) (<-chan VadEvent, error) {
	resp, err := c.http.Get(c.path("/vad/stream"))
	if err != nil {
		return nil, fmt.Errorf("GET /vad/stream: %w", err)
	}
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("GET /vad/stream: status %d", resp.StatusCode)
	}

	ch := make(chan VadEvent, 64) // buffered to avoid dropping on slow consumer
	go func() {
		defer close(ch)
		defer resp.Body.Close()

		scanner := bufio.NewScanner(resp.Body)
		scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)

		var curData strings.Builder

		for scanner.Scan() {
			select {
			case <-ctx.Done():
				return
			default:
			}
			line := scanner.Text()

			// SSE protocol: event: xxx / data: xxx / blank line separates events
			switch {
			case strings.HasPrefix(line, "event:"):
				// ignore event type — we only care about data
			case strings.HasPrefix(line, "data:"):
				curData.WriteString(strings.TrimPrefix(line, "data:"))
			case line == "":
				// blank line = one event is complete
				if curData.Len() > 0 {
					data := curData.String()
					curData.Reset()
					// nsbc-web pushes the full JSON payload (outer kind wrapper already stripped)
					var ev VadEvent
					if err := json.Unmarshal([]byte(data), &ev); err == nil {
						select {
						case ch <- ev:
						default: // channel full → drop (barge-in tolerates missing intermediate frames)
						}
					} else {
						// could be {"kind":"lag"} — skip
					}
				}
			}
		}
		log.Printf("[sse] VAD stream disconnected: %v", scanner.Err())
	}()
	return ch, nil
}

// ------ POST commands ------

func (c *NSBCWebClient) postJSON(ctx context.Context, urlPath string, body any) (*http.Response, error) {
	var reader io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return nil, err
		}
		reader = bytes.NewReader(b)
	}
	req, err := http.NewRequestWithContext(ctx, "POST", c.path(urlPath), reader)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	return c.http.Do(req)
}

// Originate — POST /originate
func (c *NSBCWebClient) Originate(ctx context.Context, caller, callee string) (string, error) {
	// nsbc-web's /originate takes the call_url directly (complete FS brace-vars format)
	callURL := fmt.Sprintf("{realm=%s,origination_caller_id_number=%s,originate_timeout=20}user/%s",
		c.cfg.Realm, caller, callee)
	resp, err := c.postJSON(ctx, "/originate", map[string]string{"call_url": callURL})
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode >= 300 {
		return "", fmt.Errorf("originate status %d: %s", resp.StatusCode, string(body))
	}
	return string(body), nil
}

// UUIDMute — POST /calls/{id}/mute  body: {on, leg}
func (c *NSBCWebClient) UUIDMute(ctx context.Context, callID string, on bool, leg int) error {
	resp, err := c.postJSON(ctx, "/calls/"+url.PathEscape(callID)+"/mute",
		map[string]any{"on": on, "leg": leg})
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode >= 300 {
		return fmt.Errorf("mute status %d: %s", resp.StatusCode, string(body))
	}
	return nil
}

// UUIDBroadcast — POST /calls/{id}/broadcast
func (c *NSBCWebClient) UUIDBroadcast(ctx context.Context, callID, wavPath string) error {
	resp, err := c.postJSON(ctx, "/calls/"+url.PathEscape(callID)+"/broadcast",
		map[string]string{"wav_path": wavPath})
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode >= 300 {
		return fmt.Errorf("broadcast status %d: %s", resp.StatusCode, string(body))
	}
	return nil
}

// UUIDVADStart — POST /calls/{id}/vad-start
func (c *NSBCWebClient) UUIDVADStart(ctx context.Context, callID string, threshold int) error {
	resp, err := c.postJSON(ctx, "/calls/"+url.PathEscape(callID)+"/vad-start",
		map[string]int{"threshold": threshold})
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode >= 300 {
		return fmt.Errorf("vad-start status %d: %s", resp.StatusCode, string(body))
	}
	return nil
}

// UUIDVADStop — POST /calls/{id}/vad-stop
func (c *NSBCWebClient) UUIDVADStop(ctx context.Context, callID string) error {
	resp, err := c.postJSON(ctx, "/calls/"+url.PathEscape(callID)+"/vad-stop", nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode >= 300 {
		return fmt.Errorf("vad-stop status %d: %s", resp.StatusCode, string(body))
	}
	return nil
}

// ListCalls — GET /calls to pull call_key → call_id mapping
var callIDRegex = regexp.MustCompile(`([A-Za-z0-9]+(?:-[A-Za-z0-9]+)*@[A-Za-z0-9.:-]+)`)

func (c *NSBCWebClient) resolveCallID(ctx context.Context, callKey string) string {
	if v, ok := c.callIDCache.Load(callKey); ok {
		return v.(string)
	}

	resp, err := c.http.Get(c.path("/calls"))
	if err != nil {
		return ""
	}
	defer resp.Body.Close()

	var calls []struct {
		CallID  string `json:"call_id"`
		Extra   string `json:"extra,omitempty"`
		Channel string `json:"channel,omitempty"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&calls); err != nil {
		return ""
	}
	for _, cc := range calls {
		if cc.CallID != "" {
			c.callIDCache.Store(callKey, cc.CallID)
			return cc.CallID
		}
	}
	return ""
}

// =====================================================================
// AI engine main loop
// =====================================================================

type AIEngine struct {
	cfg   Config
	http  *NSBCWebClient
	sess  sync.Map // call_key → *CallSession
}

func NewAIEngine(cfg Config) *AIEngine {
	return &AIEngine{cfg: cfg, http: NewNSBCWebClient(cfg)}
}

func (e *AIEngine) Run(ctx context.Context) {
	// 1) Subscribe to VAD_EVENT SSE
	vadCh, err := e.http.SubscribeVAD(ctx)
	if err != nil {
		log.Fatalf("[engine] subscribe /vad/stream failed: %v", err)
	}
	log.Println("[engine] ✅ subscribed to /vad/stream SSE")

	// 2) Start session cleanup
	go e.cleanupLoop(ctx)

	// 3) Demo: originate a test call 3s after startup
	go func() {
		time.Sleep(3 * time.Second)
		log.Println("[engine] originate demo: 1000 → 1001")
		resp, err := e.http.Originate(ctx, "1000", "1001")
		if err != nil {
			log.Printf("[engine] ❌ originate failed: %v", err)
		} else {
			log.Printf("[engine] ✅ originate → %s", resp)
			log.Println("[engine] ⚠️  Please manually answer extension 1001! After answering, speak into 1000 to test barge-in")
			// Tip: post-answer we need to POST /vad-start before we receive VAD_EVENT.
			// We auto-call vad-start lazily on the first SPEECH.
		}
	}()

	// 4) Main loop: process VAD_EVENT
	log.Println("[engine] 🎯 waiting for VAD_EVENT...")
	for {
		select {
		case <-ctx.Done():
			log.Println("[engine] shutdown")
			return
		case ev, ok := <-vadCh:
			if !ok {
				log.Println("[engine] SSE channel closed, reconnecting...")
				ch, err := e.http.SubscribeVAD(ctx)
				if err != nil {
					log.Printf("[engine] reconnect failed: %v", err)
					time.Sleep(2 * time.Second)
					continue
				}
				vadCh = ch
				continue
			}
			e.handleVAD(ctx, &ev)
		}
	}
}

func (e *AIEngine) handleVAD(ctx context.Context, ev *VadEvent) {
	// Only handle leg A (user-speaking side)
	if ev.Leg != 0 {
		return
	}

	sess := e.getOrCreateSession(ev.CallKey)
	sess.mu.Lock()
	sess.LastSeen = time.Now()
	sess.mu.Unlock()

	// First time seeing this call_key and it's SPEECH? auto vad-start lazily
	if sess.State == "IDLE" {
		callID := e.http.resolveCallID(ctx, ev.CallKey)
		if callID != "" {
			log.Printf("[engine] 🆕 new call call_key=%s → call_id=%s → vad-start", ev.CallKey, callID)
			if err := e.http.UUIDVADStart(ctx, callID, 800); err != nil {
				log.Printf("[engine] ❌ vad-start failed: %v", err)
			} else {
				sess.mu.Lock()
				sess.State = "CONFIRMED"
				sess.mu.Unlock()
			}
		} else {
			log.Printf("[engine] ⚠️  cannot resolve call_id, skipping vad-start")
		}
	}

	callID := e.http.resolveCallID(ctx, ev.CallKey)
	if callID == "" {
		return
	}

	switch ev.State {
	case "SPEECH":
		log.Printf("[barge-in] 🎯 SPEECH call_key=%s rms=%d → mute AI (leg=1)", ev.CallKey, ev.RMS)

		sess.mu.Lock()
		needMute := !sess.AIMuted
		sess.mu.Unlock()

		if needMute {
			if err := e.http.UUIDMute(ctx, callID, true, 1); err != nil {
				log.Printf("[barge-in] ❌ mute on failed: %v", err)
			} else {
				log.Println("[barge-in] ✅ mute on leg=1")
				sess.mu.Lock()
				sess.AIMuted = true
				sess.mu.Unlock()
			}
		}

		// ===== AI dialogue loop =====
		text := asrRecognize(ctx, ev)
		if text == "" {
			return
		}
		reply := llmGenerate(ctx, text)
		log.Printf("[barge-in] 💬 ASR='%s' → LLM='%s'", text, reply)

		ttsFile := ttsSynthesize(ctx, reply)
		if ttsFile == "" {
			return
		}

		// Unmute → play TTS
		sess.mu.Lock()
		sess.AIMuted = false
		sess.mu.Unlock()
		if err := e.http.UUIDMute(ctx, callID, false, 1); err != nil {
			log.Printf("[barge-in] ⚠️  mute off failed: %v", err)
		}
		if err := e.http.UUIDBroadcast(ctx, callID, ttsFile); err != nil {
			log.Printf("[barge-in] ❌ broadcast failed: %v", err)
		} else {
			log.Printf("[barge-in] 🔊 broadcast %s", ttsFile)
		}

	case "SILENCE":
		log.Printf("[vad] SILENCE call_key=%s rms=%d", ev.CallKey, ev.RMS)
		sess.mu.Lock()
		if sess.AIMuted {
			sess.mu.Unlock()
			if err := e.http.UUIDMute(ctx, callID, false, 1); err != nil {
				log.Printf("[vad] ⚠️  mute off failed: %v", err)
			} else {
				log.Println("[vad] ✅ mute off leg=1")
			}
			sess.mu.Lock()
			sess.AIMuted = false
			sess.mu.Unlock()
		}
	}
}

func (e *AIEngine) getOrCreateSession(callKey string) *CallSession {
	if v, ok := e.sess.Load(callKey); ok {
		return v.(*CallSession)
	}
	s := &CallSession{CallKey: callKey, State: "IDLE", LastSeen: time.Now()}
	actual, _ := e.sess.LoadOrStore(callKey, s)
	return actual.(*CallSession)
}

func (e *AIEngine) cleanupLoop(ctx context.Context) {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			now := time.Now()
			e.sess.Range(func(key, val any) bool {
				s := val.(*CallSession)
				s.mu.Lock()
				if now.Sub(s.LastSeen) > 60*time.Second {
					s.mu.Unlock()
					log.Printf("[session] 🗑️  cleanup stale call_key=%s", key)
					e.sess.Delete(key)
				} else {
					s.mu.Unlock()
				}
				return true
			})
		}
	}
}

// =====================================================================
// ASR / LLM / TTS — stub placeholders
// =====================================================================

func asrRecognize(ctx context.Context, ev *VadEvent) string {
	// TODO: wire up real ASR (hep_sink UDP pushes audio → ASR server → HTTP/WS returns)
	_ = ctx
	_ = ev
	return "Hello, I'd like to check my account balance"
}

func llmGenerate(ctx context.Context, userText string) string {
	// TODO: wire up LLM (OpenAI/DeepSeek/Ollama, etc.)
	_ = ctx
	return "Alright, please hold — I'm looking up your account balance."
}

func ttsSynthesize(ctx context.Context, text string) string {
	// TODO: wire up TTS (CosyVoice/edge-tts, etc.), return WAV path
	_ = ctx
	_ = text
	return "/tmp/tts-response.wav"
}

// =====================================================================
// main
// =====================================================================

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	cfg := loadConfig()
	log.Printf("[main] nsbc-web → %s", cfg.NSBCWebURL)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		sig := <-sigCh
		log.Printf("[main] signal %v, shutdown", sig)
		cancel()
	}()

	engine := NewAIEngine(cfg)
	engine.Run(ctx)
}
