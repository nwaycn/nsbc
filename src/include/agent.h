#ifndef NSBC_AGENT_H
#define NSBC_AGENT_H

/* 座席（REQ-PBX-CC-001 座席模型，P2）
 *
 * 状态机：OFFLINE→IDLE（签入）→RESERVED（被选中振铃）→ONCALL→
 * WRAP（话后整理）→IDLE；任意非通话态可 PAUSED（小休/培训）。
 * 话后整理时长由队列 wrap_time_s 驱动（ringgroup 同口径）。
 * 技能：标签字符串集合（粗骨架定槽精确匹配，路由匹配在 queue）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_EXT_MAX    32
#define AGENT_TENANT_MAX 32
#define AGENT_SKILL_MAX  8
#define AGENT_SKILL_LEN  16
#define AGENT_CAP        20000

typedef enum {
    AGENT_OFFLINE = 0,
    AGENT_IDLE,
    AGENT_RESERVED,
    AGENT_ONCALL,
    AGENT_WRAP,
    AGENT_PAUSED
} agent_state_t;

typedef enum {
    AGENT_PAUSE_NONE = 0,
    AGENT_PAUSE_BREAK,
    AGENT_PAUSE_TRAINING,
    AGENT_PAUSE_OTHER
} agent_pause_reason_t;

typedef struct {
    char tenant[AGENT_TENANT_MAX];
    char ext[AGENT_EXT_MAX];
    char name[48];
    char skills[AGENT_SKILL_MAX][AGENT_SKILL_LEN];
    int  n_skills;
    int  priority;              /* 高优先座席先选（-100..100） */
    agent_state_t state;
    agent_pause_reason_t pause_reason;
    uint64_t state_since_ms;
    uint64_t last_call_end_ms;  /* LEAST-RECENT 选路 */
    uint32_t answered_total;
} agent_t;

typedef struct agent_pool agent_pool_t;

int  agent_pool_create(agent_pool_t **out);
void agent_pool_destroy(agent_pool_t *p);

int  agent_upsert(agent_pool_t *p, const agent_t *a);
int  agent_lookup(agent_pool_t *p, const char *tenant, const char *ext,
                  agent_t *out);

/* 状态迁移（非法迁移返回 INVAL；挂机 ONCALL→WRAP，整理结束→IDLE） */
int  agent_login(agent_pool_t *p, const char *tenant, const char *ext,
                 uint64_t now_ms);
int  agent_logout(agent_pool_t *p, const char *tenant, const char *ext);
int  agent_pause(agent_pool_t *p, const char *tenant, const char *ext,
                 agent_pause_reason_t reason);
int  agent_resume(agent_pool_t *p, const char *tenant, const char *ext,
                  uint64_t now_ms);
int  agent_reserve(agent_pool_t *p, const char *tenant, const char *ext,
                   uint64_t now_ms);
int  agent_answer(agent_pool_t *p, const char *tenant, const char *ext,
                  uint64_t now_ms);
int  agent_release(agent_pool_t *p, const char *tenant, const char *ext,
                   uint64_t now_ms, int wrap_time_s);

int  agent_has_skill(const agent_t *a, const char *skill);
const char *agent_state_name(agent_state_t st);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_AGENT_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
