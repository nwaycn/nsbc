/* codec_plugin.h — 动态 codec 插件装载（方案文档 §4 P2「codec 动态装载」）
 *
 * 插件是一个共享库（.so），导出唯一符号 `nsbc_codec_plugin`：
 *
 *   static codec_ops_t my_ops = { .name = "AMR-WB", ... .id = 0 };
 *   __attribute__((visibility("default")))
 *   const nsbc_codec_plugin_t nsbc_codec_plugin = {
 *       .abi_version = NSBC_CODEC_PLUGIN_ABI,
 *       .name        = "amr-wb",
 *       .version     = "1.0.0",
 *       .ops         = &my_ops,          // id 由加载器回写
 *   };
 *
 * 约束：
 *   - 仅支持加载，不支持运行期卸载（卸载会让正在使用该 codec 的会话 UAF；
 *     替换插件请重启进程）；
 *   - 插件句柄常驻（RTLD_LOCAL，不 dlclose）；
 *   - 路径必须是绝对路径、以 .so 结尾、不含 ".."；
 *   - ops->api_version 必须是 0 或 NSBC_CODEC_API_VERSION（由 codec_register 校验）。
 */
#ifndef NSBC_CODEC_PLUGIN_H
#define NSBC_CODEC_PLUGIN_H

#include <stddef.h>
#include "media_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NSBC_CODEC_PLUGIN_ABI 1
#define NSBC_CODEC_PLUGIN_SYM "nsbc_codec_plugin"

typedef struct nsbc_codec_plugin {
    uint32_t     abi_version;   /* 必须等于 NSBC_CODEC_PLUGIN_ABI */
    const char  *name;          /* 插件名（唯一，用于运维展示） */
    const char  *version;       /* 插件版本（可空） */
    codec_ops_t *ops;           /* 插件内的静态 ops；ops->id 由加载器分配 */
} nsbc_codec_plugin_t;

/* 加载并注册插件。成功返回 NSBC_OK；name_out 输出插件名（可为 NULL）。 */
int codec_plugin_load(const char *path, char *name_out, size_t cap);

/* 已加载插件数。 */
int codec_plugin_count(void);

/* 枚举已加载插件（OAM `show codec_plugins` 用）。cb 返回非 0 时提前结束并返回该值。 */
int codec_plugin_foreach(int (*cb)(const nsbc_codec_plugin_t *p,
                                   const char *path, void *ud), void *ud);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_CODEC_PLUGIN_H */
