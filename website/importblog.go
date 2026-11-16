//go:build ignore
// +build ignore

// importblog.go — 从 freeswitch.net.cn / docs.opensips.net.cn 批量抓取文章，
// 转成 Markdown 存到 content/freeswitch/ 或 content/opensips/。
//
// 用法：
//   go run importblog.go --domain freeswitch.net.cn --out content/freeswitch
//   go run importblog.go --domain docs.opensips.net.cn --out content/opensips
//   go run importblog.go --domain freeswitch.net.cn --out content/freeswitch --start 200 --end 263
//
// 依赖：golang.org/x/net/html（解析 HTML 结构）

package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/net/html"
)

var (
	domain   string
	outDir   string
	startID  int
	endID    int
	workers  int
	overwrite bool
)

func main() {
	flag.StringVar(&domain, "domain", "", "源站域名，如 freeswitch.net.cn")
	flag.StringVar(&outDir, "out", "", "输出目录，如 content/freeswitch")
	flag.IntVar(&startID, "start", 1, "文章 ID 起始")
	flag.IntVar(&endID, "end", 0, "文章 ID 结束（包含）；0 = 自动探测首页最大 ID")
	flag.IntVar(&workers, "workers", 8, "并发抓取数")
	flag.BoolVar(&overwrite, "force", false, "覆盖已存在文件")
	flag.Parse()

	if domain == "" || outDir == "" {
		fmt.Println("usage: go run importblog.go --domain freeswitch.net.cn --out content/freeswitch [--start 1 --end 263]")
		os.Exit(2)
	}

	base := "http://" + strings.TrimPrefix(domain, "http://")

	// 1. 探测文章 ID 范围
	if endID == 0 {
		log.Printf("探测 %s 最新文章 ID...", base)
		endID = probeMaxID(base)
		log.Printf("最新 ID = %d，将抓取 %d–%d（%d 篇）", endID, startID, endID, endID-startID+1)
	}

	os.MkdirAll(outDir, 0o755)

	// 2. 并发抓取
	var wg sync.WaitGroup
	sem := make(chan struct{}, workers)
	ok, fail, skip := 0, 0, 0

	httpCl := &http.Client{Timeout: 20 * time.Second}

	for id := startID; id <= endID; id++ {
		fname := filepath.Join(outDir, fmt.Sprintf("%d.md", id))
		if !overwrite {
			if _, err := os.Stat(fname); err == nil {
				skip++
				continue
			}
		}
		wg.Add(1)
		sem <- struct{}{}
		go func(id int) {
			defer wg.Done()
			defer func() { <-sem }()

			pageURL := fmt.Sprintf("%s/%d.html", base, id)
			doc, err := fetchAndParse(httpCl, pageURL)
			if err != nil {
				log.Printf("[%d] FAIL: %v", id, err)
				fail++
				return
			}
			if doc.title == "" {
				// 404 / 不存在的 ID
				return
			}
			md := renderMarkdown(doc, id, pageURL)
			if err := os.WriteFile(fname, []byte(md), 0o644); err != nil {
				log.Printf("[%d] write: %v", id, err)
				fail++
				return
			}
			log.Printf("[%d] OK: %s", id, doc.title)
			ok++
		}(id)
	}
	wg.Wait()
	log.Printf("完成：ok=%d  fail=%d  skip=%d", ok, fail, skip)
}

// probeMaxID 从首页侧边栏 "最新" 列表里找最大文章 ID。
// 文章页面 URL 是 1-3 位纯数字 + .html（如 263.html）；
// 月份归档是 6 位数字 .html（如 202604.html），应排除。
func probeMaxID(base string) int {
	resp, err := http.Get(base + "/")
	if err != nil {
		return 1
	}
	defer resp.Body.Close()
	z := html.NewTokenizer(resp.Body)
	maxID := 1
	for {
		tt := z.Next()
		if tt == html.ErrorToken {
			break
		}
		if tt != html.StartTagToken {
			continue
		}
		n, _ := z.TagName()
		if string(n) != "a" {
			continue
		}
		for {
			kn, kv, more := z.TagAttr()
			if string(kn) == "href" {
				href := string(kv)
				// 只匹配 "NNN.html" 形式（1-3 位纯数字，不含路径分隔符）
				if !strings.Contains(href, "/") && strings.HasSuffix(href, ".html") {
					idStr := strings.TrimSuffix(href, ".html")
					if len(idStr) >= 1 && len(idStr) <= 3 {
						if id, err := strconv.Atoi(idStr); err == nil && id > maxID {
							maxID = id
						}
					}
				}
			}
			if !more {
				break
			}
		}
	}
	return maxID
}

type article struct {
	title string
	date  string
	tags  string
	body  string // 正文 HTML（已清洗）
}

// fetchAndParse 抓一篇文章，提取 title / date / tags / 正文。
func fetchAndParse(cl *http.Client, pageURL string) (*article, error) {
	resp, err := cl.Get(pageURL)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}

	root, err := html.Parse(resp.Body)
	if err != nil {
		return nil, err
	}

	doc := &article{}

	// 1. 标题：<title> 标签
	walk(root, func(n *html.Node) {
		if n.Type == html.ElementNode && n.Data == "title" {
			doc.title = strings.TrimSpace(nodeText(n))
		}
	})

	// 2. 从 meta keywords + 页面里找 tags / date
	walk(root, func(n *html.Node) {
		if n.Type != html.ElementNode {
			return
		}
		switch n.Data {
		case "meta":
			for _, a := range n.Attr {
				if strings.EqualFold(a.Key, "name") && strings.EqualFold(a.Val, "keywords") {
					for _, b := range n.Attr {
						if b.Key == "content" {
							doc.tags = strings.TrimSpace(b.Val)
						}
					}
				}
			}
		}
	})

	// 3. 找正文区域（尝试几种常见 class/id）
	bodyNode := findBodyContainer(root)
	if bodyNode != nil {
		// 在正文里找 date 线索：格式 "创建时间：2025-12-18"
		bodyHTML := renderInnerHTML(bodyNode)
		doc.body = cleanHTML(bodyHTML)

		// 正则找 date 和 tags（源站常用格式：tags：xxx 创建时间：yyyy-mm-dd）
		reDate := regexp.MustCompile(`创建时间[：:]\s*(\d{4}[-/]\d{1,2}[-/]\d{1,2})`)
		if m := reDate.FindStringSubmatch(bodyHTML); m != nil {
			doc.date = strings.ReplaceAll(m[1], "/", "-")
		}
	}

	// 日期兜底：URL 里文章 ID 不够精确，但可以用 201509 归档方式
	if doc.date == "" {
		doc.date = "2000-01-01"
	}

	return doc, nil
}

// findBodyContainer 在 HTML 中找正文主区域：
// 按优先级尝试 id="content" / class="content" / id="main-content" / class="span9" / 最大的 div。
func findBodyContainer(root *html.Node) *html.Node {
	candidates := []struct {
		sel    func(*html.Node) bool
		pri    int
	}{
		{idIs("content"), 1},
		{classIs("content"), 2},
		{idIs("main-content"), 3},
		{classIs("article"), 4},
		{classContains("span9"), 5},
		{classContains("span8"), 6},
		{classContains("col-md-9"), 7},
		{classContains("col-md-8"), 8},
	}

	var best *html.Node
	bestPri := 99
	walk(root, func(n *html.Node) {
		if n.Type != html.ElementNode {
			return
		}
		for _, c := range candidates {
			if c.sel(n) && c.pri < bestPri {
				best = n
				bestPri = c.pri
			}
		}
	})

	// 如果没找到，fallback：body 下最大的 div
	if best == nil {
		walk(root, func(n *html.Node) {
			if n.Type == html.ElementNode && n.Data == "div" && n.Parent != nil && n.Parent.Data == "body" {
				if best == nil || len(renderInnerHTML(n)) > len(renderInnerHTML(best)) {
					best = n
				}
			}
		})
	}
	return best
}

func idIs(id string) func(*html.Node) bool {
	return func(n *html.Node) bool {
		for _, a := range n.Attr {
			if a.Key == "id" && a.Val == id {
				return true
			}
		}
		return false
	}
}
func classIs(cls string) func(*html.Node) bool {
	return func(n *html.Node) bool {
		for _, a := range n.Attr {
			if a.Key == "class" && a.Val == cls {
				return true
			}
		}
		return false
	}
}
func classContains(needle string) func(*html.Node) bool {
	return func(n *html.Node) bool {
		for _, a := range n.Attr {
			if a.Key == "class" && strings.Contains(a.Val, needle) {
				return true
			}
		}
		return false
	}
}

// renderMarkdown 把 article 转成最终 .md 文件内容。
func renderMarkdown(doc *article, id int, srcURL string) string {
	// 确保标题有 # 前缀
	title := doc.title
	if !strings.HasPrefix(title, "# ") {
		title = "# " + title
	}

	front := fmt.Sprintf(`---
title: %s
date: %s
tags: %s
source: %s
slug: %d
---

`,
		escapeYAML(doc.title),
		doc.date,
		normalizeTags(doc.tags),
		srcURL,
		id,
	)

	body := doc.body
	// 把正文里残留的导航/日期 meta 行过滤掉
	body = stripMetaLine(body)

	return front + title + "\n\n" + body + "\n"
}

// stripMetaLine 去掉源站那种 "tags：xxx 创建时间：yyyy-mm-dd" 的一行 meta。
func stripMetaLine(html string) string {
	re := regexp.MustCompile(`(?i)(tags?|标签)[：:][^\n]{0,80}(创建时间|发布时间|日期)[：:][^\n]{0,40}[\n]?`)
	return re.ReplaceAllString(html, "")
}

func escapeYAML(s string) string {
	s = strings.ReplaceAll(s, "\n", " ")
	s = strings.TrimSpace(s)
	if strings.ContainsAny(s, ":#{}[]&*?|>-\"'%@`") {
		return "\"" + strings.ReplaceAll(s, "\"", "\\\"") + "\""
	}
	return s
}

func normalizeTags(raw string) string {
	if raw == "" {
		return ""
	}
	raw = strings.ReplaceAll(raw, "，", ",")
	raw = strings.ReplaceAll(raw, " ", ",")
	var parts []string
	for _, p := range strings.Split(raw, ",") {
		p = strings.TrimSpace(p)
		if p != "" {
			parts = append(parts, p)
		}
	}
	return strings.Join(parts, ",")
}

// cleanHTML 简单清洗：去掉 script/style/iframe/noscript 标签块。
func cleanHTML(s string) string {
	for _, tag := range []string{"script", "style", "iframe", "noscript"} {
		re := regexp.MustCompile(`(?is)<` + tag + `[^>]*>.*?</` + tag + `>`)
		s = re.ReplaceAllString(s, "")
	}
	return s
}

// walk 遍历所有节点，对每个元素节点调用 fn。
func walk(n *html.Node, fn func(*html.Node)) {
	if n == nil {
		return
	}
	fn(n)
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		walk(c, fn)
	}
}

// nodeText 取节点下所有文本拼接。
func nodeText(n *html.Node) string {
	var sb strings.Builder
	var walkText func(*html.Node)
	walkText = func(x *html.Node) {
		if x.Type == html.TextNode {
			sb.WriteString(x.Data)
		}
		for c := x.FirstChild; c != nil; c = c.NextSibling {
			walkText(c)
		}
	}
	walkText(n)
	return sb.String()
}

// renderInnerHTML 把节点的子树序列化成 HTML 字符串。
func renderInnerHTML(n *html.Node) string {
	var buf strings.Builder
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		html.Render(&buf, c)
	}
	return buf.String()
}
