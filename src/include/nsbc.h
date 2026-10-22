/* =============================================================================
 * nsbc.h — NSBC SBC 核心统一入口头文件
 *
 * 设计原则（不是 137 个头的无脑聚合，避免让每个 .c 重编译全项目）：
 *   1. 系统基础：<stddef> <stdint> <stdbool> <stdarg> — 每个 .c 都要
 *   2. nsbc_* 基础件：nsbc_common（返回码/宏/时间）+ nsbc_atomic + nsbc_list + nsbc_hash
 *   3. 前向声明高频跨模块结构体：b2b_core_t / sip_msg_t / sip_txn_t / nsbc_sa_t /
 *      nsbc_modmgr_t / sip_tr_proto_t — 头文件互相 include 时只需要声明大小
 *   4. 高频公共模块头：module + b2bua + log — 大多数 .c 都要
 *
 * 模块私有内部头（cdr_event.h / cdr_sink.h / sip_txn.h 等）不进 nsbc.h，
 * 模块 .c 文件自己 include 所需的具体头。nsbc.h 是"跨模块公共层"。
 *
 * 使用：绝大多数 .c 文件 `#include "nsbc.h"` 一行顶上原来 5-10 行 include。
 * 模块私有头在 .c 文件里按需追加。
 *
 * extern "C" 包装：整个文件包 C++ 兼容（Rust bindgen / 测试桩可能需要）
 * ============================================================================= */
#ifndef NSBC_H
#define NSBC_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 系统标准头（每 .c 必用，一次性解决）---- */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/* ---- nsbc 自有基础件（底层无依赖，所有其他 nsbc_* 头安全依赖它们）---- */
#include "nsbc_common.h"    /* NSBC_OK/ERR/.. 返回码 + NSBC_MIN/MAX 宏 + nsbc_now_ms() */
#include "nsbc_atomic.h"    /* sat_add/sub/cas + sat_load_ptr/store_ptr + sat_rmb_seqcst */
#include "nsbc_list.h"      /* 双链表 + 迭代宏 LIST_FOREACH */
#include "nsbc_hash.h"      /* 哈希表 + 迭代宏 hash_foreach */
#include "nsbc_id.h"        /* trace_id 生成（原子单调计数器） */

/* ----------------------------------------------------------------------------
 * 前向声明（高频跨模块结构体，完整定义在各自模块 .h 里）
 *
 * 只给指针/数组大小需要不完整类型、但头文件互相 include 避免循环依赖。
 * 模块自己 .c 文件 include 具体 .h 获取完整定义。
 * -------------------------------------------------------------------------- */

/* sip_msg.h */
typedef struct sip_msg      sip_msg_t;
typedef struct sip_hdr      sip_hdr_t;
typedef struct sip_uri      sip_uri_t;

/* sip_txn.h */
typedef struct sip_txn      sip_txn_t;
typedef enum   sip_method   sip_method_t;
typedef enum   txn_event    txn_event_t;
typedef enum   txn_state    txn_state_t;
typedef enum   txn_kind     txn_kind_t;

/* sip_transport.h */
typedef enum   sip_tr_proto sip_tr_proto_t;

/* 网络地址（net_sock.h 定义完整）*/
struct sockaddr_storage;
typedef struct sockaddr_storage nsbc_sa_t;

/* b2bua.h */
typedef struct b2b_core     b2b_core_t;
typedef struct b2b_call     b2b_call_t;

/* module.h */
typedef struct nsbc_modmgr  nsbc_modmgr_t;

/* app_ctx.h（核心装配上下文）*/
struct nsbc_ctx;

/* ----------------------------------------------------------------------------
 * 高频公共模块头（绝大多数 .c 需要）
 * -------------------------------------------------------------------------- */

/* log.h — llog 分级日志（INFO/WARN/ERR/DEBUG）*/
#include "log.h"

/* module.h — NSBC_MOD/NSBC_MOD0_CAT + modmgr_create/init/start/unload */
#include "module.h"

/* b2bua.h — b2b_core_t / b2b_call_t + b2b_*_hook_add 六组多槽链 */
#include "b2bua.h"

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* NSBC_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
