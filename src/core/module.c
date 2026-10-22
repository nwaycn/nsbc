#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "module.h"
#include "log.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>  /* REQ-ARCH-010：动态模块加载是一等公民（-ldl） */

/* category → 显示名 */
const char *mod_category_name(mod_category_t cat) {
    switch (cat) {
    case MOD_CAT_CORE:          return "core";
    case MOD_CAT_CODEC:         return "codec";
    case MOD_CAT_MEDIA_BUG:     return "media_bug";
    case MOD_CAT_ENDPOINT:      return "endpoint";
    case MOD_CAT_APPLICATION:   return "application";
    case MOD_CAT_BILLING:       return "billing";
    case MOD_CAT_SECURITY:      return "security";
    case MOD_CAT_OBSERVABILITY: return "observability";
    case MOD_CAT_CALL_CENTER:   return "call_center";
    case MOD_CAT_AI:            return "ai";
    case MOD_CAT_OUTBOUND:      return "outbound";
    case MOD_CAT_UTILITY:       return "utility";
    default:                    return "unknown";
    }
}

/* REQ-ARCH-010/011：描述符注册 + 分层拓扑初始化（fail-fast） */

typedef struct {
    const nsbc_module_t *d;
    mod_state_t st;
    void *handle; /* dlopen 句柄（可选） */
} ment_t;

struct nsbc_modmgr {
    ment_t *v;
    unsigned n;
    unsigned cap;
    /* 本次 init 的 DFS 临时状态 */
    unsigned char *color; /* 0 白 1 灰 2 黑 */
    ment_t **order;
};

/* Phase 5 CLI：全局 modmgr 指针（OAM show modules 等命令访问）
 * main.c 在 modmgr_create 后调 modmgr_set_global()，nsbcd_shutdown 时置 NULL */
static nsbc_modmgr_t *g_modmgr_global = NULL;

void modmgr_set_global(nsbc_modmgr_t *m) { g_modmgr_global = m; }
nsbc_modmgr_t *modmgr_get_global(void)   { return g_modmgr_global; }

int modmgr_create(nsbc_modmgr_t **out)
{
    nsbc_modmgr_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NSBC_NOMEM;
    m->cap = 32;
    m->v = calloc(m->cap, sizeof(ment_t));
    m->color = calloc(m->cap, 1);
    m->order = calloc(m->cap, sizeof(ment_t *));
    if (!m->v || !m->color || !m->order) {
        free(m->v);
        free(m->color);
        free(m->order);
        free(m);
        return NSBC_NOMEM;
    }
    *out = m;
    return NSBC_OK;
}

void modmgr_destroy(nsbc_modmgr_t *m)
{
    if (!m)
        return;
    for (unsigned i = 0; i < m->n; ++i) {
        if (m->v[i].handle)
            dlclose(m->v[i].handle);
    }
    free(m->v);
    free(m->color);
    free(m->order);
    free(m);
}

static int find_idx(nsbc_modmgr_t *m, const char *name)
{
    for (unsigned i = 0; i < m->n; ++i)
        if (strcmp(m->v[i].d->name, name) == 0)
            return (int)i;
    return -1;

}

int modmgr_register(nsbc_modmgr_t *m, const nsbc_module_t *mod)
{
    if (!mod || !mod->name || mod->abi != NSBC_MOD_ABI) {
        llog(LOG_ERR, "mod", "bad descriptor (abi=%u)", mod ? mod->abi : 0);
        return NSBC_INVAL;
    }
    if (find_idx(m, mod->name) >= 0) {
        llog(LOG_ERR, "mod", "duplicate module: %s", mod->name);
        return NSBC_EXISTS;
    }
    if (m->n == m->cap) {
        unsigned cap = m->cap * 2;
        ment_t *nv = realloc(m->v, cap * sizeof(ment_t));
        unsigned char *nc = realloc(m->color, cap);
        ment_t **no = realloc(m->order, cap * sizeof(ment_t *));
        if (!nv || !nc || !no) {
            free(nv);
            free(nc);
            free(no);
            return NSBC_NOMEM;
        }
        m->v = nv;
        m->color = nc;
        m->order = no;
        m->cap = cap;
    }
    m->v[m->n].d = mod;
    m->v[m->n].st = MOD_ST_REG;
    ++m->n;
    return NSBC_OK;
}

int modmgr_register_builtin(nsbc_modmgr_t *m,
                            const nsbc_module_t *const *list)
{
    int ok = 0;
    for (size_t i = 0; list[i] != NULL; ++i) {
        const char *name = list[i]->name ? list[i]->name : "(anonymous)";
        int rc = modmgr_register(m, list[i]);
        if (rc != NSBC_OK) {
            llog(LOG_CRIT, "mod", "register builtin %s failed rc=%d — REQUIRED module missing",
                 name, rc);
            return rc;
        }
        ++ok;
        llog(LOG_DEBUG, "mod", "  + registered STATIC %s", name);
    }
    llog(LOG_INFO, "mod", "register_builtin: %d STATIC modules", ok);
    return NSBC_OK;
}

int modmgr_disable(nsbc_modmgr_t *m, const char *name)
{
    if (!m || !name) return NSBC_INVAL;
    int idx = find_idx(m, name);
    if (idx < 0) {
        llog(LOG_WARN, "mod", "disable skip: unknown module '%s'", name);
        return NSBC_NOENT; /* 模块未注册（编译时没编）——忽略，不算错 */
    }
    ment_t *e = &m->v[idx];
    if (e->st == MOD_ST_DISABLED)
        return NSBC_OK; /* 幂等 */
    if (e->d->flags & MOD_F_REQUIRED) {
        llog(LOG_ERR, "mod", "cannot disable REQUIRED module '%s'", name);
        return NSBC_PERM_DENIED;
    }
    e->st = MOD_ST_DISABLED;
    llog(LOG_INFO, "mod", "module '%s' disabled by config", name);
    return NSBC_OK;
}

const char *modmgr_state_of(nsbc_modmgr_t *m, const char *name)
{
    if (!m || !name) return "invalid";
    int idx = find_idx(m, name);
    if (idx < 0) return "missing";
    ment_t *e = &m->v[idx];
    if (e->st == MOD_ST_DISABLED) {
        return (e->d->flags & MOD_F_REQUIRED) ? "required-disabled" : "disabled";
    }
    if (e->st == MOD_ST_INITED || e->st == MOD_ST_STARTED)
        return "enabled";
    return "registered";
}

/* REQ-ARCH-010：动态模块加载。
 * configure 把非 REQUIRED/CALL_PATH 模块编为 build/mod/mod_<name>.so，
 * 这里 dlopen 后 dlsym("nsbcm_<name>") 拿 NSBC_MOD/NSBC_MOD0 宏导出的描述符。
 * - configure 已保证 CALL_PATH 模块进 STATIC 组，不会编 .so → 这里不再拒绝
 * - ABI 不匹配立即 dlclose + 返回错（版本升级时最常见的 fail-fast）
 * - 注册成功后 dlopen 句柄保存在 ment_t.handle，modmgr_destroy 时 dlclose */
int modmgr_load(nsbc_modmgr_t *m, const char *so_path)
{
    if (!m || !so_path || !*so_path) return NSBC_INVAL;

    /* 从 basename 提取模块名：build/mod/mod_billing.so → billing */
    const char *base = strrchr(so_path, '/');
    base = base ? base + 1 : so_path;
    const char *p = base;
    if (strncmp(p, "mod_", 4) == 0) p += 4;  /* 去掉 configure 加的前缀 */
    char modname[NSBC_MOD_NAMELEN];
    snprintf(modname, sizeof(modname), "%s", p);
    char *ext = strrchr(modname, '.');
    if (ext) *ext = '\0';
    if (!modname[0]) {
        llog(LOG_ERR, "mod", "dlopen %s: unable to extract module name", so_path);
        return NSBC_INVAL;
    }

    char sym[128];
    snprintf(sym, sizeof(sym), "nsbcm_%s", modname);

    void *h = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        llog(LOG_ERR, "mod", "dlopen %s: %s", so_path, dlerror());
        return NSBC_ERR;
    }

    const nsbc_module_t *d = (const nsbc_module_t *)dlsym(h, sym);
    if (!d) {
        llog(LOG_ERR, "mod", "dlopen %s: no symbol '%s' (%s)",
             so_path, sym, dlerror());
        dlclose(h);
        return NSBC_NOENT;
    }

    if (d->abi != NSBC_MOD_ABI) {
        llog(LOG_ERR, "mod", "dlopen %s: ABI mismatch (got %u, want %u)",
             so_path, d->abi, NSBC_MOD_ABI);
        dlclose(h);
        return NSBC_PROTO;
    }

    /* CALL_PATH 模块 configure 已保证进 STATIC 组——理论上不会走到这里。
     * 作为双保险还是拒绝 dlopen（呼叫面模块卸载不安全）。 */
    if (d->flags & MOD_F_CALL_PATH) {
        llog(LOG_ERR, "mod", "dlopen refuse CALL_PATH module: %s", d->name);
        dlclose(h);
        return NSBC_PERM_DENIED;
    }

    int rc = modmgr_register(m, d);
    if (rc != NSBC_OK) {
        llog(LOG_ERR, "mod", "dlopen %s: register rc=%d (dup name %s?)",
             so_path, rc, d->name);
        dlclose(h);
        return rc;
    }
    /* 注册成功：保存 dlopen 句柄（modmgr_destroy 时 dlclose） */
    m->v[m->n - 1].handle = h;
    llog(LOG_INFO, "mod", "dlopen %s → %s (level %d, %s)",
         so_path, d->name, d->level,
         (d->flags & MOD_F_REQUIRED) ? "REQUIRED" : "optional");
    return NSBC_OK;
}

/* 批量加载：读 configure 生成的 mod.load 文件（每行一个 .so 路径，# 开头注释跳过）。
 * 任何一个加载失败 → 返回该错误码，但已加载的 .so 不卸载（调用方决定是否 fail-fast）。 */
int modmgr_load_all(nsbc_modmgr_t *m, const char *list_path)
{
    if (!m || !list_path) return NSBC_INVAL;

    FILE *f = fopen(list_path, "r");
    if (!f) {
        llog(LOG_WARN, "mod", "mod.load not found: %s (0 dynamic modules)", list_path);
        return NSBC_NOENT; /* 不是错误——可以没有动态模块 */
    }

    char line[512];
    int last_err = NSBC_OK;
    int loaded = 0, failed = 0;

    /* P0-3: 从 list_path 里提取 base dir（dirname 语义），与每行拼绝对路径。
     * 这样 configure 写 mod.load 时只存文件名 mod_xxx.so，install 后
     * /etc/nsbc/mod/mod.load 每行是 mod_xxx.so → dlopen /etc/nsbc/mod/mod_xxx.so */
    const char *base = NULL;
    const char *slash = strrchr(list_path, '/');
    if (slash) {
        size_t base_len = (size_t)(slash - list_path);
        char *buf = alloca(base_len + 1);
        memcpy(buf, list_path, base_len); buf[base_len] = '\0';
        base = buf;
    }

    while (fgets(line, sizeof(line), f)) {
        /* trim 前后空白 */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        size_t n = strlen(p);
        while (n > 0 && (p[n-1] == '\n' || p[n-1] == '\r' || p[n-1] == ' ')) p[--n] = '\0';

        /* 拼完整 .so 路径：base/line */
        char full[1024];
        if (base && base[0])
            snprintf(full, sizeof(full), "%s/%s", base, p);
        else
            snprintf(full, sizeof(full), "%s", p);

        int rc = modmgr_load(m, full);
        if (rc != NSBC_OK) {
            ++failed;
            last_err = rc;
        } else {
            ++loaded;
        }
    }
    fclose(f);
    llog(LOG_INFO, "mod", "mod.load done: %d loaded, %d failed (rc=%d)",
         loaded, failed, last_err);
    return failed ? last_err : NSBC_OK;
}

/* DFS 拓扑排序；只初始化 level <= upto 且未初始化的模块 */
struct dfs_ctx {
    nsbc_modmgr_t *m;
    mod_level_t upto;
    struct nsbc_ctx *app;
    int fail;
    char why[64];
};

static int dfs_visit(struct dfs_ctx *x, int idx)
{
    nsbc_modmgr_t *m = x->m;
    if (m->color[idx] == 2)
        return NSBC_OK; /* 已处理（INITED / DISABLED 都算处理完） */
    if (m->color[idx] == 1) {
        snprintf(x->why, sizeof(x->why), "dep cycle at %s",
                 m->v[idx].d->name);
        return NSBC_PROTO;
    }
    const nsbc_module_t *d = m->v[idx].d;
    if (d->level > x->upto)
        return NSBC_OK; /* 更高层稍后初始化 */
    if (m->v[idx].st == MOD_ST_DISABLED) {
        m->color[idx] = 2;
        return NSBC_OK; /* 配置里禁用 → 不 init，依赖链视为"已处理" */
    }
    m->color[idx] = 1;
    for (unsigned i = 0; i < d->ndeps; ++i) {
        if (!d->deps[i]) continue; /* P0 允许空 dep 数组元素 */
        int dep = find_idx(m, d->deps[i]);
        if (dep < 0) {
            snprintf(x->why, sizeof(x->why), "%s needs missing dep %s",
                     d->name, d->deps[i]);
            return NSBC_NOENT;
        }
        if (m->v[dep].st == MOD_ST_DISABLED) {
            snprintf(x->why, sizeof(x->why), "%s needs disabled %s",
                     d->name, d->deps[i]);
            return NSBC_ERR; /* 依赖被 disable → 这个模块也 init 不了 */
        }
        int rc = dfs_visit(x, dep);
        if (rc != NSBC_OK)
            return rc;
    }
    m->color[idx] = 2;
    if (m->v[idx].st == MOD_ST_REG) {
        int rc = d->init ? d->init(x->app) : NSBC_OK;
        if (rc != NSBC_OK) {
            snprintf(x->why, sizeof(x->why), "%s init rc=%d", d->name, rc);
            if (d->flags & MOD_F_REQUIRED)
                return rc; /* fail-fast */
            llog(LOG_WARN, "mod", "optional module %s disabled: %d",
                 d->name, rc);
            m->v[idx].st = MOD_ST_DISABLED;
            return NSBC_OK;
        }
        m->v[idx].st = MOD_ST_INITED;
        llog(LOG_INFO, "mod", "inited %s (level %d)", d->name, d->level);
    }
    return NSBC_OK;
}

int modmgr_init_level(nsbc_modmgr_t *m, mod_level_t upto,
                      struct nsbc_ctx *app)
{
    memset(m->color, 0, m->n);
    struct dfs_ctx x = {m, upto, app, 0, {0}};
    for (unsigned i = 0; i < m->n; ++i) {
        if (m->v[i].d->level > upto)
            continue;
        int rc = dfs_visit(&x, (int)i);
        if (rc != NSBC_OK) {
            llog(LOG_ERR, "mod", "init level %d failed: %s", upto, x.why);
            return rc;
        }
    }
    return NSBC_OK;
}

int modmgr_start_all(nsbc_modmgr_t *m, struct nsbc_ctx *app)
{
    for (unsigned i = 0; i < m->n; ++i) {
        ment_t *e = &m->v[i];
        if (e->st != MOD_ST_INITED || !e->d->start)
            continue;
        int rc = e->d->start(app);
        if (rc != NSBC_OK) {
            llog(LOG_ERR, "mod", "start %s: %d", e->d->name, rc);
            if (e->d->flags & MOD_F_REQUIRED)
                return rc;
            e->st = MOD_ST_DISABLED;
        } else {
            e->st = MOD_ST_STARTED;
        }
    }
    return NSBC_OK;
}

void modmgr_stop_all(nsbc_modmgr_t *m, struct nsbc_ctx *app)
{
    /* 反序 stop：后启动的先停（先摘流量再拆线） */
    for (int i = (int)m->n - 1; i >= 0; --i) {
        ment_t *e = &m->v[i];
        if (e->st == MOD_ST_STARTED && e->d->stop) {
            fprintf(stderr, "[shutdown] stopping module[%d]=%s... ", i, e->d->name);
            fflush(stderr);
            e->d->stop(app);
            fprintf(stderr, "done\n");
            fflush(stderr);
        }
    }
    fprintf(stderr, "[shutdown] modmgr_stop_all complete\n");
    fflush(stderr);
}

void modmgr_fini_all(nsbc_modmgr_t *m, struct nsbc_ctx *app)
{
    for (int i = (int)m->n - 1; i >= 0; --i) {
        ment_t *e = &m->v[i];
        if (e->st == MOD_ST_DISABLED || e->st == MOD_ST_REG)
            continue;
        if (e->d->fini)
            e->d->fini(app);
        e->st = MOD_ST_REG;
    }
}

const nsbc_module_t *modmgr_find(nsbc_modmgr_t *m, const char *name)
{
    int i = find_idx(m, name);
    return i < 0 ? NULL : m->v[i].d;
}

unsigned modmgr_count(nsbc_modmgr_t *m)
{
    return m->n;
}

const nsbc_module_t *modmgr_get_nth(nsbc_modmgr_t *m, unsigned idx)
{
    if (!m || idx >= m->n) return NULL;
    return m->v[idx].d;
}

mod_state_t modmgr_get_state_at(nsbc_modmgr_t *m, unsigned idx)
{
    if (!m || idx >= m->n) return MOD_ST_REG;
    return m->v[idx].st;
}

/* Phase 1 MOD1: 动态模块安全卸载
 *
 * 流程（白皮书 §2.3 SAFE1 + §十四 10.4 Fail-Safe）：
 *   1. 如果模块已 STARTED → 先调 stop（停流量，让新请求不再进）
 *   2. 如果模块已 INITED → 调 fini（模块自己负责 unregister 所有 Registry ops，
 *      每个 unregister 内部会置 unloading=true + 自旋 wait refcnt→0）
 *   3. dlclose handle（动态模块）—— 此时所有 registry 条目已 unloading 且 refcnt==0，
 *      没有线程再调用模块代码 → dlclose 安全
 *   4. 标记 st=MOD_ST_REG（保留描述符指针，但 .so 已卸载；后续 modmgr_load 可以重新 load 同名）
 *
 * Fail-Safe 原则：宁可泄漏 handle、宁可让 ops 条目留 unloading=true，绝不 crash。
 * 如果 fini 返回非 OK，仍然继续 dlclose 和状态标记——fini 不该失败，
 * 但万一失败了我们也不能让 unload 卡死。 */
int modmgr_unload(nsbc_modmgr_t *m, const char *name, struct nsbc_ctx *app)
{
    if (!m || !name || !name[0]) return NSBC_INVAL;

    int idx = find_idx(m, name);
    if (idx < 0) {
        llog(LOG_WARN, "mod", "unload: '%s' not found", name);
        return NSBC_NOENT;
    }
    ment_t *e = &m->v[idx];
    const nsbc_module_t *d = e->d;

    llog(LOG_INFO, "mod", "unload '%s' handle=%p st=%d category=%s",
         name, e->handle, e->st, mod_category_name(d->category));

    /* CALL_PATH 模块禁止卸载 —— 呼叫面模块卸载不安全（可能在呼叫热路径）*/
    if (d->flags & MOD_F_CALL_PATH) {
        llog(LOG_ERR, "mod", "unload refuse CALL_PATH module: %s", name);
        return NSBC_PERM_DENIED;
    }

    /* REQUIRED 模块禁止卸载 —— 核心模块，卸载后进程可能功能不完整 */
    if (d->flags & MOD_F_REQUIRED) {
        llog(LOG_ERR, "mod", "unload refuse REQUIRED module: %s", name);
        return NSBC_PERM_DENIED;
    }

    /* 1. stop（如果已 STARTED）*/
    if (e->st == MOD_ST_STARTED && d->stop) {
        llog(LOG_INFO, "mod", "unload '%s' → stop", name);
        d->stop(app);
        e->st = MOD_ST_INITED;
    }

    /* 2. fini（如果已 INITED）—— 模块自己 unregister 所有 Registry ops */
    if (e->st == MOD_ST_INITED && d->fini) {
        llog(LOG_INFO, "mod", "unload '%s' → fini", name);
        d->fini(app);
        /* 注意：fini 应该返回 NSBC_OK。即使返回失败我们也继续 dlclose——
         * fini 失败通常意味着内部状态异常，继续阻塞更危险。 */
    }

    /* 3. dlclose（动态模块）*/
    if (e->handle) {
        /* fail-safe：dlclose 前先让模块的 ops 条目进入 unloading 状态（模块 fini
         * 里应该已经调了 unregister→unload_wait，但作为双保险这里不做额外动作——
         * dlclose 的 UB 风险远小于 refcnt>0 时 dlclose）。*/
        llog(LOG_INFO, "mod", "unload '%s' → dlclose(%p)", name, e->handle);
        dlclose(e->handle);
        e->handle = NULL;
    }

    /* 4. 标记状态（保留 d 指针，因为 modmgr_find 等可能还在读它）*/
    e->st = MOD_ST_REG;

    llog(LOG_INFO, "mod", "unload '%s' done (safe)", name);
    return NSBC_OK;
}
