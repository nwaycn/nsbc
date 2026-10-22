#ifndef NSBC_SIP_DIALOG_H
#define NSBC_SIP_DIALOG_H

/* RFC3261 §12 对话层（P0）
 *
 * 稳定键：Call-ID + local-tag + remote-tag（REQ-ARCH 稳定键仅 Call-ID
 * 指呼叫级；对话级三元组保证 B2BUA 两侧 fork/重试不串话）。
 * 分片哈希（2 的幂）；P0 分片锁，P1 随查询缓存换 RCU 无锁读。
 * 路由集：Record-Route 逆序拷贝（UAC）/顺序（UAS），目标从 Contact 更新。
 */
#include "mem.h"
#include "sip_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DLG_CALLID_MAX 128
#define DLG_TAG_MAX 64
#define DLG_URI_MAX 192
#define DLG_ROUTE_MAX 4

typedef enum {
    DLG_EARLY = 0,
    DLG_CONFIRMED,
    DLG_CLOSED
} dlg_state_t;

typedef enum { DLG_UAS = 0, DLG_UAC } dlg_dir_t;

typedef struct sip_dialog {
    char callid[DLG_CALLID_MAX];
    char local_tag[DLG_TAG_MAX];
    char remote_tag[DLG_TAG_MAX]; /* EARLY UAC 时可能为空 */
    char target[DLG_URI_MAX];    /* 远端 Contact（请求目标） */
    char routes[DLG_ROUTE_MAX][DLG_URI_MAX];
    int nroutes;
    dlg_state_t state;
    dlg_dir_t dir;
    long local_cseq;
    long remote_cseq;
    sip_method_t init_method;
    void *ud; /* B2BUA leg 关联 */
} sip_dialog_t;

typedef struct dlg_mgr dlg_mgr_t;

typedef struct {
    mem_arena_t *arena;
    mem_slab_t *slab; /* 可空自建 */
    int capacity;
} dlg_mgr_cfg_t;

int dlg_mgr_create(const dlg_mgr_cfg_t *cfg, dlg_mgr_t **out);
void dlg_mgr_destroy(dlg_mgr_t *m);

/* 由入向请求建 UAS 对话骨架（local_tag 由本层生成并回填，调用方应答用） */
sip_dialog_t *dlg_uas_new(dlg_mgr_t *m, const sip_msg_t *req);

/* UAC：发 INVITE/SUBSCRIBE 前；local_tag 已在 From，remote_tag 空 */
sip_dialog_t *dlg_uac_new(dlg_mgr_t *m, const char *callid, size_t clen,
                          const char *local_tag, size_t ltlen,
                          const char *contact_target, long cseq,
                          sip_method_t init_method);

/* 响应到达更新 UAC 对话（remote-tag/Contact/Record-Route 逆序；2xx 确认）。
 * 返回 NSBC_OK / NSBC_NOENT（无此对话）。 */
int dlg_uac_on_response(dlg_mgr_t *m, const sip_msg_t *resp);

/* 对话内请求更新 UAS：remote CSeq 单调检查（491/500 由调用方决策） */
int dlg_uas_on_request(sip_dialog_t *d, const sip_msg_t *req);

sip_dialog_t *dlg_find(dlg_mgr_t *m, const char *callid, size_t clen,
                       const char *local_tag, size_t ltlen,
                       const char *remote_tag, size_t rtlen);

/* 用消息三元组定位（按方向自动交换 tag） */
sip_dialog_t *dlg_match_msg(dlg_mgr_t *m, const sip_msg_t *msg,
                            dlg_dir_t dir);

void dlg_close(dlg_mgr_t *m, sip_dialog_t *d);
uint64_t dlg_mgr_active(dlg_mgr_t *m);

#ifdef __cplusplus
}
#endif
#endif

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
