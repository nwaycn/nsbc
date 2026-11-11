#ifndef NSBC_RTP_REASM_H
#define NSBC_RTP_REASM_H

/* H.264 RTP 重组器（RFC 6184）
 *
 * 输入：relay 解密后的明文 RTP video payload（单 NAL / FU-A / STAP-A）
 * 输出：完整 access unit 的 Annex-B 字节流（00 00 00 01 前缀）
 *
 * 帧判定：marker=1 或 timestamp 变化；100ms 无 marker 超时强制 flush。
 * 安全上限：单帧 512KB，超限丢整帧；seq 回退丢整帧。
 */

#include "nsbc_common.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VID_REASM_MAX_FRAME          (512 * 1024)
#define VID_REASM_MARKER_TIMEOUT_MS  100

typedef struct vid_reasm vid_reasm_t;

/* 完整帧回调 —— frame 指向重组器内部 buffer，回调返回后即失效（调用方需自行拷贝） */
typedef void (*vid_frame_cb_t)(void *ud, const uint8_t *frame,
                               size_t len, uint32_t rtp_ts);

int  vid_reasm_create(vid_reasm_t **out, vid_frame_cb_t cb, void *ud);
void vid_reasm_destroy(vid_reasm_t *r);

/* 喂一个 RTP video packet（payload 指向 RTP header 之后的视频载荷） */
void vid_reasm_feed(vid_reasm_t *r, const uint8_t *payload, size_t len,
                    int marker, uint16_t seq, uint32_t ts, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_RTP_REASM_H */
