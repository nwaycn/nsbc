/* providers.go — Go AI Agent Provider 抽象 + 实现
 *
 * 音频格式约定（C 侧 e77e82e 大改后统一 PCM16 LE 8kHz mono）：
 *   ASRProvider  — PCM16 LE 8kHz mono → 文字
 *   LLMProvider  — 文字 → LLMReply（结构化 action）
 *   TTSProvider  — 文字 → PCM16 LE 8kHz mono
 *
 * C 侧 ai_rec_feed 现在 ring buffer 存 PCM16 LE 8kHz
 *   (g711_alaw_decode + HPF)，ai_get_rx_audio 直接吐 PCM16。
 * C 侧 ai_send_tx_audio(format=1) 期望 PCM16 LE，
 *   内部用 tx_pcm16_enc.encode() 一次编码到对端 codec。
 *
 * 实现：mock + 三个真实 Provider，全部零第三方依赖（纯 net/http + encoding）
 */
package main

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"time"
)

// =============================================================================
// LLMReply — 结构化 LLM 回复（单一事实源，和 Python 对齐）
// =============================================================================

type LLMReply struct {
	Action string `json:"action"` // "say" | "transfer" | "hangup" | "silence"
	Text   string `json:"text,omitempty"`
	Dest   string `json:"dest,omitempty"`
	Reason string `json:"reason,omitempty"`
}

// =============================================================================
// Provider 接口
// =============================================================================

type ASRProvider interface {
	Transcribe(pcm16 []byte) string // PCM16 LE 8kHz mono → 文字
}

type LLMProvider interface {
	Chat(transcript string) LLMReply
}

type TTSProvider interface {
	Synthesize(text string) []byte // 返回 PCM16 LE 8kHz mono
}

// =============================================================================
// Mock —— 零 API key，生产验证用
// =============================================================================

type MockASR struct {
	TriggerWord     string
	EnergyThreshold int
}

func (a *MockASR) Transcribe(pcm16 []byte) string {
	if len(pcm16) < 2 {
		return ""
	}
	// PCM16 LE — 按 2 字节解析 int16 样本，检查峰值是否超过阈值
	samples := Pcm16LEToInt16(pcm16)
	for _, s := range samples {
		if int(s) > a.EnergyThreshold || int(-s) > a.EnergyThreshold {
			return a.TriggerWord
		}
	}
	return ""
}

type MockLLM struct {
	TransferKeywords []string
	HangupKeywords   []string
	TransferDest     string
}

func (l *MockLLM) Chat(transcript string) LLMReply {
	if transcript == "" {
		return LLMReply{Action: "silence"}
	}
	lowered := strings.ToLower(transcript)
	for _, kw := range l.TransferKeywords {
		if strings.Contains(lowered, strings.ToLower(kw)) {
			return LLMReply{Action: "transfer", Dest: l.TransferDest, Reason: "keyword=" + kw}
		}
	}
	for _, kw := range l.HangupKeywords {
		if strings.Contains(lowered, strings.ToLower(kw)) {
			return LLMReply{Action: "hangup", Reason: "keyword=" + kw}
		}
	}
	return LLMReply{Action: "say", Text: fmt.Sprintf("我收到了您说的：%s。请问还需要什么帮助吗？", truncate(transcript, 40))}
}

type MockTTS struct {
	DurationMs int // 静音时长，PCM16 LE 8kHz mono = 2 bytes/sample × 8000 Hz × DurationMs/1000
}

func (t *MockTTS) Synthesize(text string) []byte {
	if text == "" {
		return nil
	}
	// 8kHz × 2 bytes × DurationMs/1000 = 16 * DurationMs bytes
	return make([]byte, 16*t.DurationMs)
}

// =============================================================================
// ElevenLabsTTS — text-to-speech REST → PCM16 LE 8kHz mono
// =============================================================================

type ElevenLabsTTS struct {
	APIKey   string
	VoiceID  string
	ModelID  string
	Endpoint string
	TimeoutS int
}

func (t *ElevenLabsTTS) Synthesize(text string) []byte {
	if text == "" || t.APIKey == "" {
		return nil
	}
	url := fmt.Sprintf("%s/%s?output_format=pcm_16000",
		defaultStr(t.Endpoint, "https://api.elevenlabs.io/v1/text-to-speech"),
		defaultStr(t.VoiceID, "21m00Tcm4TlvDq8ikWAM"))

	body, _ := json.Marshal(map[string]string{
		"text":     text,
		"model_id": defaultStr(t.ModelID, "eleven_monolingual_v1"),
	})

	hc := &http.Client{Timeout: time.Duration(defaultInt(t.TimeoutS, 15)) * time.Second}
	req, _ := http.NewRequest("POST", url, strings.NewReader(string(body)))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("xi-api-key", t.APIKey)
	req.Header.Set("Accept", "audio/pcm")

	resp, err := hc.Do(req)
	if err != nil {
		fmt.Fprintf(stderr, "[ElevenLabsTTS] 请求失败: %v\n", err)
		return nil
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 200))
		fmt.Fprintf(stderr, "[ElevenLabsTTS] HTTP %d: %s\n", resp.StatusCode, string(data))
		return nil
	}

	pcm16_16k, _ := io.ReadAll(resp.Body)
	if len(pcm16_16k) < 4 {
		return nil
	}
	// ElevenLabs 返回 PCM16 16kHz → downsample 到 8kHz → PCM16 LE 8kHz
	samples_16k := Pcm16LEToInt16(pcm16_16k)
	samples_8k := Downsample16kTo8k(samples_16k)
	return Int16ToPcm16LE(samples_8k)
}

// =============================================================================
// WhisperASR — audio/transcriptions REST
// =============================================================================

type WhisperASR struct {
	APIKey   string
	Model    string
	Endpoint string
	Language string
	TimeoutS int
}

func (a *WhisperASR) Transcribe(pcm16 []byte) string {
	if len(pcm16) == 0 || a.APIKey == "" {
		return ""
	}

	// 输入已经是 PCM16 LE 8kHz → 直接加 WAV header（44 bytes）
	wav := pcm16BytesToWav(pcm16, 8000)

	boundary := "----NSBCWhisperBoundary7MA4YWxkTrZu0gW"
	var buf strings.Builder

	buf.WriteString(fmt.Sprintf("--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n",
		boundary, defaultStr(a.Model, "whisper-1")))
	if a.Language != "" {
		buf.WriteString(fmt.Sprintf("--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n%s\r\n",
			boundary, a.Language))
	}
	buf.WriteString(fmt.Sprintf("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n", boundary))
	header := buf.String()

	payload := make([]byte, 0, len(header)+len(wav)+len(boundary)+4)
	payload = append(payload, header...)
	payload = append(payload, wav...)
	payload = append(payload, fmt.Sprintf("\r\n--%s--\r\n", boundary)...)

	url := defaultStr(a.Endpoint, "https://api.openai.com/v1/audio/transcriptions")
	hc := &http.Client{Timeout: time.Duration(defaultInt(a.TimeoutS, 30)) * time.Second}
	req, _ := http.NewRequest("POST", url, bytes.NewReader(payload))
	req.Header.Set("Content-Type", "multipart/form-data; boundary="+boundary)
	req.Header.Set("Authorization", "Bearer "+a.APIKey)

	resp, err := hc.Do(req)
	if err != nil {
		fmt.Fprintf(stderr, "[WhisperASR] 请求失败: %v\n", err)
		return ""
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		fmt.Fprintf(stderr, "[WhisperASR] HTTP %d: %s\n", resp.StatusCode, string(data[:minInt(len(data), 200)]))
		return ""
	}

	var blob map[string]interface{}
	if err := json.Unmarshal(data, &blob); err != nil {
		return ""
	}
	if t, ok := blob["text"].(string); ok {
		return strings.TrimSpace(t)
	}
	return ""
}

// =============================================================================
// OllamaLLM — /api/chat REST（本地/自建）
// =============================================================================

const defaultOllamaSystemPrompt = `你是一个电话客服 AI。所有回复必须是严格 JSON，不包含任何其他文字。JSON schema：{"action":"say|transfer|hangup|silence", "text":"...", "dest":"...", "reason":"..."} action=transfer 时 dest 必须填写目标分机号（如 1001）。action=say 时 text 必须是自然语言回复。action=silence 时不要 text。如果用户说'转人工'、'人工'、'找客服'，action=transfer。如果用户说'挂断'、'再见'、'bye'，action=hangup。`

type OllamaLLM struct {
	Model            string
	Endpoint         string
	APIKey           string
	SystemPrompt     string
	TransferDest     string
	TransferKeywords []string
	TimeoutS         int
}

func (l *OllamaLLM) Chat(transcript string) LLMReply {
	if transcript == "" {
		return LLMReply{Action: "silence"}
	}

	ep := defaultStr(l.Endpoint, "http://localhost:11434/api/chat")
	sysPrompt := defaultStr(l.SystemPrompt, defaultOllamaSystemPrompt)
	model := defaultStr(l.Model, "llama3.1:8b")

	body, _ := json.Marshal(map[string]interface{}{
		"model":  model,
		"stream": false,
		"format": "json",
		"messages": []map[string]string{
			{"role": "system", "content": sysPrompt},
			{"role": "user", "content": transcript},
		},
	})

	hc := &http.Client{Timeout: time.Duration(defaultInt(l.TimeoutS, 30)) * time.Second}
	req, _ := http.NewRequest("POST", ep, strings.NewReader(string(body)))
	req.Header.Set("Content-Type", "application/json")
	if l.APIKey != "" {
		req.Header.Set("Authorization", "Bearer "+l.APIKey)
	}

	resp, err := hc.Do(req)
	if err != nil {
		fmt.Fprintf(stderr, "[OllamaLLM] 请求失败: %v\n", err)
		return l.keywordFallback(transcript)
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		fmt.Fprintf(stderr, "[OllamaLLM] HTTP %d: %s\n", resp.StatusCode, string(data[:minInt(len(data), 200)]))
		return l.keywordFallback(transcript)
	}

	var blob map[string]interface{}
	if err := json.Unmarshal(data, &blob); err != nil {
		return l.keywordFallback(transcript)
	}
	msg, _ := blob["message"].(map[string]interface{})
	content, _ := msg["content"].(string)
	return l.parseContent(content, transcript)
}

func (l *OllamaLLM) parseContent(content string, transcript string) LLMReply {
	content = strings.TrimSpace(content)
	content = strings.TrimPrefix(content, "```json")
	content = strings.TrimPrefix(content, "```")
	content = strings.TrimSuffix(content, "```")

	var parsed LLMReply
	if err := json.Unmarshal([]byte(content), &parsed); err != nil {
		return l.keywordFallback(transcript)
	}

	parsed.Action = strings.ToLower(strings.TrimSpace(parsed.Action))
	switch parsed.Action {
	case "transfer":
		if parsed.Dest == "" {
			parsed.Dest = l.TransferDest
		}
		return parsed
	case "hangup", "silence":
		return parsed
	case "say":
		if parsed.Text != "" {
			return parsed
		}
	}
	return l.keywordFallback(transcript)
}

func (l *OllamaLLM) keywordFallback(transcript string) LLMReply {
	lowered := strings.ToLower(transcript)
	for _, kw := range l.TransferKeywords {
		if strings.Contains(lowered, strings.ToLower(kw)) {
			return LLMReply{Action: "transfer", Dest: l.TransferDest, Reason: "fallback keyword=" + kw}
		}
	}
	return LLMReply{Action: "say", Text: fmt.Sprintf("我收到了您说的：%s。请问还需要什么帮助吗？", truncate(transcript, 40)),
		Reason: "fallback (LLM unavailable)"}
}

// =============================================================================
// WAV builder — PCM16 LE raw → 44 字节 header + data
// =============================================================================

// pcm16BytesToWav — PCM16 LE 8kHz raw → WAV 文件字节
func pcm16BytesToWav(pcm16 []byte, sampleRate int) []byte {
	dataSize := len(pcm16)
	buf := make([]byte, 44+dataSize)
	copy(buf[0:4], "RIFF")
	binary.LittleEndian.PutUint32(buf[4:], uint32(dataSize+36))
	copy(buf[8:12], "WAVE")
	copy(buf[12:16], "fmt ")
	binary.LittleEndian.PutUint32(buf[16:], 16) // fmt chunk size
	binary.LittleEndian.PutUint16(buf[20:], 1)  // audio format PCM
	binary.LittleEndian.PutUint16(buf[22:], 1)  // channels
	binary.LittleEndian.PutUint32(buf[24:], uint32(sampleRate))
	binary.LittleEndian.PutUint32(buf[28:], uint32(sampleRate*2))
	binary.LittleEndian.PutUint16(buf[32:], 2)  // block align
	binary.LittleEndian.PutUint16(buf[34:], 16) // bits per sample
	copy(buf[36:40], "data")
	binary.LittleEndian.PutUint32(buf[40:], uint32(dataSize))
	copy(buf[44:], pcm16)
	return buf
}

// =============================================================================
// utils
// =============================================================================

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}
func defaultStr(v, d string) string {
	if v != "" {
		return v
	}
	return d
}
func defaultInt(v, d int) int {
	if v != 0 {
		return v
	}
	return d
}
func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
func randBytes(n int) []byte {
	b := make([]byte, n)
	_, _ = rand.Read(b)
	return b
}

// =============================================================================
// G.711 μ-law PCMU codec — NSBC B2BUA 传统 codec（保留做格式转换工具）
// =============================================================================
// 纯 Go 查表法，每个样例 O(1)，业界标准实现（ITU-T G.711）
var muLawDecode = [256]int16{
	-32124, -31100, -30076, -29052, -28028, -27004, -25980, -24956,
	-23932, -22908, -21884, -20860, -19836, -18812, -17788, -16764,
	-15996, -15484, -14972, -14460, -13948, -13436, -12924, -12412,
	-11900, -11388, -10876, -10364, -9852,  -9340,  -8828,  -8316,
	-7932,  -7676,  -7420,  -7164,  -6908,  -6652,  -6396,  -6140,
	-5884,  -5628,  -5372,  -5116,  -4860,  -4604,  -4348,  -4092,
	-3828,  -3608,  -3388,  -3168,  -2948,  -2728,  -2508,  -2288,
	-2068,  -1848,  -1628,  -1408,  -1188,  -968,   -748,   -528,
	-332,   -316,   -300,   -284,   -268,   -252,   -236,   -220,
	-204,   -188,   -172,   -156,   -140,   -124,   -108,   -92,
	-76,    -60,    -44,    -28,    -12,     12,     28,     44,
	60,     76,     92,     108,    124,    140,    156,    172,
	188,    204,    220,    236,    252,    268,    284,    300,
	316,    332,    528,    748,    968,    1188,   1408,   1628,
	1848,   2068,   2288,   2508,   2728,   2948,   3168,   3388,
	3608,   3828,   4092,   4348,   4604,   4860,   5116,   5372,
	5628,   5884,   6140,   6396,   6652,   6908,   7164,   7420,
	7676,   7932,   8316,   8828,   9340,   9852,   10364,  10876,
	11388,  11900,  12412,  12924,  13436,  13948,  14460,  14972,
	15484,  15996,  16764,  17788,  18812,  19836,  20860,  21884,
	22908,  23932,  24956,  25980,  27004,  28028,  29052,  30076,
	31100,  32124,
}

// Pcm16LEToInt16: 小端 PCM16 字节流 → int16 slice
func Pcm16LEToInt16(data []byte) []int16 {
	if len(data) == 0 {
		return nil
	}
	n := len(data) / 2
	out := make([]int16, n)
	for i := 0; i < n; i++ {
		out[i] = int16(binary.LittleEndian.Uint16(data[i*2:]))
	}
	return out
}

// Int16ToPcm16LE: int16 slice → 小端 PCM16 字节流
func Int16ToPcm16LE(samples []int16) []byte {
	out := make([]byte, len(samples)*2)
	for i, s := range samples {
		binary.LittleEndian.PutUint16(out[i*2:], uint16(s))
	}
	return out
}

// =============================================================================
// 重采样：整数倍最简单方案 —— AI 客服场景语音质量足够
// =============================================================================

// Upsample8kTo16k: 每个点翻倍（线性插值更准，但翻倍对 ASR 足够）
func Upsample8kTo16k(in []int16) []int16 {
	out := make([]int16, len(in)*2)
	for i, s := range in {
		out[i*2] = s
		out[i*2+1] = s
	}
	return out
}

// Downsample16kTo8k: 隔点取（偶数样本保留）
func Downsample16kTo8k(in []int16) []int16 {
	n := len(in) / 2
	out := make([]int16, n)
	for i := 0; i < n; i++ {
		out[i] = in[i*2]
	}
	return out
}

// =============================================================================
// NWAYASR — 本地 nway-asr-tcp 服务（10.0.0.80:8089）
//   协议 v1.0：4B_LE(samples) + PCM16_LE → 4B_LE(text_len) + UTF-8
//   输入：C 侧 ring buffer 存 PCM16 LE 8kHz mono（g711_alaw_decode + HPF）
//   处理：8kHz → 16kHz upsampling（服务端 Whisper 期望 16k）→ TCP 发送
// =============================================================================

type NWAYASR struct {
	Addr       string // 默认 10.0.0.80:8089
	NasrClient string // nasr-client 可执行文件路径（备用，当前走 TCP 直接转）
}

func (a *NWAYASR) Transcribe(pcm16 []byte) string {
	if a.Addr == "" {
		a.Addr = "10.0.0.80:8089"
	}

	if len(pcm16) < 4 {
		return ""
	}

	// 1. PCM16 LE 8kHz → int16 slice
	pcm8k := Pcm16LEToInt16(pcm16)
	if len(pcm8k) == 0 {
		return ""
	}

	// 2. 重采样 8k → 16k（NASR/Whisper 服务端期望 16kHz）
	pcm16k := Upsample8kTo16k(pcm8k)

	// 3. TCP 连接 nway-asr-tcp
	conn, err := net.DialTimeout("tcp", a.Addr, 5*time.Second)
	if err != nil {
		fmt.Fprintf(stderr, "[NWAYASR] dial %s FAIL: %v\n", a.Addr, err)
		return ""
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(30 * time.Second))

	// 4. 发送：4 字节 LE 采样点数 + PCM16_LE 字节流
	count := make([]byte, 4)
	binary.LittleEndian.PutUint32(count, uint32(len(pcm16k)))
	if _, err := conn.Write(count); err != nil {
		fmt.Fprintf(stderr, "[NWAYASR] write count FAIL: %v\n", err)
		return ""
	}
	payload := Int16ToPcm16LE(pcm16k)
	if _, err := conn.Write(payload); err != nil {
		fmt.Fprintf(stderr, "[NWAYASR] write payload FAIL: %v\n", err)
		return ""
	}
	if tc, ok := conn.(*net.TCPConn); ok {
		_ = tc.CloseWrite()
	}

	// 5. 接收：4 字节 LE text_len + UTF-8 transcript
	var lenBuf [4]byte
	if _, err := io.ReadFull(conn, lenBuf[:]); err != nil {
		fmt.Fprintf(stderr, "[NWAYASR] read text_len FAIL: %v\n", err)
		return ""
	}
	textLen := binary.LittleEndian.Uint32(lenBuf[:])
	if textLen > 1<<20 { // 安全上限 1MB transcript
		fmt.Fprintf(stderr, "[NWAYASR] text_len %d too large, abort\n", textLen)
		return ""
	}
	text := make([]byte, textLen)
	if textLen > 0 {
		if _, err := io.ReadFull(conn, text); err != nil {
			fmt.Fprintf(stderr, "[NWAYASR] read text FAIL: %v\n", err)
			return ""
		}
	}

	result := string(text)
	fmt.Fprintf(stderr, "[NWAYASR] transcript=%q (%d bytes input PCM16 → %d chars)\n",
		result, len(pcm16), len(result))
	return result
}

// =============================================================================
// NWAYTTS — 本地 nway-tts-tcp 服务（10.0.0.80:8090）
//   协议 v2：4B_LE(text_len) + UTF-8 text →
//            循环包 { 1B type | 4B LE payload_len | payload }
//            type 0x01 HEAD(sample_rate u32 LE), 0x02 DATA(PCM16_LE),
//                 0x03 END, 0xFF ERR
//   ★ 返回 PCM16 LE 8kHz mono —— C 侧 ai_send_tx_audio(format=1) 直接 encode 到对端 codec
// =============================================================================

type NWAYTTS struct {
	Addr string // 默认 10.0.0.80:8090
}

func (t *NWAYTTS) Synthesize(text string) []byte {
	if t.Addr == "" {
		t.Addr = "10.0.0.80:8090"
	}
	textBytes := []byte(text)

	// 1. TCP 连接
	conn, err := net.DialTimeout("tcp", t.Addr, 5*time.Second)
	if err != nil {
		fmt.Fprintf(stderr, "[NWAYTTS] dial %s FAIL: %v\n", t.Addr, err)
		return nil
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(60 * time.Second))

	// 2. 发送：4 字节 LE text_len + UTF-8 文本
	lenBuf := make([]byte, 4)
	binary.LittleEndian.PutUint32(lenBuf, uint32(len(textBytes)))
	if _, err := conn.Write(lenBuf); err != nil {
		fmt.Fprintf(stderr, "[NWAYTTS] write len FAIL: %v\n", err)
		return nil
	}
	if _, err := conn.Write(textBytes); err != nil {
		fmt.Fprintf(stderr, "[NWAYTTS] write text FAIL: %v\n", err)
		return nil
	}

	// 3. 收循环包：HEAD（拿采样率）→ DATA* → END / ERR
	var pcm16Samples []int16
	sampleRate := 0 // 等 HEAD 填充
	// P2-19：TTS 服务端若持续发 DATA，累计内存无上限；8kHz mono 32MiB ≈ 34 分钟
	const maxTTSBytes = 32 << 20
	totalPCM := 0

	for {
		var typeBuf [1]byte
		if _, err := io.ReadFull(conn, typeBuf[:]); err != nil {
			fmt.Fprintf(stderr, "[NWAYTTS] read type FAIL: %v (dropped)\n", err)
			break
		}
		pt := typeBuf[0]

		var plenBuf [4]byte
		if _, err := io.ReadFull(conn, plenBuf[:]); err != nil {
			fmt.Fprintf(stderr, "[NWAYTTS] read plen FAIL: %v\n", err)
			break
		}
		plen := binary.LittleEndian.Uint32(plenBuf[:])
		if plen > 16<<20 {
			fmt.Fprintf(stderr, "[NWAYTTS] payload_len %d too large, abort\n", plen)
			break
		}

		payload := make([]byte, plen)
		if plen > 0 {
			if _, err := io.ReadFull(conn, payload); err != nil {
				fmt.Fprintf(stderr, "[NWAYTTS] read payload FAIL: %v\n", err)
				break
			}
		}

		switch pt {
		case 0x01: // HEAD — payload[0..4] = u32 LE sample_rate
			if plen >= 4 {
				sampleRate = int(binary.LittleEndian.Uint32(payload[0:4]))
				fmt.Fprintf(stderr, "[NWAYTTS] HEAD sample_rate=%d Hz\n", sampleRate)
			}
		case 0x02: // DATA — PCM16_LE 原始字节流
			totalPCM += int(plen)
			if totalPCM > maxTTSBytes {
				fmt.Fprintf(stderr, "\n[NWAYTTS] total audio > %d bytes, abort\n", maxTTSBytes)
				return nil
			}
			samples := Pcm16LEToInt16(payload)
			pcm16Samples = append(pcm16Samples, samples...)
			fmt.Fprintf(stderr, ".")
		case 0x03: // END
			fmt.Fprintf(stderr, "\n[NWAYTTS] END. total_pcm16_samples=%d sr=%d\n",
				len(pcm16Samples), sampleRate)
			goto done
		case 0xFF: // ERR
			fmt.Fprintf(stderr, "\n[NWAYTTS] ERR: %s\n", string(payload))
			return nil
		default:
			fmt.Fprintf(stderr, "\n[NWAYTTS] unknown type 0x%02X plen=%d\n", pt, plen)
		}
	}
done:
	if len(pcm16Samples) == 0 {
		fmt.Fprintf(stderr, "[NWAYTTS] no audio received\n")
		return nil
	}

	// 4. 重采样到 8kHz（C 侧 ai_send_tx_audio format=1 固定 8kHz）
	if sampleRate != 8000 {
		switch sampleRate {
		case 16000:
			pcm16Samples = Downsample16kTo8k(pcm16Samples)
			fmt.Fprintf(stderr, "[NWAYTTS] WARN: sample_rate=16000, downsampled to 8k PCM16\n")
		default:
			fmt.Fprintf(stderr, "[NWAYTTS] ERR: sample_rate=%d 未知，无法处理\n", sampleRate)
			return nil
		}
	}
	pcm16Bytes := Int16ToPcm16LE(pcm16Samples)
	fmt.Fprintf(stderr, "[NWAYTTS] synthesized %d PCM16_LE bytes (%d samples sr=%d)\n",
		len(pcm16Bytes), len(pcm16Samples), sampleRate)
	return pcm16Bytes
}
