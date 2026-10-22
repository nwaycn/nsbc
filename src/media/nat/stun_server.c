/* NSBC STUN Server — RFC 5389  Binding Request/Response
 *
 * 最小实现（Phase 1）：
 *   - UDP bind (默认 :3478) → pthread recvfrom loop
 *   - Binding Request (method=1, class=0) → Binding Success Response (0x0101)
 *   - 返回 MAPPED-ADDRESS (legacy) + XOR-MAPPED-ADDRESS (RFC 5389 推荐)
 *   - FINGERPRINT (CRC32 ^ "STUN") — 请求带就回带
 *   - SOFTWARE (NSBC/<ver>)
 *
 * 复用 src/include/stun_msg.h：stun_parse(), stun_attr_find()
 * 自己实现：Binding Success Response 构造 + CRC32
 *
 * 线程模型：单 pthread recvfrom loop，每包无状态处理
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "stun_server.h"
#include "stun_msg.h"
#include "log.h"
#include "nsbc_common.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#ifndef NSBC_VERSION
#define NSBC_VERSION "2.6.0"
#endif

/* ==== RFC 5389 响应 class/method ==== */
#define STUN_CLASS_SUCCESS   0x10   /* bit4 = C0 */
#define STUN_FINGERPRINT_XOR 0x5354554eu   /* "STUN" */

/* ==== CRC32 (IEEE 802.3) — stun_msg.c 没有，自己做 ==== */
static uint32_t stun_crc32(const uint8_t *data, size_t len)
{
    static uint32_t tbl[256];
    static int inited = 0;
    if (!inited) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        inited = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = tbl[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ==== 大端读写 ==== */
static void w16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xff); }
static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v & 0xff);
}

/* ==== Attribute 追加（自动 4B 对齐填充）==== */
static uint8_t *attr_put(uint8_t *buf, uint16_t type, const void *val, uint16_t len)
{
    w16(buf + 0, type);
    w16(buf + 2, len);
    memcpy(buf + 4, val, len);
    uint16_t pad = (4 - (len & 3)) & 3;
    if (pad) memset(buf + 4 + len, 0, pad);
    return buf + 4 + len + pad;
}

/* ==== 构造 Binding Success Response ====
 *
 * 固定属性序列：MAPPED-ADDRESS → XOR-MAPPED-ADDRESS → SOFTWARE → [FINGERPRINT]
 *
 * 如果 need_fp=1，FINGERPRINT 放在最后（必须），length 字段包含 FINGERPRINT 长度
 */
static int build_binding_success(
    const uint8_t tid[STUN_TID_LEN],
    const struct sockaddr_in *cli,     /* 客户端 IPv4 地址 */
    const char *sw_str,
    int  need_fp,                       /* 请求是否带 FINGERPRINT */
    uint8_t *out, size_t out_cap)
{
    if (out_cap < STUN_HDR_LEN + 256) return NSBC_INVAL;

    uint8_t *hdr = out;
    /* type = (class=SUCCESS << 4) | method=BINDING
     * stun_msg.h STUN_C_SUCCESS = 0x10, STUN_M_BINDING = 0x001
     * 实际 type field 格式：RFC 5389 §6
     *   bits 0-3  : method (低)
     *   bits 4    : class C0
     *   bits 5-7  : method (中)
     *   bit 8     : class C1
     *   bits 9-11 : method (高)
     * Binding = 0x0001, Success = C0=1 C1=0 → type = (1<<4) | 1 = 0x0011
     */
    uint16_t resp_type = (STUN_CLASS_SUCCESS << 0) | STUN_M_BINDING;
    /* 但 stun_msg.h 里 STUN_CLASS_SUCCESS = 0x10（bit4=1），直接用 */
    resp_type = (STUN_C_SUCCESS << 4) | STUN_M_BINDING;
    /* 验证：RFC 5389 Table 2 — Binding Success Response type field = 0x0101 */
    /* 啊不对 type field 是 method<<2 | class：RFC 5389 §6 */
    /* 让我再确认：class 在 bit 4 (C0) 和 bit 8 (C1)，method 散在 bit 0-3, 5-7, 9-11 */
    /* Binding method = 0x0001 → bits 0-3 = 0001, bits 5-7 = 000, bits 9-11 = 000 */
    /* Success class  = C0=1, C1=0 → bit4=1, bit8=0 */
    /* type = 0x0001 | (1<<4) = 0x0011  */
    /* 不对 RFC 5389 §6 type field bit layout:
     *   15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0
     *   R  M11 M10 M9  C1  M8 M7 M6 R M5 M4 C0 M3 M2 M1 M0
     * Binding = 000 000000 0001 → bits 0,3,4,7,8,11,12,13,14,15 all 0 除了 0
     * 即 method = 0x0001 放在 bits 0-3
     * Success = C0=1 (bit4), C1=0 (bit8)
     * type = 0x0101 ? 让我数：
     *   bit0=1(M0), bit4=1(C0), 其他0 → 0b0000000100000001 = 0x0101
     */
    resp_type = 0x0101;
    w16(hdr + 0, resp_type);
    w16(hdr + 2, 0);                 /* length 先占位，后面回填 */
    w32(hdr + 4, STUN_COOKIE);
    memcpy(hdr + 8, tid, STUN_TID_LEN);

    uint8_t *p = hdr + STUN_HDR_LEN;

    /* ---- MAPPED-ADDRESS (legacy, RFC 3489) ----
     * 1B reserved + 1B family + 2B port + 4B addr = 8B
     */
    uint8_t maddr[8];
    maddr[0] = 0;
    maddr[1] = 0x01;                      /* family IPv4 */
    w16(maddr + 2, cli->sin_port);         /* 网络序已由 recvfrom 填充 */
    memcpy(maddr + 4, &cli->sin_addr, 4);
    p = attr_put(p, STUN_A_MAPPED_ADDR, maddr, sizeof(maddr));

    /* ---- XOR-MAPPED-ADDRESS (RFC 5389 推荐) ----
     * port XOR (cookie 高 2B)，addr XOR cookie（IPv4 只用 cookie 4B）
     */
    uint8_t xor_addr[8];
    xor_addr[0] = 0;
    xor_addr[1] = 0x01;
    uint16_t xport = (cli->sin_port ^ (uint16_t)(STUN_COOKIE >> 16));
    w16(xor_addr + 2, xport);
    const uint8_t *ip = (const uint8_t *)&cli->sin_addr;
    uint32_t mc_val = STUN_COOKIE;
    const uint8_t *mc = (const uint8_t *)&mc_val;
    for (int i = 0; i < 4; i++)
        xor_addr[4 + i] = ip[i] ^ mc[i];
    p = attr_put(p, STUN_A_XOR_MAPPED_ADDR, xor_addr, sizeof(xor_addr));

    /* ---- SOFTWARE ---- */
    size_t swlen = strlen(sw_str);
    if (swlen > 64) swlen = 64;
    p = attr_put(p, STUN_A_SOFTWARE, sw_str, (uint16_t)swlen);

    /* ---- 回填 length：body 总字节数（不含 header）
     * 如果 need_fp，length 额外包含 FINGERPRINT 属性 8B
     */
    size_t body_len_before_fp = (size_t)(p - hdr) - STUN_HDR_LEN;
    uint16_t final_len = (uint16_t)body_len_before_fp;

    if (need_fp) {
        final_len += 8;   /* FINGERPRINT: 4B header + 4B value */
        w16(hdr + 2, final_len);

        /* CRC32 覆盖 hdr 到 body_end（不含 FINGERPRINT 本身） */
        uint32_t crc = stun_crc32(hdr, (size_t)(p - hdr)) ^ STUN_FINGERPRINT_XOR;
        w16(p + 0, STUN_A_FINGERPRINT);
        w16(p + 2, 4);
        w32(p + 4, crc);
        p += 8;
    } else {
        w16(hdr + 2, final_len);
    }

    return (int)(p - hdr);
}

/* ==== 线程状态 ==== */
typedef struct {
    pthread_t  thread;
    int        started;
    int        stop_flag;
    int        fd;
    uint16_t   port;
    int        enabled;
    uint32_t   req_count;
} stun_state_t;

static stun_state_t g_stun = { .fd = -1 };

/* ==== recvfrom 主循环 ==== */
static void *stun_server_thread(void *ud)
{
    (void)ud;
    char sw_version[64];
    snprintf(sw_version, sizeof(sw_version), "NSBC/%s", NSBC_VERSION);

    static const int RECV_SZ = 2048;
    static const int SEND_SZ = 2048;
    uint8_t *rcv = (uint8_t *)alloca(RECV_SZ);
    uint8_t *snd = (uint8_t *)alloca(SEND_SZ);

    struct sockaddr_in cli;
    socklen_t cli_len;

    llog(LOG_INFO, "stun", "server thread started, UDP :%u", g_stun.port);

    for (;;) {
        if (g_stun.stop_flag) break;
        cli_len = sizeof(cli);
        /* SO_RCVTIMEO=100ms：定期检查 stop_flag，避免 close→join 间 fd 被复用导致 recvfrom 永久阻塞 */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
        setsockopt(g_stun.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = recvfrom(g_stun.fd, rcv, RECV_SZ, 0,
                             (struct sockaddr *)&cli, &cli_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;   /* 超时或信号中断 → 回到顶部检查 stop_flag */
            continue;
        }
        if (n < STUN_HDR_LEN) continue;

        /* 用 stun_msg.h 解析 */
        stun_msg_t msg;
        if (stun_parse(rcv, (size_t)n, &msg) != NSBC_OK) continue;

        /* 只处理 Binding Request */
        if (stun_class(&msg) != STUN_C_REQUEST) continue;
        if (stun_method(&msg) != STUN_M_BINDING) continue;

        /* 检查请求里有没有 FINGERPRINT */
        int need_fp = 0;
        stun_attr_t fp_attr;
        if (stun_attr_find(&msg, STUN_A_FINGERPRINT, &fp_attr))
            need_fp = 1;

        /* 构造响应（TID 原样复制） */
        int resp_len = build_binding_success(
            msg.tid, &cli, sw_version, need_fp,
            snd, SEND_SZ);
        if (resp_len <= 0) continue;

        sendto(g_stun.fd, snd, (size_t)resp_len, MSG_NOSIGNAL,
               (struct sockaddr *)&cli, sizeof(cli));

        g_stun.req_count++;
        if ((g_stun.req_count % 1000) == 0)
            llog(LOG_DEBUG, "stun", "binding_req count=%u", g_stun.req_count);
    }

    llog(LOG_INFO, "stun", "server thread stopped, total_req=%u", g_stun.req_count);
    return NULL;
}

/* ==== 公开 API ==== */

int stun_server_init(uint16_t port, int enabled)
{
    g_stun.port = port;
    g_stun.enabled = enabled;
    g_stun.fd = -1;
    g_stun.req_count = 0;
    return NSBC_OK;
}

int stun_server_start(void)
{
    if (!g_stun.enabled) return NSBC_OK;
    if (g_stun.started) return NSBC_OK;

    g_stun.fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (g_stun.fd < 0) {
        llog(LOG_ERR, "stun", "socket() failed errno=%d", errno);
        return NSBC_ERR;
    }
    int one = 1;
    setsockopt(g_stun.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    srv_addr.sin_port = htons(g_stun.port);

    if (bind(g_stun.fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        llog(LOG_ERR, "stun", "bind :%u failed errno=%d (address in use?)",
             g_stun.port, errno);
        close(g_stun.fd); g_stun.fd = -1;
        return NSBC_ERR;
    }

    g_stun.stop_flag = 0;
    int rc = pthread_create(&g_stun.thread, NULL, stun_server_thread, NULL);
    if (rc != 0) {
        llog(LOG_ERR, "stun", "pthread_create failed rc=%d", rc);
        close(g_stun.fd); g_stun.fd = -1;
        return NSBC_ERR;
    }

    g_stun.started = 1;
    llog(LOG_NOTICE, "stun", "server bound :%u/UDP", g_stun.port);
    return NSBC_OK;
}

void stun_server_stop(void)
{
    if (!g_stun.started) return;
    g_stun.stop_flag = 1;
    if (g_stun.fd >= 0) close(g_stun.fd);   /* 打断阻塞的 recvfrom */
    g_stun.fd = -1;
    pthread_join(g_stun.thread, NULL);
    g_stun.started = 0;
}

void stun_server_shutdown(void)
{
    stun_server_stop();
}
