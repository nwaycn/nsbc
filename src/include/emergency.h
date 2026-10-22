#ifndef NSBC_EMERGENCY_H
#define NSBC_EMERGENCY_H

/* 紧急呼叫（REQ-PBX-DP-006）
 *
 * P0（本批）：
 *  - 紧急号码表（110/119/120/122/911 等按部署域配置）精确/前缀匹配；
 *  - 豁免：免 PIN/免余额/免 Follow-me/免前转，强插优先；
 *  - 静态 ELIN：按站点/zone 配定的回呼号码，P-Asserted/Contact 带
 *    ELIN 送 PSAP（静态映射表，P0 唯一来源）。
 * P1：
 *  - ELIN 多来源（LLDP/CDP 话机位置、子网/接入交换机端口、DHCP），
 *    多 ELIN 池轮用与回呼增强（临时绑定窗口）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMG_PATTERN_MAX  16
#define EMG_ELIN_MAX     32
#define EMG_ZONE_MAX     48
#define EMG_TRUNK_MAX    32
#define EMG_TABLE_MAX    256

typedef struct {
    char pattern[EMG_PATTERN_MAX]; /* 紧急号码（全号精确；末位 * 前缀） */
    char zone[EMG_ZONE_MAX];       /* 适用站点/zone（空=全局兜底） */
    char elin[EMG_ELIN_MAX];       /* P0 静态 ELIN */
    char trunk[EMG_TRUNK_MAX];     /* 强制出局中继组（本地 PSAP 落地） */
    int  prefix_len;               /* 前缀匹配长度（全号=总长） */
    int  enabled;
} emg_route_t;

typedef struct emg_table emg_table_t;

int  emg_create(emg_table_t **out);
void emg_destroy(emg_table_t *t);
int  emg_add(emg_table_t *t, const emg_route_t *r);

/* 判定 called 是否紧急号（任何 zone 匹配即 1；快路径最先判定，
 * 位于 PIN/余额/前转策略之前）。 */
int  emg_is_emergency(emg_table_t *t, const char *called);

/* 完整解析：按 zone 精确→全局兜底顺序选路；out 返回 ELIN 与中继组；
 * 无表/无匹配→NOENT（按普通呼叫走，避免误吞号）。 */
int  emg_resolve(emg_table_t *t, const char *called, const char *zone,
                 emg_route_t *out);

/* 豁免位（供呼叫策略直接查）：紧急呼叫不做余额扣减/PIN/前转/黑名单 */
int  emg_exempt_balance(const emg_route_t *r);
int  emg_exempt_pin(const emg_route_t *r);

/* P1：ELIN 多来源注册（LLDP/CDP/子网），P0 直接返回静态 ELIN */
int  emg_elin_lookup(emg_table_t *t, const char *called, const char *zone,
                     char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_EMERGENCY_H */
