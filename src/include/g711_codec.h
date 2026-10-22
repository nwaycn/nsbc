/* G.711 μ-law <→ PCM16 编解码（公开 ITU-T G.711 查表法）
 *
 * μ-law (PCMU): 1 字节编码 16bit 样本；压扩比 ≈ 255:32768
 * 本表按 ITU-T G.711 规范预计算（a-law 目前未用；若需可加 ulaw2alaw）。
 *
 * 16bit PCM 样本范围 -32768..+32767；μ-law 编码 0x00..0xFF（静音 0xFF 或 0x7F）。
 * PCMU 常见静音：0x7F（负幅值静音）或 0xFF（正幅值静音）——两都允许。
 *
 * 线程安全：纯查表无状态。 */
#ifndef NSBC_G711_H
#define NSBC_G711_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCM16 (little-endian signed) → μ-law (PCMU) */
void g711_pcm16_to_ulaw(const int16_t *in, uint8_t *out, size_t n_samples);

/* μ-law (PCMU) → PCM16 (little-endian signed) */
void g711_ulaw_to_pcm16(const uint8_t *in, int16_t *out, size_t n_samples);

/* μ-law 静音填充（两字节 PCM16 零 → 一字节 μ-law 静音） */
extern const uint8_t g711_ulaw_silence;

#ifdef __cplusplus
}
#endif
#endif
