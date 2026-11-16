/* ai_agent_external.go — REQ-AI-MEDIA-001 + REQ-AI-BARGEIN-001: External AI Agent（Go 版）
 *
 * 纯 stdlib，零第三方依赖。套 PROTOCOL.md §4 命令字典：
 *   ai-set-producer / ai-vad-state / ai-request-transfer / ai-get-rx / ai-send-tx
 *
 * 和 go/main.go 是两个独立 entry point：go run main.go 跑常规 event_client，
 * go run ai_agent_external.go 跑 AI producer。
 *
 * 用法：
 *   go run ai_agent_external.go --api-base https://nsbc-web.local:8080 \
 *       --api-token <Admin-ext-key> --call-id <call_id> --transfer-dest 1001 */

package main

import (
	"bytes"
	"crypto/tls"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

// ---------------------------------------------------------------------------
// HTTP helper: POST /ext/v1/cmd → transparent proxy to nsbcd OAM
// ---------------------------------------------------------------------------

func sslTransport() *http.Transport {
	return &http.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
	}
}

func cmd(apiBase, apiToken, command string) (int, string) {
	url := strings.TrimRight(apiBase, "/") + "/ext/v1/cmd"
	body, _ := json.Marshal(map[string]string{"cmd": command})
	req, _ := http.NewRequest("POST", url, bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "text/plain")
	req.Header.Set("Authorization", "Bearer "+apiToken)
	client := &http.Client{Transport: sslTransport(), Timeout: 10 * time.Second}
	resp, err := client.Do(req)
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

// ---------------------------------------------------------------------------
// AI pass-through API
// ---------------------------------------------------------------------------

const maxCmdLine = 4096

func aiSetProducer(apiBase, apiToken, callID string, producer int) bool {
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-set-producer %s %d", callID, producer))
	fmt.Printf("[ai] set_producer call=%s producer=%d → %s\n", callID, producer, out[:min(len(out), 150)])
	return checkOK(rc, out)
}

type vadStateResp struct {
	aState string
	aRms   int
	bState string
	bRms   int
}

func aiVadState(apiBase, apiToken, callID string) vadStateResp {
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-vad-state %s", callID))
	if rc != 0 {
		return vadStateResp{aState: "SILENCE", bState: "SILENCE"}
	}
	r := vadStateResp{aState: "SILENCE", bState: "SILENCE"}
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
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-request-transfer %s %s", callID, dest))
	fmt.Printf("[ai] ★ request_transfer call=%s dest=%s → %s\n", callID, dest, out[:min(len(out), 120)])
	return checkOK(rc, out)
}

func aiHangup(apiBase, apiToken, callID string) bool {
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("uuid-kill %s", callID))
	fmt.Printf("[ai] ★ hangup call=%s → %s\n", callID, out[:min(len(out), 120)])
	return checkOK(rc, out)
}

func aiGetRxAudio(apiBase, apiToken, callID string, maxBytes int) []byte {
	rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-get-rx %s %d", callID, maxBytes))
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

func aiSendTxAudio(apiBase, apiToken, callID string, pcmBytes []byte, formatPCM16 bool) bool {
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
		rc, out := cmd(apiBase, apiToken, fmt.Sprintf("ai-send-tx %s %d %s", callID, fmtNum, b64))
		if !checkOK(rc, out) {
			fmt.Fprintf(os.Stderr, "[ai] send_tx FAILED chunk=%d rc=%d out=%s\n", i, rc, out[:min(len(out), 100)])
			return false
		}
	}
	return true
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// ---------------------------------------------------------------------------
// Mock AI pipeline（生产环境替换成真服务）
// ---------------------------------------------------------------------------

var keywordsTransfer = []string{"转人工", "人工", "客服", "agent", "human", "operator"}
var keywordsHangup = []string{"挂断", "再见", "拜拜", "hangup", "bye"}

func mockASR(pcmu []byte) string {
	for _, b := range pcmu {
		if b > 24 {
			return "我想转人工"
		}
	}
	return ""
}

type llmReply struct {
	Action string `json:"action"`
	Dest   string `json:"dest,omitempty"`
	Text   string `json:"text,omitempty"`
	Reason string `json:"reason,omitempty"`
}

func mockLLM(transcript string) llmReply {
	if transcript == "" {
		return llmReply{Action: "silence"}
	}
	lowered := strings.ToLower(transcript)
	for _, kw := range keywordsTransfer {
		if strings.Contains(lowered, strings.ToLower(kw)) {
			return llmReply{Action: "transfer", Dest: "1001", Reason: "keyword=" + kw}
		}
	}
	for _, kw := range keywordsHangup {
		if strings.Contains(lowered, strings.ToLower(kw)) {
			return llmReply{Action: "hangup", Reason: "keyword=" + kw}
		}
	}
	return llmReply{Action: "say", Text: fmt.Sprintf("我收到了您说的：%s。请问还需要什么帮助吗？", transcript[:min(len(transcript), 40)])}
}

func mockTTS(text string) []byte {
	if text == "" {
		return nil
	}
	return make([]byte, 16000) // 2s 静音 PCMU
}

// ---------------------------------------------------------------------------
// Barge-In controller（REQ-AI-BARGEIN-001）
// ---------------------------------------------------------------------------

type bargeIn struct {
	apiBase  string
	apiToken string
	callID   string
	rms      int
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

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	apiBase := flag.String("api-base", "https://nsbc-web.local:8080", "nsbc-web 基址")
	apiToken := flag.String("api-token", "", "Admin scope ext_key")
	callID := flag.String("call-id", "", "要接管的 call_id")
	transferDest := flag.String("transfer-dest", "1001", "transfer 默认目标")
	bargeInEnabled := flag.Bool("vad-barge-in", true, "启用 barge-in")
	bargeInRMS := flag.Int("barge-in-rms", 800, "barge-in VAD RMS 阈值")
	chunkBytes := flag.Int("chunk-bytes", 160, "ai-get-rx 每次拉多少字节")
	flag.Parse()

	if *apiToken == "" || *callID == "" {
		flag.Usage()
		os.Exit(1)
	}

	fmt.Printf("\n%s\n", strings.Repeat("=", 70))
	fmt.Printf("[ai_agent Go] 接管 call=%s  transfer_dest=%s\n", *callID, *transferDest)
	fmt.Printf("  barge_in=%v  chunk_bytes=%d\n", *bargeInEnabled, *chunkBytes)
	fmt.Printf("%s\n\n", strings.Repeat("=", 70))

	if !aiSetProducer(*apiBase, *apiToken, *callID, 1) {
		fmt.Fprintln(os.Stderr, "[ai_agent] producer 设置失败")
		os.Exit(1)
	}
	defer aiSetProducer(*apiBase, *apiToken, *callID, 0)

	var barge *bargeIn
	if *bargeInEnabled {
		barge = newBargeIn(*apiBase, *apiToken, *callID, *bargeInRMS)
	}

	if welcome := mockTTS("您好，我是 AI 助理。请问需要什么帮助？"); len(welcome) > 0 {
		fmt.Printf("[ai_agent] → TTS 欢迎语 (%d bytes)\n", len(welcome))
		aiSendTxAudio(*apiBase, *apiToken, *callID, welcome, false)
	}
	fmt.Println("[ai_agent] 循环开始 — Ctrl+C 退出")
	fmt.Println()

	for {
		rx := aiGetRxAudio(*apiBase, *apiToken, *callID, *chunkBytes)
		if len(rx) == 0 {
			time.Sleep(50 * time.Millisecond)
			continue
		}

		if barge != nil {
			barge.waitUntilSilence(2 * time.Second)
		}

		transcript := mockASR(rx)
		if transcript == "" {
			continue
		}
		fmt.Printf("[ai_agent] ASR: %s\n", transcript)

		reply := mockLLM(transcript)
		switch reply.Action {
		case "transfer":
			dest := reply.Dest
			if dest == "" {
				dest = *transferDest
			}
			fmt.Printf("[ai_agent] ★ LLM function call: transfer → dest=%s  reason=%s\n", dest, reply.Reason)
			aiRequestTransfer(*apiBase, *apiToken, *callID, dest)
			return
		case "hangup":
			fmt.Printf("[ai_agent] ★ LLM function call: hangup  reason=%s\n", reply.Reason)
			aiHangup(*apiBase, *apiToken, *callID)
			return
		case "say":
			fmt.Printf("[ai_agent] LLM say: %s\n", reply.Text)
			if barge != nil {
				barge.waitUntilSilence(1 * time.Second)
			}
			if pcm := mockTTS(reply.Text); len(pcm) > 0 {
				aiSendTxAudio(*apiBase, *apiToken, *callID, pcm, false)
			}
		}
	}
}
