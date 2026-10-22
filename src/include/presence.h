#ifndef NSBC_PRESENCE_H
#define NSBC_PRESENCE_H

/* 状态呈现（REQ-PBX-PRES-001~004，P1）
 *
 *  - 001 分机状态聚合（注册态 + 对话态）：idle/ringing/busy/dnd/offline；
 *  - 002 BLF：watchers 订阅资源（SUBSCRIBE dialog；RFC6665 框架在
 *    sip_event），状态变更发 NOTIFY，body 为 application/pidf+xml
 *    （RFC3863 basic status + RPID note，粗渲染）；
 *  - 003 资源列表订阅：RLMI + multipart（多 BLF 键一个订阅，P2 渲染）；
 *  - 004 MWI 复用 message-summary 事件包（发件侧由 voicemail 装配，
 *    本模块只提供订阅登记，状态来源走 VM 计数回调）。
 * 事件源：注册上下线来自 registrar；振铃/通话来自 core 话务事件回调
 * （A4 槽）；本模块纯聚合+订阅扇出，不碰 SIP 事务。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PRES_TENANT_MAX 32
#define PRES_ENTITY_MAX 48
#define PRES_AOR_MAX    96
#define PRES_CAP        200000
#define PRES_WATCHER_MAX 24

typedef enum {
    PRES_BASIC_CLOSED = 0,   /* offline/未注册 */
    PRES_BASIC_OPEN = 1
} pres_basic_t;

typedef enum {
    PRES_NOTE_UNKNOWN = 0,
    PRES_NOTE_IDLE,
    PRES_NOTE_RINGING,
    PRES_NOTE_ON_THE_PHONE,
    PRES_NOTE_DND,
    PRES_NOTE_AWAY
} pres_note_t;

typedef struct {
    pres_basic_t basic;
    pres_note_t  note;
} pres_state_t;

typedef struct {
    char aor[PRES_AOR_MAX]; /* watcher（话机）的联系 AOR */
    void *dlg;              /* 不透明 SIP 订阅对话（NOTIFY 发送句柄） */
    int  expires_s;
} pres_watcher_t;

/* NOTIFY 发送回调：core 装配到 sip_event 的 notify 原语；
 * body 由本模块渲染（PIDF 或 message-summary，由 event_package 区分）。
 * dlg 为订阅登记时保存的不透明句柄。 */
typedef struct {
    int  (*notify)(void *ud, void *dlg, const char *event_package,
                   const char *content_type, const char *body);
    void *ud;
} pres_ops_t;

typedef struct pres_store pres_store_t;

int  pres_create(const pres_ops_t *ops, pres_store_t **out);
void pres_destroy(pres_store_t *s);

/* 状态写入（注册/话务事件）；状态实际变化才返回 1 并触发扇出 */
int  pres_set(pres_store_t *s, const char *tenant, const char *entity,
              pres_state_t st);
int  pres_get(pres_store_t *s, const char *tenant, const char *entity,
              pres_state_t *out);

/* watcher 生命周期（SUBSCRIBE 经 core 转发）；订阅不存在资源返回 NOENT，
 * 是否允许取空订阅（RFC3856 可选）由调用方决定后登记占位实体。 */
int  pres_watch(pres_store_t *s, const char *tenant, const char *entity,
                const char *aor, void *dlg, int expires_s);
int  pres_unwatch(pres_store_t *s, void *dlg);
int  pres_watcher_count(pres_store_t *s, const char *tenant,
                        const char *entity);

/* PIDF body 粗渲染（固定单 tuple；多设备聚合 P2）。返回 body 长度或负码 */
int  pres_render_pidf(const char *tenant, const char *entity,
                      pres_state_t st, char *out, size_t cap);
/* MWI body：message-summary 纯文本（Messages-Waiting/Voice-Message） */
int  pres_render_mwi(int n_new, int n_total, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_PRESENCE_H */
