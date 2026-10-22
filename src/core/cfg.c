#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cfg.h"
#include "log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/shm.h>
#ifdef __linux__
#include <sys/syscall.h>
#ifndef SYS_memfd_create
#if defined(__x86_64__)
#define SYS_memfd_create 319
#elif defined(__aarch64__)
#define SYS_memfd_create 279
#endif
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

/* cfg_crypto.c 提供（OpenSSL 可用时；否则 enc: 解不开按错误处理） */
int cfg_secret_decrypt_gcm(const char *b64, char *out, size_t outsz);
const char *cfg_master_key(void); /* NSBC_MASTER_KEY > 密钥文件，均无返回 NULL */

/* ------------------------------------------------------------------ */
/* 节点树                                                              */
/* ------------------------------------------------------------------ */
typedef enum { N_NULL, N_SCALAR, N_MAP, N_LIST } ntype_t;
typedef struct cfg_node {
    ntype_t type;
    char *key;
    char *val;
    struct cfg_node *child;
    struct cfg_node *next;
} cfg_node_t;

typedef struct {
    pthread_rwlock_t lock;
    cfg_node_t *root;
} layer_impl_t;

struct cfg {
    layer_impl_t ly[CFG_L__COUNT];
    cfg_desc_t *desc;
    unsigned ndesc;
    unsigned desc_cap;
    int strict;
    int allow_plain;
    cfg_event_fn evfn;
    void *evud;
    char cfg_dir[PATH_MAX];   /* YAML 文件所在目录（用于相对路径 resolve） */
};

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static cfg_node_t *node_new(ntype_t t, const char *key)
{
    cfg_node_t *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->type = t;
    n->key = key ? xstrdup(key) : NULL;
    return n;
}

static void node_free(cfg_node_t *n)
{
    if (!n)
        return;
    node_free(n->child);
    node_free(n->next);
    free(n->key);
    free(n->val);
    free(n);
}

static cfg_node_t *map_get(cfg_node_t *map, const char *key)
{
    for (cfg_node_t *c = map->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0)
            return c;
    return NULL;
}

static cfg_node_t *map_put(cfg_node_t *map, ntype_t t, const char *key)
{
    cfg_node_t *c = map_get(map, key);
    if (c) {
        if (c->type != t) {
            node_free(c->child);
            free(c->val);
            c->child = NULL;
            c->val = NULL;
            c->type = t;
        }
        return c;
    }
    c = node_new(t, key);
    if (!c)
        return NULL;
    c->next = map->child;
    map->child = c;
    return c;
}

/* 点分路径查找；返回节点（调用方持锁），无有 NULL */
static cfg_node_t *node_lookup(cfg_node_t *root, const char *path)
{
    if (!root)
        return NULL;
    char buf[256];
    size_t l = strlen(path);
    if (l >= sizeof(buf))
        return NULL;
    memcpy(buf, path, l + 1);
    cfg_node_t *cur = root;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ".", &save); tok;
         tok = strtok_r(NULL, ".", &save)) {
        if (cur->type != N_MAP)
            return NULL;
        cur = map_get(cur, tok);
        if (!cur)
            return NULL;
    }
    return cur;
}

static void node_set_scalar(cfg_node_t *root, const char *path,
                            const char *val)
{
    char buf[256];
    size_t l = strlen(path);
    if (l >= sizeof(buf))
        return;
    memcpy(buf, path, l + 1);
    cfg_node_t *cur = root;
    char *save = NULL;
    char *tok = strtok_r(buf, ".", &save);
    while (tok) {
        char *nx = strtok_r(NULL, ".", &save);
        if (nx) {
            cur = map_put(cur, N_MAP, tok);
        } else {
            cfg_node_t *s = map_put(cur, N_SCALAR, tok);
            free(s->val);
            s->val = xstrdup(val);
        }
        tok = nx;
    }
}

/* ------------------------------------------------------------------ */
/* YAML 子集解析（REQ-CFG-002）                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    cfg_t *cfg;
    cfg_node_t *root;
    int strict;
    int err;
    char err_path[256];
} parse_ctx_t;

typedef struct {
    int indent;
    cfg_node_t *container; /* 当前 map/list 容器 */
} plevel_t;

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n'))
        --e;
    *e = '\0';
    return s;
}

static void unquote(char **pv)
{
    char *v = *pv;
    size_t n = strlen(v);
    if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') ||
                   (v[0] == '\'' && v[n - 1] == '\''))) {
        memmove(v, v + 1, n - 2);
        v[n - 2] = '\0';
        *pv = v;
    }
}

static const cfg_desc_t *find_desc(cfg_t *c, const char *path)
{
    for (unsigned i = 0; i < c->ndesc; ++i)
        if (strcmp(c->desc[i].path, path) == 0)
            return &c->desc[i];
    return NULL;
}

/* 标量值对描述符的合法性（类型/范围/枚举）；敏感键不在此判定密文形式
 * （plain:/file:/enc: 在 get_secret 阶段判定），STR/ANY 恒合法。 */
static int scalar_value_ok(const cfg_desc_t *d, const char *v)
{
    if (d->flags & CGF_SECRET)
        return 1;
    if (d->type == CGT_INT) {
        char *e = NULL;
        long x = strtol(v, &e, 10);
        if (e == v || *trim(e) != '\0' || x < d->min_v || x > d->max_v)
            return 0;
    } else if (d->type == CGT_BOOL) {
        if (strcmp(v, "true") && strcmp(v, "false") && strcmp(v, "0") &&
            strcmp(v, "1") && strcmp(v, "yes") && strcmp(v, "no"))
            return 0;
    } else if (d->type == CGT_ENUM && d->enumv) {
        char enbuf[256];
        snprintf(enbuf, sizeof(enbuf), "%s", d->enumv);
        int ok = 0;
        char *sv = NULL, *it = enbuf;
        while ((sv = strsep(&it, ",")) != NULL) {
            if (strcmp(trim(sv), v) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return 0;
    }
    return 1;
}

/* 收集点分路径做校验；path 累积到 buf */
static void validate_scalar(parse_ctx_t *px, const char *path, char *v)
{
    const cfg_desc_t *d = find_desc(px->cfg, path);
    if (!d) {
        if (px->strict) {
            px->err = 1;
            snprintf(px->err_path, sizeof(px->err_path), "%s", path);
        }
        return;
    }
    if (!scalar_value_ok(d, v)) {
        llog(LOG_ERR, "cfg", "validate_scalar FAIL path=%s type=%d enumv='%s' val='%s' len=%zu",
             path, d->type, d->enumv ? d->enumv : "(null)",
             v ? v : "(null)", v ? strlen(v) : 0);
        px->err = 1;
        snprintf(px->err_path, sizeof(px->err_path), "%s", path);
    }
}

typedef struct {
    dev_t dev;
    ino_t ino;
} visit_t;

static int under_dir(const char *dir, const char *child)
{
    char rd[PATH_MAX], rc[PATH_MAX];
    if (!realpath(dir, rd) || !realpath(child, rc))
        return 0;
    size_t n = strlen(rd);
    return strncmp(rd, rc, n) == 0 && (rc[n] == '/' || rc[n] == '\0');
}

static int parse_file(cfg_t *cfg, const char *path, int depth,
                      visit_t *visited, int nvisited, cfg_node_t *root,
                      char *err_path);

static int do_include(cfg_t *cfg, const char *inc, const char *parent,
                      int depth, visit_t *visited, int nvisited,
                      cfg_node_t *root, char *err_path)
{
    char child[PATH_MAX * 2 + 1];
    const char *dir = strrchr(parent, '/');
    char dirbuf[PATH_MAX];
    if (dir) {
        size_t dl = (size_t)(dir - parent);
        snprintf(dirbuf, sizeof(dirbuf), "%.*s", (int)dl, parent);
        snprintf(child, sizeof(child), "%s/%s", dirbuf, inc);
    } else {
        snprintf(child, sizeof(child), "%s", inc);
    }
    struct stat st;
    if (lstat(child, &st) != 0) {
        char sc[220];
        strncpy(sc, child, sizeof(sc) - 1); sc[sizeof(sc) - 1] = '\0';
        snprintf(err_path, 256, "include missing: %s", sc);
        return NSBC_NOENT;
    }
    if (S_ISLNK(st.st_mode) || !under_dir(dir ? dirbuf : ".", child)) {
        char sc[220];
        strncpy(sc, child, sizeof(sc) - 1); sc[sizeof(sc) - 1] = '\0';
        snprintf(err_path, 256, "include escape/symlink: %s", sc);
        return NSBC_PERM_DENIED;
    }
    for (int i = 0; i < nvisited; ++i)
        if (visited[i].dev == st.st_dev && visited[i].ino == st.st_ino) {
            char sc[220];
            strncpy(sc, child, sizeof(sc) - 1); sc[sizeof(sc) - 1] = '\0';
            snprintf(err_path, 256, "include loop: %s", sc);
            return NSBC_PERM_DENIED;
        }
    return parse_file(cfg, child, depth + 1, visited, nvisited, root,
                      err_path);
}

static int parse_file(cfg_t *cfg, const char *path, int depth,
                      visit_t *visited, int nvisited, cfg_node_t *root,
                      char *err_path)
{
    if (depth > 3) {
        snprintf(err_path, 256, "include depth > 3 at %s", path);
        return NSBC_PERM_DENIED;
    }
    struct stat st;
    if (stat(path, &st) != 0)
        return NSBC_NOENT;
    visit_t *nv = realloc(visited, (size_t)(nvisited + 1) * sizeof(visit_t));
    if (!nv)
        return NSBC_NOMEM;
    visited = nv;
    visited[nvisited].dev = st.st_dev;
    visited[nvisited].ino = st.st_ino;

    FILE *fp = fopen(path, "re");
    if (!fp)
        return NSBC_ERR;
    parse_ctx_t px = {cfg, root, cfg->strict, 0, {0}};
    plevel_t stk[64];
    int sp = 0;
    stk[0].indent = -1;
    stk[0].container = root;
    int lineno = 0;
    char linebuf[1024];
    while (fgets(linebuf, sizeof(linebuf), fp)) {
        ++lineno;
        char *s = linebuf;
        int indent = 0;
        while (s[indent] == ' ')
            ++indent;
        /* 约定缩进只用空格；行内 tab 仅允许出现在引号值中 */
        char *p = s + indent;
        p = trim(p);
        if (*p == '\0' || *p == '#')
            continue;
        while (sp > 0 && indent <= stk[sp].indent)
            --sp;
        cfg_node_t *parent = stk[sp].container;
        if (strncmp(p, "- ", 2) == 0 || strcmp(p, "-") == 0) {
            /* 首个列表项把空 map 容器转换为 list */
            if (parent->type == N_MAP && parent->child == NULL)
                parent->type = N_LIST;
            if (parent->type != N_LIST)
                continue;
            char *rest = (p[0] == '-') ? trim(p + 1) : "";
            if (*rest == '\0')
                continue;
            /* "- key: value"：列表项是 map */
            cfg_node_t *item = node_new(N_MAP, NULL);
            item->next = parent->child;
            parent->child = item;
            char *colon = strchr(rest, ':');
            if (colon) {
                *colon = '\0';
                char *k = trim(rest);
                char *v = trim(colon + 1);
                unquote(&v);
                cfg_node_t *sn = map_put(item, N_SCALAR, k);
                free(sn->val);
                sn->val = xstrdup(v);
            }
            stk[++sp].indent = indent;
            stk[sp].container = item;
            continue;
        }
        char *colon = strchr(p, ':');
        if (!colon) {
            snprintf(err_path, 256, "%s:%d syntax", path, lineno);
            fclose(fp);
            return NSBC_PROTO;
        }
        *colon = '\0';
        char *k = trim(p);
        char *v = trim(colon + 1);
        /* 行内注释截断：先剥注释（YAML 行尾 # 永远是注释，与值是否引号无关）
         * 然后 trim 尾部空格，最后 unquote 去掉值外层引号 */
        if (strchr(v, '#')) {
            /* 边界：如果引号包住了 #（如 "#hashtag" 字符串），才保留。
             * 简单实现：统计引号是否成对出现在 # 之前 */
            int in_sq = 0, in_dq = 0;
            for (char *q = v; *q && *q != '#'; ++q) {
                if (*q == '\'' && !in_dq) in_sq = !in_sq;
                else if (*q == '"' && !in_sq) in_dq = !in_dq;
            }
            if (!in_sq && !in_dq) {
                /* 引号已成对闭合，# 不在值引号内 → 截断 */
                char *h = strchr(v, '#');
                *h = '\0';
                v = trim(v);
            }
        }
        unquote(&v);
        if (strcmp(k, "include") == 0) {
            int rc = do_include(cfg, v, path, depth, visited, nvisited + 1,
                                root, err_path);
            if (rc != NSBC_OK) {
                fclose(fp);
                return rc;
            }
            continue;
        }
        if (*v == '\0') {
            /* 容器：根据后续缩进决定 map/list，先建 map，遇 "- " 改 list */
            cfg_node_t *ch = node_new(N_MAP, k);
            ch->next = parent->child;
            parent->child = ch;
            stk[++sp].indent = indent;
            stk[sp].container = ch;
        } else {
            cfg_node_t *sn = map_put(parent, N_SCALAR, k);
            free(sn->val);
            sn->val = xstrdup(v);
            /* 校验由 parse_file 末尾的点分路径遍历统一完成 */
        }
    }
    fclose(fp);
    /* 静态校验：遍历全部标量点分路径 */
    /* 迭代式收集 */
    typedef struct {
        cfg_node_t *n;
        char path[512];
    } wk_t;
    wk_t stack2[128];
    int top = 0;
    snprintf(stack2[0].path, sizeof(stack2[0].path), "%s", "");
    stack2[0].n = root;
    while (top >= 0) {
        wk_t cur = stack2[top--];
        for (cfg_node_t *c = cur.n->child; c; c = c->next) {
            char np[512];
            if (cur.path[0])
                snprintf(np, sizeof(np), "%s.%s", cur.path, c->key ? c->key : "[]");
            else
                snprintf(np, sizeof(np), "%s", c->key ? c->key : "[]");
            if (c->type == N_SCALAR && c->val)
                validate_scalar(&px, np, c->val);
            else if ((c->type == N_MAP || c->type == N_LIST) && c->child &&
                     top < 127) {
                ++top;
                stack2[top].n = c;
                snprintf(stack2[top].path, sizeof(stack2[top].path), "%s", np);
            }
        }
    }
    if (px.err) {
        snprintf(err_path, 256, "%s", px.err_path);
        return NSBC_PROTO;
    }
    return NSBC_OK;
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */
int cfg_create(cfg_t **out, int strict_unknown_keys, int allow_plain_secret)
{
    cfg_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NSBC_NOMEM;
    for (int i = 0; i < CFG_L__COUNT; ++i) {
        pthread_rwlock_init(&c->ly[i].lock, NULL);
        c->ly[i].root = node_new(N_MAP, NULL);
    }
    c->strict = strict_unknown_keys;
    c->allow_plain = allow_plain_secret;
    *out = c;
    return NSBC_OK;
}

void cfg_destroy(cfg_t *c)
{
    if (!c)
        return;
    for (int i = 0; i < CFG_L__COUNT; ++i) {
        node_free(c->ly[i].root);
        pthread_rwlock_destroy(&c->ly[i].lock);
    }
    free(c->desc);
    free(c);
}

int cfg_schema_declare(cfg_t *c, const cfg_desc_t *desc, unsigned n)
{
    if (c->ndesc + n > c->desc_cap) {
        unsigned cap = c->desc_cap ? c->desc_cap * 2 : 64;
        while (cap < c->ndesc + n)
            cap *= 2;
        cfg_desc_t *nd = realloc(c->desc, cap * sizeof(cfg_desc_t));
        if (!nd)
            return NSBC_NOMEM;
        c->desc = nd;
        c->desc_cap = cap;
    }
    memcpy(c->desc + c->ndesc, desc, n * sizeof(cfg_desc_t));
    c->ndesc += n;
    return NSBC_OK;
}

int cfg_load_defaults(cfg_t *c)
{
    cfg_node_t *root = c->ly[CFG_L_DEFAULT].root;
    for (unsigned i = 0; i < c->ndesc; ++i)
        if (c->desc[i].dflt)
            node_set_scalar(root, c->desc[i].path, c->desc[i].dflt);
    return NSBC_OK;
}

int cfg_load_file(cfg_t *c, const char *path)
{
    char err[256] = {0};
    visit_t *visited = NULL;
    cfg_node_t *tmp = node_new(N_MAP, NULL);
    int rc = parse_file(c, path, 0, visited, 0, tmp, err);
    free(visited);
    if (rc != NSBC_OK) {
        node_free(tmp);
        llog(LOG_ERR, "cfg", "load %s failed: %s (%d)", path, err, rc);
        return rc;
    }
    layer_impl_t *l = &c->ly[CFG_L_SYSTEM];
    pthread_rwlock_wrlock(&l->lock);
    node_free(l->root);
    l->root = tmp;
    pthread_rwlock_unlock(&l->lock);

    /* 记住 YAML 文件所在目录，用于后续相对路径 resolve */
    char pathdup[PATH_MAX];
    if (realpath(path, pathdup)) {
        char *slash = strrchr(pathdup, '/');
        if (slash) {
            *slash = '\0';
            snprintf(c->cfg_dir, sizeof(c->cfg_dir), "%s", pathdup);
        }
    }
    return NSBC_OK;
}

/* ------------------------------------------------------------------ */
/* 相对路径 resolve（相对于 YAML 文件所在目录）                           */
/* ------------------------------------------------------------------ */
void cfg_resolve_path(cfg_t *c, const char *src, char *dst, size_t dstsz)
{
    if (!src || !dst || dstsz == 0) {
        if (dst && dstsz > 0) dst[0] = '\0';
        return;
    }

    const char *prefix = NULL;   /* 带前缀的："file:" / "unix:" 等 */
    const char *path = src;      /* 真正要 resolve 的路径部分 */

    /* 识别常见前缀 */
    if (strncmp(src, "file:", 5) == 0) { prefix = "file:"; path = src + 5; }
    else if (strncmp(src, "unix:", 5) == 0) { prefix = "unix:"; path = src + 5; }

    /* 绝对路径、空 cfg_dir、或无 cfg —— 原样拷贝 */
    if (!path || !*path || path[0] == '/' || !c || !c->cfg_dir[0]) {
        snprintf(dst, dstsz, "%s%s", prefix ? prefix : "", path ? path : "");
        return;
    }

    /* 相对路径：normalize "../" 和 "./" 后与 cfg_dir 拼接 */
    const char *remaining = path;
    int up = 0;
    while (strncmp(remaining, "../", 3) == 0) {
        up++;
        remaining += 3;
    }
    while (strncmp(remaining, "./", 2) == 0) {
        remaining += 2;
    }
    if (strcmp(remaining, ".") == 0) remaining = "";

    /* cfg_dir 向上走 up 级（复制一份再截尾斜杠） */
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s", c->cfg_dir);
    for (int i = 0; i < up; i++) {
        char *slash = strrchr(base, '/');
        if (slash && slash != base) {
            *slash = '\0';
        } else if (slash == base) {
            base[1] = '\0';   /* 截到根 "/" */
        } else {
            break;
        }
    }

    const char *pref = prefix ? prefix : "";
    if (remaining[0] == '\0') {
        snprintf(dst, dstsz, "%s%s", pref, base);
    } else if (base[0] == '/' && base[1] == '\0') {
        /* base 是根目录 "/" */
        snprintf(dst, dstsz, "%s/%s", pref, remaining);
    } else {
        snprintf(dst, dstsz, "%s%s/%s", pref, base, remaining);
    }
}

int cfg_apply_env(cfg_t *c, const char *prefix)
{
    if (!prefix)
        prefix = "NSBC_";
    size_t pl = strlen(prefix);
    extern char **environ;
    cfg_node_t *root = c->ly[CFG_L_ENV].root;
    for (char **e = environ; *e; ++e) {
        if (strncmp(*e, prefix, pl) != 0)
            continue;
        char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        char name[256];
        size_t nl = (size_t)(eq - *e - pl);
        if (nl >= sizeof(name))
            continue;
        memcpy(name, *e + pl, nl);
        name[nl] = '\0';
        /* '__' => '.'；单 '_' 保留；全部小写 */
        char path[256];
        size_t j = 0;
        for (size_t i = 0; name[i] && j < sizeof(path) - 1; ++i) {
            if (name[i] == '_' && name[i + 1] == '_') {
                path[j++] = '.';
                ++i;
            } else {
                path[j++] = (char)tolower((unsigned char)name[i]);
            }
        }
        path[j] = '\0';
        node_set_scalar(root, path, eq + 1);
    }
    return NSBC_OK;
}

int cfg_set(cfg_t *c, cfg_layer_t layer, const char *path, const char *value)
{
    if (layer <= CFG_L_ENV || layer >= CFG_L__COUNT)
        return NSBC_INVAL;
    layer_impl_t *l = &c->ly[layer];
    pthread_rwlock_wrlock(&l->lock);
    node_set_scalar(l->root, path, value);
    pthread_rwlock_unlock(&l->lock);
    return NSBC_OK;
}

int cfg_runtime_propose(cfg_t *c, const char *path, const char *value,
                        int apply, const char **reason, int *changed)
{
    if (changed)
        *changed = 0;
    if (!c || !path || !*path || !value) {
        if (reason)
            *reason = "bad-arg";
        return NSBC_INVAL;
    }
    const cfg_desc_t *d = find_desc(c, path);
    if (!d) {
        if (reason)
            *reason = "unknown-key";
        return NSBC_NOENT;
    }
    /* 敏感键不开放在线改写（密文形式/L2 揭示随 P1 命令注册表，REQ-CFG-009） */
    if (d->flags & CGF_SECRET) {
        if (reason)
            *reason = "secret";
        return NSBC_PERM_DENIED;
    }
    /* config_version 只能由离线 migrate 改变（REQ-CFG-006），任何在线
     * 单键写入（含 dry-run）一律拒绝。 */
    if (strcmp(path, "config_version") == 0) {
        if (reason)
            *reason = "migrate-only";
        return NSBC_PERM_DENIED;
    }
    /* 与文件 reload 同黑名单口径（REQ-CFG-007）：非白名单或仅"先开后关"
     * 项一律拒绝；Prometheus 迁移项不走单键 runtime 写入。 */
    if (!(d->flags & CGF_HOT) || (d->flags & CGF_PROM_MIGRATE)) {
        if (reason)
            *reason = "E_NEED_RESTART";
        return NSBC_PERM_DENIED;
    }
    if (!scalar_value_ok(d, value)) {
        if (reason)
            *reason = "bad-value";
        return NSBC_INVAL;
    }
    /* 幂等：与现生效值相同直接成功，不写不发事件（单一真源，杜绝伪变更） */
    const char *cur = NULL;
    if (cfg_get_str(c, path, &cur, NULL) == NSBC_OK && cur &&
        strcmp(cur, value) == 0)
        return NSBC_OK;
    if (!apply)
        return NSBC_OK; /* dry-run（config.validate）：校验到此为止 */
    int rc = cfg_set(c, CFG_L_RUNTIME, path, value);
    if (rc != NSBC_OK) {
        if (reason)
            *reason = "set-failed";
        return rc;
    }
    if (changed)
        *changed = 1;
    /* 事件 detail 只带键名不带值（diff 前后值随 REQ-CFG-010 P1）；
     * cfg_set 已释放层写锁，回调内 cfg_get_* 取读锁不死锁。 */
    if (c->evfn)
        c->evfn(CFG_EV_CHANGED, path, c->evud);
    return NSBC_OK;
}

/* 标量路径收集（黑名单差异用） */
static void collect_scalars(cfg_node_t *n, const char *prefix,
                            void (*cb)(void *ud, const char *path,
                                       const char *val),
                            void *ud)
{
    for (cfg_node_t *ch = n->child; ch; ch = ch->next) {
        char np[300];
        if (prefix[0])
            snprintf(np, sizeof(np), "%s.%s", prefix,
                     ch->key ? ch->key : "[]");
        else
            snprintf(np, sizeof(np), "%s", ch->key ? ch->key : "[]");
        if (ch->type == N_SCALAR)
            cb(ud, np, ch->val ? ch->val : "");
        else if (ch->child)
            collect_scalars(ch, np, cb, ud);
    }
}

static const char *old_scalar(cfg_node_t *oldroot, const char *path)
{
    cfg_node_t *n = node_lookup(oldroot, path);
    return (n && n->type == N_SCALAR) ? n->val : NULL;
}
typedef struct { cfg_t *c; cfg_node_t *old; char bad[256]; int hit; } diff_t;
static void diff_cb(void *ud, const char *path, const char *val)
{
    diff_t *d = (diff_t *)ud;
    if (d->hit)
        return;
    /* config_version 不参与黑名单差异：它有独立的"值必须不变"检查
     * （见 publish_system），豁免后回滚到无该字段的旧快照才不被误拦。 */
    if (strcmp(path, "config_version") == 0)
        return;
    const char *ov = old_scalar(d->old, path);
    if (ov && strcmp(ov, val) == 0)
        return;
    const cfg_desc_t *ds = find_desc(d->c, path);
    if (!ds)
        return; /* 未知键：strict 已在解析阶段拦截 */
    if (!(ds->flags & CGF_HOT) || (ds->flags & CGF_PROM_MIGRATE)) {
        d->hit = 1;
        snprintf(d->bad, sizeof(d->bad), "%s", path);
    }
}
/* 删除键同样检查 */
typedef struct { diff_t *d; cfg_node_t *neww; } delchk_t;
static void del_cb(void *ud, const char *path, const char *val)
{
    delchk_t *x = (delchk_t *)ud;
    (void)val;
    cfg_node_t *n = node_lookup(x->neww, path);
    if (!n)
        diff_cb(x->d, path, "");
}

/* SYSTEM 层差异校验+原子发布（reload 文件 与 rollback 快照同源）：
 * 黑名单 diff 命中→拒绝（发 RELOAD_FAILED，调用方不发布 tmp）；
 * 通过→换根（旧树调用方负责释放与否，这里只交换指针）。 */
/* 事务树的 config_version 有效值：缺省（含 C3 期旧快照）按首版 1.0。 */
static const char *tree_version(cfg_node_t *root)
{
    cfg_node_t *n = node_lookup(root, "config_version");
    if (n && n->type == N_SCALAR && n->val && n->val[0])
        return n->val;
    return "1.0";
}

static int publish_system(cfg_t *c, cfg_node_t *tmp, const char **bad_key)
{
    layer_impl_t *l = &c->ly[CFG_L_SYSTEM];
    pthread_rwlock_wrlock(&l->lock);
    /* REQ-CFG-006：config_version 只能由离线 migrate 改变；reload/
     * rollback 事务中版本必须与当前生效值一致，缺失按 1.0 对齐。 */
    if (strcmp(tree_version(tmp), tree_version(l->root)) != 0) {
        pthread_rwlock_unlock(&l->lock);
        if (bad_key)
            *bad_key = "config_version";
        if (c->evfn)
            c->evfn(CFG_EV_RELOAD_FAILED, "config_version", c->evud);
        llog(LOG_WARN, "cfg",
             "config publish rejected: config_version may only change via "
             "offline 'config migrate'");
        return NSBC_PERM_DENIED;
    }
    diff_t d = {c, l->root, {0}, 0};
    delchk_t dc = {&d, tmp};
    collect_scalars(tmp, "", diff_cb, &d);
    collect_scalars(l->root, "", del_cb, &dc);
    if (d.hit) {
        pthread_rwlock_unlock(&l->lock);
        if (bad_key)
            *bad_key = "E_NEED_RESTART";
        if (c->evfn)
            c->evfn(CFG_EV_RELOAD_FAILED, d.bad, c->evud);
        llog(LOG_WARN, "cfg", "config publish rejected (E_NEED_RESTART): %s",
             d.bad);
        return NSBC_PERM_DENIED;
    }
    cfg_node_t *old = l->root;
    l->root = tmp;
    pthread_rwlock_unlock(&l->lock);
    node_free(old); /* P0：非 RCU；白名单读侧 P1 随查询缓存换 RCU */
    return NSBC_OK;
}

int cfg_reload_file(cfg_t *c, const char *path, const char **bad_key)
{
    char err[256] = {0};
    cfg_node_t *tmp = node_new(N_MAP, NULL);
    visit_t *visited = NULL;
    int rc = parse_file(c, path, 0, visited, 0, tmp, err);
    free(visited);
    if (rc != NSBC_OK) {
        node_free(tmp);
        if (c->evfn)
            c->evfn(CFG_EV_RELOAD_FAILED, err, c->evud);
        if (bad_key)
            *bad_key = NULL;
        return rc;
    }
    rc = publish_system(c, tmp, bad_key);
    if (rc != NSBC_OK) {
        node_free(tmp);
        return rc;
    }
    if (c->evfn)
        c->evfn(CFG_EV_RELOAD_SUCCESS, path, c->evud);
    return NSBC_OK;
}

/* ---- 配置快照落盘与回滚（REQ-CFG-006/008） ---- */
#define CFG_SNAP_DIR_KEY "system.config_snapshot_dir"
#define CFG_SNAP_KEEP_KEY "system.config_snapshot_keep"
#define CFG_SNAP_DIR_DFLT "/var/lib/nsbc/config-snapshots"

/* 快照 basename 白名单：[A-Za-z0-9._-]、.yaml 结尾、不点起止、无 ".."。 */
static int snap_name_ok(const char *s)
{
    if (!s)
        return 0;
    size_t n = strlen(s);
    if (n < 6 || n > 127 || s[0] == '.')
        return 0;
    if (strcmp(s + n - 5, ".yaml") != 0)
        return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (!isalnum(ch) && ch != '.' && ch != '_' && ch != '-')
            return 0;
    }
    return strstr(s, "..") == NULL;
}

/* 取快照目录到自有缓冲：cfg_get_str 用 __thread 临时窗，后续调用会覆盖。 */
static void snapshot_dir(cfg_t *c, char *buf, size_t bufsz)
{
    const char *dir = NULL;
    if (cfg_get_str(c, CFG_SNAP_DIR_KEY, &dir, NULL) != NSBC_OK || !dir)
        dir = CFG_SNAP_DIR_DFLT;
    snprintf(buf, bufsz, "%s", dir);
}

typedef struct {
    char **v;
    int n, cap;
} snames_t;

static int snames_push(snames_t *a, const char *name)
{
    if (a->n == a->cap) {
        int ncap = a->cap ? a->cap * 2 : 16;
        char **nv = realloc(a->v, (size_t)ncap * sizeof(*nv));
        if (!nv)
            return 0;
        a->v = nv;
        a->cap = ncap;
    }
    char *dup = xstrdup(name);
    if (!dup)
        return 0;
    a->v[a->n++] = dup;
    return 1;
}

static void snames_free(snames_t *a)
{
    for (int i = 0; i < a->n; ++i)
        free(a->v[i]);
    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

/* 名即 UTC 定宽时间戳，字典序=时间序；降序=新→旧。 */
static int snap_cmp_desc(const void *a, const void *b)
{
    const char *const *pa = (const char *const *)a;
    const char *const *pb = (const char *const *)b;
    return strcmp(*pb, *pa);
}

/* 启动快照定宽名 YYYYMMDDTHHMMSSZ.yaml（字典序=时间序）。
 * migrate- 前缀的迁移前备份（REQ-CFG-006）虽在同目录，但不进
 * snapshots/rollback 视图，也不受 keep 淘汰。 */
static int snap_boot_name(const char *s)
{
    static const char pat[] = "00000000T000000Z.yaml";
    size_t n = strlen(s);
    if (n != sizeof(pat) - 1)
        return 0;
    for (size_t i = 0; i < n; ++i) {
        if (pat[i] == '0') {
            if (s[i] < '0' || s[i] > '9')
                return 0;
        } else if (s[i] != pat[i]) {
            return 0;
        }
    }
    return 1;
}

/* 扫描目录中合法快照名（不排序）；boot_only=1 只收定宽启动快照名。
 * 目录不存在返回 NSBC_NOENT。 */
static int snapshot_scan(const char *dir, snames_t *out, int boot_only)
{
    DIR *d = opendir(dir);
    if (!d)
        return (errno == ENOENT) ? NSBC_NOENT : NSBC_ERR;
    int rc = NSBC_OK;
    const struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!snap_name_ok(de->d_name))
            continue;
        if (boot_only && !snap_boot_name(de->d_name))
            continue;
        if (!snames_push(out, de->d_name)) {
            rc = NSBC_NOMEM;
            break;
        }
    }
    closedir(d);
    return rc;
}

/* 同目录 tmp+rename 原子落盘（0600；落盘后 best-effort fsync 目录项）。 */
static int snap_write(const char *dir, const char *name, const char *data,
                      size_t len)
{
    char tmp[PATH_MAX], dst[PATH_MAX];
    int nt = snprintf(tmp, sizeof(tmp), "%s/.%s.XXXXXX", dir, name);
    int nd = snprintf(dst, sizeof(dst), "%s/%s", dir, name);
    if (nt < 0 || (size_t)nt >= sizeof(tmp) || nd < 0 ||
        (size_t)nd >= sizeof(dst))
        return NSBC_NOSPC;
    int fd = mkstemp(tmp);
    if (fd < 0)
        return NSBC_ERR;
    (void)fchmod(fd, 0600);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return NSBC_ERR;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp);
        return NSBC_ERR;
    }
    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        return NSBC_ERR;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    return NSBC_OK;
}

int cfg_snapshot_save(cfg_t *c, char *out_name, size_t outsz)
{
    if (!c)
        return NSBC_INVAL;
    char dir[512];
    snapshot_dir(c, dir, sizeof(dir));
    long keep = 20;
    cfg_get_int(c, CFG_SNAP_KEEP_KEY, &keep, NULL);
    if (keep < 1)
        keep = 20;
    if (mkdir_p(dir) != NSBC_OK) {
        llog(LOG_WARN, "cfg", "snapshot dir unavailable: %s", dir);
        return NSBC_ERR;
    }

    /* 脱敏生效视图；64KB 起步按 NOSPC 倍增（封顶 1MB）。 */
    size_t cap = 1u << 16;
    char *body = malloc(cap);
    if (!body)
        return NSBC_NOMEM;
    int rc = cfg_export_yaml(c, body, cap, 1);
    while (rc == NSBC_NOSPC) {
        cap <<= 1;
        if (cap > (1u << 20)) {
            free(body);
            return NSBC_NOMEM;
        }
        char *nb = realloc(body, cap);
        if (!nb) {
            free(body);
            return NSBC_NOMEM;
        }
        body = nb;
        rc = cfg_export_yaml(c, body, cap, 1);
    }
    if (rc != NSBC_OK) {
        free(body);
        return rc;
    }

    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char name[64];
    if (snprintf(name, sizeof(name), "%04d%02d%02dT%02d%02d%02dZ.yaml",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                 tmv.tm_min, tmv.tm_sec) <= 0) {
        free(body);
        return NSBC_ERR;
    }
    rc = snap_write(dir, name, body, strlen(body));
    free(body);
    if (rc != NSBC_OK) {
        llog(LOG_WARN, "cfg", "snapshot write failed: %s/%s rc=%d", dir,
             name, rc);
        return rc;
    }

    /* 保留策略：新→旧排序，超出 keep 的最旧份删除（失败不致命）。 */
    snames_t all = {0};
    if (snapshot_scan(dir, &all, 1) == NSBC_OK) {
        qsort(all.v, (size_t)all.n, sizeof(all.v[0]), snap_cmp_desc);
        for (int i = (int)keep; i < all.n; ++i) {
            char p[PATH_MAX];
            if (snprintf(p, sizeof(p), "%s/%s", dir, all.v[i]) > 0)
                (void)unlink(p);
        }
        snames_free(&all);
    }
    if (out_name && outsz)
        snprintf(out_name, outsz, "%s", name);
    return NSBC_OK;
}

int cfg_snapshot_list(cfg_t *c, char *out, size_t outsz, int *count)
{
    if (count)
        *count = 0;
    if (!c || !out)
        return NSBC_INVAL;
    char dir[512];
    snapshot_dir(c, dir, sizeof(dir));
    snames_t all = {0};
    int rc = snapshot_scan(dir, &all, 1);
    if (rc != NSBC_OK)
        return rc; /* 含空目录 NOENT：调用方按无快照处理 */
    qsort(all.v, (size_t)all.n, sizeof(all.v[0]), snap_cmp_desc);
    size_t used = 0;
    for (int i = 0; i < all.n; ++i) {
        int n = snprintf(out + used, outsz - used, "%s\n", all.v[i]);
        if (n < 0) {
            snames_free(&all);
            return NSBC_ERR;
        }
        if ((size_t)n >= outsz - used) {
            snames_free(&all);
            return NSBC_NOSPC;
        }
        used += (size_t)n;
    }
    if (count)
        *count = all.n;
    snames_free(&all);
    return NSBC_OK;
}

/* 快照平铺行（"<dotted.key>: <value>"）载入 tmp：注释/空行跳过，
 * 无冒号=坏文件；未知键跳过（前向兼容 WARN）；敏感行不采纳文件值，
 * 一律以当前生效值内存 carry（快照永不落敏感材料，亦防注入明文）；
 * 其余值走与 set/validate 同一条标量校验。 */
static int snapshot_load(cfg_t *c, FILE *fp, cfg_node_t *root, int *rows)
{
    int nrow = 0;
    char linebuf[1024];
    while (fgets(linebuf, sizeof(linebuf), fp)) {
        char *e = linebuf + strlen(linebuf);
        while (e > linebuf && (e[-1] == '\n' || e[-1] == '\r'))
            *--e = '\0';
        char *p = linebuf;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '\0' || *p == '#')
            continue;
        char *colon = strchr(p, ':');
        if (!colon)
            return NSBC_PROTO;
        *colon = '\0';
        char *key = trim(p);
        const char *val = colon + 1;
        while (*val == ' ' || *val == '\t')
            ++val;
        if (*key == '\0' || *val == '\0')
            return NSBC_PROTO;
        const cfg_desc_t *ds = find_desc(c, key);
        if (!ds) {
            llog(LOG_WARN, "cfg", "snapshot skip unknown key: %s", key);
            continue;
        }
        if (ds->flags & CGF_SECRET) {
            /* 敏感行恒应 ****；其它内容不采纳（防经快照注入秘密明文），
             * 统一 carry 当前生效引用。 */
            if (strcmp(val, "****") != 0)
                llog(LOG_WARN, "cfg",
                     "snapshot secret line not redacted, ignored: %s", key);
            const char *cur = NULL;
            if (cfg_get_str(c, key, &cur, NULL) == NSBC_OK)
                node_set_scalar(root, key, cur);
            continue;
        }
        if (!scalar_value_ok(ds, val))
            return NSBC_INVAL;
        node_set_scalar(root, key, val);
        ++nrow;
    }
    *rows = nrow;
    return NSBC_OK;
}

int cfg_rollback(cfg_t *c, const char *id, char *out_name, size_t outsz)
{
    if (!c)
        return NSBC_INVAL;
    char dir[512];
    snapshot_dir(c, dir, sizeof(dir));
    char name[128];
    if (id && id[0]) {
        /* 只允许定宽启动快照；migrate- 迁移备份不是平铺快照格式。 */
        if (!snap_name_ok(id) || !snap_boot_name(id))
            return NSBC_INVAL;
        snprintf(name, sizeof(name), "%s", id);
    } else {
        snames_t all = {0};
        int src = snapshot_scan(dir, &all, 1);
        if (src != NSBC_OK || all.n == 0) {
            snames_free(&all);
            return NSBC_NOENT;
        }
        qsort(all.v, (size_t)all.n, sizeof(all.v[0]), snap_cmp_desc);
        snprintf(name, sizeof(name), "%s", all.v[0]);
        snames_free(&all);
    }
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return NSBC_INVAL;
    FILE *fp = fopen(path, "re");
    if (!fp)
        return (errno == ENOENT) ? NSBC_NOENT : NSBC_ERR;
    cfg_node_t *tmp = node_new(N_MAP, NULL);
    if (!tmp) {
        fclose(fp);
        return NSBC_NOMEM;
    }
    int rows = 0;
    int rc = snapshot_load(c, fp, tmp, &rows);
    fclose(fp);
    if (rc != NSBC_OK) {
        node_free(tmp);
        return rc;
    }
    if (rows == 0) { /* 空/坏快照：拒绝，不产生空生效态 */
        node_free(tmp);
        return NSBC_PROTO;
    }
    /* 黑名单差异+原子发布，与 cfg_reload_file 同源（失败 tmp 不发布）。 */
    rc = publish_system(c, tmp, NULL);
    if (rc != NSBC_OK) {
        node_free(tmp);
        return rc;
    }
    /* 抹掉启动后的 runtime 单键覆盖（config.set），让生效态真正回到快照。 */
    layer_impl_t *rl = &c->ly[CFG_L_RUNTIME];
    pthread_rwlock_wrlock(&rl->lock);
    cfg_node_t *oldrt = rl->root;
    cfg_node_t *empty = node_new(N_MAP, NULL);
    if (!empty) { /* 极罕：SYSTEM 已发布，仅 runtime 清理失败，保持旧层不动 */
        pthread_rwlock_unlock(&rl->lock);
        return NSBC_NOMEM;
    }
    rl->root = empty;
    pthread_rwlock_unlock(&rl->lock);
    node_free(oldrt);

    if (out_name && outsz)
        snprintf(out_name, outsz, "%s", name);
    if (c->evfn)
        c->evfn(CFG_EV_ROLLBACK, name, c->evud);
    return NSBC_OK;
}

/* ---- REQ-CFG-006：配置 schema 版本与离线迁移 ---- */

int cfg_version_parse(const char *s, int *major, int *minor)
{
    if (!s || !major || !minor)
        return NSBC_INVAL;
    const char *dot = strchr(s, '.');
    if (!dot || dot == s || dot[1] == '\0')
        return NSBC_INVAL; /* 必须正好是 "M.N"，无空段、无多余字符 */
    int ma = 0, mi = 0;
    for (const char *p = s; p < dot; ++p) {
        if (*p < '0' || *p > '9')
            return NSBC_INVAL;
        ma = ma * 10 + (*p - '0');
    }
    for (const char *p = dot + 1; *p; ++p) {
        if (*p < '0' || *p > '9')
            return NSBC_INVAL;
        mi = mi * 10 + (*p - '0');
    }
    if (ma > 999 || mi > 999)
        return NSBC_INVAL;
    *major = ma;
    *minor = mi;
    return NSBC_OK;
}

int cfg_version_check(const char *have, int *newer_minor)
{
    if (newer_minor)
        *newer_minor = 0;
    int hm = 0, hn = 0, sm = 0, sn = 0;
    if (cfg_version_parse(have, &hm, &hn) != NSBC_OK ||
        cfg_version_parse(CFG_SCHEMA_VERSION, &sm, &sn) != NSBC_OK)
        return NSBC_INVAL;
    if (hm != sm)
        return NSBC_PERM_DENIED; /* 主版本不兼容：拒绝启动并提示迁移 */
    if (newer_minor && hn > sn)
        *newer_minor = 1; /* 同主版本、文件次版本更新：兼容，调用方 WARN */
    return NSBC_OK;
}

#define CFG_MIGRATE_MAX (1u << 20) /* 迁移读入上限 1MiB */

/* 整文件读入 NUL 结尾缓冲（调用方 free）；超上限 NSBC_NOSPC。 */
static int slurp_file(const char *path, char **out, size_t *outlen)
{
    FILE *fp = fopen(path, "re");
    if (!fp)
        return NSBC_NOENT;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(fp);
        return NSBC_NOMEM;
    }
    for (;;) {
        if (len == cap) {
            if (cap >= CFG_MIGRATE_MAX) {
                free(buf);
                fclose(fp);
                return NSBC_NOSPC;
            }
            char *nb = realloc(buf, cap << 1);
            if (!nb) {
                free(buf);
                fclose(fp);
                return NSBC_NOMEM;
            }
            buf = nb;
            cap <<= 1;
        }
        size_t r = fread(buf + len, 1, cap - len, fp);
        len += r;
        if (r == 0) {
            if (ferror(fp)) {
                free(buf);
                fclose(fp);
                return NSBC_ERR;
            }
            break;
        }
    }
    fclose(fp);
    char *nb = realloc(buf, len + 1); /* 缩容失败可继续用原缓冲 */
    if (nb)
        buf = nb;
    buf[len] = '\0';
    *out = buf;
    *outlen = len;
    return NSBC_OK;
}

/* 找顶层（行首无缩进）config_version 行。
 * 返回 1=找到（line、next 出参给出行首与下一换行后位置，val 为去注释/引号
 * 的值）；0=缺失；负=错误（引号不配对 PROTO、值过长 NOSPC）。 */
static int top_version_find(const char *text, const char **line,
                            const char **next, char *val, size_t valsz)
{
    static const char key[] = "config_version";
    const size_t kl = sizeof(key) - 1;
    const char *p = text;
    for (;;) {
        const char *e = strchr(p, '\n');
        const char *ee = e ? e : p + strlen(p);
        const char *s = p;
        while (s < ee && (*s == ' ' || *s == '\t'))
            ++s;
        if (s == p && (size_t)(ee - s) >= kl && strncmp(s, key, kl) == 0) {
            const char *q = s + kl;
            while (q < ee && (*q == ' ' || *q == '\t'))
                ++q;
            if (q < ee && *q == ':') {
                ++q;
                while (q < ee && (*q == ' ' || *q == '\t'))
                    ++q;
                size_t vn = (size_t)(ee - q);
                while (vn > 0 &&
                       (q[vn - 1] == ' ' || q[vn - 1] == '\t' ||
                        q[vn - 1] == '\r'))
                    --vn;
                int quoted = (vn > 0 && (q[0] == '"' || q[0] == '\''));
                if (quoted) {
                    if (vn < 2 || q[vn - 1] != q[0])
                        return NSBC_PROTO;
                    ++q;
                    vn -= 2;
                } else {
                    for (size_t i = 0; i < vn; ++i) {
                        if (q[i] == '#') { /* 无引号：行内注释同解析器口径 */
                            vn = i;
                            break;
                        }
                    }
                    while (vn > 0 && (q[vn - 1] == ' ' || q[vn - 1] == '\t'))
                        --vn;
                }
                if (vn >= valsz)
                    return NSBC_NOSPC;
                memcpy(val, q, vn);
                val[vn] = '\0';
                if (line)
                    *line = p;
                if (next)
                    *next = e ? e + 1 : ee;
                return 1;
            }
        }
        if (!e)
            return 0;
        p = e + 1;
    }
}

/* 文件开头连续注释/空行块的长度（版本行插入点）。 */
static size_t header_len(const char *t)
{
    const char *p = t;
    for (;;) {
        const char *e = strchr(p, '\n');
        const char *ee = e ? e : p + strlen(p);
        const char *s = p;
        while (s < ee && (*s == ' ' || *s == '\t'))
            ++s;
        if (*s && *s != '#')
            return (size_t)(p - t);
        if (!e)
            return (size_t)(p - t);
        p = e + 1;
    }
}

/* 迁移路径登记表：首版仅 1.0→1.0（幂等规范化：补/改顶层版本行，无结构
 * 变更）。未来新增 schema 版本时在此挂结构变换函数。 */
static int migration_supported(const char *from, const char *to)
{
    return strcmp(from, "1.0") == 0 && strcmp(to, "1.0") == 0;
}

/* 生成迁移后文本：found=1 替换原版本行；found=0 在文件头注释块后插入。 */
static int migrate_render(const char *text, int found, const char *line,
                          const char *nxt, const char *to, char **out,
                          size_t *outlen)
{
    char nv[64];
    int nn = snprintf(nv, sizeof(nv), "config_version: \"%s\"\n", to);
    if (nn < 0 || (size_t)nn >= sizeof(nv))
        return NSBC_INVAL;
    size_t pre_n = found ? (size_t)(line - text) : header_len(text);
    const char *post = found ? nxt : text + pre_n;
    size_t post_n = strlen(post);
    size_t total = pre_n + (size_t)nn + post_n;
    char *buf = malloc(total + 1);
    if (!buf)
        return NSBC_NOMEM;
    memcpy(buf, text, pre_n);
    memcpy(buf + pre_n, nv, (size_t)nn);
    memcpy(buf + pre_n + (size_t)nn, post, post_n);
    buf[total] = '\0';
    *out = buf;
    *outlen = total;
    return NSBC_OK;
}

int cfg_migrate_file(cfg_t *c, const char *path, const char *from,
                     const char *to, char *backup_out, size_t bsz)
{
    if (!c || !path || !from || !to)
        return NSBC_INVAL;
    int fmj = 0, fmn = 0, tmj = 0, tmn = 0;
    if (cfg_version_parse(from, &fmj, &fmn) != NSBC_OK ||
        cfg_version_parse(to, &tmj, &tmn) != NSBC_OK)
        return NSBC_INVAL;
    if (!migration_supported(from, to))
        return NSBC_NOTSUP;

    char *text = NULL;
    size_t len = 0;
    int rc = slurp_file(path, &text, &len);
    if (rc != NSBC_OK)
        return rc;

    char have[64];
    const char *line = NULL;
    const char *nxt = NULL;
    int found = top_version_find(text, &line, &nxt, have, sizeof(have));
    if (found < 0) {
        free(text);
        return found;
    }
    if (found) {
        if (strcmp(have, from) != 0) { /* 防对错误版本误操作 */
            free(text);
            return NSBC_INVAL;
        }
    } else if (strcmp(from, "1.0") != 0) {
        free(text); /* 无版本字段的文件只允许按首版 1.0 迁出 */
        return NSBC_INVAL;
    }

    /* 迁移前备份原文件字节到快照目录（migrate- 前缀，独立命名空间）。 */
    char dir[512];
    snapshot_dir(c, dir, sizeof(dir));
    if (mkdir_p(dir) != NSBC_OK) {
        free(text);
        return NSBC_ERR;
    }
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    char bname[256];
    if (snprintf(bname, sizeof(bname),
                 "migrate-%04d%02d%02dT%02d%02d%02dZ-%s",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                 base) >= (int)sizeof(bname) ||
        !snap_name_ok(bname)) {
        free(text);
        return NSBC_INVAL;
    }
    /* 秒级时间戳可能撞名（同秒内多次迁移）：在 .yaml 前追加序号避让，
     * 绝不静默覆盖既有备份。 */
    {
        char stem[256];
        char probe[PATH_MAX];
        int sl = (int)strlen(bname) - 5; /* 去掉 ".yaml" */
        if (sl <= 0 || sl >= (int)sizeof(stem)) {
            free(text);
            return NSBC_INVAL;
        }
        memcpy(stem, bname, (size_t)sl);
        stem[sl] = '\0';
        for (int seq = 2;; ++seq) {
            if (snprintf(probe, sizeof(probe), "%s/%s", dir, bname) < 0) {
                free(text);
                return NSBC_INVAL;
            }
            struct stat cst;
            if (stat(probe, &cst) != 0)
                break; /* 名字空闲 */
            if (seq > 999 ||
                snprintf(bname, sizeof(bname), "%s-%d.yaml", stem, seq)
                    >= (int)sizeof(bname)) {
                free(text);
                return NSBC_INVAL;
            }
        }
    }
    rc = snap_write(dir, bname, text, len);
    if (rc != NSBC_OK) {
        free(text);
        return rc;
    }

    char *ntext = NULL;
    size_t nlen = 0;
    rc = migrate_render(text, found, line, nxt, to, &ntext, &nlen);
    free(text);
    if (rc != NSBC_OK)
        return rc;

    /* 原子替换原文件：同目录 tmp+rename。 */
    char pdir[PATH_MAX];
    if (slash) {
        size_t dl = (size_t)(slash - path);
        if (dl == 0 || dl >= sizeof(pdir)) {
            free(ntext);
            return NSBC_INVAL;
        }
        snprintf(pdir, sizeof(pdir), "%.*s", (int)dl, path);
    } else {
        snprintf(pdir, sizeof(pdir), "%s", ".");
    }
    rc = snap_write(pdir, base, ntext, nlen);
    if (rc != NSBC_OK) {
        free(ntext); /* 原文件未动；备份已留存 */
        return rc;
    }

    /* 迁移后静态校验：同一 strict schema 解析（含 include 链与值域）。 */
    cfg_node_t *probe = node_new(N_MAP, NULL);
    if (!probe) {
        free(ntext);
        return NSBC_NOMEM;
    }
    char err[256] = {0};
    visit_t *visited = NULL;
    int prc = parse_file(c, path, 0, visited, 0, probe, err);
    free(visited);
    node_free(probe);
    if (prc != NSBC_OK) {
        /* 校验失败：用备份字节恢复原文件（跨挂载点安全；备份保留排查）。 */
        char bp[PATH_MAX];
        char *orig = NULL;
        size_t olen = 0;
        if (snprintf(bp, sizeof(bp), "%s/%s", dir, bname) > 0 &&
            slurp_file(bp, &orig, &olen) == NSBC_OK) {
            (void)snap_write(pdir, base, orig, olen);
            free(orig);
        }
        free(ntext);
        llog(LOG_ERR, "cfg",
             "migrate post-check failed: %s; original restored from %s",
             err, bname);
        return prc;
    }
    free(ntext);
    if (backup_out && bsz)
        snprintf(backup_out, bsz, "%s", bname);
    llog(LOG_NOTICE, "cfg", "config migrated %s: %s -> %s (backup %s)",
         path, from, to, bname);
    return NSBC_OK;
}

void cfg_set_event_cb(cfg_t *c, cfg_event_fn fn, void *ud)
{
    c->evfn = fn;
    c->evud = ud;
}

static cfg_node_t *find_effective(cfg_t *c, const char *path,
                                  layer_impl_t **inlayer)
{
    for (int li = CFG_L_SESSION; li >= CFG_L_DEFAULT; --li) {
        layer_impl_t *l = &c->ly[li];
        pthread_rwlock_rdlock(&l->lock);
        cfg_node_t *n = node_lookup(l->root, path);
        if (n && n->type == N_SCALAR) {
            if (inlayer)
                *inlayer = l;
            return n; /* 调用方读完即解锁：值在锁内拷贝出去 */
        }
        pthread_rwlock_unlock(&l->lock);
    }
    return NULL;
}

int cfg_get_str(cfg_t *c, const char *path, const char **out,
                cfg_layer_t *layer)
{
    layer_impl_t *l = NULL;
    cfg_node_t *n = find_effective(c, path, &l);
    if (!n)
        return NSBC_NOENT;
    /* 标量值会在热发布时整体替换节点（map_put 新建），指针安全窗口由
     * 调用方立即拷贝保证；规范做法见函数注释（P0 简化）。 */
    static __thread char copy[512];
    snprintf(copy, sizeof(copy), "%s", n->val ? n->val : "");
    pthread_rwlock_unlock(&l->lock);

    /* P0: YAML 行尾注释 strip 防御 + 外围引号剥离。
     * 2026-10-01 Bug#2: extern_ip: ""          # 注释 → cfg_get_str 返回
     * 整个 ""          # 注释 字符串，导致 extern_ip[0]='"'\0' ≠ '\0'
     * 跳过 auto-detect → local_host 被污染 → net_udp_open 返回 -3 (NSBC_INVAL)。 */
    {
        size_t len = strlen(copy);
        /* 找行尾注释: # 前有空白则截断 */
        for (size_t i = 0; i < len; i++) {
            if (copy[i] == '#' && (i == 0 || copy[i-1] == ' ' || copy[i-1] == '\t')) {
                copy[i] = '\0';
                while (i > 0 && (copy[i-1] == ' ' || copy[i-1] == '\t'))
                    copy[--i] = '\0';
                break;
            }
        }
        /* 剥外围成对单/双引号 */
        len = strlen(copy);
        if (len >= 2 && (copy[0] == '"' || copy[0] == '\'')
            && copy[0] == copy[len-1]) {
            memmove(copy, copy+1, len-2);
            copy[len-2] = '\0';
        }
    }

    if (layer) {
        /* 层索引：ly 数组固定基址，可反推 */
        *layer = (cfg_layer_t)(l - c->ly);
    }
    *out = copy;
    return NSBC_OK;
}

int cfg_get_int(cfg_t *c, const char *path, long *out, cfg_layer_t *layer)
{
    const char *s = NULL;
    cfg_layer_t l;
    if (cfg_get_str(c, path, &s, &l) != NSBC_OK)
        return NSBC_NOENT;
    *out = strtol(s, NULL, 10);
    if (layer)
        *layer = l;
    return NSBC_OK;
}

int cfg_get_bool(cfg_t *c, const char *path, int *out, cfg_layer_t *layer)
{
    const char *s = NULL;
    cfg_layer_t l;
    if (cfg_get_str(c, path, &s, &l) != NSBC_OK)
        return NSBC_NOENT;
    *out = (!strcmp(s, "true") || !strcmp(s, "1") || !strcmp(s, "yes"));
    if (layer)
        *layer = l;
    return NSBC_OK;
}

const char *cfg_layer_name(cfg_layer_t l)
{
    static const char *names[CFG_L__COUNT] = {
        "default", "system", "env", "profile", "runtime", "session"};
    return names[l];
}

static int path_secret(cfg_t *c, const char *path)
{
    const cfg_desc_t *d = find_desc(c, path);
    return d && (d->flags & CGF_SECRET);
}

int cfg_show(cfg_t *c, const char *path, char *out, size_t outsz, int reveal)
{
    const char *v = NULL;
    cfg_layer_t l;
    if (cfg_get_str(c, path, &v, &l) != NSBC_OK)
        return NSBC_NOENT;
    if (!reveal && path_secret(c, path))
        snprintf(out, outsz, "****\t# layer=%s", cfg_layer_name(l));
    else
        snprintf(out, outsz, "%s\t# layer=%s", v, cfg_layer_name(l));
    return NSBC_OK;
}

int cfg_get_secret(cfg_t *c, const char *path, char *out, size_t outsz)
{
    const char *v = NULL;
    if (cfg_get_str(c, path, &v, NULL) != NSBC_OK)
        return NSBC_NOENT;
    if (!strncmp(v, "plain:", 6)) {
        if (!c->allow_plain) {
            llog(LOG_ERR, "cfg", "plain secret rejected in production: %s",
                 path);
            return NSBC_PERM_DENIED;
        }
        snprintf(out, outsz, "%s", v + 6);
        return NSBC_OK;
    }
    if (!strncmp(v, "file:", 5)) {
        FILE *fp = fopen(v + 5, "re");
        if (!fp)
            return NSBC_NOENT;
        size_t n = fread(out, 1, outsz - 1, fp);
        fclose(fp);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            --n;
        out[n] = '\0';
        return NSBC_OK;
    }
    if (!strncmp(v, "enc:", 4)) {
        if (!cfg_master_key()) {
            llog(LOG_ERR, "cfg", "master key missing: %s", path);
            return NSBC_AUTH;
        }
        int rc = cfg_secret_decrypt_gcm(v + 4, out, outsz);
        if (rc != NSBC_OK)
            llog(LOG_ERR, "cfg", "decrypt failed: %s", path);
        return rc;
    }
    return NSBC_PROTO;
}

/* 导出生效视图（描述符即权威键集合，REQ-CFG-013） */
int cfg_export_yaml(cfg_t *c, char *out, size_t outsz, int redact)
{
    size_t used = 0;
    int n = snprintf(out, outsz, "# effective config snapshot (redact=%d)\n",
                     redact);
    if (n < 0 || (size_t)n >= outsz)
        return NSBC_NOSPC;
    used = (size_t)n;
    for (unsigned i = 0; i < c->ndesc; ++i) {
        const char *v = NULL;
        if (cfg_get_str(c, c->desc[i].path, &v, NULL) != NSBC_OK)
            continue;
        const char *show = v;
        char masked[64];
        if (redact && (c->desc[i].flags & CGF_SECRET)) {
            snprintf(masked, sizeof(masked), "****");
            show = masked;
        }
        n = snprintf(out + used, outsz - used, "%s: %s\n",
                     c->desc[i].path, show);
        if (n < 0)
            return NSBC_ERR;
        if ((size_t)n >= outsz - used)
            return NSBC_NOSPC; /* 整行装不下：不输出截断行，调用方扩容重试 */
        used += (size_t)n;
    }
    return NSBC_OK;
}

/* REQ-CFG-005 阶段二：共享段后端三级回退 memfd→shm_open→SysV */
int cfg_dynamic_precheck(cfg_t *c, const char **failed_item)
{
    (void)c;
    int fd = -1;
#if defined(SYS_memfd_create)
    fd = (int)syscall(SYS_memfd_create, "nsbc-probe", MFD_CLOEXEC);
#endif
    if (fd >= 0) {
        close(fd);
        return NSBC_OK;
    }
    fd = shm_open("/nsbc-probe", O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC,
                  0600);
    if (fd >= 0) {
        close(fd);
        shm_unlink("/nsbc-probe");
        return NSBC_OK;
    }
    int sid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (sid >= 0) {
        shmctl(sid, IPC_RMID, NULL);
        return NSBC_OK;
    }
    if (failed_item)
        *failed_item = "shared-segment-backend";
    return NSBC_ERR;
}

/* REQ-ARCH-010：从 SYSTEM 层 YAML 树里读 modules.disable / modules.enable 两个 N_LIST。
 * 遍历 cfg_node_t->child 链表，每项若为 N_SCALAR 则 val 拷出。 */
int cfg_get_module_list(cfg_t *c, char ***out_disables, int *out_ndis,
                        char ***out_enables, int *out_nen)
{
    if (!c) return NSBC_INVAL;
    if (out_disables) { *out_disables = NULL; }
    if (out_ndis)     { *out_ndis = 0; }
    if (out_enables)  { *out_enables = NULL; }
    if (out_nen)      { *out_nen = 0; }

    layer_impl_t *l = &c->ly[CFG_L_SYSTEM];
    pthread_rwlock_rdlock(&l->lock);

    cfg_node_t *mods = node_lookup(l->root, "modules");
    if (!mods || mods->type != N_MAP) {
        pthread_rwlock_unlock(&l->lock);
        return NSBC_NOENT; /* YAML 里没有 modules 段 → 全 enable（REQUIRED + 非 REQUIRED） */
    }

    char **dis = NULL; int nd = 0, capd = 0;
    char **en  = NULL; int ne = 0, cape = 0;

    for (cfg_node_t *child = mods->child; child; child = child->next) {
        if (!child->key) continue;
        int is_disable = (strcmp(child->key, "disable") == 0);
        int is_enable  = (strcmp(child->key, "enable")  == 0);
        if (!is_disable && !is_enable) continue;
        if (child->type != N_LIST) continue;

        int *cnt = is_disable ? &nd : &ne;
        int *cap = is_disable ? &capd : &cape;
        char ***arr = is_disable ? &dis : &en;

        for (cfg_node_t *item = child->child; item; item = item->next) {
            if (item->type != N_SCALAR || !item->val || !item->val[0]) continue;
            if (*cnt == *cap) {
                int nc = *cap ? *cap * 2 : 4;
                char **na = realloc(*arr, (size_t)nc * sizeof(char *));
                if (!na) break;
                *arr = na;
                *cap = nc;
            }
            (*arr)[(*cnt)++] = strdup(item->val);
        }
    }

    pthread_rwlock_unlock(&l->lock);

    if (out_disables) { *out_disables = dis; *out_ndis = nd; }
    else {
        for (int i = 0; i < nd; ++i) free(dis[i]);
        free(dis);
    }
    if (out_enables) { *out_enables = en; *out_nen = ne; }
    else {
        for (int i = 0; i < ne; ++i) free(en[i]);
        free(en);
    }
    return NSBC_OK;
}
