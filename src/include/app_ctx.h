#ifndef NSBC_APP_CTX_H
#define NSBC_APP_CTX_H

#include "nsbc_common.h"
#include "nsbc_atomic.h"
#include "cfg.h"
#include "mem.h"
#include "reactor.h"
#include "worker.h"
#include "timer_wheel.h"
#include "cdr_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main 装配出的全局运行上下文，模块 init(start/stop/fini) 的唯一入参。
 * 模块不得私自创建线程/长生命周期堆对象（REQ-ARCH-010）。 */

typedef struct route_table route_table_t;

typedef struct nsbc_ctx {
    cfg_t *cfg;
    reactor_t *re;
    worker_pool_t *wpool;
    tw_t *tw;
    mem_rcu_t *rcu;

    /* 路由表（sip 层装配，供 registrar / outreg / b2b 只读查询） */
    route_table_t *rt;

    /* 内存五域（7.1）：SIGNAL / PACKET / MEDIA / TRANSCODE / RECORD */
    mem_arena_t *arena[5];

    /* 优雅停机 */
    nsbc_atomic_t stopping;       /* 置 1 后不再接入新呼叫 */
    nsbc_atomic_t active_calls;   /* 在通呼叫数，drain 到 0 退出 */
    int64_t drain_deadline_ms;    /* 最长 drain 窗口 */

    /* 数据/配置目录 */
    const char *config_file;
    const char *data_dir;

    /* endpoints 模块在 signaling 之前登记的 Digest HA1 凭证回调
     * （签名同 sip_digest.h 的 digest_ha1_fn；此处不引 sip 头以保持
     * core 不依赖 sip 层的目录方向，REQ-7.8）。无凭证库时为 NULL。 */
    const char *(*ha1_cb)(const char *realm, size_t rlen,
                          const char *user, size_t ulen, void *ud);
    void *ha1_ud;

    /* 各装配模块挂载的运行时指针（供 OAM/兄弟模块只读查询） */
    void *sig_rt;     /* core/signaling.c: sig_runtime_t */
    void *sip_tr;     /* sip_transport_t（signaling 创建后挂载，供 outreg/b2b 只读查询） */
    void *b2b;        /* b2b_core_t */
    void *loc_db;     /* loc_db_t */
    void *reg_core;   /* reg_core_t */
    void *bridge;     /* bridge_mgr_t */
    void *txn_mgr;    /* txn_mgr_t（OBS 指标） */
    void *ptable;     /* ptable_t（OBS 指标） */
    void *dbal;       /* dbal_t */
    void *rec_mgr;    /* rec_mgr_t（recorder.h 通话录音管理器指针） */
    void *billing;    /* billing_t（rating.h 费率/账户模块） */
    /* REQ-PBX-CC-002：ACD 队列 + 座席池（P2 装配入口，callcenter 模块 start 时赋值） */
    void *cq_mgr;     /* cc_mgr_t*（queue 模块） */
    void *agent_pool; /* agent_pool_t*（agent 模块） */

    /* OAM 只读计数槽（A3）：避免 oam 横向 include registrar 兄弟头；
     * 组合根在对象创建后接线，模式同 ha1_cb。当前唯一使用者：
     * control 命令 "show registrations"。 */
    uint64_t (*binding_count_fn)(void *ud);
    void *binding_count_ud;

    /* 结构化事件上报槽（A4）：ha 等同层模块不得横向 include oam/obs.h；
     * oam 模块 start 时接线为 obs_event，调用方需做 NULL 防护。
     * level: llog 级别（0=EMERG~7=DEBUG），-1=默认 INFO(6)。 */
    void (*event_fn)(int level, const char *domain_action, const char *fmt, ...);

    /* CDR 上报槽（REQ-PBX-CDR-001）：sip/core 话务生命周期点产出
     * cdr_event_t（START/ANSWER/END），billing 模块 start 时接线为
     * cdr_sink；调用方 NULL 防护。结构定义见 cdr_event.h（中立头）。 */
    void (*cdr_emit)(void *ud, const cdr_event_t *ev);
    void *cdr_ud;

    /* ESL 事件推送槽（Phase 2 增量 2026-10-15）：CDR/HANGUP/PLAYBACK_STOP
     * 等事件点 fire 到 esl_server（oam 层），让订阅的外部系统（计费/CRM）
     * 收到实时 JSON 事件。esl_start 时绑定；调用方 NULL 防护。 */
    void (*esl_event_fire)(const char *name, const char *json_body, size_t json_len);

    /* §8 双模打断槽（2026-10-15 增量）：ESL uuid_break 命令
     * 需要调 playback_engine 和 dp_sleep——但 oam 层不能 include mod/media
     * 层头文件。用 ctx 槽位让 mod/media 模块 start 时绑定，oam 层只调
     * 槽位不 include 反向头。 */
    int  (*pb_stop_if_playing)(void *call_key);
    int  (*dp_sleep_cancel)(void *call_key);
    int  (*dp_sleep_active)(void *call_key);
} nsbc_ctx_t;

static inline void ctx_request_stop(nsbc_ctx_t *c)
{
    sat_write(c->stopping, 1);
}
static inline int ctx_stopping(nsbc_ctx_t *c)
{
    return sat_read(c->stopping) != 0;
}

/* main.c 提供：全局上下文访问器（reload 命令线程/UDS 查询线程取 cfg/rt）。
 * 启动完成前与退出后返回 NULL。 */
nsbc_ctx_t *nsbc_get_ctx(void);

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
