#ifndef NSBC_SDP_INTERNAL_H
#define NSBC_SDP_INTERNAL_H

/* parse/sdp 模块内部跨文件声明（公共 API 见 sdp.h） */

#include "sdp.h"

/* sdp_attr.c */
sdp_dir_t sdp_dir_from_flag(const char *p, size_t len);
nsbc_rc_t sdp_parse_conn(const sip_tok_t *v, sdp_conn_t *c);
nsbc_rc_t sdp_parse_rtpmap_val(const sip_tok_t *v, int *pt,
                               sip_tok_t *enc, sip_tok_t *rate,
                               sip_tok_t *channels);
nsbc_rc_t sdp_parse_rtcp_attr(const sdp_attr_t *a, int *port,
                              sdp_conn_t *c, int *has_conn);

#endif /* NSBC_SDP_INTERNAL_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
