/* main.go — Go External AI Agent（daemon 模式）
 *
 * 纯 stdlib，零第三方依赖。套 PROTOCOL.md §4 + §4.4
 *
 * 架构：
 *   daemon 常驻 → SSE 订阅 /api/v1/calls/stream → 收到 CALL_ANSWERED
 *   → go handleCall(callID) 每通一个 goroutine → producer=1 → greeting
 *   → 主循环（ASR→LLM→TTS）→ CALL_END 事件退出
 *
 * Provider：
 *   --asr mock|whisper|nway  --llm mock|ollama|openai_chat  --tts mock|elevenlabs|nway
 *
 * 用法（daemon 模式，一直监听）：
 *   go run main.go --api-base https://nsbc-web.local:443 --api-token <ext_key> \
 *       --asr nway --tts nway --llm mock
 *
 * 用法（单通模式，手动指定 call-id）：
 *   go run main.go --api-base https://nsbc-web.local:443 --api-token <ext_key> \
 *       --call-id <call_id> --asr nway --tts nway
 */
package main

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"crypto/sha1"
	"crypto/tls"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"event_client/nsbc_sdk"
)

// =============================================================================
// HTTP cmd + AI pass-through API（和 Python client.py 对称）
// =============================================================================

var stderr = os.Stderr

// welcomeText 是静态欢迎语；cachedGreeting 在 daemon 启动时预合成一次，
// 避免每通来电都阻塞等 ONNX TTS（比实时慢时，短通话在 BYE 前都拿不到欢迎语）。
const welcomeText = "你好，我是 sbc 智能客服，请问有什么能为您服务！"

var cachedGreeting []byte

func sslTransport() *http.Transport {
	return &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
}

func cmd(apiBase, apiToken, command string, timeout time.Duration) (int, string) {
	body, _ := json.Marshal(map[string]string{"cmd": command})
	req, _ := http.NewRequest("POST", strings.TrimRight(apiBase, "/")+"/ext/v1/cmd",
		bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "text/plain")
	req.Header.Set("Authorization", "Bearer "+apiToken)
	hc := &http.Client{Transport: sslTransport(), Timeout: timeout}
	resp, err := hc.Do(req)
	if err != nil {
		return -1, err.Error()
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		return resp.StatusCode, string(data)
	}
	return 0, string(data)
}

func checkOK(rc int, out string) bool {
	return rc == 0 && strings.Contains(out, "OK") && !strings.Contains(out, "ERR")
}

const maxCmdLine = 4096

func aiSetProducer(apiBase, apiToken, callID string, producer int) bool {
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-set-producer %s %d", callID, producer), 10*time.Second)
	fmt.Printf("[ai] set_producer call=%s producer=%d → %s\n", callID, producer, truncate(out, 150))
	if checkOK(rc, out) {
		return true
	}
	// cleanup 阶段 session 可能已被 C 侧 call_destroy 清除，
	// producer=0 的语义是"标记 AI 不再接管"，session 不存在就等于"没在接管"，视为成功。
	if producer == 0 && strings.Contains(out, "notfound") {
		fmt.Printf("[ai] set_producer producer=0 notfound (session 已清) → 视为 OK\n")
		return true
	}
	return false
}

type vadResp struct {
	aState, bState string
	aRms, bRms     int
}

func aiVadState(apiBase, apiToken, callID string) vadResp {
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-vad-state %s", callID), 5*time.Second)
	if rc != 0 {
		return vadResp{aState: "SILENCE", bState: "SILENCE"}
	}
	r := vadResp{aState: "SILENCE", bState: "SILENCE"}
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "OK A_leg=") {
			parts := strings.Fields(strings.TrimPrefix(line, "OK A_leg="))
			if len(parts) > 0 {
				r.aState = parts[0]
			}
			if len(parts) > 1 {
				fmt.Sscanf(parts[1], "rms=%d", &r.aRms)
			}
		} else if strings.HasPrefix(line, "OK B_leg=") {
			parts := strings.Fields(strings.TrimPrefix(line, "OK B_leg="))
			if len(parts) > 0 {
				r.bState = parts[0]
			}
			if len(parts) > 1 {
				fmt.Sscanf(parts[1], "rms=%d", &r.bRms)
			}
		}
	}
	return r
}

// sendAudioPaced 把 PCM16_LE 音频按实时节奏分片推给内核（format=1）。
//
// C 侧 bridge_send_raw 单帧上限 2048B（扣 12B RTP 头），整段直塞会被
// -EMSGSIZE 拒绝，ai_send_tx_audio 再归为 NSBC_NOENT（rc=-6）。
// 每片 640B（40ms PCM16_LE @8kHz：320 samples × 16bit）以降低 HTTPS+UDS 单次往返开销；
// C 侧 ai_send_tx_audio(format=1) 会直接用 G.711 encoder.encode() 一次编码到对端 codec。
func sendAudioPaced(apiBase, apiToken, callID string, pcm16 []byte,
	ws *wsTransport, cs *callSession, bargeIn bool, bargeRMS int) bool {
	const frameBytes = 640 // 40ms PCM16_LE @8kHz (320 samples × 2 bytes)
	const frameMs = 40
	start := time.Now()
	sent := 0
	for off := 0; off < len(pcm16); off += frameBytes {
		if cs != nil {
			select {
			case <-cs.stopped:
				return false
			case <-cs.done:
				return false
			default:
			}
		}
		if bargeIn && sent > 0 && sent%5 == 0 {
			if vad := aiVadState(apiBase, apiToken, callID); vad.aState == "SPEECH" && vad.aRms >= bargeRMS {
				fmt.Printf("[ai_agent] ★ barge-in! 用户说话 → 停 TTS (rms=%d)\n", vad.aRms)
				return false
			}
		}
		end := off + frameBytes
		if end > len(pcm16) {
			end = len(pcm16)
		}
		var ok bool
		if ws != nil {
			ok = ws.sendTx(pcm16[off:end])
		} else {
			ok = aiSendTx(apiBase, apiToken, callID, pcm16[off:end], true) // format=1 PCM16
		}
		if !ok {
			return false
		}
		sent++
		due := start.Add(time.Duration(sent*frameMs) * time.Millisecond)
		if d := time.Until(due); d > 0 {
			time.Sleep(d)
		}
	}
	return true
}

func aiRequestTransfer(apiBase, apiToken, callID, dest string) bool {
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-request-transfer %s %s", callID, dest), 10*time.Second)
	fmt.Printf("[ai] ★ request_transfer call=%s dest=%s → %s\n", callID, dest, truncate(out, 120))
	return checkOK(rc, out)
}

func aiHangup(apiBase, apiToken, callID string) bool {
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("uuid-kill %s", callID), 10*time.Second)
	fmt.Printf("[ai] ★ hangup call=%s → %s\n", callID, truncate(out, 120))
	return checkOK(rc, out)
}

func aiGetRx(apiBase, apiToken, callID string, maxBytes int) []byte {
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-get-rx %s %d", callID, maxBytes), 5*time.Second)
	if rc != 0 {
		return nil
	}
	for _, line := range strings.Split(out, "\n") {
		if strings.HasPrefix(line, "b64:") {
			b, err := base64.StdEncoding.DecodeString(strings.TrimSpace(strings.TrimPrefix(line, "b64:")))
			if err != nil {
				return nil
			}
			return b
		}
	}
	return nil
}

func aiSendTx(apiBase, apiToken, callID string, pcmBytes []byte, formatPCM16 bool) bool {
	if len(pcmBytes) == 0 {
		return true
	}
	fmtNum := 0
	if formatPCM16 {
		fmtNum = 1
	}
	chunkCap := (maxCmdLine - 80) * 3 / 4
	for i := 0; i < len(pcmBytes); i += chunkCap {
		end := i + chunkCap
		if end > len(pcmBytes) {
			end = len(pcmBytes)
		}
		b64 := base64.StdEncoding.EncodeToString(pcmBytes[i:end])
		rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-send-tx %s %d %s", callID, fmtNum, b64), 10*time.Second)
		if !checkOK(rc, out) {
			fmt.Fprintf(stderr, "[ai] send_tx FAILED chunk=%d rc=%d out=%s\n", i, rc, truncate(out, 100))
			return false
		}
	}
	return true
}

// =============================================================================
// WebSocket transport — RFC 6455 子集（纯 stdlib，零依赖）
// =============================================================================

const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

type wsOpcode byte

const (
	wsOpBinary wsOpcode = 0x2
	wsOpClose  wsOpcode = 0x8
	wsOpPing   wsOpcode = 0x9
	wsOpPong   wsOpcode = 0xA
)

type wsFrame struct {
	opcode  wsOpcode
	payload []byte
}

type wsTransport struct {
	conn   net.Conn
	rw     *bufio.ReadWriter
	mu     sync.Mutex
	closed bool

	lastPingSent       time.Time
	lastClientActivity time.Time
	pendingPing        bool

	apiBase, apiToken, callID string
}

func newWsTransport(apiBase, apiToken, callID string) (*wsTransport, error) {
	base := strings.TrimRight(apiBase, "/")
	scheme := "ws"
	if strings.HasPrefix(base, "https://") {
		scheme = "wss"
		base = base[len("https://"):]
	} else if strings.HasPrefix(base, "http://") {
		base = base[len("http://"):]
	}
	wsURL := fmt.Sprintf("%s://%s/ws/ai-media/%s?token=%s", scheme, base, callID, url.QueryEscape(apiToken))

	hostPort := base
	if idx := strings.Index(base, "/"); idx >= 0 {
		hostPort = base[:idx]
	}
	if _, _, err := net.SplitHostPort(hostPort); err != nil {
		defaultPort := "80"
		if scheme == "wss" {
			defaultPort = "443"
		}
		hostPort = net.JoinHostPort(hostPort, defaultPort)
	}
	var conn net.Conn
	var err error
	if scheme == "wss" {
		conn, err = tls.Dial("tcp", hostPort, &tls.Config{InsecureSkipVerify: true})
	} else {
		conn, err = net.DialTimeout("tcp", hostPort, 10*time.Second)
	}
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", wsURL, err)
	}

	keyBytes := make([]byte, 16)
	_, _ = rand.Read(keyBytes)
	key := base64.StdEncoding.EncodeToString(keyBytes)

	req, _ := http.NewRequest("GET", wsURL, nil)
	req.Header.Set("Upgrade", "websocket")
	req.Header.Set("Connection", "Upgrade")
	req.Header.Set("Sec-WebSocket-Key", key)
	req.Header.Set("Sec-WebSocket-Version", "13")

	rawReq := req.Method + " " + wsURL[len(scheme)+3:] + " HTTP/1.1\r\n"
	for k, v := range req.Header {
		rawReq += k + ": " + v[0] + "\r\n"
	}
	rawReq += "\r\n"

	if _, err = conn.Write([]byte(rawReq)); err != nil {
		conn.Close()
		return nil, fmt.Errorf("upgrade write: %w", err)
	}

	conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	resp, err := http.ReadResponse(bufio.NewReader(conn), req)
	conn.SetReadDeadline(time.Time{})
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("upgrade response: %w", err)
	}
	if resp.StatusCode != 101 {
		conn.Close()
		return nil, fmt.Errorf("upgrade failed: %s", resp.Status)
	}

	expectedAccept := computeAccept(key)
	actualAccept := resp.Header.Get("Sec-WebSocket-Accept")
	if actualAccept != expectedAccept {
		conn.Close()
		return nil, fmt.Errorf("accept mismatch: got %q want %q", actualAccept, expectedAccept)
	}

	fmt.Printf("[WsTransport] 已连接 → handler 自动 producer=1 + VAD on\n")

	now := time.Now()
	return &wsTransport{
		conn:               conn,
		rw:                 bufio.NewReadWriter(bufio.NewReader(conn), bufio.NewWriter(conn)),
		lastPingSent:       now.Add(-30 * time.Second),
		lastClientActivity: now,
	}, nil
}

func computeAccept(key string) string {
	h := sha1.New()
	h.Write([]byte(key + wsGUID))
	return base64.StdEncoding.EncodeToString(h.Sum(nil))
}

func (t *wsTransport) wsRead() (wsFrame, error) {
	t.conn.SetReadDeadline(time.Now().Add(50 * time.Millisecond))

	var hdr [2]byte
	if _, err := io.ReadFull(t.rw.Reader, hdr[:]); err != nil {
		return wsFrame{}, err
	}

	var opcode wsOpcode = wsOpcode(hdr[0] & 0x0F)
	masked := (hdr[1] & 0x80) != 0
	length := int(hdr[1] & 0x7F)

	if length == 126 {
		var lb [2]byte
		if _, err := io.ReadFull(t.rw.Reader, lb[:]); err != nil {
			return wsFrame{}, err
		}
		length = int(lb[0])<<8 | int(lb[1])
	} else if length == 127 {
		var lb [8]byte
		if _, err := io.ReadFull(t.rw.Reader, lb[:]); err != nil {
			return wsFrame{}, err
		}
		length = int(lb[4])<<24 | int(lb[5])<<16 | int(lb[6])<<8 | int(lb[7])
	}
	if length > 256*1024 {
		return wsFrame{}, fmt.Errorf("frame too large: %d", length)
	}

	var mask [4]byte
	if masked {
		if _, err := io.ReadFull(t.rw.Reader, mask[:]); err != nil {
			return wsFrame{}, err
		}
	}

	payload := make([]byte, length)
	if length > 0 {
		if _, err := io.ReadFull(t.rw.Reader, payload); err != nil {
			return wsFrame{}, err
		}
		if masked {
			for i := range payload {
				payload[i] ^= mask[i%4]
			}
		}
	}

	return wsFrame{opcode: opcode, payload: payload}, nil
}

func (t *wsTransport) wsWrite(opcode wsOpcode, payload []byte) error {
	t.mu.Lock()
	defer t.mu.Unlock()

	var buf []byte
	buf = append(buf, byte(0x80|byte(opcode)))

	n := len(payload)
	switch {
	case n < 126:
		buf = append(buf, byte(n))
	case n < 65536:
		buf = append(buf, 126, byte(n>>8), byte(n))
	default:
		buf = append(buf, 127, 0, 0, 0, 0, byte(n>>24), byte(n>>16), byte(n>>8), byte(n))
	}

	if _, err := t.conn.Write(buf); err != nil {
		return err
	}
	if n > 0 {
		if _, err := t.conn.Write(payload); err != nil {
			return err
		}
	}
	return t.rw.Flush()
}

func (t *wsTransport) sendTx(pcmu []byte) bool {
	if len(pcmu) == 0 {
		return true
	}
	return t.wsWrite(wsOpBinary, pcmu) == nil
}

func (t *wsTransport) close() {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return
	}
	t.closed = true
	t.mu.Unlock()

	_ = t.wsWrite(wsOpClose, nil)
	t.conn.Close()
}

// =============================================================================
// Provider 工厂
// =============================================================================

func buildASR(name string, apiKey string, nwayAddr string) ASRProvider {
	switch name {
	case "whisper":
		return &WhisperASR{APIKey: apiKey, Model: "whisper-1", TimeoutS: 30}
	case "nway":
		return &NWAYASR{Addr: nwayAddr}
	}
	return &MockASR{TriggerWord: "我想转人工", EnergyThreshold: 24}
}

func buildLLM(name, transferDest string, apiKey, endpoint, model string, transferKeywords []string) LLMProvider {
	switch name {
	case "ollama", "openai_chat", "deepseek":
		return &OllamaLLM{Model: model, Endpoint: endpoint, APIKey: apiKey,
			TransferDest: transferDest, TransferKeywords: transferKeywords, TimeoutS: 30}
	}
	return &MockLLM{TransferDest: transferDest,
		TransferKeywords: transferKeywords,
		HangupKeywords:    []string{"挂断", "再见", "拜拜", "hangup", "bye"}}
}

func buildTTS(name, apiKey, voiceID, nwayAddr string) TTSProvider {
	switch name {
	case "elevenlabs":
		return &ElevenLabsTTS{APIKey: apiKey, VoiceID: voiceID, ModelID: "eleven_monolingual_v1", TimeoutS: 15}
	case "nway":
		return &NWAYTTS{Addr: nwayAddr}
	}
	return &MockTTS{DurationMs: 2000}
}

// =============================================================================
// 每通电话的运行状态
// =============================================================================

type callSession struct {
	callID    string
	orig, to  string // 主被叫
	done      chan struct{} // 关闭时表示该通话已结束
	stopped   chan struct{} // 收到 hangup 事件时关闭
	stopOnce  sync.Once
}

func newCallSession(callID, orig, to string) *callSession {
	return &callSession{
		callID:   callID,
		orig:     orig,
		to:       to,
		done:     make(chan struct{}),
		stopped:  make(chan struct{}),
	}
}

func (s *callSession) markDone() {
	close(s.done)
}

func (s *callSession) markStopped() {
	s.stopOnce.Do(func() { close(s.stopped) })
}

// callTracker — 跟踪所有活跃的 per-call goroutine
type callTracker struct {
	mu     sync.Mutex
	calls  map[string]*callSession
}

func newCallTracker() *callTracker {
	return &callTracker{calls: make(map[string]*callSession)}
}

func (t *callTracker) add(cs *callSession) bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	if _, exists := t.calls[cs.callID]; exists {
		return false // 已在处理
	}
	t.calls[cs.callID] = cs
	return true
}

func (t *callTracker) remove(callID string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	delete(t.calls, callID)
}

// stopCall — CALL_END 事件到达，通知对应 goroutine 退出
func (t *callTracker) stopCall(callID string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if cs, ok := t.calls[callID]; ok {
		cs.markStopped()
	}
}

func (t *callTracker) activeCount() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return len(t.calls)
}

// =============================================================================
// handleCall — 每通电话的 AI 处理 goroutine（daemon 模式入口）
// =============================================================================

func handleCall(
	cs *callSession,
	apiBase, apiToken string,
	transferDest string,
	chunkBytes int,
	bargeInEnabled bool,
	bargeInRMS int,
	transport string,
	asr ASRProvider, llm LLMProvider, tts TTSProvider,
) {
	callID := cs.callID
	fmt.Printf("\n%s\n", strings.Repeat("═", 70))
	fmt.Printf("[ai_agent] ★ 接管通话 call=%s  %s → %s\n", callID, cs.orig, cs.to)
	fmt.Printf("  transport=%s  barge_in=%v  chunk=%d\n", transport, bargeInEnabled, chunkBytes)
	fmt.Printf("  ASR=%T  LLM=%T  TTS=%T\n", asr, llm, tts)
	fmt.Printf("%s\n\n", strings.Repeat("═", 70))

	// --- transport ---
	var httpMode bool
	var ws *wsTransport
	var wsRxCh chan []byte
	var wsErrCh chan error

	if transport == "ws" || transport == "auto" {
		w, err := newWsTransport(apiBase, apiToken, callID)
		if err == nil {
			ws = w
			httpMode = false
			wsRxCh = make(chan []byte, 128)
			wsErrCh = make(chan error, 1)
			go ws.runReadLoop(wsRxCh, wsErrCh)
		} else {
			if transport == "ws" {
				fmt.Fprintf(stderr, "[ai_agent] 指定 --transport ws 但连接失败: %v\n", err)
				ws = nil
				httpMode = true
			} else {
				fmt.Fprintf(stderr, "[ai_agent] WS 连接失败 (%v)，降级 HTTP polling\n", err)
				httpMode = true
			}
		}
	} else {
		httpMode = true
	}

	if httpMode {
		if !aiSetProducer(apiBase, apiToken, callID, 1) {
			fmt.Fprintln(stderr, "[ai_agent] producer 设置失败")
			cs.markDone()
			return
		}
	}

	// cleanup：退出时 producer=0 + 关闭 WS
	defer func() {
		fmt.Printf("[ai_agent] ★ cleanup call=%s producer=0\n", callID)
		if ws != nil {
			ws.close()
		} else {
			aiSetProducer(apiBase, apiToken, callID, 0)
		}
		cs.markDone()
	}()

	// --- welcome greeting ---
	// 优先用 daemon 启动时预合成的欢迎语（零延迟）；没有则现场合成。
	welcome := cachedGreeting
	if len(welcome) == 0 {
		welcome = tts.Synthesize(welcomeText)
	}
	if len(welcome) > 0 {
		fmt.Printf("[ai_agent] → TTS 欢迎语 (%d bytes)\n", len(welcome))
		sendAudioPaced(apiBase, apiToken, callID, welcome, ws, cs, bargeInEnabled, bargeInRMS)
	}

	fmt.Printf("[ai_agent] 循环开始 — 等待用户说话 ...\n")

	// --- 主循环 ---
	// 用 ticker 做 VAD 检测（主动 barge-in）
	vadTicker := time.NewTicker(100 * time.Millisecond)
	defer vadTicker.Stop()

	var (
		consecutiveSilence = 0
		ttsPlaying         = false // 当前是否在播 TTS
		stopTTSCh          = make(chan struct{})

		// --- ASR 累积缓冲 ---
		// NASR 需要 ≥ 300-500ms 连续语音才能稳定识别；每帧 160B = 20ms PCMU
		// 满 4800B (30 帧 = 600ms) 强制 flush；或静音 ≥ 2s (silenceTick≥20) flush
		asrBuf      []byte
		speechInBuf = false // 缓冲内是否检测到有能量（避免全程静音也喂 ASR）
	)

	const (
		asrBufMaxBytes   = 4800 // 600ms PCMU @ 8kHz
		asrSilenceFlsuhTick = 20 // 2s 静音 (tick 100ms) → flush
	)

	flushASR := func(reason string) {
		if len(asrBuf) == 0 {
			return
		}
		if !speechInBuf {
			// 全程静音缓冲，不浪费一次 NASR 调用
			asrBuf = asrBuf[:0]
			return
		}
		fmt.Printf("[ai_agent] ★ ASR flush reason=%s buf=%d bytes (%d ms)\n",
			reason, len(asrBuf), len(asrBuf)/160*20)
		transcript := asr.Transcribe(asrBuf)
		asrBuf = asrBuf[:0]
		speechInBuf = false

		if transcript == "" {
			return
		}
		fmt.Printf("[ai_agent] ASR: %s\n", transcript)

		reply := llm.Chat(transcript)
		switch reply.Action {
		case "transfer":
			dest := reply.Dest
			if dest == "" {
				dest = transferDest
			}
			fmt.Printf("[ai_agent] ★ LLM → transfer dest=%s reason=%s\n", dest, reply.Reason)
			aiRequestTransfer(apiBase, apiToken, callID, dest)
			return

		case "hangup":
			fmt.Printf("[ai_agent] ★ LLM → hangup reason=%s\n", reply.Reason)
			aiHangup(apiBase, apiToken, callID)
			return

		case "say":
			fmt.Printf("[ai_agent] LLM say: %s\n", reply.Text)
			ttsPlaying = true

			pcmu := tts.Synthesize(reply.Text)
			if len(pcmu) > 0 {
				sendAudioPaced(apiBase, apiToken, callID, pcmu, ws, cs, bargeInEnabled, bargeInRMS)
				fmt.Printf("[ai_agent] ← TTS 回复 (%d bytes)\n", len(pcmu))
			}
			ttsPlaying = false
		}
	}

	defer func() {
		// 通话退出前 flush 剩余缓冲
		if len(asrBuf) > 0 {
			flushASR("CALL_END")
		}
	}()

	for {
		select {
		// === 收到 hangup 事件或 daemon 正在退出 ===
		case <-cs.stopped:
			fmt.Printf("[ai_agent] ★ 通话已结束（CALL_END 事件）\n")
			return

		case <-cs.done:
			return

		// === 主循环 tick ===
		default:
		}

		// --- VAD barge-in：检测到用户说话 → 立即停 TTS ---
		vadSpeechNow := false
		if bargeInEnabled {
			select {
			case <-vadTicker.C:
				vad := aiVadState(apiBase, apiToken, callID)
				if vad.aState == "SPEECH" && vad.aRms >= bargeInRMS {
					vadSpeechNow = true
					if ttsPlaying {
						fmt.Printf("[ai_agent] ★ barge-in! 用户说话 → 停 TTS\n")
						select {
						case stopTTSCh <- struct{}{}:
						default:
						}
						ttsPlaying = false
					}
					consecutiveSilence = 0
				} else {
					consecutiveSilence++
				}
			default:
			}
		}

		// --- 静音 flush：用户说完了就把缓冲喂 ASR ---
		if len(asrBuf) > 0 && consecutiveSilence >= asrSilenceFlsuhTick {
			flushASR("silence")
			consecutiveSilence = 0
		}

		// --- 拉用户语音 ---
		var rx []byte

		if ws != nil {
			select {
			case rx = <-wsRxCh:
			case err := <-wsErrCh:
				fmt.Fprintf(stderr, "[ai_agent] WS 断开: %v\n", err)
				return
			case <-time.After(100 * time.Millisecond):
				// 没数据但缓冲可能已满 → flush 试试
				if len(asrBuf) >= asrBufMaxBytes {
					flushASR("buf-full")
				}
				continue
			}
		} else {
			rx = aiGetRx(apiBase, apiToken, callID, chunkBytes)
			if len(rx) == 0 {
				time.Sleep(50 * time.Millisecond)
				if len(asrBuf) >= asrBufMaxBytes {
					flushASR("buf-full")
				}
				continue
			}
		}

		// --- 累积缓冲 ---
		asrBuf = append(asrBuf, rx...)
		if vadSpeechNow {
			speechInBuf = true
		}

		// 缓冲满 → 强制 flush（哪怕 VAD 还说在说话，也把前 600ms 送出去）
		if len(asrBuf) >= asrBufMaxBytes {
			flushASR("buf-full")
		}
	}

}

// =============================================================================
// main — daemon 模式（默认） 或 单通模式（--call-id 指定）
// =============================================================================

func main() {
	apiBase := flag.String("api-base", "https://nsbc-web.local:443", "nsbc-web 基址")
	apiToken := flag.String("api-token", "", "Admin scope ext_key")
	callID := flag.String("call-id", "", "单通模式：手动指定 call_id（不启 daemon）")
	sseURL := flag.String("sse-url", "", "SSE stream URL（默认 {api_base}/api/v1/calls/stream）")
	transferDest := flag.String("transfer-dest", "1001", "transfer 默认目标")
	bargeInEnabled := flag.Bool("vad-barge-in", true, "启用 barge-in")
	bargeInRMS := flag.Int("barge-in-rms", 800, "barge-in VAD RMS 阈值")
	chunkBytes := flag.Int("chunk-bytes", 160, "ai-get-rx 每次拉多少字节")
	transport := flag.String("transport", "auto", "http | ws | auto")
	insecure := flag.Bool("insecure", true, "跳过 TLS 证书验证（默认 true，自签证书）")

	// Provider flags
	asrName := flag.String("asr", "mock", "mock | whisper | nway")
	llmName := flag.String("llm", "mock", "mock | ollama | openai_chat")
	ttsName := flag.String("tts", "mock", "mock | elevenlabs | nway")

	apiKeyWhisper := flag.String("api-key-whisper", os.Getenv("NSBC_WHISPER_API_KEY"), "")
	llmEndpoint := flag.String("llm-endpoint", getEnv("NSBC_OLLAMA_ENDPOINT", "http://localhost:11434/api/chat"), "")
	llmModel := flag.String("llm-model", getEnv("NSBC_OLLAMA_MODEL", "llama3.1:8b"), "")
	apiKeyLLM := flag.String("api-key-llm", os.Getenv("NSBC_OLLAMA_API_KEY"), "")
	apiKeyTTS := flag.String("api-key-tts", os.Getenv("NSBC_ELEVENLABS_API_KEY"), "")
	ttsVoiceID := flag.String("tts-voice-id", getEnv("NSBC_ELEVENLABS_VOICE_ID", "21m00Tcm4TlvDq8ikWAM"), "")
	nwayASRAddr := flag.String("nway-asr-addr", "10.0.0.80:8089", "nway ASR TCP 服务地址")
	nwayTTSAddr := flag.String("nway-tts-addr", "10.0.0.80:8090", "nway TTS TCP 服务地址")

	flag.Parse()

	if *apiToken == "" {
		flag.Usage()
		fmt.Fprintln(stderr, "\n错误: --api-token 是必需的")
		os.Exit(1)
	}

	if *insecure {
		nsbc_sdk.SetSSLVerify(false)
	}

	// --- providers ---
	transferKeywords := []string{"转人工", "人工", "客服", "人工服务", "找人工", "转客服",
		"agent", "human", "operator", "representative"}
	asr := buildASR(*asrName, *apiKeyWhisper, *nwayASRAddr)
	llm := buildLLM(*llmName, *transferDest, *apiKeyLLM, *llmEndpoint, *llmModel, transferKeywords)
	tts := buildTTS(*ttsName, *apiKeyTTS, *ttsVoiceID, *nwayTTSAddr)

	// ============================================================
	// 单通模式：--call-id 已指定 → 直接跑，不启 daemon
	// ============================================================
	if *callID != "" {
		fmt.Printf("\n%s\n", strings.Repeat("=", 70))
		fmt.Printf("[ai_agent Go] 单通模式 call=%s transfer_dest=%s\n", *callID, *transferDest)
		fmt.Printf("  transport=%s  barge_in=%v  chunk=%d\n", *transport, *bargeInEnabled, *chunkBytes)
		fmt.Printf("  ASR=%s  LLM=%s  TTS=%s\n", *asrName, *llmName, *ttsName)
		fmt.Printf("%s\n\n", strings.Repeat("=", 70))

		cs := newCallSession(*callID, "?", "?")
		handleCall(cs, *apiBase, *apiToken, *transferDest, *chunkBytes,
			*bargeInEnabled, *bargeInRMS, *transport, asr, llm, tts)
		return
	}

	// ============================================================
	// daemon 模式：SSE 订阅 → 每通一个 goroutine
	// ============================================================
	fmt.Printf("\n%s\n", strings.Repeat("=", 70))
	fmt.Printf("[ai_agent Go] ★ DAEMON 模式 — 常驻监听\n")
	fmt.Printf("  api_base=%s\n", *apiBase)
	fmt.Printf("  transport=%s  barge_in=%v  chunk=%d\n", *transport, *bargeInEnabled, *chunkBytes)
	fmt.Printf("  ASR=%s  LLM=%s  TTS=%s\n", *asrName, *llmName, *ttsName)
	fmt.Printf("  等待新来电 ... (Ctrl+C 退出)\n")
	fmt.Printf("%s\n\n", strings.Repeat("=", 70))

	// 预合成欢迎语（静态文本）一次，后续每通来电零延迟直发。
	cachedGreeting = tts.Synthesize(welcomeText)
	fmt.Printf("[daemon] 欢迎语预合成 %d bytes\n", len(cachedGreeting))

	// 组装 SSE URL：/ext/v1/events/stream（ext API Key 鉴权；
	// /api/v1/calls/stream 走 JWT RBAC，外部 agent 不适用）
	sseUrl := *sseURL
	if sseUrl == "" {
		sseUrl = strings.TrimRight(*apiBase, "/") + "/ext/v1/events/stream"
	}
	fmt.Printf("[daemon] SSE → %s\n", sseUrl)

	headers := map[string]string{
		"Authorization": "Bearer " + *apiToken,
	}

	tracker := newCallTracker()

	// 启动 SSE 客户端（自动重连）
	sseClient := nsbc_sdk.NewSseClient(sseUrl, 30, headers)
	if err := sseClient.Connect(); err != nil {
		fmt.Fprintf(stderr, "[daemon] ❌ SSE 连接失败: %v\n", err)
		fmt.Fprintln(stderr, "请检查：")
		fmt.Fprintln(stderr, "  1) nsbc-web 是否在运行")
		fmt.Fprintln(stderr, "  2) API token 是否有效")
		fmt.Fprintln(stderr, "  3) API Key 是否有 readwrite scope")
		os.Exit(2)
	}
	defer sseClient.Close()
	fmt.Printf("[daemon] ✅ SSE 已连接\n")

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	rawCh := make(chan nsbc_sdk.SseEvent, 64)
	go sseClient.IterEvents(rawCh)

	fmt.Printf("[daemon] 等待事件 ...\n\n")

	for {
		select {
		case <-sigCh:
			fmt.Printf("\n[daemon] ★ 收到退出信号，关闭所有活跃通话 (%d)\n", tracker.activeCount())
			// 给所有活跃 session 发退出信号
			tracker.mu.Lock()
			for _, cs := range tracker.calls {
				cs.markStopped()
			}
			tracker.mu.Unlock()
			// 等一下 goroutine 清理
			time.Sleep(500 * time.Millisecond)
			fmt.Println("[daemon] 已退出")
			return

		case ev, ok := <-rawCh:
			if !ok {
				fmt.Fprintln(stderr, "[daemon] SSE channel 关闭")
				return
			}

			frame := nsbc_sdk.SseEventToFrame(ev)
			if frame == nil {
				continue
			}

			callID := strField(frame.Fields, "call_id")
			if callID == "" {
				continue
			}

			orig := strField(frame.Fields, "orig")
			if orig == "" || orig == "?" {
				/* C 内核 session 事件用 from 字段（answer/create/end/hangup），
				 * CDR 用 from；strField 缺失返回 "?" 而非 ""，故这里要显式判 "?" */
				orig = strField(frame.Fields, "from")
			}
			to := strField(frame.Fields, "to")

			switch frame.Domain {
			case "CALL_ANSWERED":
				if orig == "" {
					fmt.Printf("[sse] ⚠ CALL_ANSWERED 缺少主叫字段，raw=%s\n", frame.Raw)
				}
				fmt.Printf("[sse] ✅ CALL_ANSWERED call=%s %s→%s\n", callID, orig, to)
				cs := newCallSession(callID, orig, to)
				if tracker.add(cs) {
					go func() {
						defer tracker.remove(callID)
						handleCall(cs, *apiBase, *apiToken, *transferDest, *chunkBytes,
							*bargeInEnabled, *bargeInRMS, *transport, asr, llm, tts)
					}()
				} else {
					fmt.Printf("[sse] ⚠ 该通话已在处理中，跳过\n")
				}

			case "CALL_END", "END":
				disp := strField(frame.Fields, "disposition")
				if disp == "?" {
					disp = strField(frame.Fields, "disp")
				}
				fmt.Printf("[sse] ❌ CALL_END call=%s %s→%s disp=%s\n",
					callID, orig, to, disp)
				tracker.stopCall(callID)

			case "CALL_INCOMING":
				fmt.Printf("[sse] 📞 CALL_INCOMING call=%s %s→%s\n", callID, orig, to)

			case "CDR":
				fmt.Printf("[sse] 📋 CDR call=%s %s→%s disp=%s dur=%sbill=%s\n",
					callID, orig, to, strField(frame.Fields, "disposition"),
					strField(frame.Fields, "duration_s"), strField(frame.Fields, "billsec"))

			default:
				fmt.Printf("[sse] %s call=%s %v\n", frame.Domain, callID, frame.Fields)
			}
		}
	}
}

// =============================================================================
// Utils
// =============================================================================

func getEnv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func strField(m map[string]interface{}, key string) string {
	if v, ok := m[key].(string); ok {
		return v
	}
	if v, ok := m[key].(float64); ok {
		return fmt.Sprintf("%v", v)
	}
	return "?"
}

// =============================================================================
// WebSocket 读循环 + 心跳
// =============================================================================

const (
	wsPingInterval  = 30 * time.Second
	wsPongTimeout   = 10 * time.Second
	wsZombieTimeout = 120 * time.Second
)

func (t *wsTransport) runReadLoop(rxCh chan []byte, errCh chan error) {
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()

	for range ticker.C {
		now := time.Now()

		// A: 读帧
		frame, err := t.wsRead()
		if err == nil {
			t.lastClientActivity = now
			if t.pendingPing {
				t.pendingPing = false
			}

			switch frame.opcode {
			case wsOpBinary:
				select {
				case rxCh <- frame.payload:
				default:
					<-rxCh
					rxCh <- frame.payload
				}
			case wsOpPing:
				_ = t.wsWrite(wsOpPong, frame.payload)
			case wsOpClose:
				_ = t.wsWrite(wsOpClose, nil)
				errCh <- fmt.Errorf("peer close")
				return
			case wsOpPong:
				// OK
			}
		} else {
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				// 50ms deadline 正常过期
			} else if err.Error() != "EOF" {
				errCh <- err
				return
			} else {
				errCh <- err
				return
			}
		}

		// B: 心跳三重检查
		if now.Sub(t.lastClientActivity) > wsZombieTimeout {
			errCh <- fmt.Errorf("zombie: %s no client data", wsZombieTimeout)
			return
		}
		if t.pendingPing && now.Sub(t.lastPingSent) > wsPongTimeout {
			errCh <- fmt.Errorf("pong timeout: %s", wsPongTimeout)
			return
		}
		if !t.pendingPing && now.Sub(t.lastPingSent) > wsPingInterval {
			if err := t.wsWrite(wsOpPing, []byte("ping")); err != nil {
				errCh <- err
				return
			}
			t.lastPingSent = now
			t.pendingPing = true
		}
	}
}
