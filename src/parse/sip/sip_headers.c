/* 头名 → 类型表：长名表 + compact 单字符 O(1) switch
 * （v/f/t/i/m/l/k/s/c/e/u/r/x/o） */

#include "sip_common.h"

static const struct {
    const char      *name;
    sip_hdr_type_t   type;
} hdr_tab[] = {
    {"Accept",                 SIP_H_ACCEPT},
    {"Accept-Contact",         SIP_H_ACCEPT_CONTACT},
    {"Accept-Encoding",        SIP_H_ACCEPT_ENCODING},
    {"Accept-Language",        SIP_H_ACCEPT_LANGUAGE},
    {"Allow",                  SIP_H_ALLOW},
    {"Allow-Events",           SIP_H_ALLOW_EVENTS},
    {"Authorization",          SIP_H_AUTHORIZATION},
    {"Call-ID",                SIP_H_CALL_ID},
    {"Contact",                SIP_H_CONTACT},
    {"Content-Disposition",    SIP_H_CONTENT_DISPOSITION},
    {"Content-Encoding",       SIP_H_CONTENT_ENCODING},
    {"Content-Language",       SIP_H_CONTENT_LANGUAGE},
    {"Content-Length",         SIP_H_CONTENT_LENGTH},
    {"Content-Type",           SIP_H_CONTENT_TYPE},
    {"CSeq",                   SIP_H_CSEQ},
    {"Date",                   SIP_H_DATE},
    {"Event",                  SIP_H_EVENT},
    {"Expires",                SIP_H_EXPIRES},
    {"From",                   SIP_H_FROM},
    {"Max-Forwards",           SIP_H_MAX_FORWARDS},
    {"Min-Expires",            SIP_H_MIN_EXPIRES},
    {"Min-SE",                 SIP_H_MIN_SE},
    {"Organization",           SIP_H_ORGANIZATION},
    {"P-Asserted-Identity",    SIP_H_P_ASSERTED_IDENTITY},
    {"P-Preferred-Identity",   SIP_H_P_PREFERRED_IDENTITY},
    {"P-Called-Party-ID",      SIP_H_P_CALLED_PARTY_ID},
    {"P-Access-Network-Info",  SIP_H_P_ACCESS_NETWORK_INFO},
    {"P-Early-Media",          SIP_H_P_EARLY_MEDIA},
    {"P-Charging-Function-Addresses", SIP_H_P_CHARGING_FUNCTION_ADDRESSES},
    {"P-Charging-Vector",      SIP_H_P_CHARGING_VECTOR},
    {"Path",                   SIP_H_PATH},
    {"Priority",               SIP_H_PRIORITY},
    {"Privacy",                SIP_H_PRIVACY},
    {"Proxy-Authenticate",     SIP_H_PROXY_AUTHENTICATE},
    {"Proxy-Authorization",    SIP_H_PROXY_AUTHORIZATION},
    {"Proxy-Require",          SIP_H_PROXY_REQUIRE},
    {"Reason",                 SIP_H_REASON},
    {"Record-Route",           SIP_H_RECORD_ROUTE},
    {"Refer-To",               SIP_H_REFER_TO},
    {"Referred-By",            SIP_H_REFERRED_BY},
    {"Replaces",               SIP_H_REPLACES},
    {"Reply-To",               SIP_H_REPLY_TO},
    {"Require",                SIP_H_REQUIRE},
    {"Resource-Priority",      SIP_H_RESOURCE_PRIORITY},
    {"Retry-After",            SIP_H_RETRY_AFTER},
    {"Route",                  SIP_H_ROUTE},
    {"RSeq",                   SIP_H_RSEQ},
    {"Server",                 SIP_H_SERVER},
    {"Service-Route",          SIP_H_SERVICE_ROUTE},
    {"Session-Expires",        SIP_H_SESSION_EXPIRES},
    {"Subject",                SIP_H_SUBJECT},
    {"Subscription-State",     SIP_H_SUBSCRIPTION_STATE},
    {"Supported",              SIP_H_SUPPORTED},
    {"Timestamp",              SIP_H_TIMESTAMP},
    {"To",                     SIP_H_TO},
    {"Unsupported",            SIP_H_UNSUPPORTED},
    {"User-Agent",             SIP_H_USER_AGENT},
    {"Via",                    SIP_H_VIA},
    {"Warning",                SIP_H_WARNING},
    {"WWW-Authenticate",       SIP_H_WWW_AUTHENTICATE},
};

sip_hdr_type_t sip_hdr_type_lookup(const char *name, size_t len)
{
    if (len == 1) {
        switch (sip_lower(name[0])) {
        case 'u': return SIP_H_ALLOW_EVENTS;
        case 'i': return SIP_H_CALL_ID;
        case 'm': return SIP_H_CONTACT;
        case 'e': return SIP_H_CONTENT_ENCODING;
        case 'l': return SIP_H_CONTENT_LENGTH;
        case 'c': return SIP_H_CONTENT_TYPE;
        case 'o': return SIP_H_EVENT;
        case 'f': return SIP_H_FROM;
        case 'r': return SIP_H_REFER_TO;
        case 'x': return SIP_H_SESSION_EXPIRES;
        case 's': return SIP_H_SUBJECT;
        case 'k': return SIP_H_SUPPORTED;
        case 't': return SIP_H_TO;
        case 'v': return SIP_H_VIA;
        }
        return SIP_H_UNKNOWN;
    }
    for (size_t i = 0; i < NSBC_ARRAY_SIZE(hdr_tab); ++i) {
        size_t n = strlen(hdr_tab[i].name);
        if (n == len && sip_ci_eq(name, hdr_tab[i].name, n))
            return hdr_tab[i].type;
    }
    return SIP_H_UNKNOWN;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
