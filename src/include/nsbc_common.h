#ifndef NSBC_COMMON_H
#define NSBC_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 对应 REQ-ARCH-004：所有时间基准使用单调时钟 */
uint64_t nsbc_now_ms(void);

/* 返回码（生产树扩展：迁移自 poc，新增码供事务/网络/媒体层使用） */
typedef enum {
    NSBC_OK = 0,
    NSBC_ERR = -1,
    NSBC_NOMEM = -2,
    NSBC_INVAL = -3,
    NSBC_PERM_DENIED = -4,
    NSBC_AGAIN = -5,      /* EAGAIN 语义：现在不可用，稍后重试 */
    NSBC_NOENT = -6,      /* 查找无结果 */
    NSBC_EXISTS = -7,     /* 对象已存在 */
    NSBC_TIMEOUT = -8,
    NSBC_NOSPC = -9,      /* 端口/容量等资源耗尽（过载，回 503） */
    NSBC_PROTO = -10,     /* 协议格式错误（400） */
    NSBC_NOTSUP = -11,    /* 当前构建/阶段不支持（P1 转码执行等） */
    NSBC_AUTH = -12,      /* 需要鉴权（401/407） */
    NSBC_EOF = -13,
    NSBC_SHUTDOWN = -14,  /* 正在优雅退出，拒绝新工作 */
    NSBC_EXPIRED = -15,   /* nonce/会话等已过期（Digest 据此回 stale=true） */
    NSBC_BUSY = -16       /* 模块忙：unload_wait 超时（refcnt 未归零，Fail-Safe 泄漏）*/
} nsbc_rc_t;

#define NSBC_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NSBC_CONTAINER_OF(ptr, type, member)                                    \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define NSBC_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NSBC_MAX(a, b) ((a) > (b) ? (a) : (b))

#define NSBC_LOGI(...) fprintf(stdout, "[INFO] " __VA_ARGS__)
#define NSBC_LOGE(...) fprintf(stderr, "[ERR ] " __VA_ARGS__)

/* 断言式自检 */
#define NSBC_CHECK(cond, ...)                                                  \
    do {                                                                      \
        if (!(cond)) {                                                        \
            NSBC_LOGE("check failed: %s (%s:%d) ", #cond, __FILE__,           \
                      __LINE__);                                               \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

#endif /* NSBC_COMMON_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
