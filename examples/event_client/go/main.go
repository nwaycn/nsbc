// NSBC Event/CDR subscription & originate client (Go CLI demo).
//
// Thin shell — all real API lives in ./nsbc_sdk. This file is ONLY
// flag parsing + mode dispatch + pretty printing.
package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"event_client/nsbc_sdk"
)

// Terminal colors (disabled when stdout is not a tty).
var useColor = isatty(os.Stdout.Fd())

var colorMap = map[string]string{
	"CALL_INCOMING":   "\033[36m",
	"CALL_ANSWERED":   "\033[32m",
	"CALL_END":        "\033[31m",
	"REGISTER":        "\033[33m",
	"CDR":             "\033[35m",
}

const (
	reset = "\033[0m"
	dim   = "\033[2m"
	bold  = "\033[1m"
)

func isatty(fd uintptr) bool {
	fi, _ := os.Stdout.Stat()
	return (fi.Mode() & os.ModeCharDevice) != 0
}

func c(code string) string {
	if useColor {
		return code
	}
	return ""
}

func banner(text string) {
	fmt.Fprintf(os.Stdout, "%s%s%s\n", c(dim), text, c(reset))
}

func printFrame(f *nsbc_sdk.Frame) {
	shortTS := f.Ts
	if len(f.Ts) >= 23 {
		shortTS = f.Ts[11:23]
	}
	cc := colorMap[f.Domain]
	if cc == "" {
		cc = "\033[37m"
	}

	if f.Domain == "CDR" {
		callID := strField(f.Fields, "call_id")
		from := strField(f.Fields, "from")
		to := strField(f.Fields, "to")
		disp := strField(f.Fields, "disposition")
		dur := strField(f.Fields, "duration_s")
		bill := strField(f.Fields, "billsec")
		fmt.Fprintf(os.Stdout, "%s %s%12s%s call_id=%s %s→%s disp=%s dur=%ss bill=%ss\n",
			shortTS, c(cc), "CDR", c(reset), callID, from, to, disp, dur, bill)
		return
	}

	// Events: show prioritized fields first
	var parts []string
	for _, k := range []string{"call_id", "orig", "to", "src_ip", "trunk", "profile", "hangup_by"} {
		if v, ok := f.Fields[k]; ok {
			parts = append(parts, fmt.Sprintf("%s=%v", k, v))
		}
	}
	for k, v := range f.Fields {
		skip := false
		for _, s := range []string{"call_id", "orig", "to", "src_ip", "trunk", "profile", "hangup_by"} {
			if k == s {
				skip = true
				break
			}
		}
		if !skip {
			parts = append(parts, fmt.Sprintf("%s=%v", k, v))
		}
	}
	body := strings.Join(parts, " ")
	if body == "" {
		body = f.Raw
	}
	fmt.Fprintf(os.Stdout, "%s %s%15s%s %s\n", shortTS, c(cc+f.Domain), f.Domain, c(reset), body)
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

func printTimeline(tl map[string]interface{}) {
	callID, _ := tl["call_id"].(string)
	fmt.Println()
	fmt.Fprintf(os.Stdout, "%s══════ Call Lifecycle: %s ══════%s\n", c(bold), callID, c(reset))
	if events, ok := tl["events"].([]interface{}); ok {
		for _, evI := range events {
			ev, _ := evI.(map[string]interface{})
			domain, _ := ev["domain"].(string)
			ts, _ := ev["ts"].(string)
			cc := colorMap[domain]
			if cc == "" {
				cc = "\033[37m"
			}
			fmt.Fprintf(os.Stdout, "  %s%15s%s  %s\n", c(cc), domain, c(reset), ts)
		}
	}
	if cdr, ok := tl["cdr"].(map[string]interface{}); ok && cdr != nil {
		fmt.Fprintf(os.Stdout, "  %s%12s%s  CDR record\n", c(colorMap["CDR"]), "CDR", c(reset))
		fmt.Fprintf(os.Stdout, "    %s → %s  disp=%s  dur=%ss  bill=%ss\n",
			strField(cdr, "from"), strField(cdr, "to"),
			strField(cdr, "disposition"), strField(cdr, "duration_s"), strField(cdr, "billsec"))
	}
	fmt.Fprintf(os.Stdout, "%s──────────────────────────────────%s\n", c(bold), c(reset))
	fmt.Println()
}

func main() {
	// Flags
	mode := flag.String("mode", "auto", "auto|originate+subscribe|subscribe|originate")
	uds := flag.String("uds", nsbc_sdk.DefaultUds, "Unix Domain Socket path")
	apiBase := flag.String("api-base", nsbc_sdk.DefaultApiBase, "nsbc-web HTTP base URL")
	sseUrl := flag.String("sse-url", nsbc_sdk.DefaultSseUrl, "SSE stream URL (for --transport sse)")
	transport := flag.String("transport", "uds", "subscribe transport: uds|sse")
	topics := flag.String("topics", nsbc_sdk.DefaultTopics, "subscribe topics (comma-separated)")
	origVia := flag.String("orig-via", "auto", "originate path: auto|uds|web")
	callUrl := flag.String("call-url", "", "explicit call_url (overrides orig-to/app)")
	origTo := flag.String("orig-to", "1000", "destination extension")
	app := flag.String("app", "echo", "post-originate app: echo|park|hangup")
	callerID := flag.String("caller-id", "", "origination caller ID number")
	callerName := flag.String("caller-name", "", "origination caller ID name")
	timeout := flag.Int("timeout", 0, "ringing timeout seconds")
	video := flag.Bool("video", true, "enable video (default: H264,VP8)")
	audio := flag.Bool("audio", true, "enable multi-audio (default: PCMU,PCMA,G722,OPUS)")
	insecure := flag.Bool("insecure", false, "disable TLS cert verification")
	apiToken := flag.String("api-token", "", "Bearer API Key for HTTP originate / SSE")
	webhookURL := flag.String("webhook-url", "", "POST each event to this URL")
	exitOnCdr := flag.Bool("exit-on-cdr", false, "exit after first CDR")
	flag.Parse()

	if *insecure {
		nsbc_sdk.SetSSLVerify(false)
	}

	// Build originate options (used in originate+subscribe / originate modes)
	opts := nsbc_sdk.OriginateOpts{
		OrigTo:     *origTo,
		App:        *app,
		Timeout:    *timeout,
		CallerID:   *callerID,
		CallerName: *callerName,
		CallUrl:    *callUrl,
	}
	if *audio {
		opts.Audio = []string{"PCMU", "PCMA", "G722", "OPUS"}
	}
	if *video {
		opts.Video = []string{"H264", "VP8"}
	}

	// Mode dispatch
	if *mode == "originate" {
		runOriginate(opts, *origVia, *uds, *apiBase, *apiToken)
		return
	}
	if *mode == "subscribe" {
		runSubscribe(*uds, *sseUrl, *apiToken, *topics, *transport, *webhookURL, *exitOnCdr)
		return
	}
	// default: originate+subscribe
	runOriginateAndSubscribe(opts, *origVia, *uds, *sseUrl, *apiBase, *apiToken, *topics, *transport, *webhookURL, *exitOnCdr)
}

func runOriginate(opts nsbc_sdk.OriginateOpts, via, udsPath, apiBase, apiToken string) {
	callUrl := nsbc_sdk.BuildCallUrl(opts)
	banner(fmt.Sprintf("[originate via=%s  call_url=%s]", via, callUrl))
	r := nsbc_sdk.Originate(callUrl, via, udsPath, apiBase, apiToken)
	if r.OK {
		fmt.Fprintf(os.Stdout, "[OK] via=%s → %v\n", r.Via, r.Payload)
	} else {
		fmt.Fprintf(os.Stderr, "[FAIL] via=%s %v\n", r.Via, r.Payload)
		os.Exit(3)
	}
}

func runSubscribe(udsPath, sseUrl, apiToken, topics, transport, webhookURL string, exitOnCdr bool) {
	runTransportLoop(udsPath, sseUrl, apiToken, topics, transport, webhookURL, exitOnCdr)
}

func runOriginateAndSubscribe(opts nsbc_sdk.OriginateOpts, via, udsPath, sseUrl, apiBase, apiToken, topics, transport, webhookURL string, exitOnCdr bool) {
	callUrl := nsbc_sdk.BuildCallUrl(opts)
	banner(fmt.Sprintf("[1/2] originate via=%s  call_url=%s]", via, callUrl))
	r := nsbc_sdk.Originate(callUrl, via, udsPath, apiBase, apiToken)
	if !r.OK {
		fmt.Fprintf(os.Stderr, "[FAIL] originate %v\n", r.Payload)
		os.Exit(3)
	}
	fmt.Fprintf(os.Stdout, "[OK] originate dispatched via %s → %v\n", r.Via, r.Payload)
	banner("[2/2] subscribing — waiting for events ...")
	runTransportLoop(udsPath, sseUrl, apiToken, topics, transport, webhookURL, exitOnCdr)
}

func runTransportLoop(udsPath, sseUrl, apiToken, topics, transport, webhookURL string, exitOnCdr bool) {
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	frameCh := make(chan *nsbc_sdk.Frame, 128)

	switch transport {
	case "sse":
		var headers map[string]string
		if apiToken != "" {
			headers = map[string]string{"Authorization": "Bearer " + apiToken}
		}
		client := nsbc_sdk.NewSseClient(sseUrl, 30, headers)
		if err := client.Connect(); err != nil {
			fmt.Fprintf(os.Stderr, "[FAIL] SSE %s\n", err)
			os.Exit(2)
		}
		defer client.Close()
		banner(fmt.Sprintf("[transport=SSE, url=%s — Ctrl-C to quit]", sseUrl))
		rawCh := make(chan nsbc_sdk.SseEvent, 64)
		go client.IterEvents(rawCh)
		go func() {
			for ev := range rawCh {
				if f := nsbc_sdk.SseEventToFrame(ev); f != nil {
					select {
					case frameCh <- f:
					default:
					}
				}
			}
			close(frameCh)
		}()
	default:
		if _, err := os.Stat(udsPath); err != nil {
			fmt.Fprintf(os.Stderr, "[FAIL] UDS not found: %s (try --transport sse)\n", udsPath)
			os.Exit(2)
		}
		client := nsbc_sdk.NewClientUds(udsPath)
		if err := client.Connect(); err != nil {
			fmt.Fprintf(os.Stderr, "[FAIL] %v\n", err)
			os.Exit(2)
		}
		defer client.Close()
		banner(client.Banner())
		if resp, err := client.Subscribe(topics); err == nil {
			for _, ln := range strings.Split(resp, "\n") {
				ln = strings.TrimSpace(ln)
				if ln != "" {
					banner("[sub] " + ln)
				}
			}
		}
		banner(fmt.Sprintf("[transport=UDS, topics='%s' — Ctrl-C to quit]", topics))
		go client.IterFrames(frameCh)
	}

	correlator := nsbc_sdk.NewCallCorrelator(printTimeline)
	var webhook *nsbc_sdk.WebhookSink
	if webhookURL != "" {
		webhook = nsbc_sdk.NewWebhookSink(webhookURL, 5, 2)
		banner("[webhook] every event → POST " + webhookURL)
	}

	for {
		select {
		case <-sigCh:
			fmt.Fprintf(os.Stderr, "\n[quit]\n")
			if webhook != nil {
				ok, fail := webhook.Stats()
				banner(fmt.Sprintf("[webhook stats] ok=%d fail=%d", ok, fail))
			}
			return
		case f, ok := <-frameCh:
			if !ok {
				banner("[transport closed]")
				return
			}
			printFrame(f)
			correlator.Feed(f)
			if webhook != nil {
				webhook.Send(f)
			}
			if exitOnCdr && f.Domain == "CDR" {
				time.Sleep(100 * time.Millisecond)
				return
			}
		}
	}
}
