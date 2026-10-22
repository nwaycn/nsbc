#ifndef NSBC_G711_H
#define NSBC_G711_H

/* G.711 µ-law/A-law 基线（PCMU=0 / PCMA=8，RFC 3551）
 * 对应 REQ-TRANS-004（内置编解码）/ REQ-PLAY-001（G.711 放音 P0）
 *
 * 中间表示：S16 native endian 单声道 8000Hz；
 * 查表实现零 malloc、可 SIMD 批量化（P1 性能轮）。
 * 分层：media 层；被 codec 注册、player/IVR/dtmf INBAND 共用。
 */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单样本（-32768..32767 ↔ 8bit 码），接线批给 256 项查找表 */
uint8_t g711_ulaw_pack(int16_t sample);
int16_t g711_ulaw_unpack(uint8_t code);
uint8_t g711_alaw_pack(int16_t sample);
int16_t g711_alaw_unpack(uint8_t code);

/* 批量：返回写入/产出样本数；src/dst 禁止重叠 */
size_t g711_ulaw_encode(const int16_t *pcm, size_t samples, uint8_t *dst);
size_t g711_ulaw_decode(const uint8_t *code, size_t bytes, int16_t *pcm);
size_t g711_alaw_encode(const int16_t *pcm, size_t samples, uint8_t *dst);
size_t g711_alaw_decode(const uint8_t *code, size_t bytes, int16_t *pcm);

/* µ↔A 跨律直转（不经 PCM 展宽的省一遍舍入版本，P1 优化项） */
size_t g711_ulaw_to_alaw(const uint8_t *src, size_t bytes, uint8_t *dst);
size_t g711_alaw_to_ulaw(const uint8_t *src, size_t bytes, uint8_t *dst);

/* 内置 ops 自注册到 codec_registry（模块 init 调一次） */
int g711_register(void);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_G711_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
