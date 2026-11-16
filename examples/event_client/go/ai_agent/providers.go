/* providers.go — Go AI Agent Provider 抽象 + 实现
 *
 * 设计：和 Python nsbc_ai.providers 完全对称
 *   ASRProvider  — PCMU 8kHz → 文字
 *   LLMProvider  — 文字 → LLMReply（结构化 action）
 *   TTSProvider  — 文字 → PCMU 8kHz
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
	Transcribe(pcmu []byte) string
}

type LLMProvider interface {
	Chat(transcript string) LLMReply
}

type TTSProvider interface {
	Synthesize(text string) []byte // 返回 PCMU 8kHz
}

// =============================================================================
// Mock —— 零 API key，生产验证用
// =============================================================================

type MockASR struct {
	TriggerWord      string
	EnergyThreshold  int
}

func (a *MockASR) Transcribe(pcmu []byte) string {
	if len(pcmu) == 0 {
		return ""
	}
	for _, b := range pcmu {
		if int(b) > a.EnergyThreshold {
			return a.TriggerWord
		}
	}
	return ""
}

type MockLLM struct {
	TransferDest      string
	TransferKeywords  []string
	HangupKeywords    []string
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
	DurationMs int // 静音时长，PCMU 每 1ms 1 字节
}

func (t *MockTTS) Synthesize(text string) []byte {
	if text == "" {
		return nil
	}
	return make([]byte, t.DurationMs)
}

// =============================================================================
// ElevenLabsTTS — text-to-speech REST → PCMU 8kHz
// =============================================================================

type ElevenLabsTTS struct {
	APIKey    string
	VoiceID   string
	ModelID   string
	Endpoint  string
	TimeoutS  int
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

	pcm16, _ := io.ReadAll(resp.Body)
	if len(pcm16) < 2 {
		return nil
	}
	return pcm16ToPcmu(pcm16)
}

// =============================================================================
// WhisperASR — audio/transcriptions REST
// =============================================================================

type WhisperASR struct {
	APIKey    string
	Model     string
	Endpoint  string
	Language  string
	TimeoutS  int
}

func (a *WhisperASR) Transcribe(pcmu []byte) string {
	if len(pcmu) == 0 || a.APIKey == "" {
		return ""
	}

	wav := pcmuToWavPcm16(pcmu, 8000)

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
	Model           string
	Endpoint        string
	APIKey          string
	SystemPrompt    string
	TransferDest    string
	TransferKeywords []string
	TimeoutS        int
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
// PCM16 ↔ PCMU + PCMU → WAV 辅助（G.711 μ-law，纯 stdlib）
// =============================================================================

// pcm16ToPcmu — 16-bit signed PCM16 LE → 8-bit unsigned PCMU (G.711 μ-law)
// 16kHz PCM16 downsample 2x → 8kHz，同时量化
func pcm16ToPcmu(pcm16 []byte) []byte {
	nSamples := len(pcm16) / 4 // 每 4 字节 = 2 个 PCM16 样本（降采样）
	out := make([]byte, 0, nSamples)
	for i := 0; i < nSamples; i++ {
		off := i * 4
		s16 := int16(binary.LittleEndian.Uint16(pcm16[off:]))
		out = append(out, pcm16ToUlaw(int(s16)))
	}
	return out
}

func pcm16ToUlaw(sample int) byte {
	sign := byte(0)
	if sample < 0 {
		sign = 0x80
		sample = -sample
	}
	if sample > 32635 {
		sample = 32635
	}
	sample += 0x84
	exponent := 7
	for i := 0; i < 7; i++ {
		if (sample & (0x4000 >> i)) != 0 {
			exponent = 6 - i
			break
		}
	}
	mantissa := (sample >> (exponent + 3)) & 0x0F
	ulaw := byte(^((sign) | (byte(exponent) << 4) | byte(mantissa)) & 0xFF)
	return ulaw
}

// pcmuToWavPcm16 — PCMU 8kHz → 44 字节 WAV header + PCM16 LE 8kHz raw
func pcmuToWavPcm16(pcmu []byte, sampleRate int) []byte {
	nSamples := len(pcmu)
	pcm16 := make([]byte, nSamples*2)
	for i, u := range pcmu {
		s16 := ulawToPcm16(u)
		binary.LittleEndian.PutUint16(pcm16[i*2:], uint16(int16(s16)))
	}

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

func ulawToPcm16(ulaw byte) int {
	ulaw = ^ulaw
	sign := int((ulaw & 0x80) >> 7)
	exponent := int((ulaw >> 4) & 0x07)
	mantissa := int(ulaw & 0x0F)
	sample := (mantissa << 3) | 0x84
	if exponent > 0 {
		sample <<= exponent
	}
	sample -= 0x84
	if sign != 0 {
		sample = -sample
	}
	sample <<= 2
	if sample > 32767 {
		sample = 32767
	}
	if sample < -32768 {
		sample = -32768
	}
	return sample
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

