#ifndef NSBC_TRACE_ID_H
#define NSBC_TRACE_ID_H

/* 分布式链路标识（REQ-OBS-004，P0 日志关联 / P1 跨组件传播）
 *
 * 形态：128bit 随机 → 32 hex（无分隔，方便日志 grep 与头字段携带）。
 * 传播：入向请求带 X-Trace-ID 且合法则沿用（跨 SBC/PBX/媒体面同一条
 * 链）；缺失则新建。工作线程进入呼叫处理时 trace_enter，日志用
 * llog_cid(..., trace_current(), ...) 打点，离开时 clear（线程复用
 * 防串单）。与 SIP Call-ID 并存：Call-ID 标对话，trace 标整条业务链
 * （可跨多个对话/中继转接）。
 * 分层：base（0），只依赖 nsbc_id 随机源。
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACE_ID_HEX_LEN 32
#define TRACE_ID_BUF_SZ  (TRACE_ID_HEX_LEN + 1)

/* 新 trace id（32 hex + NUL）；随机源失败返回非 0（不退化 rand） */
int  trace_new(char out[TRACE_ID_BUF_SZ]);

/* 校验入向携带值（仅 32 小写 hex；非法入站值必须拒绝后新建，
 * 防日志注入：调用方不得把任意字符串透传进日志头） */
int  trace_is_valid(const char *s);

/* 沿用或新建：in 合法→复制返回 0；in 非法/空→新建。返回 0=沿用，
 * 1=新建，<0=参数/随机错误。 */
int  trace_from_inbound(const char *in, char out[TRACE_ID_BUF_SZ]);

/* 当前工作线程 trace（未设置返回 NULL）；enter 拷贝，clear 复位 */
void trace_enter(const char *id32);
const char *trace_current(void);
void trace_clear(void);

/* X-Trace-ID 头行渲染（"X-Trace-ID: xxxx\r\n"），返回长度；
 * cap 不足返回负。供 sip_build/HTTP 出口统一携带。 */
int  trace_render_header(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_TRACE_ID_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
