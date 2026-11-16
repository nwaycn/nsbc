package main

import (
	"bytes"
	"flag"
	"html/template"
	"log"
	"mime"
	"net/http"
	"path/filepath"
	"strings"
	"time"

	"github.com/caddyserver/certmagic"
)

type Server struct {
	store *Store
	r     *Renderer
}

func main() {
	// 命令行 flag 可覆盖 INI 配置。config 路径本身不受 ini 影响，必须用 flag 指定。
	configPath := flag.String("config", "website.ini", "配置文件路径 (INI)")
	addr := flag.String("addr", "", "HTTP 监听地址 (覆盖 ini [general].addr)")
	contentDir := flag.String("content", "", "Markdown 内容目录")
	dbPath := flag.String("db", "", "SQLite 索引路径")
	tplDir := flag.String("templates", "", "模板目录")
	assetsDir := flag.String("assets", "", "静态资源目录")
	flag.Parse()

	cfg, err := LoadINI(*configPath)
	if err != nil {
		log.Fatalf("load config %s: %v", *configPath, err)
	}

	// flag 显式传参时覆盖 ini 值。
	if *addr != "" {
		cfg.Addr = *addr
	}
	if *contentDir != "" {
		cfg.ContentDir = *contentDir
	}
	if *dbPath != "" {
		cfg.DBDir = *dbPath
	}
	if *tplDir != "" {
		cfg.TplDir = *tplDir
	}
	if *assetsDir != "" {
		cfg.AssetsDir = *assetsDir
	}

	store, err := OpenStore(cfg.DBDir)
	if err != nil {
		log.Fatalf("open store: %v", err)
	}
	defer store.Close()
	if err := store.Sync(cfg.ContentDir); err != nil {
		log.Printf("initial content sync: %v", err)
	}
	stopWatch := store.Watch(cfg.ContentDir, 2*time.Second)
	defer stopWatch()

	renderer, err := NewRenderer(cfg.TplDir)
	if err != nil {
		log.Fatalf("parse templates: %v", err)
	}

	s := &Server{store: store, r: renderer}
	mux := http.NewServeMux()

	assets := http.StripPrefix("/assets/", http.FileServer(http.Dir(cfg.AssetsDir)))
	mux.Handle("GET /assets/", withCache(assets))

	// 中文路由（无前缀）
	s.registerPages(mux, "zh")
	mux.HandleFunc("GET /docs", func(w http.ResponseWriter, r *http.Request) { s.handleDocs(w, r, "zh") })
	mux.HandleFunc("GET /docs/{$}", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/docs", http.StatusMovedPermanently)
	})
	mux.HandleFunc("GET /docs/{slug}", func(w http.ResponseWriter, r *http.Request) { s.handleDoc(w, r, "zh") })
	mux.HandleFunc("GET /blog", func(w http.ResponseWriter, r *http.Request) { s.handleBlog(w, r, "zh") })
	mux.HandleFunc("GET /blog/{$}", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/blog", http.StatusMovedPermanently)
	})
	mux.HandleFunc("GET /blog/{slug}", func(w http.ResponseWriter, r *http.Request) { s.handlePost(w, r, "zh") })

	// 英文路由（/en/ 前缀，剥掉前缀后复用同一套 handler，Lang 设为 en）
	enMux := http.NewServeMux()
	s.registerPages(enMux, "en")
	enMux.HandleFunc("GET /docs", func(w http.ResponseWriter, r *http.Request) { s.handleDocs(w, r, "en") })
	enMux.HandleFunc("GET /docs/{$}", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/en/docs", http.StatusMovedPermanently)
	})
	enMux.HandleFunc("GET /docs/{slug}", func(w http.ResponseWriter, r *http.Request) { s.handleDoc(w, r, "en") })
	enMux.HandleFunc("GET /blog", func(w http.ResponseWriter, r *http.Request) { s.handleBlog(w, r, "en") })
	enMux.HandleFunc("GET /blog/{$}", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/en/blog", http.StatusMovedPermanently)
	})
	enMux.HandleFunc("GET /blog/{slug}", func(w http.ResponseWriter, r *http.Request) { s.handlePost(w, r, "en") })
	mux.Handle("GET /en/", http.StripPrefix("/en", enMux))

	// /en 裸路径 → /en/
	mux.HandleFunc("GET /en", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/en/", http.StatusMovedPermanently)
	})

	// 旧静态站链接：保留 301，避免已有外链与书签失效。
	legacy := map[string]string{
		"/index.html":      "/",
		"/features.html":   "/features",
		"/learn.html":      "/learn",
		"/webrtc-sip.html": "/webrtc-sip",
		"/download.html":   "/download",
		"/docs.html":       "/handbook",
		"/community.html":  "/community",
		"/news.html":       "/blog",
		"/news":            "/blog",
		"/about.html":      "/about",
	}
	for from, to := range legacy {
		from, to := from, to
		mux.HandleFunc("GET "+from, func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, to, http.StatusMovedPermanently)
		})
	}

	mux.HandleFunc("GET /{path...}", s.handleNotFound)

	handler := securityHeaders(requestLog(mux))

	if cfg.ACMEEnable {
		runHTTPS(cfg, handler)
	} else {
		runHTTP(cfg, handler)
	}
}

// runHTTP 纯 HTTP 监听，适用于开发、内网预览或反代后的后端。
func runHTTP(cfg Config, handler http.Handler) {
	log.Printf("NSBC website listening on http://%s (content: %s)", cfg.Addr, cfg.ContentDir)
	srv := &http.Server{
		Addr:              cfg.Addr,
		Handler:           handler,
		ReadHeaderTimeout: 10 * time.Second,
	}
	if err := srv.ListenAndServe(); err != nil {
		log.Fatal(err)
	}
}

// runHTTPS 启用 Let's Encrypt / ZeroSSL 自动证书：certmagic 负责
//   - :80  HTTP-01 challenge + 自动 301 → HTTPS
//   - :443 TLS  自动签发 + 到期前 30 天后台续签
func runHTTPS(cfg Config, handler http.Handler) {
	certmagic.DefaultACME.Agreed = true
	certmagic.DefaultACME.Email = cfg.ACMEEmail
	if cfg.ACMEStaging {
		certmagic.DefaultACME.CA = certmagic.LetsEncryptStagingCA
		log.Printf("ACME using Let's Encrypt staging CA (cert will NOT be trusted by browsers)")
	}
	if cfg.ACMECache != "" {
		certmagic.Default.Storage = &certmagic.FileStorage{Path: cfg.ACMECache}
	}

	log.Printf("NSBC website starting ACME HTTPS for %s", cfg.ACMEDomain)
	log.Printf("  email: %s", cfg.ACMEEmail)
	if cfg.ACMECache != "" {
		log.Printf("  cache: %s", cfg.ACMECache)
	}
	log.Printf("  certmagic will listen on :443 (HTTPS) and :80 (challenge + redirect)")

	// HTTPS() 是阻塞式的：内部自己 start goroutine 处理 challenge 和
	// 后台续签，主 goroutine 阻塞在 TLS listener 上直到退出。
	if err := certmagic.HTTPS([]string{cfg.ACMEDomain}, handler); err != nil {
		log.Fatalf("acme https: %v", err)
	}
}

func (s *Server) registerPages(mux *http.ServeMux, lang string) {
	pages := []struct {
		path, template, titleEn, titleZh, descEn, descZh, active string
	}{
		{"/", "page_index",
			"NSBC — SIP Session Border Controller for Real-Time Communications",
			"NSBC — 为实时通信而生的 SIP 会话边界控制器",
			"Open-source SIP platform combining SBC security, PBX features and SIP proxy performance in a single binary.",
			"NSBC 是开源的 SIP 全栈平台，集 SBC 安全、PBX 功能与 SIP 代理性能于一体，单二进制部署。", "home"},
		{"/features", "page_features",
			"Features — NSBC", "功能特性 — NSBC",
			"Signaling, media, security, routing, observability and client SDKs — the full NSBC capability overview.",
			"信令、媒体、安全、路由、可观测性与客户端 SDK，一页了解 NSBC 的完整能力。", "features"},
		{"/learn", "page_learn",
			"Introduction to SIP and SBC — NSBC", "SIP 与 SBC 原理科普 — NSBC",
			"Understand SIP protocol and SBC fundamentals from a single call: signaling, media, INVITE flow, SDP, NAT.",
			"从一通电话开始理解 SIP 协议与 SBC 原理：信令与媒体、INVITE 流程、SDP、NAT 与媒体锚定。", "learn"},
		{"/webrtc-sip", "page_webrtc-sip",
			"WebRTC ↔ SIP Interop — NSBC", "WebRTC 与 SIP 互通 — NSBC",
			"WebRTC and SIP interoperability: WSS signaling, DTLS-SRTP, ICE, codec conversion and troubleshooting.",
			"WebRTC 与 SIP 互通详解：WSS 信令、DTLS-SRTP、ICE、编解码转换与故障排查。", "learn"},
		{"/download", "page_download",
			"Get Source — NSBC", "获取源码 — NSBC",
			"Clone NSBC from GitHub or Gitee and build nsbcd and nsbc-web step by step.",
			"从 GitHub 或 Gitee 获取 NSBC 源码，按步骤构建 nsbcd 与 nsbc-web。", "download"},
		{"/handbook", "page_handbook",
			"Configuration Handbook — NSBC", "配置手册 — NSBC",
			"NSBC handbook: minimal YAML, module loading, SIP & media, dial plan, metrics and REST API.",
			"NSBC 配置手册：最小 YAML、模块加载、SIP 与媒体、拨号计划、指标与 REST API。", "docs"},
		{"/community", "page_community",
			"Community — NSBC", "社区 — NSBC",
			"Contribute to NSBC: file issues, submit code, write modules and event clients.",
			"参与 NSBC 开发：提交 Issue、贡献代码、编写模块与事件客户端。", "community"},
		{"/about", "page_about",
			"About — NSBC", "关于 — NSBC",
			"NSBC is maintained by Nway Technology, open-sourced under Apache-2.0.",
			"NSBC 由上海宁卫信息技术有限公司维护，Apache-2.0 开源。", "about"},
	}
	for _, p := range pages {
		p := p
		pattern := "GET " + p.path
		if p.path == "/" {
			pattern = "GET /{$}"
		}
		title := p.titleZh
		desc := p.descZh
		content := p.template
		if lang == "en" {
			title = p.titleEn
			desc = p.descEn
			content = p.template + "_en"
		}
		mux.HandleFunc(pattern, func(w http.ResponseWriter, r *http.Request) {
			s.render(w, r, PageData{
				Title:       title,
				Description: desc,
				Active:      p.active,
				Content:     content,
				Lang:        lang,
			})
		})
	}
}

func (s *Server) handleDocs(w http.ResponseWriter, r *http.Request, lang string) {
	docs, err := s.store.ListDocs()
	if err != nil {
		log.Printf("list docs: %v", err)
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}
	// 为英文页面选择英文字段（无英文时回退到中文）。
	for i := range docs {
		docs[i] = localizeDoc(docs[i], lang)
	}
	title := "文档中心 — NSBC"
	desc := "NSBC 配置、SIP 与媒体、WebRTC 互通与排障文档。"
	if lang == "en" {
		title = "Documentation — NSBC"
		desc = "NSBC configuration, SIP & media, WebRTC interoperability and troubleshooting docs."
	}
	s.render(w, r, PageData{
		Title:       title,
		Description: desc,
		Active:      "docs",
		Content:     "page_docs",
		Docs:        docs,
		Lang:        lang,
	})
}

func (s *Server) handleDoc(w http.ResponseWriter, r *http.Request, lang string) {
	slug := r.PathValue("slug")
	doc, err := s.store.Get("docs", slug)
	if err == ErrNotFound {
		s.notFound(w, r, lang)
		return
	}
	if err != nil {
		log.Printf("get doc %s: %v", slug, err)
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}
	docs, err := s.store.ListDocs()
	if err != nil {
		log.Printf("list docs: %v", err)
	}
	for i := range docs {
		docs[i] = localizeDoc(docs[i], lang)
	}
	localized := localizeDoc(*doc, lang)
	doc = &localized
	s.render(w, r, PageData{
		Title:       doc.Title + " — NSBC",
		Description: doc.Summary,
		Active:      "docs",
		Content:     "page_doc",
		Docs:        docs,
		Doc:         doc,
		Markdown:    template.HTML(doc.HTML),
		Lang:        lang,
	})
}

func (s *Server) handleBlog(w http.ResponseWriter, r *http.Request, lang string) {
	posts, err := s.store.ListBlog()
	if err != nil {
		log.Printf("list blog: %v", err)
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}
	for i := range posts {
		posts[i] = localizeDoc(posts[i], lang)
	}
	title := "博客 — NSBC"
	desc := "NSBC 版本发布、实现细节与运维经验。"
	if lang == "en" {
		title = "Blog — NSBC"
		desc = "NSBC releases, implementation notes and ops experience."
	}
	s.render(w, r, PageData{
		Title:       title,
		Description: desc,
		Active:      "blog",
		Content:     "page_blog",
		Posts:       posts,
		Lang:        lang,
	})
}

func (s *Server) handlePost(w http.ResponseWriter, r *http.Request, lang string) {
	post, err := s.store.Get("blog", r.PathValue("slug"))
	if err == ErrNotFound {
		s.notFound(w, r, lang)
		return
	}
	if err != nil {
		log.Printf("get post: %v", err)
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}
	localized := localizeDoc(*post, lang)
	post = &localized
	s.render(w, r, PageData{
		Title:       post.Title + " — NSBC",
		Description: post.Summary,
		Active:      "blog",
		Content:     "page_post",
		Post:        post,
		Markdown:    template.HTML(post.HTML),
		Lang:        lang,
	})
}

// localizeDoc 根据 Lang 返回目标语言的副本；缺英文字段时自动回退到中文。
func localizeDoc(d Document, lang string) Document {
	if lang != "en" {
		// 中文：清空 en 字段避免模板误用
		d.TitleEn, d.SummaryEn, d.HTMLEn, d.TagsEn = "", "", "", ""
		return d
	}
	if d.TitleEn != "" {
		d.Title = d.TitleEn
	}
	if d.SummaryEn != "" {
		d.Summary = d.SummaryEn
	}
	if d.HTMLEn != "" {
		d.HTML = d.HTMLEn
	}
	if d.TagsEn != "" {
		d.Tags = d.TagsEn
	}
	return d
}

func (s *Server) handleNotFound(w http.ResponseWriter, r *http.Request) {
	lang := "zh"
	if strings.HasPrefix(r.URL.Path, "/en") {
		lang = "en"
	}
	s.notFound(w, r, lang)
}

func (s *Server) notFound(w http.ResponseWriter, r *http.Request, lang string) {
	title := "页面不存在 — NSBC"
	desc := "请求的页面不存在。"
	if lang == "en" {
		title = "Page not found — NSBC"
		desc = "The requested page does not exist."
	}
	s.renderBody(w, r, PageData{
		Title:       title,
		Description: desc,
		Content:     "page_404",
		Lang:        lang,
	}, http.StatusNotFound)
}

func (s *Server) render(w http.ResponseWriter, r *http.Request, data PageData) {
	s.renderBody(w, r, data, http.StatusOK)
}

func (s *Server) renderBody(w http.ResponseWriter, r *http.Request, data PageData, status int) {
	if data.Canonical == "" {
		data.Canonical = r.URL.Path
	}
	if data.Canonical == "" {
		data.Canonical = "/"
	}
	data.Year = time.Now().Year()
	if data.Lang == "en" {
		data.Root = "/en"
	} else {
		data.Root = ""
	}

	// 计算切换语言后的路径：加/去 /en 前缀
	currentPath := r.URL.Path
	if data.SwitchURL == "" {
		if data.Lang == "en" {
			// 英文页 → 中文：去掉 /en 前缀
			data.SwitchURL = strings.TrimPrefix(currentPath, "/en")
			if data.SwitchURL == "" {
				data.SwitchURL = "/"
			}
		} else {
			// 中文页 → 英文：加 /en 前缀
			data.SwitchURL = "/en" + currentPath
			if currentPath == "/" {
				data.SwitchURL = "/en/"
			}
		}
	}
	// 更新 canonical 使其包含正确的 /en 前缀（对 SEO 友好）
	if data.Lang == "en" && !strings.HasPrefix(data.Canonical, "/en") {
		data.Canonical = "/en" + data.Canonical
	} else if data.Lang != "en" && strings.HasPrefix(data.Canonical, "/en") {
		data.Canonical = strings.TrimPrefix(data.Canonical, "/en")
		if data.Canonical == "" {
			data.Canonical = "/"
		}
	}

	var body bytes.Buffer
	if err := s.r.Execute(&body, data.Content, data); err != nil {
		log.Printf("render content %s: %v", data.Content, err)
		http.Error(w, "template error", http.StatusInternalServerError)
		return
	}
	data.Body = template.HTML(body.String())

	var page bytes.Buffer
	if err := s.r.Execute(&page, "layout", data); err != nil {
		log.Printf("render layout %s: %v", r.URL.Path, err)
		http.Error(w, "template error", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(status)
	w.Write(page.Bytes())
}

func withCache(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if ext := filepath.Ext(r.URL.Path); mime.TypeByExtension(ext) != "" {
			w.Header().Set("Cache-Control", "public, max-age=3600")
		}
		next.ServeHTTP(w, r)
	})
}

func securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "strict-origin-when-cross-origin")
		next.ServeHTTP(w, r)
	})
}

func requestLog(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("%s %s %s", r.Method, r.URL.Path, time.Since(start).Round(time.Millisecond))
	})
}
