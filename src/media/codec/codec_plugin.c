/* codec_plugin.c — 动态 codec 插件装载实现
 *
 * 只负责 dlopen + ABI 校验 + codec_register 登记；卸载不支持（见头文件说明）。
 * 加载是低频控制面操作，允许持锁；注册表本身用 g_reg_lock + 原子发布，热路径无锁。
 */

#include "codec_plugin.h"

#include "nsbc_common.h"
#include "log.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CODEC_PLUGIN_MAX 8
#define CODEC_PLUGIN_PATH_MAX 240

typedef struct {
    void                       *handle;
    const nsbc_codec_plugin_t  *plugin;
    char                        path[CODEC_PLUGIN_PATH_MAX];
} codec_plugin_slot_t;

static codec_plugin_slot_t g_plugins[CODEC_PLUGIN_MAX];
static int                 g_plugin_count;
static pthread_mutex_t     g_plugin_lock = PTHREAD_MUTEX_INITIALIZER;

/* 路径白名单式校验：绝对路径、.so 结尾、无路径穿越、长度受限。 */
static int plugin_path_ok(const char *path)
{
    size_t n;
    if (path == NULL || path[0] != '/')
        return 0;
    n = strlen(path);
    if (n < 4 || n >= CODEC_PLUGIN_PATH_MAX)
        return 0;
    if (strcmp(path + n - 3, ".so") != 0)
        return 0;
    if (strstr(path, "..") != NULL)
        return 0;
    return 1;
}

int codec_plugin_load(const char *path, char *name_out, size_t cap)
{
    void *h;
    const nsbc_codec_plugin_t *p;
    codec_id_t id;
    int i;
    int rc = NSBC_ERR;

    if (name_out != NULL && cap > 0)
        name_out[0] = '\0';
    if (!plugin_path_ok(path))
        return NSBC_INVAL;

    pthread_mutex_lock(&g_plugin_lock);

    for (i = 0; i < g_plugin_count; ++i) {
        if (strcmp(g_plugins[i].path, path) == 0) {
            rc = NSBC_EXISTS;
            goto out;
        }
    }
    if (g_plugin_count >= CODEC_PLUGIN_MAX) {
        llog(LOG_ERR, "codec", "plugin slots full (%d)", CODEC_PLUGIN_MAX);
        rc = NSBC_NOMEM;
        goto out;
    }

    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        llog(LOG_ERR, "codec", "dlopen(%s) failed: %s", path, dlerror());
        rc = NSBC_ERR;
        goto out;
    }

    p = (const nsbc_codec_plugin_t *)dlsym(h, NSBC_CODEC_PLUGIN_SYM);
    if (p == NULL || p->abi_version != NSBC_CODEC_PLUGIN_ABI ||
        p->name == NULL || p->name[0] == '\0' ||
        p->ops == NULL || p->ops->name[0] == '\0') {
        llog(LOG_ERR, "codec",
             "plugin %s rejected: missing/invalid '%s' (abi=%u)",
             path, NSBC_CODEC_PLUGIN_SYM,
             p != NULL ? (unsigned)p->abi_version : 0u);
        dlclose(h);
        rc = NSBC_INVAL;
        goto out;
    }

    for (i = 0; i < g_plugin_count; ++i) {
        if (strcmp(g_plugins[i].plugin->name, p->name) == 0) {
            llog(LOG_WARN, "codec", "plugin '%s' already loaded", p->name);
            dlclose(h);
            rc = NSBC_EXISTS;
            goto out;
        }
    }

    /* ops->id=0 → 注册表分配动态 ID。若 codec 名与已注册项同名，
     * 注册表不会覆盖：名称查找仍命中先注册者，这里显式告警避免静默遮蔽。 */
    {
        const codec_ops_t *dup =
            codec_registry_find_name(p->ops->name, strlen(p->ops->name));
        if (dup != NULL && dup != p->ops)
            llog(LOG_WARN, "codec",
                 "plugin '%s' codec name '%s' shadows existing id=%u",
                 p->name, p->ops->name, (unsigned)dup->id);
    }
    id = codec_register(p->ops);
    if (id == CODEC_NONE) {
        llog(LOG_ERR, "codec", "plugin '%s' codec registration failed", p->name);
        dlclose(h);
        rc = NSBC_ERR;
        goto out;
    }

    g_plugins[g_plugin_count].handle = h;
    g_plugins[g_plugin_count].plugin = p;
    snprintf(g_plugins[g_plugin_count].path,
             sizeof(g_plugins[g_plugin_count].path), "%s", path);
    ++g_plugin_count;

    if (name_out != NULL && cap > 0)
        snprintf(name_out, cap, "%s", p->name);

    llog(LOG_INFO, "codec",
         "plugin '%s' v%s loaded: codec=%s id=%u path=%s",
         p->name, p->version != NULL ? p->version : "-",
         p->ops->name, (unsigned)id, path);
    rc = NSBC_OK;

out:
    pthread_mutex_unlock(&g_plugin_lock);
    return rc;
}

int codec_plugin_count(void)
{
    int n;
    pthread_mutex_lock(&g_plugin_lock);
    n = g_plugin_count;
    pthread_mutex_unlock(&g_plugin_lock);
    return n;
}

int codec_plugin_foreach(int (*cb)(const nsbc_codec_plugin_t *p,
                                   const char *path, void *ud), void *ud)
{
    int i;
    int rc = 0;

    if (cb == NULL)
        return NSBC_INVAL;

    pthread_mutex_lock(&g_plugin_lock);
    for (i = 0; i < g_plugin_count; ++i) {
        rc = cb(g_plugins[i].plugin, g_plugins[i].path, ud);
        if (rc != 0)
            break;
    }
    pthread_mutex_unlock(&g_plugin_lock);
    return rc;
}
