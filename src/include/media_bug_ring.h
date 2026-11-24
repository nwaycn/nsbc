/* media_bug_ring.h — threaded bug + pre-buffer（方案文档 §3.3 P2-8）
 *
 * 慢消费者（落盘/编码/跨进程上报）不应在媒体线程里执行。把回调挂到本
 * ring 后：
 *   - 媒体线程只做 trylock + memcpy，绝不阻塞等待；
 *   - 生产者拿不到锁（消费者正在回调）立即丢帧并计数，不反压 SIP 媒体面；
 *   - ring 满时覆盖最旧帧（pre-buffer 语义：始终保留最近 depth 帧）；
 *   - 独立 worker 线程串行回调，回调里可以安全做重活。
 *
 * 用法（P2-10 推荐的正确姿势）：
 *   media_bug_ring_t *r = media_bug_ring_create("asr", asr_on_frame, asr_ud,
 *                                               BUG_PCM_READ, 32, 4096);
 *   ...
 *   media_bug_ring_destroy(r);   // 注销 consumer、冲刷剩余帧、join worker
 *
 * 约束：
 *   - 缓冲在 create 时按 depth × max_frame_bytes 一次性分配，热路径零 malloc；
 *   - 超长帧（> max_frame_bytes）直接丢弃，避免为单个大帧 realloc；
 *   - 丢帧数通过 media_bug_consumer_note_drop() 汇总到 show media_bugs。
 */
#ifndef NSBC_MEDIA_BUG_RING_H
#define NSBC_MEDIA_BUG_RING_H

#include <stddef.h>
#include "media_bug.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct media_bug_ring media_bug_ring_t;

/* 创建并注册全局消费者。depth=pre-buffer 深度（1..1024），
 * max_frame_bytes=单帧载荷上限。失败返回 NULL。 */
media_bug_ring_t *media_bug_ring_create(const char *name, media_bug_cb_t cb,
                                        void *ud, int type_mask, int depth,
                                        size_t max_frame_bytes);

/* 停止并回收：注销 consumer → worker 冲刷剩余帧 → join → free。 */
void media_bug_ring_destroy(media_bug_ring_t *r);

/* 媒体线程调用（一般作为 media_bug_cb_t 的实现体）。
 * 返回 0=入队，-1=参数/超长帧丢弃，-2=消费者忙丢弃，-3=已停止。 */
int media_bug_ring_push(media_bug_ring_t *r, const media_bug_frame_t *f);

/* 单行统计：depth/pending/pushed/delivered/dropped。 */
int media_bug_ring_stats(media_bug_ring_t *r, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_MEDIA_BUG_RING_H */
