# NSBC Website

基于 Go 标准库 `net/http` + `goldmark` + SQLite 的动态双语站点，零运行时依赖。

## 目录结构

```
website/
├── main.go            路由、监听、INI 加载、双语 mux 注册
├── content.go         Document 结构体、SQLite schema、Markdown 解析（含 <!-- en --> 分隔）
├── render.go          模板渲染、PageData、lang/Root/SwitchURL 计算
├── config.go          INI 解析器（~110 行，无第三方依赖）
├── website.ini        运行时配置（addr、路径、ACME）
├── nway_build.sh      交叉编译 Linux amd64
├── setup-acme-cache.sh 生产机上创建 /var/lib/nsbc/website/acme-cache 并改写 INI
├── makeico.go         从一张大图生成 favicon.ico + favicon.png（//go:build ignore）
│
├── content/           动态内容（Markdown），启动时全量同步到 SQLite
│   ├── docs/*.md      →  /docs/quickstart   和  /en/docs/quickstart
│   └── blog/*.md      →  /blog/nsbc-1.0     和  /en/blog/nsbc-1.0
│
├── templates/
│   ├── layout.html     全站布局（header/footer/导航/语言切换/备案）
│   ├── dynamic.html    docs/blog 列表 + 详情 + 404 模板
│   └── pages/*.html    8 个静态页：index / features / download / handbook
│                        learn / about / community / webrtc-sip
│                        每页必须同时定义 `page_xxx` (CN) 和 `page_xxx_en` (EN)
│
├── assets/             静态文件，直接由 http.FileServer 暴露 /assets
│   ├── css/style.css
│   ├── js/main.js
│   └── img/favicon.ico / favicon.png / nsbc-logo.jpg
│
└── data/site.db        SQLite 索引（首次启动自动建表，旧库自动 ALTER TABLE 补列）
```

## 本地开发

```powershell
# 1. 进入目录
Set-Location c:\github\nsbc\website

# 2. 编译
$env:GOPROXY = "https://goproxy.cn,direct"
& "C:\Go\bin\go.exe" build -o nsbc-site.exe .

# 3. 启动（默认读 website.ini 的 addr=127.0.0.1:8080）
.\nsbc-site.exe

# 4. 预览
#    http://127.0.0.1:8080/          中文首页
#    http://127.0.0.1:8080/en        英文首页
#    http://127.0.0.1:8080/docs/quickstart
#    http://127.0.0.1:8080/en/docs/quickstart
```

**内容热同步**：启动后首次扫描 `content/` 并同步到 SQLite。之后改动 Markdown，**重启进程**即可生效（没有 watch 模式）。

## 运行时配置：website.ini

```ini
[general]
addr = 127.0.0.1:8080          ; HTTP 监听地址

[paths]
content    = content           ; Markdown 目录
db         = data/site.db      ; SQLite 索引文件
templates  = templates
assets     = assets

[acme]
enable  = false                ; true → Let's Encrypt 自动证书（需 :80 :443 公网可达）
domain  = nsbc.nway.com.cn
email   = 1354608370@qq.com
cache_dir = /var/lib/nsbc/website/acme-cache
staging = false                ; true → LE 测试 CA（不被浏览器信任）
```

命令行参数可覆盖 INI：`.\nsbc-site.exe -addr :8081` 仅监听 8081，ACME 段原样保留。

## 增删改查 — 内容（Markdown）

### 新增一篇 docs 或 blog

**新建文件** `content/docs/my-feature.md` 或 `content/blog/my-feature.md`，front matter + 中文正文 + `<!-- en -->` + 英文正文：

```markdown
---
title: 中文标题
title_en: English Title
order: 20                       ; docs 列表排序（越小越靠前，blog 不需要）
date: 2026-11-16
summary: 中文摘要…
summary_en: English summary…
tags: 标签1, 标签2
tags_en: tag1, tag2
---

中文正文。支持完整 Markdown：表格、代码块、引用、列表、图片。

<!-- en -->

English body here. Same Markdown features available.
```

**启动后自动注册**：
- docs → `{{.Root}}/docs/my-feature`
- blog → `{{.Root}}/blog/my-feature`
- 英文自动加 `/en` 前缀：`{{.Root}}/en/docs/my-feature`

如果没写 `<!-- en -->` 分隔符，英文页会**自动回退到中文内容**（不加翻译、不报错）。

### 修改已有 Markdown

直接改 `.md` 文件，重启进程即可。SQLite 是幂等 upsert，同一 slug 会覆盖旧条目。

### 删除 Markdown

**两步**：
1. 删除 `.md` 文件
2. 重启进程 — 启动扫描时发现文件没了，会从 SQLite 删掉对应行

### 中英分隔符

支持三种写法（空格可选）：`<!-- en -->` / `<!--en-->` / `<!-- en: -->`。分隔符**之前**是中文正文，**之后**是英文正文。分隔符本身会被剥离，不会渲染到 HTML。

### 不翻译的内容（受保护）

以下部分**不要放在** `<!-- en -->` 后面，保持原文：
- 代码块里的命令、路径、URL、RFC 号、端口号
- front matter 里的 YAML 字段名（`title:` 不是 `title_zh:`）
- 表格里的版本号、度量值（例如 "5062"、"Opus maxplaybackrate=8000"）

## 增删改查 — 模板

### 新增一个静态页面（例如 `/pricing`）

1. `templates/pages/pricing.html` 写两个 define 块：

```html
{{define "page_pricing"}}
<!-- CN 版内容 -->
{{end}}

{{define "page_pricing_en"}}
<!-- EN 版内容 -->
{{end}}
```

2. 在 `main.go` 的 `registerPages()` 里注册一次，路由同时覆盖 `/pricing` 和 `/en/pricing`。
3. **所有** `href="/xxx"` 必须改成 `href="{{.Root}}/xxx"`，否则英文页会跳回中文。

### 修改布局（导航 / 页脚 / 语言切换）

编辑 `templates/layout.html`。nav 里的链接用 `{{.Root}}` 前缀：
```html
<a href="{{.Root}}/docs">{{lang . "文档" "Docs"}}</a>
```
语言切换按钮当前语言显示为 `<span>` 禁用态，另一种语言是 `<a href="{{.SwitchURL}}">`。

### 修改 docs/blog 列表 / 详情页

编辑 `templates/dynamic.html` 里的 `page_docs` / `page_doc` / `page_blog` / `page_post` / `page_404` define 块。所有硬编码中文必须用 `{{lang . "中文" "English"}}` 替换。

### 新增 / 替换 favicon / logo

```powershell
Set-Location c:\github\nsbc\website
go run makeico.go assets/img/新源图.jpg assets/img/favicon.ico
# 自动同时生成 assets/img/favicon.png（256x256）
```

## 生产部署

```bash
# 1. 交叉编译
cd website
GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -o nsbc-site .

# 2. 推到服务器，然后：
sudo ./setup-acme-cache.sh           # 建目录 + 改 website.ini acme.enable=true

# 3. 启动
sudo nsbc-site                       # certmagic 自动 :80 challenge + :443 HTTPS
```

`setup-acme-cache.sh` 会把 `website.ini` 的 `[acme]` 段强制写成：
```ini
enable=true
domain=nsbc.nway.com.cn
email=1354608370@qq.com
cache_dir=/var/lib/nsbc/website/acme-cache
staging=false
```
证书到期前 30 天自动续期，后台进程静默完成。

## 依赖

| 包 | 用途 |
|---|---|
| `modernc.org/sqlite` | 纯 Go SQLite，无 CGO |
| `github.com/yuin/goldmark` | Markdown → HTML |
| `github.com/caddyserver/certmagic` | 生产环境 Let's Encrypt 自动证书 |
| `golang.org/x/image/draw` | makeico.go 里的双线性缩放 |

## 常见问题

**Q: 改了 Markdown 为什么页面没更新？**
A: 本项目**没有** content watcher。重启进程，启动扫描会重新 upsert SQLite。也可以直接删 `data/site.db` 再启动，效果相同。

**Q: 如何只更新 en 正文但保留 zh 不变？**
A: 编辑 `<!-- en -->` 之后的部分，分隔符之前不要动。`title_en` / `summary_en` / `tags_en` 同理。

**Q: blog 的 date 字段有什么用？**
A: 列表页按 date 倒序排列。docs 用 `order` 字段升序排列。

**Q: 部署在 nginx / Apache 反代后面怎么办？**
A: INI 里 `acme.enable=false`，让反代处理 HTTPS 和证书，NSBC 只跑 `addr = 127.0.0.1:8080`。反代把 `/` 和 `/en/` 都转发给 127.0.0.1:8080。
