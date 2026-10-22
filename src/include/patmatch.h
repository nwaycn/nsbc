#ifndef NSBC_PATMATCH_H
#define NSBC_PATMATCH_H

#include "nsbc_common.h"

/* PCRE2 要求在 include pcre2.h 之前定义 CODE_UNIT_WIDTH
 * 拨号规则只处理 ASCII 号码 → 用 8-bit 版本 (pcre2_8 / -lpcre2-8) */
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#include <pcre2.h>

/* 对应 REQ-PBX-DP-001 的匹配类型
 * PM_REGEX 使用 PCRE2（兼容 Perl 风格 \d \w \b 等，FreeSWITCH 迁移友好） */
typedef enum {
    PM_EXACT = 0, /* 精确: "1001" */
    PM_PREFIX,    /* 前缀: "010" */
    PM_RANGE,     /* 长度区间: pattern 任意/数字, 命中长度 [min_len,max_len] */
    PM_DIGIT,     /* 数字通配: X=0-9 N=2-9 Z=1-9, 其余字符按字面 */
    PM_REGEX      /* PCRE2 正则（支持 Perl \d \w \s \b \A \Z 等） */
} pm_type_t;

typedef struct {
    pm_type_t type;
    char pattern[256];  /* 从 96 扩到 256：regex 可能长 */
    uint8_t min_len; /* PM_RANGE 使用 */
    uint8_t max_len;
    pcre2_code *re;   /* PM_REGEX 使用（PCRE2 compiled code） */
} pm_pattern_t;

/* pattern 文本约定:
 *   "=1001"  精确
 *   "_010"   前缀
 *   "[3:11]" 长度区间 3~11 位（pattern 字段存任意附加数字前缀，可空）
 *   "_13XXXXXXXXX" 数字通配（以 _ 开头且含 X/N/Z 时按 PM_DIGIT；调用方显式指定）
 *   "%^9[0-9]+$" 正则
 *   裸 "^1\d{2}$" 也自动识别为 REGEX（stub_mods.c dp_parse_match 负责）
 * 返回 0 成功，失败为负错误码 */
int pm_init(pm_pattern_t *p, pm_type_t type, const char *pattern,
            uint8_t min_len, uint8_t max_len);

/* 命中返回 1，不命中 0，参数错误 -1 */
int pm_match(const pm_pattern_t *p, const char *digits);

/* Advisor 2026-10-12n: 带捕获组输出的匹配 API
 * 命中时 out_groups[0] = 完整匹配串 ($0), out_groups[1..] = 正则捕获组 ($1, $2...)
 * 非 PM_REGEX 或无捕获组时仅 out_groups[0] 有效（= digits 原文）。
 * 调用方负责 out_groups 的初始化（建议全部 memset(0)）。 */
#define PM_MAX_GROUPS 8
#define PM_GROUP_SZ   128
int pm_match_groups(const pm_pattern_t *p, const char *digits,
                    char out_groups[PM_MAX_GROUPS][PM_GROUP_SZ]);

void pm_fini(pm_pattern_t *p);

#endif /* NSBC_PATMATCH_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
