#ifndef NSBC_RINGGROUP_H
#define NSBC_RINGGROUP_H

/* 振铃组（REQ-PBX-RG-001，P0）
 *
 * 策略：simultaneous 同振、sequence 顺序、random 随机、
 * memory（最近最少接听优先，P1）；整组超时后 fallback（语音信箱/
 * 下一组/IVR）；成员 DND/未注册跳过；成员接听后组内其他腿 CANCEL；
 * wrap time（话后整理）内不响该成员。
 * 分机清单来自 extension 画像，实时在线状态接 presence（B4）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RG_NAME_MAX      32
#define RG_MEMBER_MAX    64
#define RG_FALLBACK_MAX  48

typedef enum {
    RG_STRATEGY_SIMUL = 0,
    RG_STRATEGY_SEQUENCE,
    RG_STRATEGY_RANDOM,
    RG_STRATEGY_MEMORY       /* P1 */
} rg_strategy_t;

typedef struct {
    char     name[RG_NAME_MAX];
    char     tenant[32];
    char     members[RG_MEMBER_MAX][32]; /* 分机号 */
    int      nmembers;
    rg_strategy_t strategy;
    int      ring_timeout_s;     /* 单人/整组策略各自解释（见注释） */
    int      wrap_time_s;        /* 话后整理 */
    int      skip_busy;          /* 忙/DND 跳过 */
    char     fallback[RG_FALLBACK_MAX]; /* 超时目的地（IVR/VM/另一组） */
    int      enabled;
} ringgroup_t;

typedef struct rg_mgr rg_mgr_t;

int  rg_create(rg_mgr_t **out);
void rg_destroy(rg_mgr_t *r);

int  rg_add(rg_mgr_t *r, const ringgroup_t *g);
int  rg_remove(rg_mgr_t *r, const char *tenant, const char *name);
const ringgroup_t *rg_get(rg_mgr_t *r, const char *tenant,
                          const char *name);

/* 计算本轮振铃目标序列（SIMUL 全量；SEQ 按上次接通位轮转起点；
 * RANDOM 洗牌）。在线/忙判定由调用方（presence）做，本函数只出名单。
 * 返回写入 out 的成员数（out_cap 为槽数）。 */
int  rg_ring_targets(rg_mgr_t *r, const char *tenant, const char *name,
                     char out[][32], int out_cap, uint32_t rnd);

/* 话后整理：成员接听后调用，ts=单调时钟；wrap 期内 rg_available=0 */
void rg_note_answered(rg_mgr_t *r, const char *tenant, const char *name,
                      const char *member, uint64_t ts);
int  rg_in_wrap(rg_mgr_t *r, const char *tenant, const char *name,
                const char *member, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_RINGGROUP_H */
