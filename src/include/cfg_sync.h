/* cfg_sync — C core ↔ Rust web 配置全量拉取 API
 *
 * 两种场景：
 *   cfg_sync_after_reload() — plain reload 后同步（无 retry，web 肯定已启动）
 *   cfg_sync_on_startup()   — 启动时拉取（有 retry 等 data.sock）
 */
#ifndef NSBC_CFG_SYNC_H
#define NSBC_CFG_SYNC_H

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

int cfg_sync_after_reload(void);
int cfg_sync_on_startup(void);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_CFG_SYNC_H */
