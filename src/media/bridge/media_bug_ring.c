/* media_bug_ring.c — threaded bug + pre-buffer 实现（方案文档 §3.3 P2-8）
 *
 * 生产者（媒体线程）：pthread_mutex_trylock → memcpy → signal → 返回。
 * trylock 失败即丢帧，绝不等待；ring 满时推进 head 覆盖最旧帧。
 * 消费者（worker）：锁内拷到 scratch，解锁后回调，避免长时间持锁阻塞媒体线程。
 */
#include "media_bug_ring.h"

#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB_RING_MIN_DEPTH      1
#define MB_RING_MAX_DEPTH      1024
#define MB_RING_DROP_BATCH     64   /* 丢帧数累积到该值才上报一次 */

typedef struct mb_ring_slot {
    uint8_t          *buf;      /* create 时一次性分配 */
    size_t            len;
    media_bug_frame_t frame;
} mb_ring_slot_t;

struct media_bug_ring {
    char             name[MEDIA_BUG_NAME_MAX];
    media_bug_cb_t   cb;
    void            *ud;
    int              type_mask;
    int              depth;
    size_t           max_frame_bytes;

    mb_ring_slot_t  *slots;
    int              head;
    int              tail;
    int              count;

    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    pthread_t        th;
    int              th_started;
    int              stop;

    uint8_t         *scratch;    /* worker 专用，容量 max_frame_bytes */

    uint64_t pushed;
    uint64_t delivered;
    uint64_t dropped;
    uint64_t pend_drop; /* 待上报到 media_bug 的丢帧数 */
};

static void ring_trampoline(media_bug_frame_t *frame, void *ud);

/* 把累积的丢帧数汇总到 media_bug 的消费者统计（O(消费者槽数)，仅丢帧时调用）*/
static void ring_report_drops(media_bug_ring_t *r)
{
    uint64_t n = __atomic_exchange_n(&r->pend_drop, 0, __ATOMIC_RELAXED);
    if (n != 0)
        media_bug_consumer_note_drop(ring_trampoline, r, n);
}

static inline void ring_note_drop(media_bug_ring_t *r)
{
    __atomic_fetch_add(&r->dropped, 1, __ATOMIC_RELAXED);
    uint64_t pd = __atomic_add_fetch(&r->pend_drop, 1, __ATOMIC_RELAXED);
    if (pd >= MB_RING_DROP_BATCH)
        ring_report_drops(r);
}

static void ring_trampoline(media_bug_frame_t *frame, void *ud)
{
    (void)media_bug_ring_push((media_bug_ring_t *)ud, frame);
}

static void *ring_worker(void *arg)
{
    media_bug_ring_t *r = (media_bug_ring_t *)arg;

    pthread_mutex_lock(&r->lock);
    for (;;) {
        while (!r->stop && r->count == 0)
            pthread_cond_wait(&r->cond, &r->lock);
        if (r->count == 0)
            break;   /* stop 且已冲刷完 */

        mb_ring_slot_t *s = &r->slots[r->head];
        memcpy(r->scratch, s->buf, s->len);

        /* 解锁后回调：消费者可能很慢，不能挡住媒体线程 trylock。
         * 载荷已拷到 scratch，槽位可立即被生产者复用。 */
        media_bug_frame_t f = s->frame;
        f.data = r->scratch;
        if (f.pcm != NULL && (const uint8_t *)f.pcm == s->buf)
            f.pcm = (const int16_t *)(const void *)r->scratch;

        r->head = (r->head + 1) % r->depth;
        r->count--;
        pthread_mutex_unlock(&r->lock);

        r->cb(&f, r->ud);
        __atomic_fetch_add(&r->delivered, 1, __ATOMIC_RELAXED);

        pthread_mutex_lock(&r->lock);
    }
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

media_bug_ring_t *media_bug_ring_create(const char *name, media_bug_cb_t cb,
                                        void *ud, int type_mask, int depth,
                                        size_t max_frame_bytes)
{
    if (name == NULL || name[0] == '\0' || cb == NULL)
        return NULL;
    if (depth < MB_RING_MIN_DEPTH) depth = MB_RING_MIN_DEPTH;
    if (depth > MB_RING_MAX_DEPTH) depth = MB_RING_MAX_DEPTH;
    if (max_frame_bytes == 0)
        return NULL;

    media_bug_ring_t *r = (media_bug_ring_t *)calloc(1, sizeof(*r));
    if (r == NULL)
        return NULL;

    snprintf(r->name, sizeof(r->name), "%s", name);
    r->cb = cb;
    r->ud = ud;
    r->type_mask = type_mask;
    r->depth = depth;
    r->max_frame_bytes = max_frame_bytes;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);

    r->slots = (mb_ring_slot_t *)calloc((size_t)depth, sizeof(*r->slots));
    r->scratch = (uint8_t *)malloc(max_frame_bytes);
    if (r->slots == NULL || r->scratch == NULL)
        goto fail;

    for (int i = 0; i < depth; ++i) {
        r->slots[i].buf = (uint8_t *)malloc(max_frame_bytes);
        if (r->slots[i].buf == NULL)
            goto fail;
    }

    if (pthread_create(&r->th, NULL, ring_worker, r) != 0)
        goto fail;
    r->th_started = 1;

    {
        media_bug_consumer_t c;
        memset(&c, 0, sizeof(c));
        snprintf(c.name, sizeof(c.name), "%s", r->name);
        c.type_mask = type_mask;
        c.cb = ring_trampoline;
        c.ud = r;
        if (media_bug_register_global_consumer(&c) != 0)
            goto fail;
    }

    log_info("media_bug", "ring '%s' created depth=%d max_frame=%zu",
             r->name, depth, max_frame_bytes);
    return r;

fail:
    if (r->th_started) {
        pthread_mutex_lock(&r->lock);
        r->stop = 1;
        pthread_cond_broadcast(&r->cond);
        pthread_mutex_unlock(&r->lock);
        pthread_join(r->th, NULL);
    }
    if (r->slots != NULL) {
        for (int i = 0; i < depth; ++i)
            free(r->slots[i].buf);
        free(r->slots);
    }
    free(r->scratch);
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->lock);
    free(r);
    return NULL;
}

void media_bug_ring_destroy(media_bug_ring_t *r)
{
    if (r == NULL) return;

    /* 先注销：热路径停止投递后才停 worker，避免回调 ud 已释放 */
    {
        media_bug_consumer_t c;
        memset(&c, 0, sizeof(c));
        snprintf(c.name, sizeof(c.name), "%s", r->name);
        c.cb = ring_trampoline;
        c.ud = r;
        media_bug_unregister_global_consumer(&c);
    }

    pthread_mutex_lock(&r->lock);
    r->stop = 1;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
    if (r->th_started) {
        pthread_join(r->th, NULL);
        r->th_started = 0;
    }

    ring_report_drops(r);

    uint64_t pushed = __atomic_load_n(&r->pushed, __ATOMIC_RELAXED);
    uint64_t delivered = __atomic_load_n(&r->delivered, __ATOMIC_RELAXED);
    uint64_t dropped = __atomic_load_n(&r->dropped, __ATOMIC_RELAXED);
    log_info("media_bug", "ring '%s' destroyed pushed=%llu delivered=%llu dropped=%llu",
             r->name, (unsigned long long)pushed,
             (unsigned long long)delivered, (unsigned long long)dropped);

    for (int i = 0; i < r->depth; ++i)
        free(r->slots[i].buf);
    free(r->slots);
    free(r->scratch);
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

int media_bug_ring_push(media_bug_ring_t *r, const media_bug_frame_t *f)
{
    if (r == NULL || f == NULL)
        return -1;
    if (f->data == NULL || f->data_len == 0 || f->data_len > r->max_frame_bytes) {
        ring_note_drop(r);
        return -1;
    }

    /* 消费者正在回调 → 立即丢帧，绝不阻塞媒体线程 */
    if (pthread_mutex_trylock(&r->lock) != 0) {
        ring_note_drop(r);
        return -2;
    }
    if (r->stop) {
        pthread_mutex_unlock(&r->lock);
        ring_note_drop(r);
        return -3;
    }

    if (r->count == r->depth) {
        /* 满：覆盖最旧帧，保留最近 depth 帧（pre-buffer 语义）*/
        r->head = (r->head + 1) % r->depth;
        r->count--;
        ring_note_drop(r);
    }

    mb_ring_slot_t *s = &r->slots[r->tail];
    memcpy(s->buf, f->data, f->data_len);
    s->len = f->data_len;
    s->frame = *f;
    s->frame.data = s->buf;
    if (f->pcm != NULL && (const uint8_t *)f->pcm == f->data)
        s->frame.pcm = (const int16_t *)(const void *)s->buf;

    r->tail = (r->tail + 1) % r->depth;
    r->count++;
    __atomic_fetch_add(&r->pushed, 1, __ATOMIC_RELAXED);
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);
    return 0;
}

int media_bug_ring_stats(media_bug_ring_t *r, char *out, size_t cap)
{
    if (r == NULL || out == NULL || cap == 0)
        return 0;

    pthread_mutex_lock(&r->lock);
    int pending = r->count;
    pthread_mutex_unlock(&r->lock);

    return snprintf(out, cap,
                    "ring '%s' depth=%d pending=%d pushed=%llu delivered=%llu "
                    "dropped=%llu max_frame_bytes=%zu\n",
                    r->name, r->depth, pending,
                    (unsigned long long)__atomic_load_n(&r->pushed, __ATOMIC_RELAXED),
                    (unsigned long long)__atomic_load_n(&r->delivered, __ATOMIC_RELAXED),
                    (unsigned long long)__atomic_load_n(&r->dropped, __ATOMIC_RELAXED),
                    r->max_frame_bytes);
}
