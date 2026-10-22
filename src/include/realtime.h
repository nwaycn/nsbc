#ifndef NSBC_REALTIME_H
#define NSBC_REALTIME_H

/* REQ-ARCH-004 补充：实时时钟仅允许用于对外展示——
 * SIP Date 头、CDR 墙上时间戳；时长计算仍用 nsbc_now_ms() 单调差值。
 */

#include <stdint.h>
#include <stddef.h>

uint64_t nsbc_wall_ms(void);  /* Unix epoch 毫秒 */
uint64_t nsbc_wall_us(void);  /* Unix epoch 微秒 */

/* ISO8601 本地时间，如 2026-09-26T15:08:51.123+08:00，out 至少 32 字节 */
int nsbc_wall_iso8601(char *out, size_t outsz);

#endif /* NSBC_REALTIME_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
