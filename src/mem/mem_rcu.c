/* mem_rcu.c — RCU 宽限期回收（7.1.5：引用计数 + RCU，读侧无锁） */
#include "mem.h"

#include <pthread.h>
#include <stdatomic.h>   /* P1: _Atomic */
#include "nsbc_atomic.h"

#define N_EPOCHS 3

typedef struct rcu_node {
    struct rcu_node *next;
    mem_rcu_free_fn fn;
    void *ptr;
} rcu_node_t;

struct mem_rcu_reader {
    /* 0=静止（不在临界区）；否则为进入时 epoch+1（1..3） */
    nsbc_atomic_t state;
    struct mem_rcu *rcu;
    struct mem_rcu_reader *gnext;
};

struct mem_rcu {
    pthread_mutex_t lock;           /* 仅写者回收路径使用，读侧不碰 */
    nsbc_atomic_t epoch;              /* 0..2 循环 */
    rcu_node_t *limbo[N_EPOCHS];
    mem_rcu_reader_t *readers;
    uint64_t nretired;
    uint64_t nreclaimed;
    _Atomic int destroyed;           /* P1: 销毁标记 — 1=thread_join 拒绝，thread_leave 跳过链表操作 */
    _Atomic int refcount;            /* P1: 引用计数 — 初始 1(destroy 方持)，join+1，destroy-1，leave-1，到 0 才 free r */
};

mem_rcu_t *mem_rcu_create(void)
{
    mem_rcu_t *r = (mem_rcu_t *)calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    if (pthread_mutex_init(&r->lock, NULL) != 0) {
        free(r);
        return NULL;
    }
    r->epoch = (0);
    /* P1: 初始化引用计数 = 1（destroy 方持有）
     * 初始 destroyed=0 — calloc 已清零 */
    atomic_init(&r->refcount, 1);
    return r;
}

static void free_list(mem_rcu_t *r, rcu_node_t **head)
{
    rcu_node_t *n = *head;
    while (n) {
        rcu_node_t *next = n->next;
        n->fn(n->ptr);
        free(n);
        r->nreclaimed++;
        n = next;
    }
    *head = NULL;
}

void mem_rcu_destroy(mem_rcu_t *r)
{
    if (!r)
        return;
    /* P1: 先置 destroyed=1 + 释放 limbo 链表 —
     * 让未退出的 reader 线程 thread_leave 跳过对 r->lock / r->readers 的访问 */
    atomic_store(&r->destroyed, 1);
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < N_EPOCHS; i++)
        free_list(r, &r->limbo[i]);
    pthread_mutex_unlock(&r->lock);

    /* P1: 释放 destroy 方自己持有的引用 */
    int rc = atomic_fetch_sub(&r->refcount, 1);
    if (rc == 1) {
        /* 引用计数归零 — 没有未退出的 reader 线程 */
        pthread_mutex_destroy(&r->lock);
        free(r);
    }
    /* else: 还有 reader 线程持有 refcount，等最后一个 thread_leave 来 free */
}

mem_rcu_reader_t *mem_rcu_thread_join(mem_rcu_t *r)
{
    if (!r)
        return NULL;
    /* P1: 如果已经 destroyed，拒绝新 reader 加入 */
    if (atomic_load(&r->destroyed))
        return NULL;
    mem_rcu_reader_t *rd =
        (mem_rcu_reader_t *)calloc(1, sizeof(*rd));
    if (!rd)
        return NULL;
    rd->state = (0);
    rd->rcu = r;

    /* P1: 持有一个引用 */
    atomic_fetch_add(&r->refcount, 1);

    pthread_mutex_lock(&r->lock);
    rd->gnext = r->readers;
    r->readers = rd;
    pthread_mutex_unlock(&r->lock);
    return rd;
}

void mem_rcu_thread_leave(mem_rcu_reader_t *rd)
{
    if (!rd)
        return;
    mem_rcu_t *r = rd->rcu;

    /* P1: 只有 r 没被 destroyed 时才安全访问 r->lock / r->readers。
     * destroyed=1 时 destroy 方可能已经释放了 limbo，但 refcount>0 时 r 本身
     * 还活着（不会 free），所以读 destroyed 是安全的。 */
    if (r != NULL && !atomic_load(&r->destroyed)) {
        pthread_mutex_lock(&r->lock);
        mem_rcu_reader_t **pp = &r->readers;
        while (*pp && *pp != rd)
            pp = &(*pp)->gnext;
        if (*pp)
            *pp = rd->gnext;
        pthread_mutex_unlock(&r->lock);
    }
    /* destroyed=1: 跳过链表摘除（destroy 方可能已经在处理/整个 readers 不重要了） */

    free(rd);

    /* P1: 释放自己的引用 — 如果是最后一个引用，free r */
    if (r != NULL) {
        int rc = atomic_fetch_sub(&r->refcount, 1);
        if (rc == 1) {
            pthread_mutex_destroy(&r->lock);
            free(r);
        }
    }
}

void mem_rcu_read_lock(mem_rcu_reader_t *rd)
{
    if (!rd)
        return;
    /* 先宣告 epoch（seq_cst：宣告不得与后续数据读重排），再读数据 */
    unsigned e = __atomic_load_n((&rd->rcu->epoch), __ATOMIC_ACQUIRE);
    __atomic_store_n((&rd->state), (e + 1), __ATOMIC_SEQ_CST);
}

void mem_rcu_read_unlock(mem_rcu_reader_t *rd)
{
    /* seq_cst：临界区内的数据访问不得与宣告清除重排 */
    __atomic_store_n((&rd->state), (0), __ATOMIC_SEQ_CST);
}

/* 调用方持有 r->lock。返回 1=推进了一个宽限期，0=仍有读者停留在旧 epoch */
static int advance_locked(mem_rcu_t *r)
{
    unsigned e = __atomic_load_n((&r->epoch), __ATOMIC_ACQUIRE);
    for (mem_rcu_reader_t *rd = r->readers; rd; rd = rd->gnext) {
        unsigned s = __atomic_load_n((&rd->state), __ATOMIC_ACQUIRE);
        if (s != 0 && s != e + 1)
            return 0;
    }

    unsigned newe = (e + 1) % N_EPOCHS;
    /* 推进后，(newe+1)%3 链上的对象已度过两个 epoch 推进，安全释放 */
    free_list(r, &r->limbo[(newe + 1) % N_EPOCHS]);
    __atomic_store_n((&r->epoch), (newe), __ATOMIC_RELEASE);
    return 1;
}

int mem_rcu_advance(mem_rcu_t *r)
{
    if (!r)
        return NSBC_INVAL;
    pthread_mutex_lock(&r->lock);
    int ok = advance_locked(r);
    pthread_mutex_unlock(&r->lock);
    return ok ? NSBC_OK : NSBC_ERR;
}

void mem_rcu_retire(mem_rcu_t *r, void *ptr, mem_rcu_free_fn fn)
{
    if (!r || !ptr || !fn)
        return;

    rcu_node_t *n = (rcu_node_t *)malloc(sizeof(*n));
    if (!n)
        return; /* 回收路径属低频管理面；节点分配失败不影响信令 */
    n->ptr = ptr;
    n->fn = fn;
    n->next = NULL;

    pthread_mutex_lock(&r->lock);
    unsigned e = __atomic_load_n((&r->epoch), __ATOMIC_ACQUIRE);
    n->next = r->limbo[e];
    r->limbo[e] = n;
    r->nretired++;
    (void)advance_locked(r);
    (void)advance_locked(r);
    pthread_mutex_unlock(&r->lock);
}

uint64_t mem_rcu_pending(const mem_rcu_t *r)
{
    return r ? r->nretired - r->nreclaimed : 0;
}

uint64_t mem_rcu_reclaimed(const mem_rcu_t *r)
{
    return r ? r->nreclaimed : 0;
}

static void rcu_release_obj(void *obj)
{
    mem_obj_release(obj);
}

void mem_rcu_retire_obj(mem_rcu_t *r, void *obj)
{
    mem_rcu_retire(r, obj, rcu_release_obj);
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
