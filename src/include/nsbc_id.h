#ifndef NSBC_ID_H
#define NSBC_ID_H

/* 随机数/标识（REQ-ARCH-004 无关，但 RTP SSRC RFC 3550 §8.1 要求不可预测，
 * Call-ID/branch/tag 同样必须随机）：
 * 优先 getrandom(2)（Linux 3.17；3.10 回退 /dev/urandom），永不退化为 rand()。
 */

#include <stddef.h>
#include <stdint.h>

int nsbc_rand_fill(void *buf, size_t len);

/* RFC 4122 v4 UUID，out 至少 37 字节，返回写出长度（不含 NUL 依赖：显式置 NUL） */
int nsbc_uuid4(char *out, size_t outsz);

/* SIP branch（不含 z9hG4bK 前缀部分由调用方拼）/ Call-ID 生成 */
int nsbc_rand_token(char *out, size_t outsz, size_t hex_len);

/* Call-ID：32 hex 随机 + '@' + host 标识 */
int nsbc_callid_make(char *out, size_t outsz, const char *host_tag);

/* RFC 3550 §8.1 随机 SSRC（32bit，调用方负责碰撞重选） */
uint32_t nsbc_ssrc_random(void);

#endif /* NSBC_ID_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
