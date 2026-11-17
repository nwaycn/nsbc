package main

import (
	"bytes"
	"database/sql"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/yuin/goldmark"
	"github.com/yuin/goldmark/extension"
	"github.com/yuin/goldmark/renderer/html"
	_ "modernc.org/sqlite"
)

// Document 是一条已索引的内容。标题/摘要/正文均提供中英文两套，
// 英文未提供时（字段为空、或正文无 <!-- en --> 分隔符）回退到中文。
type Document struct {
	Kind         string
	Version      string // docs 才用，如 "v0.5"；blog/freeswitch/opensips 留空
	Slug         string
	Title        string // 中文标题
	Summary      string // 中文摘要
	HTML         string // 中文 HTML
	Date         string
	Tags         string
	Order        int
	Mtime        int64

	TitleEn   string // 可选英文标题
	SummaryEn string // 可选英文摘要
	HTMLEn    string // 可选英文 HTML
	TagsEn    string // 可选英文 tags
}

var (
	ErrNotFound = errors.New("document not found")
	markdown    = goldmark.New(
		goldmark.WithExtensions(extension.GFM),
		goldmark.WithRendererOptions(html.WithUnsafe()),
	)
)

type Store struct {
	db         *sql.DB
	syncMu     sync.Mutex
	contentDir string
}

func OpenStore(path string) (*Store, error) {
	if dir := filepath.Dir(path); dir != "." && dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return nil, fmt.Errorf("create db dir: %w", err)
		}
	}
	dsn := "file:" + filepath.ToSlash(path) + "?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open sqlite: %w", err)
	}
	db.SetMaxOpenConns(1)
	if _, err := db.Exec(`
CREATE TABLE IF NOT EXISTS documents (
  kind        TEXT NOT NULL,
  version     TEXT NOT NULL DEFAULT '',
  slug        TEXT NOT NULL,
  title       TEXT NOT NULL,
  summary     TEXT NOT NULL DEFAULT '',
  html        TEXT NOT NULL DEFAULT '',
  date        TEXT NOT NULL DEFAULT '',
  tags        TEXT NOT NULL DEFAULT '',
  ord         INTEGER NOT NULL DEFAULT 0,
  mtime       INTEGER NOT NULL DEFAULT 0,
  title_en    TEXT NOT NULL DEFAULT '',
  summary_en  TEXT NOT NULL DEFAULT '',
  html_en     TEXT NOT NULL DEFAULT '',
  tags_en     TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (kind, version, slug)
)`); err != nil {
		db.Close()
		return nil, fmt.Errorf("create schema: %w", err)
	}
	// 旧版本表迁移
	for _, col := range []string{"title_en", "summary_en", "html_en", "tags_en"} {
		db.Exec(fmt.Sprintf("ALTER TABLE documents ADD COLUMN %s TEXT NOT NULL DEFAULT ''", col))
	}
	// docs 版本化迁移：version 列
	db.Exec(`ALTER TABLE documents ADD COLUMN version TEXT NOT NULL DEFAULT ''`)
	// docs 有了 version 列后，如果还存在旧版 PRIMARY KEY(kind, slug) 会冲突 —
	// SQLite 不支持修改主键，建个新表重导。
	ensureVersionedPK(db)
	return &Store{db: db}, nil
}

func (s *Store) Close() error { return s.db.Close() }

// ensureVersionedPK 把旧表 PRIMARY KEY(kind, slug) 替换成 (kind, version, slug)。
// SQLite 不支持修改主键，所以用 CREATE TABLE AS 重建。
func ensureVersionedPK(db *sql.DB) {
	var pkCols int
	db.QueryRow(`SELECT COUNT(*) FROM pragma_table_info('documents')
		WHERE pk=1`).Scan(&pkCols)
	if pkCols == 3 {
		return // 已是 versioned PK
	}
	db.Exec(`
CREATE TABLE IF NOT EXISTS documents_new (
  kind        TEXT NOT NULL,
  version     TEXT NOT NULL DEFAULT '',
  slug        TEXT NOT NULL,
  title       TEXT NOT NULL,
  summary     TEXT NOT NULL DEFAULT '',
  html        TEXT NOT NULL DEFAULT '',
  date        TEXT NOT NULL DEFAULT '',
  tags        TEXT NOT NULL DEFAULT '',
  ord         INTEGER NOT NULL DEFAULT 0,
  mtime       INTEGER NOT NULL DEFAULT 0,
  title_en    TEXT NOT NULL DEFAULT '',
  summary_en  TEXT NOT NULL DEFAULT '',
  html_en     TEXT NOT NULL DEFAULT '',
  tags_en     TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (kind, version, slug)
)`)
	db.Exec(`INSERT OR IGNORE INTO documents_new
	SELECT kind, COALESCE(version, ''), slug, title, summary, html, date, tags, ord, mtime,
	       title_en, summary_en, html_en, tags_en
	FROM documents`)
	db.Exec(`DROP TABLE IF EXISTS documents`)
	db.Exec(`ALTER TABLE documents_new RENAME TO documents`)
}

// Sync 扫描 content/{docs,blog,freeswitch,opensips}，只重新解析 mtime 变化的文件，并删除已移除的文件。
// docs 目录是特殊处理：递归扫描 content/docs/vX.X/*.md，version 从中间目录名提取。
// 其他 kind（blog/freeswitch/opensips）是扁平目录，version 留空。
func (s *Store) Sync(dir string) error {
	s.syncMu.Lock()
	defer s.syncMu.Unlock()
	s.contentDir = dir

	seen := make(map[string]bool) // key: "kind|version|slug"
	changed, removed := 0, 0

	for _, kind := range []string{"docs", "blog", "freeswitch", "opensips"} {
		root := filepath.Join(dir, kind)
		if err := s.syncKind(root, kind, seen, &changed); err != nil {
			if os.IsNotExist(err) {
				continue
			}
			log.Printf("content: sync %s: %v", kind, err)
		}
	}

	// 删 stale
	rows, err := s.db.Query(`SELECT kind, version, slug FROM documents`)
	if err != nil {
		return err
	}
	var stale [][3]string
	for rows.Next() {
		var k, v, slug string
		if err := rows.Scan(&k, &v, &slug); err == nil {
			key := k + "|" + v + "|" + slug
			if !seen[key] {
				stale = append(stale, [3]string{k, v, slug})
			}
		}
	}
	rows.Close()
	for _, key := range stale {
		if _, err := s.db.Exec(`DELETE FROM documents WHERE kind=? AND version=? AND slug=?`,
			key[0], key[1], key[2]); err == nil {
			removed++
		}
	}

	if changed > 0 || removed > 0 {
		log.Printf("content synced: %d updated, %d removed", changed, removed)
	}
	return nil
}

// syncKind 递归扫描一个 kind 的目录。
// kind="docs" 时，content/docs/v0.5/quickstart.md → version="v0.5", slug="quickstart"
// kind="docs" 时，content/docs/quickstart.md（无版本子目录）→ version=""，保留旧结构兼容
// 其他 kind 只扫直接子文件，version 永远是 ""
func (s *Store) syncKind(root, kind string, seen map[string]bool, changed *int) error {
	entries, err := os.ReadDir(root)
	if err != nil {
		return err
	}

	if kind == "docs" {
		// 先处理扁平文件（向后兼容旧结构）
		for _, e := range entries {
			if e.IsDir() {
				continue
			}
			name := e.Name()
			if !strings.HasSuffix(strings.ToLower(name), ".md") {
				continue
			}
			slug := strings.TrimSuffix(name, filepath.Ext(name))
			if err := s.indexFile(root, kind, "", slug, seen, changed); err != nil {
				log.Printf("content: %v", err)
			}
		}
		// 再处理版本子目录
		for _, e := range entries {
			if !e.IsDir() {
				continue
			}
			ver := e.Name()
			// 版本目录要求匹配 vX.Y 模式（v0.5 / v1.0 / devel 等）
			if !versionDirNameOK(ver) {
				continue
			}
			subRoot := filepath.Join(root, ver)
			subFiles, err := os.ReadDir(subRoot)
			if err != nil {
				continue
			}
			for _, f := range subFiles {
				if f.IsDir() || !strings.HasSuffix(strings.ToLower(f.Name()), ".md") {
					continue
				}
				slug := strings.TrimSuffix(f.Name(), filepath.Ext(f.Name()))
				if err := s.indexFile(subRoot, kind, ver, slug, seen, changed); err != nil {
					log.Printf("content: %v", err)
				}
			}
		}
		return nil
	}

	// blog / freeswitch / opensips — 扁平
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(strings.ToLower(e.Name()), ".md") {
			continue
		}
		slug := strings.TrimSuffix(e.Name(), filepath.Ext(e.Name()))
		if err := s.indexFile(root, kind, "", slug, seen, changed); err != nil {
			log.Printf("content: %v", err)
		}
	}
	return nil
}

// versionDirNameOK 接受的版本目录名：v0.5 / v1.0 / v1.11 / v2.4 / devel / latest 等
func versionDirNameOK(name string) bool {
	name = strings.TrimSpace(name)
	if name == "" {
		return false
	}
	// 允许 v\d+(\.\d+)* 或 devel / latest
	if strings.HasPrefix(strings.ToLower(name), "devel") {
		return true
	}
	if strings.HasPrefix(strings.ToLower(name), "latest") {
		return true
	}
	for _, prefix := range []string{"v", "V"} {
		if strings.HasPrefix(name, prefix) {
			rest := name[1:]
			rest = strings.ReplaceAll(rest, ".", "")
			if _, err := strconv.Atoi(rest); err == nil {
				return true
			}
		}
	}
	return false
}

func (s *Store) indexFile(dir, kind, version, slug string,
	seen map[string]bool, changed *int) error {

	full := filepath.Join(dir, slug+".md")
	info, err := os.Stat(full)
	if err != nil {
		return err
	}
	seen[kind+"|"+version+"|"+slug] = true

	var old int64
	err = s.db.QueryRow(`SELECT mtime FROM documents WHERE kind=? AND version=? AND slug=?`,
		kind, version, slug).Scan(&old)
	if err == nil && old == info.ModTime().UnixNano() {
		return nil
	}

	doc, err := parseDocument(full, kind, version, slug, info.ModTime().UnixNano())
	if err != nil {
		return err
	}
	if _, err := s.db.Exec(`
INSERT INTO documents (kind, version, slug, title, summary, html, date, tags, ord, mtime,
  title_en, summary_en, html_en, tags_en)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(kind, version, slug) DO UPDATE SET
  title=excluded.title, summary=excluded.summary, html=excluded.html,
  date=excluded.date, tags=excluded.tags, ord=excluded.ord, mtime=excluded.mtime,
  title_en=excluded.title_en, summary_en=excluded.summary_en,
  html_en=excluded.html_en, tags_en=excluded.tags_en`,
		doc.Kind, doc.Version, doc.Slug,
		doc.Title, doc.Summary, doc.HTML, doc.Date, doc.Tags, doc.Order, doc.Mtime,
		doc.TitleEn, doc.SummaryEn, doc.HTMLEn, doc.TagsEn); err != nil {
		return fmt.Errorf("upsert %s: %w", full, err)
	}
	*changed++
	return nil
}

// Watch 每 interval 检查一次文件变化。轮询实现不引入额外依赖，变更后 2 秒内生效。
func (s *Store) Watch(dir string, interval time.Duration) func() {
	ticker := time.NewTicker(interval)
	done := make(chan struct{})
	go func() {
		for {
			select {
			case <-ticker.C:
				if err := s.Sync(dir); err != nil {
					log.Printf("content watch: %v", err)
				}
			case <-done:
				return
			}
		}
	}()
	return func() {
		close(done)
		ticker.Stop()
	}
}

func (s *Store) ListDocs(version string) ([]Document, error) {
	return s.listVersioned(`SELECT kind, version, slug, title, summary, html, date, tags, ord, title_en, summary_en, html_en, tags_en FROM documents
		WHERE kind='docs' AND version=? ORDER BY ord ASC, title ASC`, version)
}

func (s *Store) ListBlog() ([]Document, error) {
	return s.list(`SELECT kind, slug, title, summary, html, date, tags, ord, title_en, summary_en, html_en, tags_en FROM documents
		WHERE kind='blog' ORDER BY date DESC, title ASC`)
}

// ListFreeswitch / ListOpenSIPS 和 blog 结构相同，按日期倒序。
func (s *Store) ListFreeswitch() ([]Document, error) {
	return s.list(`SELECT kind, slug, title, summary, html, date, tags, ord, title_en, summary_en, html_en, tags_en FROM documents
		WHERE kind='freeswitch' ORDER BY date DESC, title ASC`)
}

func (s *Store) ListOpenSIPS() ([]Document, error) {
	return s.list(`SELECT kind, slug, title, summary, html, date, tags, ord, title_en, summary_en, html_en, tags_en FROM documents
		WHERE kind='opensips' ORDER BY date DESC, title ASC`)
}

// list 不含 version 列的查询（blog/freeswitch/opensips 用）。
func (s *Store) list(query string) ([]Document, error) {
	rows, err := s.db.Query(query)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var docs []Document
	for rows.Next() {
		var d Document
		if err := rows.Scan(&d.Kind, &d.Slug, &d.Title, &d.Summary, &d.HTML, &d.Date, &d.Tags, &d.Order,
			&d.TitleEn, &d.SummaryEn, &d.HTMLEn, &d.TagsEn); err != nil {
			return nil, err
		}
		docs = append(docs, d)
	}
	return docs, rows.Err()
}

// listVersioned 含 version 列的查询（docs 用）。
func (s *Store) listVersioned(query string, args ...any) ([]Document, error) {
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var docs []Document
	for rows.Next() {
		var d Document
		if err := rows.Scan(&d.Kind, &d.Version, &d.Slug, &d.Title, &d.Summary, &d.HTML, &d.Date, &d.Tags, &d.Order,
			&d.TitleEn, &d.SummaryEn, &d.HTMLEn, &d.TagsEn); err != nil {
			return nil, err
		}
		docs = append(docs, d)
	}
	return docs, rows.Err()
}

func (s *Store) Get(kind, version, slug string) (*Document, error) {
	var d Document
	err := s.db.QueryRow(`SELECT kind, version, slug, title, summary, html, date, tags, ord, title_en, summary_en, html_en, tags_en
		FROM documents WHERE kind=? AND version=? AND slug=?`, kind, version, slug).
		Scan(&d.Kind, &d.Version, &d.Slug, &d.Title, &d.Summary, &d.HTML, &d.Date, &d.Tags, &d.Order,
			&d.TitleEn, &d.SummaryEn, &d.HTMLEn, &d.TagsEn)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &d, nil
}

// ListVersions 返回所有非空、非 flat-document 的 docs 版本号，按版本号倒序（最新在前）。
func (s *Store) ListVersions() ([]string, error) {
	rows, err := s.db.Query(`SELECT DISTINCT version FROM documents WHERE kind='docs' AND version != ''`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var vers []string
	for rows.Next() {
		var v string
		if err := rows.Scan(&v); err == nil {
			vers = append(vers, v)
		}
	}
	return sortVersions(vers), rows.Err()
}

// LatestVersion 返回当前最新的 docs 版本；没文档时返回默认 "v0.5"。
func (s *Store) LatestVersion() (string, error) {
	vers, err := s.ListVersions()
	if err != nil {
		return "v0.5", err
	}
	if len(vers) == 0 {
		return "v0.5", nil
	}
	return vers[len(vers)-1], nil // sortVersions 结果最新在末尾
}

// sortVersions 把版本号（v0.5 / v1.11 / devel 等）按语义版本升序排列。
// devel / latest 永远放末尾。
func sortVersions(vers []string) []string {
	isSpecial := func(v string) bool {
		v = strings.ToLower(v)
		return strings.HasPrefix(v, "devel") || strings.HasPrefix(v, "latest")
	}
	sort.SliceStable(vers, func(i, j int) bool {
		vi, vj := vers[i], vers[j]
		if isSpecial(vi) && !isSpecial(vj) {
			return false // special 放后面
		}
		if !isSpecial(vi) && isSpecial(vj) {
			return true
		}
		// 两个都是普通版本号：按数值比较
		return compareSemver(vi, vj) < 0
	})
	return vers
}

func compareSemver(a, b string) int {
	a = strings.TrimPrefix(a, "v")
	a = strings.TrimPrefix(a, "V")
	b = strings.TrimPrefix(b, "v")
	b = strings.TrimPrefix(b, "V")
	ap := strings.Split(a, ".")
	bp := strings.Split(b, ".")
	max := len(ap)
	if len(bp) > max {
		max = len(bp)
	}
	for i := 0; i < max; i++ {
		var ai, bi int
		if i < len(ap) {
			ai, _ = strconv.Atoi(ap[i])
		}
		if i < len(bp) {
			bi, _ = strconv.Atoi(bp[i])
		}
		if ai != bi {
			if ai < bi {
				return -1
			}
			return 1
		}
	}
	return 0
}

func parseDocument(path, kind, version, fallbackSlug string, mtime int64) (*Document, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read %s: %w", path, err)
	}
	fm, body := parseFrontMatter(string(raw))

	// 正文按 `<!-- en -->` 拆成中文 / 英文两段。
	zhBody := body
	var enBody string
	if idx, mlen := findSeparator(body); idx >= 0 {
		zhBody = strings.TrimSpace(body[:idx])
		enBody = strings.TrimSpace(body[idx+mlen:])
	}

	// 注：sanitizeBody 已撤回。importblog.go 在抓取时已经做过
	// HTML 标签剥离、实体还原、TOC/标题去重、BOM 清理。
	// 运行时再做 dedent 会破坏原文有意的格式缩进（shell 命令、
	// 代码块示例、层次化段落等），见 commit 3e25326 回退说明。

	slug := strings.TrimSpace(fm["slug"])
	if slug == "" {
		slug = fallbackSlug
	}
	title := strings.TrimSpace(fm["title"])
	if title == "" {
		title = firstHeading(zhBody)
	}
	if title == "" {
		title = slug
	}
	summary := strings.TrimSpace(fm["summary"])
	if summary == "" {
		summary = firstParagraph(zhBody, 140)
	}

	// 英文元数据：front matter 优先，否则从英文正文推断，再回退到中文。
	titleEn := strings.TrimSpace(fm["title_en"])
	if titleEn == "" && enBody != "" {
		titleEn = firstHeading(enBody)
	}
	if titleEn == "" {
		titleEn = title // 回退到中文标题，避免英文页显示空
	}
	summaryEn := strings.TrimSpace(fm["summary_en"])
	if summaryEn == "" && enBody != "" {
		summaryEn = firstParagraph(enBody, 160)
	}

	order, _ := strconv.Atoi(strings.TrimSpace(fm["order"]))

	var zhHTML bytes.Buffer
	if err := markdown.Convert([]byte(zhBody), &zhHTML); err != nil {
		return nil, fmt.Errorf("render zh %s: %w", path, err)
	}
	var enHTML string
	if enBody != "" {
		var buf bytes.Buffer
		if err := markdown.Convert([]byte(enBody), &buf); err != nil {
			log.Printf("render en %s: %v", path, err)
		} else {
			enHTML = buf.String()
		}
	}
	// 未提供英文正文时，enHTML 留空，路由层会回退到 zhHTML。

	doc := &Document{
		Kind:       kind,
		Version:    version,
		Slug:       slug,
		Title:      title,
		Summary:    summary,
		HTML:       zhHTML.String(),
		Date:       strings.TrimSpace(fm["date"]),
		Tags:       normalizeTags(fm["tags"]),
		Order:      order,
		Mtime:      mtime,
		TitleEn:    titleEn,
		SummaryEn:  summaryEn,
		HTMLEn:     enHTML,
		TagsEn:     normalizeTags(fm["tags_en"]),
	}
	return doc, nil
}

// findSeparator 在正文中查找 `<!-- en -->` 注释分隔符。
// 允许前后空白与大小写变体，返回起始索引；未找到返回 -1。
// findSeparator 在正文中搜索英文分隔符，返回 (起始位置, 分隔符长度)；未命中返回 (-1, 0)。
func findSeparator(body string) (int, int) {
	bodyLo := strings.ToLower(body)
	for _, marker := range []string{"<!-- en -->", "<!--en-->", "<!-- en: -->", "<!-- en:-->"} {
		if idx := strings.Index(bodyLo, strings.ToLower(marker)); idx >= 0 {
			return idx, len(marker)
		}
	}
	return -1, 0
}

func parseFrontMatter(raw string) (map[string]string, string) {
	meta := make(map[string]string)
	// 剥 UTF-8 BOM（EF BB BF），否则 strings.HasPrefix("---\n") 会失败
	raw = strings.TrimPrefix(raw, "\uFEFF")
	raw = strings.ReplaceAll(raw, "\r\n", "\n")
	if !strings.HasPrefix(raw, "---\n") {
		return meta, raw
	}
	rest := raw[len("---\n"):]
	end := strings.Index(rest, "\n---")
	if end < 0 {
		return meta, raw
	}
	for _, line := range strings.Split(rest[:end], "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		key, value, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		meta[strings.ToLower(strings.TrimSpace(key))] = strings.TrimSpace(value)
	}
	body := strings.TrimPrefix(rest[end+len("\n---"):], "\n")
	return meta, body
}

func firstHeading(body string) string {
	for _, line := range strings.Split(body, "\n") {
		if strings.HasPrefix(line, "# ") {
			return strings.TrimSpace(strings.TrimPrefix(line, "# "))
		}
	}
	return ""
}

// firstParagraph 取第一段可读文本作为摘要，跳过标题、代码块与表格。
func firstParagraph(body string, limit int) string {
	inFence := false
	for _, line := range strings.Split(body, "\n") {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "```") {
			inFence = !inFence
			continue
		}
		if inFence || trimmed == "" || strings.HasPrefix(trimmed, "#") ||
			strings.HasPrefix(trimmed, "|") || strings.HasPrefix(trimmed, ">") {
			continue
		}
		text := strings.TrimSpace(strings.TrimPrefix(trimmed, "- "))
		runes := []rune(text)
		if len(runes) > limit {
			return string(runes[:limit]) + "…"
		}
		return text
	}
	return ""
}

func normalizeTags(raw string) string {
	parts := strings.Split(raw, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return strings.Join(out, ",")
}
