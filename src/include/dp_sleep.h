/* dp_sleep.h — Dialplan 非阻塞 Sleep（§7 2026-10-15）
 *
 * 核心设计：
 *   - 严禁系统 sleep。用 ctx->tw（timer_wheel）异步定时。
 *   - sleep app 调用 dp_sleep_add → tw_add → 返回 APP_RC_PENDING。
 *   - 定时器到期 → dp_sleep 回调 → 重入 dp_execute_app_chain 从 resume_idx 继续。
 *   - 可打断：uuid_break / hangup 时调 dp_sleep_cancel(call_key) → tw_del。
 *
 * 线程安全：timer_wheel 回调在 reactor 线程里触发；所有操作在同一 reactor 线程，
 * 不需要额外锁。
 */

#ifndef NSBC_DP_SLEEP_H
#define NSBC_DP_SLEEP_H

#include "dialplan.h"
#include "timer_wheel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 挂起：call_key 关联的 Dialplan 在 resume_idx 处挂起 ms 毫秒。
 * rule 必须拷贝（DP reload 会换表），内部自己复制 app_chain。
 * tw: ctx->tw（timer wheel）。 */
int dp_sleep_add(tw_t *tw, void *call_key, const dp_rule_t *rule,
                 size_t resume_idx, int ms);

/* 取消：call_key 关联的 sleep（如 uuid_break/hangup）。
 * 返回 1=已取消，0=不存在。 */
int dp_sleep_cancel(void *call_key);

/* 查询：call_key 是否在 sleep 中 */
int dp_sleep_active(void *call_key);

/* 模块 init：全局初始化（dp_module.c start 时调） */
void dp_sleep_init(void);
void dp_sleep_fini(void);

/* 获取全局 app_reg（dp_sleep_init 里创建，供 dialplan 执行 app_chain 时传入） */
struct app_reg;
struct app_reg *dp_sleep_app_reg(void);

#ifdef __cplusplus
}
#endif

#endif /* NSBC_DP_SLEEP_H */
