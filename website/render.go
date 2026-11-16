package main

import (
	"html/template"
	"io"
	"path/filepath"
	"strings"
)

type PageData struct {
	Title       string
	Description string
	Canonical   string
	SwitchURL   string // 切换语言后的完整路径
	Active      string
	Content     string
	Docs        []Document
	Posts       []Document
	Doc         *Document
	Post        *Document
	Markdown    template.HTML
	Body        template.HTML
	Lang        string // "zh" 或 "en"
	Root        string // "" 或 "/en"，用于模板中拼接 href
	Year        int
}

type Renderer struct {
	t *template.Template
}

func NewRenderer(dir string) (*Renderer, error) {
	funcs := template.FuncMap{
		"tags": func(raw string) []string {
			if strings.TrimSpace(raw) == "" {
				return nil
			}
			parts := strings.Split(raw, ",")
			out := make([]string, 0, len(parts))
			for _, p := range parts {
				if p = strings.TrimSpace(p); p != "" {
					out = append(out, p)
				}
			}
			return out
		},
		// lang 根据当前 PageData.Lang 返回对应语言的字符串。
		// 用法：{{lang . "English" "中文"}}
		"lang": func(d PageData, en, zh string) string {
			if d.Lang == "en" {
				return en
			}
			return zh
		},
	}
	t, err := template.New("layout").Funcs(funcs).ParseGlob(filepath.Join(dir, "*.html"))
	if err != nil {
		return nil, err
	}
	t, err = t.ParseGlob(filepath.Join(dir, "pages", "*.html"))
	if err != nil {
		return nil, err
	}
	t, err = t.ParseGlob(filepath.Join(dir, "dynamic.html"))
	if err != nil {
		return nil, err
	}
	return &Renderer{t: t}, nil
}

func (r *Renderer) Execute(w io.Writer, name string, data any) error {
	return r.t.ExecuteTemplate(w, name, data)
}
