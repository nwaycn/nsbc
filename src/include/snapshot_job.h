/* snapshot_job.h — VoLTE/WebRTC 视频抓图作业（一次性 tap）
 *
 * 场景：通话中业务需要抓取对端一帧画面做人脸比对/活体检测。
 * 特性（方案文档 §3.4）：
 *   - 不改变 relay/转码模式，纯旁路 tap（BUG_VIDEO_AU 全局消费者）；
 *   - 支持 H.264 / H.265 / VP8 / VP9（按 AU 携带的 codec_id 自动分派解码器）；
 *   - 异步作业：立即返回 job_id，worker 线程完成解码→JPEG 落盘；
 *   - 主动发 PLI 请求关键帧（500ms 限频），亚秒级返回；
 *   - 全局并发上限 16；同一呼叫同一腿最短间隔 2s（start 内强制）；
 *   - 失败原因明确（NO_VIDEO/NO_KEYFRAME/NO_PARAM_SETS/DECODE_FAIL/
 *     TOO_SMALL/BLACK_FRAME/BUSY/TIMEOUT）。
 *
 * 线程/内存约束：
 *   - 媒体回调只做类型过滤 + 拷入有界槽位（持作业锁，无 IO/无 malloc）；
 *   - 编解码会话与 JPEG 缓冲全部在 worker 线程内按需分配；
 *   - 作业槽位固定 16，终态保留到槽位被下一个作业回收，供状态查询。
 */
#ifndef NSBC_SNAPSHOT_JOB_H
#define NSBC_SNAPSHOT_JOB_H

#include <stdint.h>
#include <stddef.h>
#include "media_bridge.h"   /* bridge_mgr_t */

#ifdef __cplusplus
extern "C" {
#endif

#define SNAPSHOT_MAX_FRAMES          3      /* 单作业最多候选帧（选优）*/
#define SNAPSHOT_DEFAULT_TIMEOUT_MS  2500   /* 等关键帧的默认超时 */
#define SNAPSHOT_MIN_INTERVAL_MS     2000   /* 同一呼叫同一腿最短间隔 */

typedef enum {
    SNAPSHOT_OK = 0,
    SNAPSHOT_INVAL,             /* 参数非法（leg 越界/call 为空）*/
    SNAPSHOT_NO_VIDEO,          /* 该腿未协商视频 / 无 relay */
    SNAPSHOT_NO_KEYFRAME,       /* 超时仍无关键帧（PLI 无响应）*/
    SNAPSHOT_NO_PARAM_SETS,     /* 有 IDR 但缺 SPS/PPS，无法解码 */
    SNAPSHOT_NO_CODEC,          /* 该腿编码无可用解码器（未编入 FFmpeg）*/
    SNAPSHOT_DECODE_FAIL,       /* 解码器初始化/解码出错 */
    SNAPSHOT_TOO_SMALL,         /* 分辨率过低（<160x120）*/
    SNAPSHOT_BLACK_FRAME,       /* 画面接近全黑 */
    SNAPSHOT_BUSY,              /* 并发/槽位不足 */
    SNAPSHOT_TIMEOUT,           /* 其它超时 */
    SNAPSHOT_CANCEL,            /* 被 purge 取消 */
} snapshot_result_t;

const char *snapshot_result_name(int result);

typedef struct {
    int      result;            /* snapshot_result_t */
    int      leg;               /* 0=A 1=B */
    int      width;
    int      height;
    int      frames_captured;   /* 成功解码的候选帧数 */
    uint64_t elapsed_ms;
    uint32_t ssrc;              /* 抓图所在视频 SSRC */
    uint32_t rtp_ts;            /* 该 AU 的 RTP 时间戳 */
    uint64_t bytes;             /* JPEG 字节数 */
    char     path[256];         /* 落盘路径；失败时为空 */
} snapshot_result_info_t;

/* 完成回调（worker 线程调用，禁止阻塞/IO；ud 由调用方保证生命周期）*/
typedef void (*snapshot_done_cb_t)(uint64_t job_id, void *ud,
                                   const snapshot_result_info_t *info);

/* 启动抓图作业。返回 job_id（>0）；0 表示启动失败，*why 给出明确原因
 * （可为 NULL）。结果通过 cb 异步通知，也可用 snapshot_job_get() 轮询。 */
uint64_t snapshot_job_start(bridge_mgr_t *m, void *call_key, int leg,
                            int timeout_ms, int want_frames,
                            snapshot_done_cb_t cb, void *ud,
                            snapshot_result_t *why);

/* 查询作业状态：*running=1 表示仍在执行（info->result 无意义）。
 * 返回 NSBC_OK / NSBC_NOENT。 */
int snapshot_job_get(uint64_t job_id, snapshot_result_info_t *out,
                     int *running);

/* 取消并回收全部作业（关机/重载时调用）。 */
void snapshot_job_purge_all(void);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SNAPSHOT_JOB_H */
