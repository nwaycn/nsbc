#include "xml_parse.h"
#include <string.h>

/* P0 占位实现：字节级浅检查/浅提取；P1 换 libxml2（禁外部实体/禁 DTD）。 */

int xml_wellformed_check(const char *buf, size_t len)
{
    if (!buf || len < 4)
        return NSBC_PROTO;
    /* 拒绝 DOCTYPE / ENTITY（XXE 防护，P1 libxml2 同样关闭） */
    static const char doctype[] = "<!DOCTYPE";
    static const char entity[] = "<!ENTITY";
    for (size_t i = 0; i + 8 < len; ++i) {
        if (!strncasecmp(buf + i, doctype, sizeof(doctype) - 1) ||
            !strncasecmp(buf + i, entity, sizeof(entity) - 1))
            return NSBC_PROTO;
    }
    /* 找首个 '<'，要求是 xml 声明或元素 */
    size_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\r' || buf[i] == '\n' ||
                       buf[i] == '\t'))
        ++i;
    if (i >= len || buf[i] != '<')
        return NSBC_PROTO;
    /* 末尾必须存在 '>' */
    return (memchr(buf, '>', len) != NULL) ? NSBC_OK : NSBC_PROTO;
}

int xml_element_text(const char *buf, size_t len, const char *element,
                     char *out, size_t outsz)
{
    char open[128], close[128];
    int n = snprintf(open, sizeof(open), "<%s", element);
    snprintf(close, sizeof(close), "</%s>", element);
    if (n <= 0)
        return NSBC_INVAL;
    const char *p = buf;
    const char *end = buf + len;
    const char *s = NULL;
    size_t ol = strlen(open);
    for (; p + ol <= end; ++p) {
        if (!strncmp(p, open, ol) &&
            (p[ol] == '>' || p[ol] == ' ' || p[ol] == '\t')) {
            s = p + ol;
            break;
        }
    }
    if (!s)
        return NSBC_NOENT;
    const char *gt = memchr(s, '>', (size_t)(end - s));
    if (!gt)
        return NSBC_PROTO;
    ++gt;
    const char *e = NULL;
    size_t cl = strlen(close);
    for (const char *q = gt; q + cl <= end; ++q) {
        if (!strncmp(q, close, cl)) {
            e = q;
            break;
        }
    }
    if (!e)
        return NSBC_PROTO;
    size_t n2 = (size_t)(e - gt);
    if (n2 + 1 > outsz)
        n2 = outsz - 1;
    memcpy(out, gt, n2);
    out[n2] = '\0';
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
