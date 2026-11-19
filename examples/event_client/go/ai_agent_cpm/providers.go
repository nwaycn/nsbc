package main

/*
 * ai_agent_cpm — MiniCPM-o 4.5 一体化语音 provider
 *
 * 架构：
 *   sentenceBuf (PCM16 LE 8kHz, int16)
 *       │
 *       ├─ upsample8k16kFloat32 → float32 16kHz
 *       ├─ Ollama /api/chat (model=openbmb/minicpm-o4.5, audio content)
 *       │
 *       ├─ 返回 text: 匹配 keyword → transfer/hangup/say
 *       └─ 返回 audio (float32 16kHz) → downsample16k8k → PCM16 LE 8kHz → sendAudioPaced
 *
 * 依赖：
 *   Ollama 同机运行 + ollama pull openbmb/minicpm-o4.5
 *   CPU 推理 ~3-5s (10.0.0.80 需要 2GB+ swap)
 */

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"strings"
	"time"
)

// LLMReply — 统一返回（文字决策 + 可选 TTS 音频）
type LLMReply struct {
	Text         string // 文字回复（用于 action 决策 + 日志）
	Action       string // "say" / "transfer" / "hangup"
	TransferDest string
	AudioPCM16   []byte // ★ MiniCPM-o 原生 TTS 输出，PCM16 LE 8kHz int16
}

// LLMProvider — CPM 版用 ChatAudio 一体化
type LLMProvider interface {
	ChatAudio(ctx context.Context, pcm16_8k []byte) (*LLMReply, error)
}

// ========== PCM16 ↔ float32 ↔ 重采样 ==========

// PCM16Int16LEToFloat32 — int16 LE 字节 → float32 [-1.0, 1.0]
func PCM16Int16LEToFloat32(pcm16LE []byte) []float32 {
	n := len(pcm16LE) / 2
	out := make([]float32, n)
	for i := 0; i < n; i++ {
		s := int16(binary.LittleEndian.Uint16(pcm16LE[i*2:]))
		out[i] = float32(s) / 32768.0
	}
	return out
}

// Float32ToPCM16Int16LE — float32 [-1.0, 1.0] → int16 LE 字节
func Float32ToPCM16Int16LE(samples []float32) []byte {
	out := make([]byte, len(samples)*2)
	for i, v := range samples {
		if v > 1.0 {
			v = 1.0
		}
		if v < -1.0 {
			v = -1.0
		}
		s := int16(v * 32767.0)
		binary.LittleEndian.PutUint16(out[i*2:], uint16(s))
	}
	return out
}

// Upsample8kTo16kFloat32 — 8kHz float32 → 16kHz float32（每个样本复制 2 次）
func Upsample8kTo16kFloat32(in []float32) []float32 {
	out := make([]float32, len(in)*2)
	for i, s := range in {
		out[i*2] = s
		out[i*2+1] = s
	}
	return out
}

// Downsample16kTo8kFloat32 — 16kHz float32 → 8kHz float32（每 2 取 1）
func Downsample16kTo8kFloat32(in []float32) []float32 {
	out := make([]float32, len(in)/2)
	for i := range out {
		out[i] = in[i*2]
	}
	return out
}

// ========== Ollama MiniCPM-o 多模态 Provider ==========

type CPMOllamaProvider struct {
	Endpoint     string   // e.g. http://127.0.0.1:11434
	Model        string   // e.g. openbmb/minicpm-o4.5
	SystemPrompt string   // AI persona
	TransferDest string   // 转人工默认目标
	TransferKw   []string // 转人工关键词
	HangupKw     []string // 挂断关键词
	Client       *http.Client
}

func NewCPMOllamaProvider(endpoint, model, systemPrompt, transferDest string,
	transferKw, hangupKw []string) *CPMOllamaProvider {
	if endpoint == "" {
		endpoint = "http://127.0.0.1:11434"
	}
	if model == "" {
		model = "openbmb/minicpm-o4.5"
	}
	if systemPrompt == "" {
		systemPrompt = "你是 NSBC 智能客服助手，名字叫小冰。" +
			"请用中文简洁回答用户问题，保持对话自然。" +
			"如果用户说'转人工'或'帮我转接'，请回复'好的，正在为您转接人工客服'。" +
			"如果用户说'再见'或'挂断'，请礼貌告别。"
	}
	return &CPMOllamaProvider{
		Endpoint:     endpoint,
		Model:        model,
		SystemPrompt: systemPrompt,
		TransferDest: transferDest,
		TransferKw:   transferKw,
		HangupKw:     hangupKw,
		Client:       &http.Client{Timeout: 120 * time.Second},
	}
}

// ChatAudio — MiniCPM-o 一体化语音交互
// 输入: PCM16 LE 8kHz int16 字节流
// 输出: LLMReply（文字决策 + 可选 TTS 音频）
func (p *CPMOllamaProvider) ChatAudio(ctx context.Context, pcm16_8k []byte) (*LLMReply, error) {
	if len(pcm16_8k) == 0 {
		return &LLMReply{Action: "say", Text: ""}, nil
	}

	// --- Step 1: 8k PCM16 int16 → 16k float32 ---
	f32Samples := PCM16Int16LEToFloat32(pcm16_8k)
	f32_16k := Upsample8kTo16kFloat32(f32Samples)

	// float32 raw bytes
	f32Bytes := make([]byte, len(f32_16k)*4)
	for i, v := range f32_16k {
		binary.LittleEndian.PutUint32(f32Bytes[i*4:], math.Float32bits(v))
	}

	// --- Step 2: 构造 Ollama 请求 ---
	audioDataB64 := base64.StdEncoding.EncodeToString(f32Bytes)

	reqBody := map[string]interface{}{
		"model":  p.Model,
		"stream": false,
		"messages": []map[string]interface{}{
			{
				"role":    "system",
				"content": p.SystemPrompt,
			},
			{
				"role": "user",
				"content": []map[string]interface{}{
					{
						"type": "audio",
						"audio": map[string]interface{}{
							"data":        audioDataB64,
							"sample_rate": 16000,
						},
					},
				},
			},
		},
	}

	jsonBytes, err := json.Marshal(reqBody)
	if err != nil {
		return nil, fmt.Errorf("marshal req: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, "POST", p.Endpoint+"/api/chat",
		bytes.NewReader(jsonBytes))
	if err != nil {
		return nil, fmt.Errorf("new request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")

	fmt.Printf("[CPM] → POST %s/api/chat model=%s audio=%d bytes (16k float32 %d ms)\n",
		p.Endpoint, p.Model, len(f32Bytes), len(f32_16k)/16000*1000)

	t0 := time.Now()
	resp, err := p.Client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("ollama do: %w", err)
	}
	defer resp.Body.Close()

	body, _ := io.ReadAll(resp.Body)
	elapsed := time.Since(t0)

	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("ollama %s (%v): %s", resp.Status, elapsed,
			string(body[:minInt(len(body), 500)]))
	}

	// --- Step 3: 解析返回 ---
	var raw map[string]interface{}
	if err := json.Unmarshal(body, &raw); err != nil {
		return nil, fmt.Errorf("unmarshal: %w (body=%s)", err, string(body[:minInt(len(body), 300)]))
	}

	// 优先取 message.content，兼容 response 字段
	text := ""
	audioB64 := ""
	audioSR := 0

	if msg, ok := raw["message"].(map[string]interface{}); ok {
		if c, ok := msg["content"].(string); ok {
			text = c
		}
		if aud, ok := msg["audio"].(map[string]interface{}); ok {
			if d, ok := aud["data"].(string); ok {
				audioB64 = d
			}
			if sr, ok := aud["sample_rate"].(float64); ok {
				audioSR = int(sr)
			}
		}
	}
	if text == "" {
		if r, ok := raw["response"].(string); ok {
			text = r
		}
	}

	text = strings.TrimSpace(text)
	fmt.Printf("[CPM] ← text=%q audio=%s elapsed=%v\n", text,
		func() string {
			if audioB64 != "" {
				return fmt.Sprintf("yes (sr=%d, %d b64 chars)", audioSR, len(audioB64))
			}
			return "no"
		}(), elapsed)

	// --- Step 4: keyword 匹配 → action ---
	reply := &LLMReply{Text: text}
	for _, kw := range p.TransferKw {
		if strings.Contains(text, kw) {
			reply.Action = "transfer"
			reply.TransferDest = p.TransferDest
			break
		}
	}
	if reply.Action == "" {
		for _, kw := range p.HangupKw {
			if strings.Contains(text, kw) {
				reply.Action = "hangup"
				break
			}
		}
	}
	if reply.Action == "" {
		reply.Action = "say"
	}

	// --- Step 5: 解析音频输出 ---
	if audioB64 != "" {
		audioBytes, err := base64.StdEncoding.DecodeString(audioB64)
		if err != nil {
			fmt.Printf("[CPM] ⚠ audio base64 decode failed: %v\n", err)
		} else {
			if audioSR == 0 {
				audioSR = 16000
			}

			// 判断格式：如果是 .wav header 开头
			if len(audioBytes) > 12 && string(audioBytes[0:4]) == "RIFF" {
				fmt.Printf("[CPM] ★ audio is WAV, extracting PCM data...\n")
				// WAV data chunk 在 44 bytes header 之后（简单处理）
				if len(audioBytes) > 44 {
					audioBytes = audioBytes[44:]
				}
				// 假设 WAV 是 PCM16 int16（常见）
				// MiniCPM-o 也可能给 float32 — 这里按 sample_rate 决定
				if audioSR == 16000 {
					// 如果是 WAV PCM16 int16，每个样本 2 字节
					nSamples := len(audioBytes) / 2
					f32Arr := make([]float32, nSamples)
					for i := 0; i < nSamples; i++ {
						s := int16(binary.LittleEndian.Uint16(audioBytes[i*2:]))
						f32Arr[i] = float32(s) / 32768.0
					}
					f32_8k := Downsample16kTo8kFloat32(f32Arr)
					reply.AudioPCM16 = Float32ToPCM16Int16LE(f32_8k)
				}
			} else {
				// 直接 raw float32
				nF32 := len(audioBytes) / 4
				f32Arr := make([]float32, nF32)
				for i := 0; i < nF32; i++ {
					bits := binary.LittleEndian.Uint32(audioBytes[i*4:])
					f32Arr[i] = math.Float32frombits(bits)
				}

				var f32_8k []float32
				if audioSR == 16000 {
					f32_8k = Downsample16kTo8kFloat32(f32Arr)
				} else if audioSR == 8000 {
					f32_8k = f32Arr
				} else {
					ratio := float64(8000) / float64(audioSR)
					n8k := int(float64(len(f32Arr)) * ratio)
					f32_8k = make([]float32, n8k)
					for i := 0; i < n8k; i++ {
						src := int(float64(i) / ratio)
						if src < len(f32Arr) {
							f32_8k[i] = f32Arr[src]
						}
					}
				}
				reply.AudioPCM16 = Float32ToPCM16Int16LE(f32_8k)
			}

			if len(reply.AudioPCM16) > 0 {
				fmt.Printf("[CPM] ★ TTS → PCM16 8kHz %d bytes (%d ms)\n",
					len(reply.AudioPCM16), len(reply.AudioPCM16)/320*20)
			}
		}
	}

	return reply, nil
}

// ========== 工具 ==========

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
