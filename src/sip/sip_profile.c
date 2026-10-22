/* NSBC SIP Profile — C 侧运行时实现（REQ-SIP-PROFILE-001）
 *
 * 架构（v2 大并发整改）：
 *   - sip_profile_t 数组是全局运行时配置集合：Web sip_profiles DB 表 →
 *     UDS `profiles.reload <json>` 全量原子替换
 *   - profile **不拥有独立监听 socket**：SIP UDP/TCP/TLS 统一由
 *     signaling transport（sip_transport.c + reactor）监听。
 *     历史教训：本模块曾独立 socket()+SO_REUSEPORT bind 5060 但从未
 *     注册 reactor——内核把 UDP 包 hash 到无人读的 socket，REGISTER
 *     随机丢弃（10.0.0.25 生产事故）。profile 只承载配置/行为数据
 *     （realm/ACL/DTMF/PRACK/RTP 端口段），socket 层必须唯一。
 *   - 并发安全（无 UAF）：
 *       读侧  rwlock_rdlock 取指针；数组内容加载后不可变
 *       写侧  mutex + rwlock_wrlock 原子换数组指针
 *       旧数组进 retired 墓地，**只在 shutdown 释放**。
 *       reload 是低频管理操作、数组 ~2KB/条，用极小内存换"读侧指针
 *       永不过期"的绝对安全。（替代原 epoch 轮转方案：epoch 只有
 *       0/1 两值，释放条件永远不成立，8 次 reload 后 force free
 *       会造成在途读者 UAF。）
 *   - 高并发性能：黑白名单 CIDR 在 reload 时预编译为 (net,mask)
 *     uint32 数组，数据面判定零字符串解析、零内存拷贝、零分配。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sip_profile.h"
#include "log.h"
#include "nsbc_common.h"

#include <cjson/cJSON.h>

#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ========================================================================
 * 全局状态（单进程）
 * ======================================================================*/

typedef struct {
    sip_profile_t  *prof;      /* 当前生效数组（rwlock 保护指针+count） */
    unsigned        count;
    pthread_mutex_t lock;      /* 写侧互斥（reload 独占） */
    pthread_rwlock_t rwlock;   /* 读侧读写锁 */
    sip_profile_t **retired;   /* 退役数组墓地：只在 shutdown 释放 */
    unsigned        retired_n;
    unsigned        retired_cap;
} sip_state_t;

/* 锁静态初始化：sip_profiles_init() 幂等，彻底消除"init 无人调用
 * 导致锁未初始化"的隐患（glibc 零值锁恰好可用，但不可移植）。 */
static sip_state_t g = {
    .lock   = PTHREAD_MUTEX_INITIALIZER,
    .rwlock = PTHREAD_RWLOCK_INITIALIZER,
};

/* ========================================================================
 * 初始化 / 销毁
 * ======================================================================*/

int sip_profiles_init(void)
{
    /* g 的锁为 PTHREAD_*_INITIALIZER 静态初始化，幂等可重复调用 */
    return NSBC_OK;
}

void sip_profiles_shutdown(void)
{
    /* 仅在进程退出/测试收尾调用：此刻 free retired 数组是安全的
     * （与在途读者并发属退出路径，可接受）。 */
    pthread_mutex_lock(&g.lock);
    pthread_rwlock_wrlock(&g.rwlock);
    for (unsigned i = 0; i < g.count; ++i)
        sip_profile_unbind(&g.prof[i]);
    free(g.prof);
    g.prof = NULL;
    g.count = 0;
    for (unsigned i = 0; i < g.retired_n; ++i)
        free(g.retired[i]);
    free(g.retired);
    g.retired = NULL;
    g.retired_n = g.retired_cap = 0;
    pthread_rwlock_unlock(&g.rwlock);
    pthread_mutex_unlock(&g.lock);
    /* 锁不 destroy：init 幂等 + 单测可多轮 init/reload/shutdown */
}

/* ========================================================================
 * bind / unbind —— 校验语义（无独立 socket）
 *
 * SIP 监听 socket 唯一归属 signaling transport；本函数只做配置
 * 合法性校验（fail-fast，坏配置在 reload 入口整体回滚）。
 * ======================================================================*/

int sip_profile_bind(sip_profile_t *prof)
{
    if (!prof) return NSBC_INVAL;
    prof->udp_fd = prof->tcp_fd = prof->tls_fd = -1;

    if (!prof->enabled) {
        prof->state = SIP_PROFILE_DISABLED;
        return NSBC_OK;
    }

    /* ---- 校验 1：sip_ip 必须是合法 IPv4 字面量（空/0.0.0.0 = 通配） ----
     * 原 bug：inet_pton 返回值未检查，非法 IP 静默变成 0.0.0.0。 */
    if (prof->sip_ip[0] != '\0' && strcmp(prof->sip_ip, "0.0.0.0") != 0) {
        struct in_addr t;
        if (inet_pton(AF_INET, prof->sip_ip, &t) != 1) {
            llog(LOG_ERR, "sip-profile", "%s: invalid sip_ip '%s' (IPv4 literal only)",
                 prof->name, prof->sip_ip);
            prof->state = SIP_PROFILE_ERROR;
            return NSBC_ERR;
        }
    }

    /* ---- 校验 2：启用的协议端口必须非 0 ---- */
    if (prof->enable_udp && prof->sip_udp_port == 0) {
        llog(LOG_ERR, "sip-profile", "%s: enable_udp but sip_udp_port=0", prof->name);
        prof->state = SIP_PROFILE_ERROR;
        return NSBC_ERR;
    }
    if (prof->enable_tcp && prof->sip_tcp_port == 0) {
        llog(LOG_ERR, "sip-profile", "%s: enable_tcp but sip_tcp_port=0", prof->name);
        prof->state = SIP_PROFILE_ERROR;
        return NSBC_ERR;
    }
    if (prof->enable_tls)
        llog(LOG_WARN, "sip-profile",
             "%s: enable_tls=1 由 signaling transport 的 TLS listener 承担"
             "（profile 级 TLS 端口字段暂未接入）", prof->name);

    /* ---- 校验 3：RTP 端口段 ---- */
    if (prof->rtp_port_start == 0 || prof->rtp_port_end == 0 ||
        prof->rtp_port_end < prof->rtp_port_start) {
        llog(LOG_ERR, "sip-profile", "%s: invalid rtp port range %u-%u",
             prof->name, prof->rtp_port_start, prof->rtp_port_end);
        prof->state = SIP_PROFILE_ERROR;
        return NSBC_ERR;
    }

    prof->state = SIP_PROFILE_OK;
    llog(LOG_NOTICE, "sip-profile", "%s: OK UDP=%s:%u TCP=%s:%u RTP=%u-%u (shared transport)",
         prof->name, prof->sip_ip, prof->sip_udp_port,
         prof->sip_ip, prof->sip_tcp_port,
         prof->rtp_port_start, prof->rtp_port_end);
    return NSBC_OK;
}

void sip_profile_unbind(sip_profile_t *prof)
{
    if (!prof) return;
    if (prof->udp_fd >= 0) { close(prof->udp_fd); prof->udp_fd = -1; }
    if (prof->tcp_fd >= 0) { close(prof->tcp_fd); prof->tcp_fd = -1; }
    if (prof->tls_fd >= 0) { close(prof->tls_fd); prof->tls_fd = -1; }
}

/* ========================================================================
 * 旧数组退役（墓地）—— reload 换表后旧指针仍被在途读者持有，
 * 绝不在运行期 free。shutdown 统一释放。
 * ======================================================================*/

static void retire_array(sip_profile_t *old)
{
    if (!old) return;
    if (g.retired_n == g.retired_cap) {
        unsigned ncap = g.retired_cap ? g.retired_cap * 2 : 8;
        sip_profile_t **nr = realloc(g.retired, ncap * sizeof(*nr));
        if (!nr) {
            /* OOM：宁可泄漏也不 free 在途读者可能持有的数组 */
            llog(LOG_ERR, "sip-profile",
                 "retired realloc OOM: leak old array (will not be freed)");
            return;
        }
        g.retired = nr;
        g.retired_cap = ncap;
    }
    g.retired[g.retired_n++] = old;
}

/* ========================================================================
 * 读侧 API
 *
 * 返回的指针指向数组元素：数组内容加载后不可变，且旧数组进墓地
 * 不释放（直到 shutdown），因此指针在整个进程生命周期内有效。
 * ======================================================================*/

sip_profile_t *sip_profile_find(const char *name)
{
    if (!name || !*name) return NULL;
    pthread_rwlock_rdlock(&g.rwlock);
    sip_profile_t *found = NULL;
    for (unsigned i = 0; i < g.count; ++i) {
        if (g.prof[i].state != SIP_PROFILE_OK) continue;
        if (strcmp(g.prof[i].name, name) == 0) {
            found = &g.prof[i];
            break;
        }
    }
    pthread_rwlock_unlock(&g.rwlock);
    return found;
}

unsigned sip_profile_count(void)
{
    pthread_rwlock_rdlock(&g.rwlock);
    unsigned c = g.count;
    pthread_rwlock_unlock(&g.rwlock);
    return c;
}

sip_profile_t *sip_profile_at(unsigned idx)
{
    pthread_rwlock_rdlock(&g.rwlock);
    sip_profile_t *p = (idx < g.count) ? &g.prof[idx] : NULL;
    pthread_rwlock_unlock(&g.rwlock);
    return p;
}

/* ========================================================================
 * CIDR 解析 / 预编译 —— 高并发热路径优化
 *
 * reload 时把 "10.0.0.0/24,192.168.0.0/16" 编译成 (net,mask) 数组；
 * 数据面每包判定只需 N 次 uint32 AND+CMP（无 strtok/inet_pton/1KB 拷贝）。
 * ======================================================================*/

/* 单条 CIDR → (net, mask)（主机字节序）。支持 "a.b.c.d"（= /32）。
 * 返回 1 成功，0 非法。 */
static int cidr_parse_one(const char *cidr, uint32_t *net_out, uint32_t *mask_out)
{
    if (!cidr || !*cidr) return 0;

    char net_str[INET_ADDRSTRLEN + 4];  /* 够放 "255.255.255.255/32" */
    int  prefix = 32;
    const char *slash = strchr(cidr, '/');
    if (slash) {
        size_t nlen = (size_t)(slash - cidr);
        if (nlen >= sizeof(net_str)) return 0;
        memcpy(net_str, cidr, nlen);
        net_str[nlen] = '\0';
        prefix = atoi(slash + 1);
        if (prefix < 0 || prefix > 32) return 0;
    } else {
        snprintf(net_str, sizeof(net_str), "%s", cidr);
    }

    struct in_addr net_addr;
    if (inet_pton(AF_INET, net_str, &net_addr) != 1) return 0;
    *net_out  = ntohl(net_addr.s_addr);
    *mask_out = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
    return 1;
}

static int cidr_match_one(const char *cidr, const char *src_ip)
{
    if (!cidr || !*cidr || !src_ip || !*src_ip) return 0;
    uint32_t net, mask;
    struct in_addr ip_addr;
    if (inet_pton(AF_INET, src_ip, &ip_addr) != 1) return 0;
    if (!cidr_parse_one(cidr, &net, &mask)) return 0;
    uint32_t ip = ntohl(ip_addr.s_addr);
    return ((net & mask) == (ip & mask)) ? 1 : 0;
}

/* 预编译一条 ACL 字符串 → rules[]。
 * 返回：>0 = 编译条数；-1 = 超 PROFILE_ACL_MAX（调用方回退字符串路径）；
 *       0 = 空列表。非法条目跳过（与字符串路径语义一致）。 */
static int acl_compile(const char *list, profile_cidr_t *rules)
{
    if (!list || !*list) return 0;

    char buf[1024];
    size_t len = strlen(list);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, list, len);
    buf[len] = '\0';

    int n = 0, overflow = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t tlen = strlen(tok);
        while (tlen > 0 && (tok[tlen-1] == ' ' || tok[tlen-1] == '\t'))
            tok[--tlen] = '\0';
        if (*tok) {
            uint32_t net, mask;
            if (cidr_parse_one(tok, &net, &mask)) {
                if (n < PROFILE_ACL_MAX) {
                    rules[n].net  = net;
                    rules[n].mask = mask;
                    n++;
                } else {
                    overflow = 1;
                }
            }
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return overflow ? -1 : n;
}

static void profile_compile_acl(sip_profile_t *p)
{
    p->wl_n = acl_compile(p->ip_whitelist, p->wl);
    p->bl_n = acl_compile(p->ip_blacklist, p->bl);
    if (p->wl_n < 0 || p->bl_n < 0)
        llog(LOG_WARN, "sip-profile",
             "%s: ACL 条目超 %d，热路径回退字符串逐包解析", p->name, PROFILE_ACL_MAX);
}

static int compiled_list_match(const profile_cidr_t *rules, int n, uint32_t ip)
{
    for (int i = 0; i < n; ++i)
        if ((rules[i].net & rules[i].mask) == (ip & rules[i].mask))
            return 1;
    return 0;
}

/* ========================================================================
 * 填充默认值（parse_json 调用，字段默认值对齐 Rust sip_profiles 表）
 * ======================================================================*/

static void profile_set_defaults(sip_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->enabled          = 1;
    strncpy(p->sip_ip, "0.0.0.0", sizeof(p->sip_ip) - 1);
    p->sip_udp_port     = 5060;
    p->sip_tcp_port     = 5060;
    p->sip_tls_port     = 5061;
    p->enable_udp       = 1;
    p->enable_tcp       = 1;
    p->enable_tls       = 0;
    strncpy(p->rtp_ip, "0.0.0.0", sizeof(p->rtp_ip) - 1);
    p->rtp_port_start   = 10000;
    p->rtp_port_end     = 59999;
    p->enable_prack     = 0;
    p->enable_update    = 1;
    p->enable_t140      = 0;
    p->dtmf_mode        = PROFILE_DTMF_RFC4733;
    strncpy(p->realm, "nsbc.local", sizeof(p->realm) - 1);
    p->auth_calls       = 0;     /* 默认 external 模式（不强制认证） */
    p->ip_whitelist[0]  = '\0';  /* 空=不限制 */
    p->ip_blacklist[0]  = '\0';  /* 空=不拦截 */
    p->udp_fd = p->tcp_fd = p->tls_fd = -1;
    p->wl_n = p->bl_n = 0;
    p->state = SIP_PROFILE_DISABLED;
}

/* cJSON → sip_profile_t（单对象） */
static void parse_json_object(cJSON *obj, sip_profile_t *p)
{
    profile_set_defaults(p);

    /* 标识 */
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "name")) && cJSON_IsString(j))
        snprintf(p->name, sizeof(p->name), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "enabled")))
        p->enabled = cJSON_IsTrue(j) ? 1 : 0;

    /* SIP 监听 */
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "sip_ip")) && cJSON_IsString(j))
        snprintf(p->sip_ip, sizeof(p->sip_ip), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "external_sip_ip")) && cJSON_IsString(j))
        snprintf(p->external_sip_ip, sizeof(p->external_sip_ip), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "sip_udp_port")) && cJSON_IsNumber(j))
        p->sip_udp_port = (uint16_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "sip_tcp_port")) && cJSON_IsNumber(j))
        p->sip_tcp_port = (uint16_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "sip_tls_port")) && cJSON_IsNumber(j))
        p->sip_tls_port = (uint16_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "enable_udp")))
        p->enable_udp = cJSON_IsTrue(j) ? 1 : 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "enable_tcp")))
        p->enable_tcp = cJSON_IsTrue(j) ? 1 : 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "enable_tls")))
        p->enable_tls = cJSON_IsTrue(j) ? 1 : 0;

    /* RTP */
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "rtp_ip")) && cJSON_IsString(j))
        snprintf(p->rtp_ip, sizeof(p->rtp_ip), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "external_rtp_ip")) && cJSON_IsString(j))
        snprintf(p->external_rtp_ip, sizeof(p->external_rtp_ip), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "rtp_port_start")) && cJSON_IsNumber(j))
        p->rtp_port_start = (uint16_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "rtp_port_end")) && cJSON_IsNumber(j))
        p->rtp_port_end = (uint16_t)j->valueint;

    /* 协议特性 */
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "enable_prack")))
        p->enable_prack = cJSON_IsTrue(j) ? 1 : 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "enable_update")))
        p->enable_update = cJSON_IsTrue(j) ? 1 : 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "enable_t140")))
        p->enable_t140 = cJSON_IsTrue(j) ? 1 : 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "dtmf_mode")) && cJSON_IsString(j)) {
        if      (!strcmp(j->valuestring, "info"))    p->dtmf_mode = PROFILE_DTMF_INFO;
        else if (!strcmp(j->valuestring, "inband")) p->dtmf_mode = PROFILE_DTMF_INBAND;
        else if (!strcmp(j->valuestring, "auto"))   p->dtmf_mode = PROFILE_DTMF_AUTO;
        else                                         p->dtmf_mode = PROFILE_DTMF_RFC4733;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "realm")) && cJSON_IsString(j))
        snprintf(p->realm, sizeof(p->realm), "%s", j->valuestring);

    /* 鉴权模式 */
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "auth_calls"))) {
        if (cJSON_IsTrue(j))       p->auth_calls = 1;
        else if (cJSON_IsFalse(j)) p->auth_calls = 0;
        else if (cJSON_IsNumber(j)) p->auth_calls = j->valueint ? 1 : 0;
    }
    /* IP 黑白名单（CIDR 逗号分隔） */
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "ip_whitelist")) && cJSON_IsString(j))
        snprintf(p->ip_whitelist, sizeof(p->ip_whitelist), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(obj, "ip_blacklist")) && cJSON_IsString(j))
        snprintf(p->ip_blacklist, sizeof(p->ip_blacklist), "%s", j->valuestring);

    /* ★ 诊断日志：解析后的 NAT 字段 */
    llog(LOG_INFO, "sip-profile", "parse [%s] sip=[%s] rtp=[%s] ext_sip=[%s] ext_rtp=[%s]",
         p->name, p->sip_ip, p->rtp_ip, p->external_sip_ip, p->external_rtp_ip);

    /* 热路径优化：预编译 ACL（字符串字段保留，超限/手工填充时回退） */
    profile_compile_acl(p);
}

/* ========================================================================
 * sip_profiles_reload_json — 全量原子替换
 *
 * [1] 解析 JSON → new_prof[]（先校验，不触碰任何 socket）
 * [2] 逐条校验（sip_ip/端口/RTP 段/重名）→ 失败整体回滚，旧表不动
 * [3] mutex + wrlock 原子换指针（读者瞬间看到新表或旧表，绝无中间态）
 * [4] 旧数组进 retired 墓地，shutdown 才释放（读侧指针永不过期）
 * ======================================================================*/

int sip_profiles_reload_json(const char *json, char *out_err, size_t err_sz)
{
    if (!json || !*json) {
        if (out_err) snprintf(out_err, err_sz, "empty json");
        return NSBC_INVAL;
    }

    /* ★ 诊断日志：打完整 JSON，看 external_sip_ip / external_rtp_ip 有没有 */
    llog(LOG_INFO, "sip-profile", "reload_json len=%zu body=%.800s", strlen(json), json);

    /* ---- [1] 解析 JSON 数组 ---- */
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        if (out_err) snprintf(out_err, err_sz, "invalid json: expected array");
        if (root) cJSON_Delete(root);
        return NSBC_INVAL;
    }
    int n = cJSON_GetArraySize(root);
    if (n <= 0) {
        /* 空数组：允许——清空所有 profile */
        cJSON_Delete(root);
        root = NULL;   /* 已释放：防下方统一 Delete 处双重释放 */
    }

    sip_profile_t *new_prof = calloc((size_t)(n > 0 ? n : 1), sizeof(sip_profile_t));
    if (!new_prof) {
        if (out_err) snprintf(out_err, err_sz, "OOM");
        if (root) cJSON_Delete(root);
        return NSBC_NOMEM;
    }

    /* ---- [2] 逐条填充 + 校验 ---- */
    int loaded = 0;
    for (int i = 0; i < n; ++i) {
        cJSON *obj = cJSON_GetArrayItem(root, i);
        if (!obj || !cJSON_IsObject(obj)) {
            if (out_err) snprintf(out_err, err_sz, "item[%d] not object", i);
            goto rollback;
        }
        sip_profile_t *p = &new_prof[loaded];
        parse_json_object(obj, p);
        if (!p->name[0]) {
            if (out_err) snprintf(out_err, err_sz, "item[%d] missing name", i);
            goto rollback;
        }
        /* 重名拒绝：find 只会命中第一条，重名必然是配置错误 */
        for (int k = 0; k < loaded; ++k) {
            if (strcmp(new_prof[k].name, p->name) == 0) {
                if (out_err) snprintf(out_err, err_sz,
                                      "duplicate profile name '%s'", p->name);
                goto rollback;
            }
        }
        if (sip_profile_bind(p) != NSBC_OK) {
            if (out_err) snprintf(out_err, err_sz, "profile[%d]=%s invalid", loaded, p->name);
            goto rollback;
        }
        loaded++;
    }
    cJSON_Delete(root);
    root = NULL;

    /* ---- [3][4] 原子切换 + 旧数组退役 ---- */
    pthread_mutex_lock(&g.lock);          /* 独占 reload */
    pthread_rwlock_wrlock(&g.rwlock);     /* 阻塞所有读侧 */

    sip_profile_t *old_prof = g.prof;

    g.prof  = new_prof;
    g.count = (unsigned)loaded;

    pthread_rwlock_unlock(&g.rwlock);

    /* 旧数组进墓地（不 free：在途读者可能仍持有其内部指针） */
    retire_array(old_prof);
    unsigned retired_total = g.retired_n;

    pthread_mutex_unlock(&g.lock);

    llog(LOG_NOTICE, "sip-profile", "reload: loaded=%u (retired=%u)",
         loaded, retired_total);
    return NSBC_OK;

rollback:
    /* 失败：new_prof 从未发布，无读者，直接 free */
    for (int j = 0; j < loaded; ++j)
        sip_profile_unbind(&new_prof[j]);
    free(new_prof);
    if (root) cJSON_Delete(root);
    return NSBC_ERR;
}

/* ========================================================================
 * 公共字符串 CIDR API（兼容语义，供手工填充字段/溢出回退路径使用）
 * ======================================================================*/

/* RFC1918 私网网段（与 stub_mods.c 默认值 + db/mod.rs DEFAULT 保持一致） */
#define RFC1918_CIDR_STR "10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8"

/* §15 性能优化：RFC1918 私网快速判定
 * 热点路径（规则级 CIDR 检查默认就是 RFC1918）跳过 strtok_r + inet_pton + cidr_match_one
 * 直接 uint32 范围比较，一次 inet_pton 后 O(1) 判定 */
static inline int is_rfc1918_fast(const char *src_ip)
{
    struct in_addr a;
    if (inet_pton(AF_INET, src_ip, &a) != 1) return 0;  /* 非 IPv4 → 走慢路径 */
    uint32_t ip = ntohl(a.s_addr);
    /* 10.0.0.0/8 */
    if ((ip & 0xFF000000u) == 0x0A000000u) return 1;
    /* 172.16.0.0/12 (172.16.0.0 - 172.31.255.255) */
    if ((ip & 0xFFF00000u) == 0xAC100000u) return 1;
    /* 192.168.0.0/16 */
    if ((ip & 0xFFFF0000u) == 0xC0A80000u) return 1;
    /* 127.0.0.0/8 */
    if ((ip & 0xFF000000u) == 0x7F000000u) return 1;
    return 0;
}

int sip_ip_in_cidr_list(const char *cidr_list, const char *src_ip)
{
    if (!cidr_list || !*cidr_list || !src_ip || !*src_ip) return -1;

    /* §15 fast path：默认 RFC1918 私网列表 → O(1) 范围比较，跳过 strtok_r + inet_pton */
    if (strcmp(cidr_list, RFC1918_CIDR_STR) == 0) {
        return is_rfc1918_fast(src_ip) ? 1 : 0;
    }

    /* 复制一份避免修改原字符串 */
    char buf[1024];
    size_t len = strlen(cidr_list);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, cidr_list, len);
    buf[len] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        /* trim 前后空格 */
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t tlen = strlen(tok);
        while (tlen > 0 && (tok[tlen-1] == ' ' || tok[tlen-1] == '\t'))
            tok[--tlen] = '\0';
        if (*tok && cidr_match_one(tok, src_ip)) return 1;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return 0;  /* 没命中 */
}

int sip_profile_check_ip(const sip_profile_t *prof, const char *src_ip)
{
    if (!prof || !src_ip) return SIP_IP_ACCEPT;

    /* src_ip 一次性转 uint32（主机字节序）；非 IPv4 → 回退字符串路径 */
    struct in_addr a;
    int have_ip = (inet_pton(AF_INET, src_ip, &a) == 1);
    uint32_t ip = have_ip ? ntohl(a.s_addr) : 0;

    /* 1) 黑名单优先：命中 → silent drop（零响应防扫描） */
    if (prof->ip_blacklist[0] != '\0') {
        int hit;
        if (prof->bl_n > 0 && have_ip)
            hit = compiled_list_match(prof->bl, prof->bl_n, ip);
        else
            hit = sip_ip_in_cidr_list(prof->ip_blacklist, src_ip);
        if (hit == 1)
            return SIP_IP_DROP;
    }

    /* 2) 白名单：有值但未命中 → reject（可选回 403） */
    if (prof->ip_whitelist[0] != '\0') {
        int hit;
        if (prof->wl_n > 0 && have_ip)
            hit = compiled_list_match(prof->wl, prof->wl_n, ip);
        else
            hit = sip_ip_in_cidr_list(prof->ip_whitelist, src_ip);
        if (hit != 1)
            return SIP_IP_REJECT;
    }

    return SIP_IP_ACCEPT;
}

/* ========================================================================
 * 数据面多 profile 归属判定（sip_profile_match_src）
 *
 * 语义（两遍扫描，黑名单全域防御优先）：
 *   1) 任一 enabled+OK profile 的黑名单命中 → DROP（silent，零响应防扫描）
 *   2) 白名单命中某 profile → 归属该 profile（realm/DTMF 等行为参数适用）
 *   3) 均未命中但至少一个 profile 配了白名单 → REJECT（caller 回 403，
 *      "仅名单内 IP 放行"的防火墙语义）
 *   4) 无任何 ACL 配置 → NULL + ACCEPT（不改变现状）
 * 非 IPv4 源（v6 / WS 桥 AF_UNSPEC）→ 回退字符串路径 / 直接放行。
 * ======================================================================*/
sip_profile_t *sip_profile_match_src(const char *src_ip, int *verdict)
{
    if (verdict)
        *verdict = SIP_IP_ACCEPT;
    if (!src_ip || !*src_ip)
        return NULL;

    /* src_ip 一次性转 uint32（主机字节序）；非 IPv4 → 回退字符串路径 */
    struct in_addr a;
    int have_ip = (inet_pton(AF_INET, src_ip, &a) == 1);
    uint32_t ip = have_ip ? ntohl(a.s_addr) : 0;

    pthread_rwlock_rdlock(&g.rwlock);

    /* ---- 第一遍：黑名单全域扫描（防御性：跨 profile 生效，宁严勿松） ---- */
    for (unsigned i = 0; i < g.count; ++i) {
        sip_profile_t *p = &g.prof[i];
        if (p->state != SIP_PROFILE_OK || p->ip_blacklist[0] == '\0')
            continue;
        int hit = (p->bl_n > 0 && have_ip)
                    ? compiled_list_match(p->bl, p->bl_n, ip)
                    : sip_ip_in_cidr_list(p->ip_blacklist, src_ip);
        if (hit == 1) {
            pthread_rwlock_unlock(&g.rwlock);
            if (verdict)
                *verdict = SIP_IP_DROP;
            return NULL;
        }
    }

    /* ---- 第二遍：白名单归属（第一个命中的 profile 赢） ---- */
    sip_profile_t *found = NULL;
    int wl_configured = 0;
    for (unsigned i = 0; i < g.count; ++i) {
        sip_profile_t *p = &g.prof[i];
        if (p->state != SIP_PROFILE_OK || p->ip_whitelist[0] == '\0')
            continue;
        wl_configured = 1;
        int hit = (p->wl_n > 0 && have_ip)
                    ? compiled_list_match(p->wl, p->wl_n, ip)
                    : sip_ip_in_cidr_list(p->ip_whitelist, src_ip);
        if (hit == 1) {
            found = p;
            break;
        }
    }

    pthread_rwlock_unlock(&g.rwlock);

    if (!found && wl_configured && verdict)
        *verdict = SIP_IP_REJECT;
    return found;
}

/* 按 SIP 监听端口查找 profile（UDP/TCP 都查；返回第一个 enabled+OK 的）。
 * 多端口场景（external=5080 / internal=5060）下最高效——从 5080 进来的
 * 包直接归属 external profile。找不到返回 NULL。 */
sip_profile_t *sip_profile_match_port(uint16_t udp_port, uint16_t tcp_port)
{
    pthread_rwlock_rdlock(&g.rwlock);
    sip_profile_t *found = NULL;
    for (unsigned i = 0; i < g.count; ++i) {
        sip_profile_t *p = &g.prof[i];
        if (p->state != SIP_PROFILE_OK || !p->enabled) continue;
        if ((udp_port > 0 && p->enable_udp && p->sip_udp_port == udp_port) ||
            (tcp_port > 0 && p->enable_tcp && p->sip_tcp_port == tcp_port)) {
            found = p;
            break;
        }
    }
    pthread_rwlock_unlock(&g.rwlock);
    return found;
}
