#ifndef NSBC_CONFERENCE_H
#define NSBC_CONFERENCE_H

/* 会议室（REQ-MEET-001~004，P1）
 *
 * 能力：房间/PIN/成员上限/主持人；进出提示音与成员计数播报；静音/
 * 解除静音、主持人 DTMF 控制（踢人/锁定/开始录音）；混音经
 * media/mixer（mixer_* 原语），录音经 media/recorder（siprec 可选）。
 * 分层约束：call_key 不透明，媒体动作走 conf_ops_t 由组合根装配。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONF_NAME_MAX    32
#define CONF_PIN_MAX     16
#define CONF_MEMBER_MAX  128
#define CONF_ROOM_CAP    256
#define CONF_EXT_MAX     32

typedef enum {
    CONF_ROLE_LISTENER = 0,
    CONF_ROLE_GUEST,
    CONF_ROLE_MODERATOR
} conf_role_t;

typedef struct {
    char        name[CONF_NAME_MAX];
    char        tenant[32];
    char        pin[CONF_PIN_MAX];       /* 入会 PIN（空=公开） */
    char        mod_pin[CONF_PIN_MAX];   /* 主持人 PIN（空=首位为主持人） */
    int         max_members;
    int         announce_join_leave;     /* 进出播报 */
    int         record_enabled;          /* 允许录音（主持人发起） */
    int         locked;                  /* 锁定后新成员拒绝 */
    int         wait_for_moderator;      /* 无主持人时听等待音乐 */
} conf_room_t;

typedef struct {
    char  ext[CONF_EXT_MAX];
    void *call_key;
    conf_role_t role;
    int   muted;
    int   talking;       /* 能量/VAD 指示，由媒体面刷新 */
} conf_member_t;

typedef struct {
    /* 媒体面：把成员腿接入命名混音桥（mixer），离开摘除 */
    int  (*mix_attach)(void *ud, const char *room, void *call_key);
    int  (*mix_detach)(void *ud, const char *room, void *call_key);
    int  (*play_room)(void *ud, const char *room, const char *clip);
    int  (*record_start)(void *ud, const char *room, const char *path);
    int  (*record_stop)(void *ud, const char *room);
    void *ud;
} conf_ops_t;

typedef struct conf_mgr conf_mgr_t;

int  conf_create(const conf_ops_t *ops, conf_mgr_t **out);
void conf_destroy(conf_mgr_t *m);

int  conf_room_add(conf_mgr_t *m, const conf_room_t *r);
int  conf_room_remove(conf_mgr_t *m, const char *tenant, const char *name);
const conf_room_t *conf_room_get(conf_mgr_t *m, const char *tenant,
                                 const char *name);

/* 入会（PIN 校验：mod_pin 命中→主持人，pin 命中→成员；公开房首位
 * 自动主持人）。返回时成员已 attach 混音。 */
int  conf_join(conf_mgr_t *m, const char *tenant, const char *name,
               const char *ext, const char *pin, void *call_key);
int  conf_leave(conf_mgr_t *m, const char *tenant, const char *name,
                void *call_key);

/* DTMF 控制（*1 静音切换 / *2 举手/发言指示 / *3 主持人锁房 /
 * *4 录音切换；粗骨架仅实现状态位，提示音由组合根补） */
int  conf_dtmf_cmd(conf_mgr_t *m, const char *tenant, const char *name,
                   const void *call_key, const char *cmd);

int  conf_member_count(conf_mgr_t *m, const char *tenant, const char *name,
                       int *total, int *moderators);
int  conf_set_talking(conf_mgr_t *m, const void *call_key, int talking);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_CONFERENCE_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
