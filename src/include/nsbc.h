/* =============================================================================
 * nsbc.h — NSBC SBC 核心统一入口头文件
 *
 * 设计原则：不是无脑聚合让每个 .c 重编译全项目，而是三层头文件分治：
 *
 *   Tier 1+2  POSIX 通用系统头（下面 include 的）——一个 .c 一行顶掉 5-10 行
 *   Tier 3    专用系统头（sys/epoll sys/eventfd sys/mman sys/shm poll signal
 *             dirent sched getopt sys/un 等）——.c 文件按需自己 include
 *   Tier 4    第三方重头（openssl sqlite3 cjson 等）——绝对不进 nsbc.h，
 *             改一次 OpenSSL 版本会让全项目重编译
 *
 * nsbc 自有头：nsbc_common / nsbc_atomic / nsbc_list / nsbc_hash / nsbc_id
 * + 高频公共模块：log / module / b2bua —— 绝大多数 .c 都要。
 *
 * 模块私有内部头（cdr_event.h cdr_wal.h sip_txn.h net_sock.h sip_msg.h 等）
 * 完整定义在各自 .h 里，通过 b2bua.h → 传递 include，.c 文件按需再追加。
 * nsbc.h 禁止自己瞎写 typedef 前向声明——真实定义在各模块头里，不能冲突。
 *
 * 使用：绝大多数 .c 文件 `#include "nsbc.h"` 一行顶掉原来 5-10 行系统头。
 * 项目私有头（reactor.h event_bus.h esl.h 等）在 .c 文件里按需追加。
 *
 * extern "C" 包装：整个文件包 C++ 兼容（Rust bindgen / 测试桩可能需要）
 * ============================================================================= */
#ifndef NSBC_H
#define NSBC_H

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
 * Tier 1+2: POSIX 通用系统头（170 个 .c 里绝大多数用）
 *
 * 顺序按依赖排：sys/types.h 必须在 netinet/arpa/sys/socket 之前
 * -------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp */
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>

/* ----------------------------------------------------------------------------
 * nsbc 自有基础件（底层无依赖，所有其他 nsbc_* 头安全依赖它们）
 * -------------------------------------------------------------------------- */
#include "nsbc_common.h"    /* NSBC_OK/ERR/.. 返回码 + NSBC_MIN/MAX 宏 + nsbc_now_ms() */
#include "nsbc_atomic.h"    /* sat_add/sub/cas + sat_load_ptr/store_ptr + sat_rmb_seqcst */
#include "nsbc_list.h"      /* 双链表 + 迭代宏 LIST_FOREACH */
#include "nsbc_hash.h"      /* 哈希表 + 迭代宏 hash_foreach */
#include "nsbc_id.h"        /* trace_id 生成（原子单调计数器） */

/* ----------------------------------------------------------------------------
 * 高频公共模块头（绝大多数 .c 需要）
 *
 * 真实类型定义都在这些模块头里：
 *   log.h      —— 分级日志（INFO/WARN/ERR/DEBUG）
 *   module.h   —— NSBC_MOD/NSBC_MOD0_CAT + modmgr_create/init/start/unload
 *   b2bua.h    —— b2b_core_t / b2b_call_t + b2b_*_hook_add 六组多槽链
 *                 → 传递 include sip_msg.h sip_txn.h sip_transport.h net_sock.h
 *                 所以 sip_msg_t / txn_state_t / nsbc_sa_t / sip_tr_proto_t 等
 *                 都通过 b2bua.h → 真实头文件拿到，nsbc.h 不再自己写前向声明
 * -------------------------------------------------------------------------- */
#include "log.h"
#include "module.h"
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
