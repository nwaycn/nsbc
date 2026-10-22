/**
 * NSBC AI 对话引擎 — Go 实现（HTTP 版 · v3）
 *
 * 架构硬约束：所有外部交互必须经 nsbc-web（Rust 边车），Go 引擎不碰 UDS。
 *
 * 编译：go mod init aiengine && go mod tidy && go build -o aiengine .
 * 运行：./aiengine                    (默认连 http://127.0.0.1:8080)
 *     NSBC_WEB_URL=http://10.0.0.1:9000 ./aiengine   (自定义)
 *
 * 交互协议（nsbc-web → Go）：
 *   SSE  GET  /api/v1/vad/stream              → VAD_EVENT 实时事件流
 *   POST /api/v1/calls/:id/mute              → uuid_mute on/off [leg]
 *   POST /api/v1/calls/:id/broadcast         → uuid_broadcast <wav_path>
 *   POST /api/v1/calls/:id/vad-start         → uuid_vad_start <threshold>
 *   POST /api/v1/calls/:id/vad-stop          → uuid_vad_stop
 *   POST /api/v1/originate                   → originate {vars}url
 *   GET  /api/v1/calls                       → show calls（取 call-id）
 *
 * AI barge-in 核心循环：
 *   1. POST /originate → B 腿振铃 → 用户手动接听
 *   2. POST /vad-start 启动 VAD
 *   3. SSE 收 VAD_EVENT state=SPEECH → POST /mute on leg=1 → ASR → LLM → TTS → POST /broadcast → POST /mute off
 *   4. SSE 收 VAD_EVENT state=SILENCE → （清理）
 *
 * ASR / LLM / TTS：空函数占位（接入真实服务时替换）。
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
// 配置
// =====================================================================

type Config struct {
	NSBCWebURL string // nsbc-web 地址（带 /api/v1 前缀）
	Realm      string
	Timeout    time.Duration
}

func loadConfig() Config {
	base := os.Getenv("NSBC_WEB_URL")
	if base == "" {
		base = "http://127.0.0.1:8080"
	}
	// 确保结尾没有 /
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
// VAD_EVENT 事件（精确匹配 C 侧 vad_engine.c snprintf）
// =====================================================================

// nsbc-web SSE payload 里直接是 {"event":"VAD_EVENT","call_key":"0x7f...",...}
type VadEvent struct {
	Event   string `json:"event"`    // "VAD_EVENT"
	CallKey string `json:"call_key"` // "0x7f..."
	Leg     int    `json:"leg"`      // 0=用户侧 1=AI侧
	State   string `json:"state"`    // "SPEECH" | "SILENCE"
	RMS     uint16 `json:"rms"`
	TsNs    int64  `json:"ts_ns"`
}

// =====================================================================
// CallSession — 追踪单个通话
// =====================================================================

type CallSession struct {
	CallKey  string    // C 指针地址（VAD_EVENT 里唯一标识）
	CallID   string    // SIP Call-ID（HTTP 命令需要）
	State    string    // "IDLE"/"CONFIRMED"/"SPEAKING"/"TERMINATED"
	AIMuted  bool      // AI 侧当前是否被静音
	LastSeen time.Time
	mu       sync.Mutex
}

// =====================================================================
// NSBCWebClient — nsbc-web HTTP API 客户端
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

// ------ SSE 事件订阅 ------

// SubscribeVAD 订阅 /vad/stream SSE，返回事件 channel
func (c *NSBCWebClient) SubscribeVAD(ctx context.Context) (<-chan VadEvent, error) {
	resp, err := c.http.Get(c.path("/vad/stream"))
	if err != nil {
		return nil, fmt.Errorf("GET /vad/stream: %w", err)
	}
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("GET /vad/stream: status %d", resp.StatusCode)
	}

	ch := make(chan VadEvent, 64) // 缓冲防止慢消费者丢事件
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

			// SSE 协议：event: xxx / data: xxx / 空行分隔事件
			switch {
			case strings.HasPrefix(line, "event:"):
				// 忽略 event 类型，我们只关心 data
			case strings.HasPrefix(line, "data:"):
				curData.WriteString(strings.TrimPrefix(line, "data:"))
			case line == "":
				// 空行 = 一个事件结束
				if curData.Len() > 0 {
					data := curData.String()
					curData.Reset()
					// nsbc-web 推的是完整 JSON payload（已经剥掉了外层 kind）
					var ev VadEvent
					if err := json.Unmarshal([]byte(data), &ev); err == nil {
						select {
						case ch <- ev:
						default: // channel 满则丢（barge-in 不怕丢中间帧）
						}
					} else {
						// 可能是 {"kind":"lag"} — 跳过
					}
				}
			}
		}
		log.Printf("[sse] VAD stream disconnected: %v", scanner.Err())
	}()
	return ch, nil
}

// ------ POST 命令 ------

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
	// nsbc-web 的 /originate 直接接收 call_url（完整 FS brace vars 格式）
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

// ListCalls — GET /calls 拿 call_key → call_id 映射
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
// AI 引擎主循环
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
	// 1) 订阅 VAD_EVENT SSE
	vadCh, err := e.http.SubscribeVAD(ctx)
	if err != nil {
		log.Fatalf("[engine] 连 /vad/stream 失败: %v", err)
	}
	log.Println("[engine] ✅ 已订阅 /vad/stream SSE")

	// 2) 启动 session 清理
	go e.cleanupLoop(ctx)

	// 3) 演示：3 秒后 originate 测试电话
	go func() {
		time.Sleep(3 * time.Second)
		log.Println("[engine] originate 测试: 1000 → 1001")
		resp, err := e.http.Originate(ctx, "1000", "1001")
		if err != nil {
			log.Printf("[engine] ❌ originate 失败: %v", err)
		} else {
			log.Printf("[engine] ✅ originate → %s", resp)
			log.Println("[engine] ⚠️  请手动接听分机 1001！接听后对着分机 1000 说话测试 barge-in")
			// 提示：接听后需要先 POST /vad-start 才能收到 VAD_EVENT
			// 但我们第一次收到 SPEECH 时自动调 vad-start（lazy init）
		}
	}()

	// 4) 主循环：处理 VAD_EVENT
	log.Println("[engine] 🎯 等待 VAD_EVENT...")
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
					log.Printf("[engine] reconnect 失败: %v", err)
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
	// 只处理 A 腿（用户说话侧）
	if ev.Leg != 0 {
		return
	}

	sess := e.getOrCreateSession(ev.CallKey)
	sess.mu.Lock()
	sess.LastSeen = time.Now()
	sess.mu.Unlock()

	// 第一次见这个 call_key 且是 SPEECH？先自动 vad-start（lazy init）
	if sess.State == "IDLE" {
		callID := e.http.resolveCallID(ctx, ev.CallKey)
		if callID != "" {
			log.Printf("[engine] 🆕 新通话 call_key=%s → call_id=%s → vad-start", ev.CallKey, callID)
			if err := e.http.UUIDVADStart(ctx, callID, 800); err != nil {
				log.Printf("[engine] ❌ vad-start 失败: %v", err)
			} else {
				sess.mu.Lock()
				sess.State = "CONFIRMED"
				sess.mu.Unlock()
			}
		} else {
			log.Printf("[engine] ⚠️  无法解析 call_id，跳过 vad-start")
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
				log.Printf("[barge-in] ❌ mute on 失败: %v", err)
			} else {
				log.Println("[barge-in] ✅ mute on leg=1")
				sess.mu.Lock()
				sess.AIMuted = true
				sess.mu.Unlock()
			}
		}

		// ===== AI 对话循环 =====
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

		// 恢复静音 → 播放 TTS
		sess.mu.Lock()
		sess.AIMuted = false
		sess.mu.Unlock()
		if err := e.http.UUIDMute(ctx, callID, false, 1); err != nil {
			log.Printf("[barge-in] ⚠️  mute off 失败: %v", err)
		}
		if err := e.http.UUIDBroadcast(ctx, callID, ttsFile); err != nil {
			log.Printf("[barge-in] ❌ broadcast 失败: %v", err)
		} else {
			log.Printf("[barge-in] 🔊 broadcast %s", ttsFile)
		}

	case "SILENCE":
		log.Printf("[vad] SILENCE call_key=%s rms=%d", ev.CallKey, ev.RMS)
		sess.mu.Lock()
		if sess.AIMuted {
			sess.mu.Unlock()
			if err := e.http.UUIDMute(ctx, callID, false, 1); err != nil {
				log.Printf("[vad] ⚠️  mute off 失败: %v", err)
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
					log.Printf("[session] 🗑️  清理超时 call_key=%s", key)
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
// ASR / LLM / TTS — 空函数占位
// =====================================================================

func asrRecognize(ctx context.Context, ev *VadEvent) string {
	// TODO: 接真实 ASR（hep_sink UDP 推音频 → ASR Server → HTTP/WS 返回）
	_ = ctx
	_ = ev
	return "你好，我想查询一下账户余额"
}

func llmGenerate(ctx context.Context, userText string) string {
	// TODO: 接 LLM（OpenAI/DeepSeek/Ollama 等）
	_ = ctx
	return "好的，请稍等，正在为您查询账户余额。"
}

func ttsSynthesize(ctx context.Context, text string) string {
	// TODO: 接 TTS（CosyVoice/edge-tts 等），返回 WAV 路径
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
