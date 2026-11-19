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
	body = stripMetaLine(body)
	body = polishMarkdown(body)

	return front + body + "\n"
}

// stripMetaLine 去掉源站那种 "tags：xxx 创建时间：yyyy-mm-dd" 的一行 meta。
func stripMetaLine(html string) string {
	re := regexp.MustCompile(`(?i)(tags?|标签)[：:][^\n]{0,80}(创建时间|发布时间|日期)[：:][^\n]{0,40}[\n]?`)
	return re.ReplaceAllString(html, "")
}

// polishMarkdown 最终润色：
//   - 去掉孤立的空格式化标记（**、*、`）
//   - 去掉多余的 ---（孤立 hr 一般来自页面分隔，不是正文）
//   - 去掉正文首行与 front matter title 重复的 H1
//   - 去掉尾部孤立联系信息（QQ群、作者署名）前后的空白
func polishMarkdown(s string) string {
	// 去掉孤立空格式化标记（行首/行尾只有 ** 或 * 或 `）
	s = regexp.MustCompile(`(?m)^\*\*\s*$`).ReplaceAllString(s, "")
	s = regexp.MustCompile(`(?m)^\*\s*$`).ReplaceAllString(s, "")
	s = regexp.MustCompile(`(?m)^` + "`" + `\s*$`).ReplaceAllString(s, "")
	// 孤立 --- 行（上下文空）
	s = regexp.MustCompile(`\n\n---\n\n`).ReplaceAllString(s, "\n\n")
	s = regexp.MustCompile(`(?m)^---\s*$\n`).ReplaceAllString(s, "")
	// 重复 H1 相邻
	s = regexp.MustCompile(`\n# [^\n]+\n\n# ([^\n]+)\n`).ReplaceAllString(s, "\n# $1\n")
	// 压缩空行
	s = regexp.MustCompile(`\n{3,}`).ReplaceAllString(s, "\n\n")
	s = strings.TrimSpace(s)
	return s
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

// cleanHTML 清洗抓取到的正文 HTML：
//   1. 剥 script/style/iframe/noscript 标签块
//   2. 剥 HTML 注释 <!-- ... -->
//   3. <br> → \n，<hr> → \n\n---\n\n
//   4. 内部文章链接 <a href="NNN.html"> → 保留文本；其余 <a href> 转 md 链接
//   5. <img src=url alt=text> → ![text](url)
//   6. <h1-6>、<blockquote>、<ul>/<ol>/<li>、<strong>/<b>、<em>/<i>、<code>/<pre> → md 语法
//   7. 剥剩余 HTML 标签，还原实体
//
// 结果：纯 Markdown 正文，段落间有正确空行，可直接喂 goldmark。
func cleanHTML(s string) string {
	// 1. script/style/iframe/noscript
	for _, tag := range []string{"script", "style", "iframe", "noscript"} {
		re := regexp.MustCompile(`(?is)<` + tag + `[^>]*>.*?</` + tag + `>`)
		s = re.ReplaceAllString(s, "")
	}
	// 2. 注释
	commentRe := regexp.MustCompile(`(?s)<!--.*?-->`)
	s = commentRe.ReplaceAllString(s, "")

	// 3. <br> → \n，<hr> → md hr
	s = regexp.MustCompile(`(?i)<br\s*/?>`).ReplaceAllString(s, "\n")
	s = regexp.MustCompile(`(?i)<hr\s*/?>`).ReplaceAllString(s, "\n\n---\n\n")

	// 4. 内部文章链接 → 保留文本（站外链接也要保留）
	linkRe := regexp.MustCompile(`(?is)<a\s+href="\d+\.html"[^>]*>(.*?)</a>`)
	s = linkRe.ReplaceAllString(s, "$1")

	// 5. <img> → ![alt](src)
	imgRe := regexp.MustCompile(`(?is)<img\s+([^>]*)>`)
	s = imgRe.ReplaceAllStringFunc(s, func(m string) string {
		src := regexp.MustCompile(`(?is)src=["']([^"']+)["']`).FindStringSubmatch(m)
		alt := regexp.MustCompile(`(?is)alt=["']([^"']*)["']`).FindStringSubmatch(m)
		if len(src) < 2 {
			return ""
		}
		altText := ""
		if len(alt) >= 2 {
			altText = alt[1]
		}
		return "![" + altText + "](" + src[1] + ")"
	})

	// 6. 块级元素 → md 段落分隔
	s = regexp.MustCompile(`(?i)</p>`).ReplaceAllString(s, "\n\n")
	s = regexp.MustCompile(`(?i)</div>`).ReplaceAllString(s, "\n\n")
	s = regexp.MustCompile(`(?i)</li>`).ReplaceAllString(s, "\n")
	s = regexp.MustCompile(`(?i)</ul>`).ReplaceAllString(s, "\n\n")
	s = regexp.MustCompile(`(?i)</ol>`).ReplaceAllString(s, "\n\n")
	s = regexp.MustCompile(`(?i)</blockquote>`).ReplaceAllString(s, "\n\n")

	s = regexp.MustCompile(`(?i)<h1[^>]*>`).ReplaceAllString(s, "\n\n# ")
	s = regexp.MustCompile(`(?i)</h1>`).ReplaceAllString(s, "\n\n")
	s = regexp.MustCompile(`(?i)<h2[^>]*>`).ReplaceAllString(s, "\n\n## ")
	s = regexp.MustCompile(`(?i)</h2>`).ReplaceAllString(s, "\n\n")
	s = regexp.MustCompile(`(?i)<h3[^>]*>`).ReplaceAllString(s, "\n\n### ")
	s = regexp.MustCompile(`(?i)</h3>`).ReplaceAllString(s, "\n\n")
	s = regexp.MustCompile(`(?i)<h4[^>]*>`).ReplaceAllString(s, "\n\n#### ")
	s = regexp.MustCompile(`(?i)</h4>`).ReplaceAllString(s, "\n\n")

	// 6a. 列表项 → "- "（不管原 ul/ol，先统一）
	s = regexp.MustCompile(`(?is)<li[^>]*>`).ReplaceAllString(s, "- ")

	// 6b. 内联格式化
	s = regexp.MustCompile(`(?is)<strong[^>]*>`).ReplaceAllString(s, "**")
	s = regexp.MustCompile(`(?i)</strong>`).ReplaceAllString(s, "**")
	s = regexp.MustCompile(`(?is)<b[^>]*>`).ReplaceAllString(s, "**")
	s = regexp.MustCompile(`(?i)</b>`).ReplaceAllString(s, "**")
	s = regexp.MustCompile(`(?is)<em[^>]*>`).ReplaceAllString(s, "*")
	s = regexp.MustCompile(`(?i)</em>`).ReplaceAllString(s, "*")
	s = regexp.MustCompile(`(?is)<i[^>]*>`).ReplaceAllString(s, "*")
	s = regexp.MustCompile(`(?i)</i>`).ReplaceAllString(s, "*")
	s = regexp.MustCompile(`(?is)<code[^>]*>`).ReplaceAllString(s, "`")
	s = regexp.MustCompile(`(?i)</code>`).ReplaceAllString(s, "`")
	s = regexp.MustCompile(`(?is)<pre[^>]*>`).ReplaceAllString(s, "\n\n```\n")
	s = regexp.MustCompile(`(?i)</pre>`).ReplaceAllString(s, "\n```\n\n")

	// 6c. <blockquote> 开头加 >
	s = regexp.MustCompile(`(?is)<blockquote[^>]*>`).ReplaceAllString(s, "\n\n> ")

	// 7. 所有剩余 <a href="...">text</a> → [text](url)
	s = regexp.MustCompile(`(?is)<a\s+href=["']([^"']+)["'][^>]*>(.*?)</a>`).ReplaceAllStringFunc(s, func(m string) string {
		sub := regexp.MustCompile(`(?is)<a\s+href=["']([^"']+)["'][^>]*>(.*?)</a>`).FindStringSubmatch(m)
		if len(sub) < 3 {
			return m
		}
		url := sub[1]
		text := regexp.MustCompile(`<[^>]+>`).ReplaceAllString(sub[2], "")
		text = strings.TrimSpace(text)
		if text == "" {
			text = url
		}
		return "[" + text + "](" + url + ")"
	})

	// 8. 剥剩余标签，还原实体
	s = regexp.MustCompile(`(?s)<[^>]+>`).ReplaceAllString(s, "")
	s = htmlUnescape(s)

	// 9. 压缩多余空行
	s = regexp.MustCompile(`\n{3,}`).ReplaceAllString(s, "\n\n")
	s = regexp.MustCompile(`[ \t]+\n`).ReplaceAllString(s, "\n")
	s = strings.TrimSpace(s) + "\n"
	return s
}

func htmlUnescape(s string) string {
	// 不引入额外包：自己处理常见实体
	repl := map[string]string{
		"&nbsp;": " ",
		"&amp;":  "&",
		"&lt;":   "<",
		"&gt;":   ">",
		"&quot;": "\"",
		"&#39;":  "'",
		"&#34;":  "\"",
		"&middot;": "·",
		"&hellip;": "…",
	}
	for k, v := range repl {
		s = strings.ReplaceAll(s, k, v)
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
