/* main.go — Go External AI Agent（HTTP polling + WebSocket Binary frame 可选）
 *
 * 纯 stdlib，零第三方依赖。套 PROTOCOL.md §4 + §4.4
 *
 * 传输层：
 *   --transport http   HTTP polling + base64（零依赖）
 *   --transport ws     WebSocket Binary frame（零 base64 开销）
 *   --transport auto   优先 WS 降级 HTTP
 *
 * Provider：
 *   --asr mock|whisper  --llm mock|ollama|openai_chat  --tts mock|elevenlabs
 *
 * 用法：
 *   go run main.go --api-base https://nsbc-web.local:8080 \
 *       --api-token <Admin-ext-key> --call-id <call_id> \
 *       --transport ws --asr whisper --tts elevenlabs --llm ollama */
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
	"strings"
	"sync"
	"time"
)

// =============================================================================
// HTTP cmd + AI pass-through API（和 Python client.py 对称）
// =============================================================================

var stderr = os.Stderr

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
	return checkOK(rc, out)
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
//
// Opcode: 0x2 Binary / 0x8 Close / 0x9 Ping / 0xA Pong
// Server→Client MASK=0；Client→Server MASK=1（RFC 6455 强制）
// Heartbeat: 30s ping → 10s pong timeout → 120s zombie disconnect
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

// wsTransport — Go 的 mutex + channel 版双向桥接
type wsTransport struct {
	conn   net.Conn
	rw     *bufio.ReadWriter
	mu     sync.Mutex  // 保护写
	closed bool

	// 心跳状态
	lastPingSent       time.Time
	lastClientActivity time.Time
	pendingPing        bool

	apiBase, apiToken, callID string
}

func newWsTransport(apiBase, apiToken, callID string) (*wsTransport, error) {
	// Build WS URL
	base := strings.TrimRight(apiBase, "/")
	scheme := "ws"
	if strings.HasPrefix(base, "https://") {
		scheme = "wss"
		base = base[len("https://"):]
	} else if strings.HasPrefix(base, "http://") {
		base = base[len("http://"):]
	}
	wsURL := fmt.Sprintf("%s://%s/ws/ai-media/%s?token=%s", scheme, base, callID, url.QueryEscape(apiToken))

	// Dial
	var conn net.Conn
	var err error
	if scheme == "wss" {
		conn, err = tls.Dial("tcp", base[:strings.Index(base, "/")], &tls.Config{InsecureSkipVerify: true})
	} else {
		conn, err = net.DialTimeout("tcp", base[:strings.Index(base, "/")], 10*time.Second)
	}
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", wsURL, err)
	}

	// HTTP Upgrade handshake
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

	// Verify Sec-WebSocket-Accept
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

// wsRead — 读一帧（带 50ms deadline，不阻塞主循环）
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

// wsWrite — 写一帧（Server→Client MASK=0）
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

// --- 公共方法，和 HttpTransport 对齐 ---

func (t *wsTransport) getRx(maxBytes int) []byte {
	// Go 版：在主循环里用 wsRxChannel receive，这里返回空表示"等待下一帧"
	return nil
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
// BargeIn（和 Python 对称）
// =============================================================================

type bargeIn struct {
	apiBase, apiToken, callID string
	rms                       int
}

func newBargeIn(apiBase, apiToken, callID string, rms int) *bargeIn {
	return &bargeIn{apiBase: apiBase, apiToken: apiToken, callID: callID, rms: rms}
}

func (b *bargeIn) userSpeaking() bool {
	r := aiVadState(b.apiBase, b.apiToken, b.callID)
	return r.aState == "SPEECH" && r.aRms >= b.rms
}

func (b *bargeIn) waitUntilSilence(timeout time.Duration) bool {
	consecutiveSilence := 0
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if b.userSpeaking() {
			consecutiveSilence = 0
		} else {
			consecutiveSilence++
			if consecutiveSilence >= 10 {
				return true
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	return false
}

// =============================================================================
// Provider 工厂（--asr / --llm / --tts → 真实实例）
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
// main
// =============================================================================

func main() {
	apiBase := flag.String("api-base", "https://nsbc-web.local:8080", "nsbc-web 基址")
	apiToken := flag.String("api-token", "", "Admin scope ext_key")
	callID := flag.String("call-id", "", "要接管的 call_id")
	transferDest := flag.String("transfer-dest", "1001", "transfer 默认目标")
	bargeInEnabled := flag.Bool("vad-barge-in", true, "启用 barge-in")
	bargeInRMS := flag.Int("barge-in-rms", 800, "barge-in VAD RMS 阈值")
	chunkBytes := flag.Int("chunk-bytes", 160, "ai-get-rx 每次拉多少字节")
	transport := flag.String("transport", "auto", "http | ws | auto")

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

	if *apiToken == "" || *callID == "" {
		flag.Usage()
		os.Exit(1)
	}

	// --- providers ---
	transferKeywords := []string{"转人工", "人工", "客服", "人工服务", "找人工", "转客服",
		"agent", "human", "operator", "representative"}
	asr := buildASR(*asrName, *apiKeyWhisper, *nwayASRAddr)
	llm := buildLLM(*llmName, *transferDest, *apiKeyLLM, *llmEndpoint, *llmModel, transferKeywords)
	tts := buildTTS(*ttsName, *apiKeyTTS, *ttsVoiceID, *nwayTTSAddr)

	fmt.Printf("\n%s\n", strings.Repeat("=", 70))
	fmt.Printf("[ai_agent Go] call=%s transfer_dest=%s\n", *callID, *transferDest)
	fmt.Printf("  transport=%s  barge_in=%v  chunk=%d\n", *transport, *bargeInEnabled, *chunkBytes)
	fmt.Printf("  ASR=%s  LLM=%s  TTS=%s\n", *asrName, *llmName, *ttsName)
	fmt.Printf("%s\n\n", strings.Repeat("=", 70))

	// --- transport ---
	var httpMode bool
	var ws *wsTransport
	var wsRxCh chan []byte
	var wsErrCh chan error

	if *transport == "ws" || (*transport == "auto" && isTruthy(*transport == "auto")) {
		w, err := newWsTransport(*apiBase, *apiToken, *callID)
		if err == nil {
			ws = w
			httpMode = false
			wsRxCh = make(chan []byte, 128)
			wsErrCh = make(chan error, 1)
			// 启动 WS 读写 goroutine
			go ws.runReadLoop(wsRxCh, wsErrCh)
		} else {
			if *transport == "ws" {
				fmt.Fprintf(stderr, "[ai_agent] 指定 --transport ws 但连接失败: %v\n", err)
				os.Exit(1)
			}
			fmt.Fprintf(stderr, "[ai_agent] WS 连接失败 (%v)，降级 HTTP polling\n", err)
			httpMode = true
		}
	} else {
		httpMode = true
	}

	if httpMode {
		if !aiSetProducer(*apiBase, *apiToken, *callID, 1) {
			fmt.Fprintln(stderr, "[ai_agent] producer 设置失败")
			os.Exit(1)
		}
		defer func() { aiSetProducer(*apiBase, *apiToken, *callID, 0) }()
	}

	var barge *bargeIn
	if *bargeInEnabled {
		barge = newBargeIn(*apiBase, *apiToken, *callID, *bargeInRMS)
	}

	// --- welcome ---
	if welcome := tts.Synthesize("您好，我是 AI 助理。请问需要什么帮助？"); len(welcome) > 0 {
		fmt.Printf("[ai_agent] → TTS 欢迎语 (%d bytes)\n", len(welcome))
		if ws != nil {
			ws.sendTx(welcome)
		} else {
			aiSendTx(*apiBase, *apiToken, *callID, welcome, false)
		}
	}
	fmt.Println("[ai_agent] 循环开始 — Ctrl+C 退出")
	fmt.Println()

	// --- 主循环 ---
	for {
		var rx []byte

		if ws != nil {
			select {
			case rx = <-wsRxCh:
				// OK, got binary frame
			case err := <-wsErrCh:
				fmt.Fprintf(stderr, "[ai_agent] WS 断开: %v\n", err)
				ws.close()
				return
			case <-time.After(100 * time.Millisecond):
				// 没数据，循环
				continue
			}
		} else {
			rx = aiGetRx(*apiBase, *apiToken, *callID, *chunkBytes)
			if len(rx) == 0 {
				time.Sleep(50 * time.Millisecond)
				continue
			}
		}

		if barge != nil {
			barge.waitUntilSilence(2 * time.Second)
		}

		transcript := asr.Transcribe(rx)
		if transcript == "" {
			continue
		}
		fmt.Printf("[ai_agent] ASR: %s\n", transcript)

		reply := llm.Chat(transcript)
		switch reply.Action {
		case "transfer":
			dest := reply.Dest
			if dest == "" {
				dest = *transferDest
			}
			fmt.Printf("[ai_agent] ★ LLM → transfer dest=%s reason=%s\n", dest, reply.Reason)
			aiRequestTransfer(*apiBase, *apiToken, *callID, dest)
			if ws != nil {
				ws.close()
			}
			return
		case "hangup":
			fmt.Printf("[ai_agent] ★ LLM → hangup reason=%s\n", reply.Reason)
			aiHangup(*apiBase, *apiToken, *callID)
			if ws != nil {
				ws.close()
			}
			return
		case "say":
			fmt.Printf("[ai_agent] LLM say: %s\n", reply.Text)
			if barge != nil {
				barge.waitUntilSilence(1 * time.Second)
			}
			pcmu := tts.Synthesize(reply.Text)
			if len(pcmu) > 0 {
				var ok bool
				if ws != nil {
					ok = ws.sendTx(pcmu)
				} else {
					ok = aiSendTx(*apiBase, *apiToken, *callID, pcmu, false)
				}
				if ok {
					fmt.Printf("[ai_agent] ← TTS 回复 (%d bytes)\n", len(pcmu))
				} else {
					fmt.Fprintln(stderr, "[ai_agent] ← TTS 发送失败！")
				}
			}
		}
	}
}

// --- WebSocket 读循环 + 心跳（和 Rust ws.rs 完全对称）---

const (
	wsPingInterval   = 30 * time.Second
	wsPongTimeout    = 10 * time.Second
	wsZombieTimeout  = 120 * time.Second
)

// runReadLoop — WS 读 goroutine + 心跳（和 Rust ws.rs 完全对称）
// rxCh / errCh 都是双向 channel（main 里声明 chan []byte 传进来，本函数 send，main 里 receive）
func (t *wsTransport) runReadLoop(rxCh chan []byte, errCh chan error) {
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()

	for range ticker.C {
		now := time.Now()

		// A: 读帧（50ms deadline 在 wsRead 内）
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
					// 缓冲区满，丢最旧的 — 不阻塞读循环
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
			// read deadline / EOF / 真正错误
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				// 50ms deadline 正常过期 — 继续下面的心跳检查
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

// --- utils ---

func getEnv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func isTruthy(b bool) bool { return b } // always true — placeholder for future non-bool
