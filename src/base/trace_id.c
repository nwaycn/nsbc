/* trace-id——粗骨架（REQ-OBS-004）
 *
 * 接线点（信号入口/出口）：
 *  - signaling 收 INVITE/REGISTER：读 X-Trace-ID（自定义头，直达枚举
 *    暂无槽，先原始头扫描）→ trace_from_inbound → trace_enter；
 *  - 出口外呼/集群复制：trace_render_header 携带同 id；
 *  - 工作线程回收（worker 任务结束钩子）：trace_clear 防线程复用串单。
 * llog_cid 已是活宏（log.h），本文件激活其调用口径而非新增日志 API。
 */
#include "trace_id.h"
#include "nsbc_id.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static __thread char tl_id[TRACE_ID_BUF_SZ];

int trace_new(char out[TRACE_ID_BUF_SZ])
{
    uint8_t raw[16];
    static const char hex[] = "0123456789abcdef";
    int i;
    if (out == NULL)
        return -1;
    if (nsbc_rand_fill(raw, sizeof(raw)) != 0)
        return -1;
    for (i = 0; i < 16; ++i) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 0x0F];
    }
    out[TRACE_ID_HEX_LEN] = '\0';
    return 0;
}

int trace_is_valid(const char *s)
{
    int i;
    if (s == NULL)
        return 0;
    for (i = 0; i < TRACE_ID_HEX_LEN; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return s[TRACE_ID_HEX_LEN] == '\0';
}

int trace_from_inbound(const char *in, char out[TRACE_ID_BUF_SZ])
{
    if (out == NULL)
        return -1;
    if (trace_is_valid(in)) {
        memcpy(out, in, TRACE_ID_BUF_SZ);
        return 0;
    }
    if (trace_new(out) != 0)
        return -1;
    return 1;
}

void trace_enter(const char *id32)
{
    if (id32 == NULL) {
        tl_id[0] = '\0';
        return;
    }
    if (trace_is_valid(id32))
        memcpy(tl_id, id32, TRACE_ID_BUF_SZ);
}

const char *trace_current(void)
{
    return tl_id[0] == '\0' ? NULL : tl_id;
}

void trace_clear(void)
{
    tl_id[0] = '\0';
}

int trace_render_header(char *out, size_t cap)
{
    int n;
    if (out == NULL || cap == 0)
        return -1;
    if (tl_id[0] == '\0' && trace_new(tl_id) != 0)
        return -1;
    n = snprintf(out, cap, "X-Trace-ID: %s\r\n", tl_id);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}
