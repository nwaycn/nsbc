// NSBC 实时事件/CDR 订阅客户端 (Unix Domain Socket / Go)
//
// 纯 stdlib，无任何第三方依赖。go build 直接产出二进制。
//
// 用法:
//   go run main.go                                   # 默认 /tmp/nsbc-e2e/control.sock
//   go run main.go /var/run/nsbc/control.sock        # 自定义 UDS 路径
//   go run main.go --topics events,cdr,log
//   go build -o event_client . && ./event_client
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"os"
	"regexp"
	"strings"
	"time"
)

// 帧格式: YYYY-MM-DDTHH:MM:SS.mmm <DOMAIN> <body>
var frameRE = regexp.MustCompile(`^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3})\s+(\S+)\s+(.*)$`)

const (
	prompt = "nsbc> "
	banner = "NSBC "
)

var colorMap = map[string]string{
	"CALL_INCOMING": "\033[36m",
	"CALL_ANSWERED": "\033[32m",
	"CALL_END":      "\033[31m",
	"REGISTER":      "\033[33m",
	"CDR":           "\033[35m",
}

const reset = "\033[0m"

func color(domain string) string {
	if c, ok := colorMap[domain]; ok {
		return c
	}
	return "\033[37m"
}

func printFrame(line string) {
	line = strings.TrimRight(line, "\r\n")
	if line == "" {
		return
	}
	m := frameRE.FindStringSubmatch(line)
	if m == nil {
		if strings.HasPrefix(line, banner) {
			fmt.Fprintf(os.Stdout, "\033[2m[banner] %s%s\n", line, reset)
		} else if strings.HasPrefix(line, "OK ") || strings.HasPrefix(line, "ERR ") || strings.HasPrefix(line, "subscribed:") {
			fmt.Fprintf(os.Stdout, "\033[2m[cmd] %s%s\n", line, reset)
		}
		return
	}
	tsStr, domain, body := m[1], m[2], m[3]
	// 缩短显示时间: 15:32:01.123
	if ts, err := time.Parse("2006-01-02T15:04:05.000", tsStr); err == nil {
		tsStr = ts.Format("15:04:05.000")
	}
	if domain == "CDR" {
		var ev map[string]any
		if err := json.Unmarshal([]byte(body), &ev); err == nil {
			summary := fmt.Sprintf("call_id=%v %v→%v disp=%v dur=%vs bill=%vs",
				ev["call_id"], ev["from"], ev["to"], ev["disposition"], ev["duration_s"], ev["billsec"])
			fmt.Fprintf(os.Stdout, "%s %s%12s%s %s\n", tsStr, color("CDR"), "CDR", reset, summary)
		} else {
			fmt.Fprintf(os.Stdout, "%s %s%12s%s %s\n", tsStr, color("CDR"), "CDR", reset, truncate(body, 120))
		}
	} else {
		fmt.Fprintf(os.Stdout, "%s %s%15s%s %s\n", tsStr, color(domain), domain, reset, body)
	}
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}

// recvUntilPrompt: 读直到看到 nsbc> 提示符或整体超时。
// 每次 ReadString 前重置 deadline，避免中途超时导致已读部分丢失。
func recvUntilPrompt(c net.Conn, timeout time.Duration) string {
	deadline := time.Now().Add(timeout)
	var buf strings.Builder
	reader := bufio.NewReaderSize(c, 65536)
	for time.Now().Before(deadline) {
		c.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		line, err := reader.ReadString('\n')
		if line != "" {
			buf.WriteString(line)
			if strings.Contains(line, prompt) {
				break
			}
		}
		if err != nil {
			if nerr, ok := err.(net.Error); ok && nerr.Timeout() {
				continue // 局部超时，整体 deadline 还没到就继续
			}
			break // EOF / 其他错误
		}
	}
	c.SetReadDeadline(time.Time{})
	return buf.String()
}

func main() {
	var (
		udsPath  string
		topics   string
		pullOnly bool
	)
	flag.StringVar(&udsPath, "uds", "/tmp/nsbc-e2e/control.sock", "control socket path")
	flag.StringVar(&topics, "topics", "events,cdr", "订阅 topics，逗号分隔")
	flag.BoolVar(&pullOnly, "pull-only", false, "只连不订")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s [flags]\n\nflags:\n", os.Args[0])
		flag.PrintDefaults()
	}
	flag.Parse()
	if flag.NArg() > 0 {
		udsPath = flag.Arg(0)
	}

	// 检查 socket 文件存在
	if fi, err := os.Stat(udsPath); err != nil || (fi != nil && fi.Mode()&os.ModeSocket == 0) {
		fmt.Fprintf(os.Stderr, "ERR: UDS socket not found or not a socket: %s\n", udsPath)
		os.Exit(2)
	}

	c, err := net.DialTimeout("unix", udsPath, 5*time.Second)
	if err != nil {
		fmt.Fprintf(os.Stderr, "ERR dial: %v\n", err)
		os.Exit(1)
	}
	defer c.Close()

	fmt.Fprintf(os.Stdout, "\033[2m[connected to %s]%s\n", udsPath, reset)

	// 收 banner + 提示符
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	reader := bufio.NewReaderSize(c, 65536)
	for {
		line, err := reader.ReadString('\n')
		if line != "" {
			if strings.HasPrefix(line, banner) {
				fmt.Fprintf(os.Stdout, "\033[2m%s%s", line, reset)
			}
			if strings.HasSuffix(line, prompt) {
				break
			}
		}
		if err != nil {
			break
		}
	}
	c.SetReadDeadline(time.Time{})

	if !pullOnly {
		// 发 sub 命令
		_, err := fmt.Fprintf(c, "sub %s\n", topics)
		if err != nil {
			fmt.Fprintf(os.Stderr, "ERR send sub: %v\n", err)
			os.Exit(1)
		}
		resp := recvUntilPrompt(c, 2*time.Second)
		for _, ln := range strings.Split(resp, "\n") {
			ln = strings.TrimSpace(ln)
			if ln != "" && !strings.HasPrefix(ln, prompt) {
				fmt.Fprintf(os.Stdout, "\033[2m[sub] %s%s\n", ln, reset)
			}
		}
		fmt.Fprintf(os.Stdout, "\033[2m[subscribed: %s — Ctrl-C to quit]%s\n", topics, reset)
	}

	// 实时循环: 阻塞读 \n 分隔帧
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			if err.Error() != "EOF" {
				fmt.Fprintf(os.Stderr, "\n\033[31m[disconnected: %v]%s\n", err, reset)
			} else {
				fmt.Fprintf(os.Stderr, "\n\033[2m[server closed]%s\n", reset)
			}
			return
		}
		printFrame(line)
	}
}
