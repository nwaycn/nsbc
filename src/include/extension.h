#ifndef NSBC_EXTENSION_H
#define NSBC_EXTENSION_H

/* 分机对象模型（REQ-PBX-EXT-001 主 / 003 多租户 / 004 分机策略）
 *
 * 现状：mod/endpoints 仅有 user→HA1 凭证表（注册 Digest 用），
 * 无业务属性。本头定义完整分机画像（租户隔离、业务权限等级、
 * 呼叫策略位、显示名/部门），HA1 仍以 endpoints.c 的 ep_store 为
 * Digest 唯一数据源；本模块为业务侧只读画像，注册路径不依赖本头
 * （分层同属 mod，接线时 ep_init 装配两侧）。
 *
 * 多租户铁律（项目风险清单：串号/串话单）：所有查找必须带
 * tenant 键，禁止全租户扫描型 API 暴露给呼叫快路径。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EXT_TENANT_MAX  32
#define EXT_NUMBER_MAX  32
#define EXT_NAME_MAX    64
#define EXT_DEPT_MAX    64
#define EXT_MAX_PROFILES (500*10000/10) /* 容量按租户分机规划，粗上限 */

/* 业务权限等级（dialplan rule.perm 的被叫侧门）：
 * 位掩码，分机 cos 与规则 perm 按包含判定 */
typedef enum {
    EXT_PERM_INTERNAL = 1u << 0, /* 内呼 */
    EXT_PERM_LOCAL    = 1u << 1, /* 本地市话 */
    EXT_PERM_DOMESTIC = 1u << 2, /* 国内长途 */
    EXT_PERM_MOBILE   = 1u << 3, /* 移动网 */
    EXT_PERM_INTL     = 1u << 4, /* 国际长途 */
    EXT_PERM_PREMIUM  = 1u << 5, /* 高费率/声讯 */
    EXT_PERM_EMERG    = 1u << 6  /* 紧急呼叫（默认全发，见 emergency） */
} ext_perm_t;

typedef enum {
    EXT_ST_DISABLED = 0,
    EXT_ST_ENABLED,
    EXT_ST_HOTDESK  /* 登录占用态（EXT-004 移动坐席，P1） */
} ext_status_t;

typedef struct {
    char     tenant[EXT_TENANT_MAX]; /* 租户 id（空=默认租户） */
    char     number[EXT_NUMBER_MAX]; /* 分机号（租户内唯一） */
    char     display[EXT_NAME_MAX];
    char     dept[EXT_DEPT_MAX];
    uint32_t cos;                    /* ext_perm_t 位掩码 */
    uint32_t flags;                  /* ext_flag_t 位 */
    ext_status_t status;
    int      max_concurrent;         /* 同时注册 Contact 上限（0=默认） */
} ext_profile_t;

enum {
    EXT_F_RECORD_ALL  = 1u << 0,
    EXT_F_VOICEMAIL   = 1u << 1,
    EXT_F_DND         = 1u << 2,     /* 免打扰 */
    EXT_F_NO_FWD      = 1u << 3,     /* 禁止前转（高权分机） */
    EXT_F_FORCE_PIN   = 1u << 4
};

typedef struct ext_mgr ext_mgr_t;

int  ext_mgr_create(int shard_bits, ext_mgr_t **out);
void ext_mgr_destroy(ext_mgr_t *m);

/* 管理面：增改（同键覆盖）/删/查；全部带 tenant 隔离键 */
int  ext_upsert(ext_mgr_t *m, const ext_profile_t *p);
int  ext_remove(ext_mgr_t *m, const char *tenant, const char *number);
const ext_profile_t *ext_lookup(ext_mgr_t *m, const char *tenant,
                                const char *number);

/* 呼叫快路径权限判定：cos 包含 need 全位即放行 */
int  ext_has_perm(const ext_profile_t *p, uint32_t need);
/* 跨租户调用合法性（默认拒绝；中继落地策略显式放行由调用方判） */
int  ext_same_tenant(const ext_profile_t *a, const ext_profile_t *b);

uint64_t ext_count(const ext_mgr_t *m, const char *tenant);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_EXTENSION_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
