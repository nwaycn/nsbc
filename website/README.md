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

## 历史专栏 — FreeSWITCH / OpenSIPS / Kamailio

网站集成了两个历史技术专栏，从旧站 `freeswitch.net.cn`（2015 起约 239 篇）和 `docs.opensips.net.cn`（约 62 篇）归档而来。

### 目录结构

```
content/
├── freeswitch/*.md     →  /freeswitch/N
└── opensips/*.md       →  /opensips/N
```

每篇文章用源站的文章 ID 作为文件名（如 `263.md`），slug 就是这个数字。路由自动映射：

- `/freeswitch/263` 显示 "Na Stream 新能力发布：被叫侧实时 TTS 检测"
- `/opensips/1` 显示 "WLBLOG 项目说明"
- 英文页自动加 `/en` 前缀：`/en/freeswitch/263`、`/en/opensips/1`

### 格式（简化 front matter）

历史专栏文章**不做双语翻译**（源站全中文），用简化版 front matter：

```markdown
---
title: 文章标题
date: 2026-12-10
tags: FreeSWITCH,ASR,TTS
source: http://freeswitch.net.cn/263.html
slug: 263
---

正文（中文 Markdown / HTML 混合）。
```

和 docs/blog 相比的差异：
- 没有 `title_en` / `<!-- en -->` — 英文页回退到中文内容 + 中文标题
- 多了 `source:` 字段，指向源站原始文章 URL（便于回溯）
- `slug:` 字段明确写出来（等于文件名的数字）

### 增加新文章

**方式 A：手工新建**（推荐用于原创或编辑过的新内容）

```markdown
---
title: FreeSWITCH mod_sofia 调优笔记
date: 2026-11-17
tags: FreeSWITCH,sofia,调优
---

正文。
```

文件名取一个唯一数字（比如 `264.md`，下一个可用 ID），放到 `content/freeswitch/` 或 `content/opensips/` 下。重启进程即生效。

**方式 B：批量从旧站抓取**（importblog.go）

```powershell
# 首次抓取（自动探测最新 ID）
Set-Location c:\github\nsbc\website
$env:GOPROXY = "https://goproxy.cn,direct"
& "C:\Go\bin\go.exe" run importblog.go --domain freeswitch.net.cn --out content/freeswitch --workers 10 --force
& "C:\Go\bin\go.exe" run importblog.go --domain docs.opensips.net.cn --out content/opensips --workers 8 --force

# 增量更新：只抓指定 ID 范围（--force 覆盖已有文件）
& "C:\Go\bin\go.exe" run importblog.go --domain freeswitch.net.cn --out content/freeswitch --start 264 --end 300
```

importblog.go 会：
1. 从首页侧边栏 "最新" 列表自动探测最大文章 ID（跳过月份归档 `202604.html` 这类 6 位编号）
2. 并发抓取每篇文章 HTML（默认 10 worker，--workers 可调）
3. 用 `golang.org/x/net/html` tokenizer 找到正文容器（按 id/class 优先级：`content` → `main-content` → `article` → `span9` → 最大 div）
4. 从 `<title>` 标签、meta keywords、源站 "创建时间：yyyy-mm-dd" 正则提取 title/tags/date
5. 清洗：剥 script/style/iframe/noscript、剥 HTML 注释、剥 `<br/>` `<hr/>`、剥内部导航链接、剥所有残留 HTML 标签只留文本
6. 写 Markdown 文件（UTF-8 无 BOM）

注意：源站部分老文章会返回 HTTP 500（约 24 篇 freeswitch + 3 篇 opensips），属源站服务器故障，importblog.go 会跳过并记录在日志里。

### 修改 / 删除

和 docs/blog 完全一样：
- **改** — 直接编辑 `.md` 文件，重启进程
- **删** — 删 `.md` 文件，重启进程
- **批量替换作者名** — 已通过 PowerShell 脚本把所有 "上海老李" 改成 "上海李浩"，如需再次批量替换可参考根目录历史命令

### 架构说明

`content.go` 的 `Scan()` 扫描 `[docs, blog, freeswitch, opensips]` 四个目录，每个目录的文件按 `WHERE kind='xxx'` 查询分类。`ListBlog()` / `ListFreeswitch()` / `ListOpenSIPS()` 全部在 SQLite 上独立查询，互不干扰。`main.go` 的 `handleList` / `handleDetail` 是通用 handler，通过 `kind` / `Active` / `Title` / `Description` 四个参数区分三个专栏（blog / freeswitch / opensips）。

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
