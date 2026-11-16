package main

import (
	"bytes"
	"database/sql"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
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
  PRIMARY KEY (kind, slug)
)`); err != nil {
		db.Close()
		return nil, fmt.Errorf("create schema: %w", err)
	}
	// 旧版本表迁移：补四个英文字段。
	for _, col := range []string{"title_en", "summary_en", "html_en", "tags_en"} {
		db.Exec(fmt.Sprintf("ALTER TABLE documents ADD COLUMN %s TEXT NOT NULL DEFAULT ''", col))
	}
	return &Store{db: db}, nil
}

func (s *Store) Close() error { return s.db.Close() }

// Sync 扫描 content/{docs,blog}，只重新解析 mtime 变化的文件，并删除已移除的文件。
func (s *Store) Sync(dir string) error {
	s.syncMu.Lock()
	defer s.syncMu.Unlock()
	s.contentDir = dir

	seen := make(map[string]bool)
	changed, removed := 0, 0

	for _, kind := range []string{"docs", "blog", "freeswitch", "opensips"} {
		root := filepath.Join(dir, kind)
		entries, err := os.ReadDir(root)
		if err != nil {
			if os.IsNotExist(err) {
				continue
			}
			return fmt.Errorf("read %s: %w", root, err)
		}
		for _, entry := range entries {
			if entry.IsDir() || !strings.HasSuffix(strings.ToLower(entry.Name()), ".md") {
				continue
			}
			full := filepath.Join(root, entry.Name())
			info, err := entry.Info()
			if err != nil {
				log.Printf("content: stat %s: %v", full, err)
				continue
			}
			slug := strings.TrimSuffix(entry.Name(), filepath.Ext(entry.Name()))
			seen[kind+"/"+slug] = true

			var old int64
			err = s.db.QueryRow(`SELECT mtime FROM documents WHERE kind=? AND slug=?`, kind, slug).Scan(&old)
			if err == nil && old == info.ModTime().UnixNano() {
				continue
			}

			doc, err := parseDocument(full, kind, slug, info.ModTime().UnixNano())
			if err != nil {
				log.Printf("content: %v", err)
				continue
			}
			if _, err := s.db.Exec(`
INSERT INTO documents (kind, slug, title, summary, html, date, tags, ord, mtime, title_en, summary_en, html_en, tags_en)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(kind, slug) DO UPDATE SET
  title=excluded.title, summary=excluded.summary, html=excluded.html,
  date=excluded.date, tags=excluded.tags, ord=excluded.ord, mtime=excluded.mtime,
  title_en=excluded.title_en, summary_en=excluded.summary_en,
  html_en=excluded.html_en, tags_en=excluded.tags_en`,
				doc.Kind, doc.Slug, doc.Title, doc.Summary, doc.HTML, doc.Date, doc.Tags, doc.Order, doc.Mtime,
				doc.TitleEn, doc.SummaryEn, doc.HTMLEn, doc.TagsEn); err != nil {
				log.Printf("content: upsert %s: %v", full, err)
				continue
			}
			changed++
		}
	}

	rows, err := s.db.Query(`SELECT kind, slug FROM documents`)
	if err != nil {
		return err
	}
	var stale [][2]string
	for rows.Next() {
		var kind, slug string
		if err := rows.Scan(&kind, &slug); err == nil && !seen[kind+"/"+slug] {
			stale = append(stale, [2]string{kind, slug})
		}
	}
	rows.Close()
	for _, key := range stale {
		if _, err := s.db.Exec(`DELETE FROM documents WHERE kind=? AND slug=?`, key[0], key[1]); err == nil {
			removed++
		}
	}

	if changed > 0 || removed > 0 {
		log.Printf("content synced: %d updated, %d removed", changed, removed)
	}
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

func (s *Store) ListDocs() ([]Document, error) {
	return s.list(`SELECT kind, slug, title, summary, html, date, tags, ord, title_en, summary_en, html_en, tags_en FROM documents
		WHERE kind='docs' ORDER BY ord ASC, title ASC`)
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

func (s *Store) Get(kind, slug string) (*Document, error) {
	var d Document
	err := s.db.QueryRow(`SELECT kind, slug, title, summary, html, date, tags, ord, title_en, summary_en, html_en, tags_en
		FROM documents WHERE kind=? AND slug=?`, kind, slug).
		Scan(&d.Kind, &d.Slug, &d.Title, &d.Summary, &d.HTML, &d.Date, &d.Tags, &d.Order,
			&d.TitleEn, &d.SummaryEn, &d.HTMLEn, &d.TagsEn)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &d, nil
}

func parseDocument(path, kind, fallbackSlug string, mtime int64) (*Document, error) {
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
