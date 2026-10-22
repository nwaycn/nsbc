/* hep_sink.c — HEP3 实时推流实现（REQ-MEDIA-HEP-001 P0）
 *
 * 架构：
 *   media_bug 回调 ──▶ ring_push(frame_desc) ──▶ MPSC ring ──▶ hep_worker × N
 *                                                                  │
 *                                                                  ▼
 *                                                         HEP3 封装 + sendmmsg
 *                                                         失败 → drop + metrics
 *
 * 热路径约束：hep_sink_on_frame（media_bug 回调）必须无锁、
 * sendto/malloc-free——回调里只 memcpy + ring_push，worker 做重活。
 *
 * HEP3 头 network byte order（强制，错了 Homer 收不到）。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* sendmmsg + htobe64 需要 GNU 扩展 */
#endif

#include "hep_sink.h"
#include "media_bug.h"
#include "log.h"
#include "net_sock.h"

#include <arpa/inet.h>
#include <endian.h>   /* htobe64 */
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* ===== MPSC ring buffer（简化版，复用 mem_ring.c 的 seq 模式）===== */

/* 每格 frame_desc + seq 状态字；calloc 初始化 seq=0 */
typedef struct {
    uint32_t seq;                                     /* 0=空, pos+1+size=满 */
    uint8_t  frame[HEP3_MAX_FRAME_LEN];               /* 完整 HEP3 帧（chunks+PAYLOAD） */
    size_t   frame_len;                               /* 实际帧长度 */
    uint64_t timestamp_ns;
} hep_slot_t;

typedef struct {
    hep_slot_t *slots;
    uint32_t    mask;       /* size-1 */
    uint32_t    head;       /* consumer 读位置 */
    uint32_t    tail;       /* producer 写位置 */
} hep_ring_t;

static int ring_init(hep_ring_t *r, int size) {
    /* size 必须是 2 的幂——默认 8192 */
    r->slots = (hep_slot_t *)malloc((size_t)size * sizeof(hep_slot_t));
    if (!r->slots) return -1;
    r->mask = (uint32_t)size - 1;
    r->head = 0;
    r->tail = 0;
    /* 关键：每个 slot 的初始 seq 必须等于它的位置索引！
     * ring_push 检查 seq != t (t=tail & mask)，如果 seq 全是 0，
     * 第二次 push 时 t=1, seq=0 → 永远判定 ring full！ */
    for (int i = 0; i < size; ++i) {
        r->slots[i].seq = (uint32_t)i;
        r->slots[i].frame_len = 0;
    }
    return 0;
}

static void ring_free(hep_ring_t *r) {
    free(r->slots);
    r->slots = NULL;
}

/* 生产者：media_bug 回调调用。满返回 -1（调用方决定丢帧）。
 * MPSC 关键：多个 producer（不同 reactor 线程）可能哈希到同一 ring，
 * 必须用 atomic CAS 获取唯一写权限，避免 slot 覆盖。 */
static inline int ring_push(hep_ring_t *r, const hep_frame_t *f) {
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    for (;;) {
        hep_slot_t *slot = &r->slots[t & r->mask];
        uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        if (seq != t) return -1; /* ring full */
        /* CAS：确保只有一个 producer 拿到这个 slot 的写权限 */
        uint32_t expected_t = t;
        if (__atomic_compare_exchange_n(&r->tail, &expected_t, t + 1,
                                        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            /* memcpy 完整 HEP3 帧到 slot（热路径唯一内存复制） */
            memcpy(slot->frame, f->frame, f->frame_len);
            slot->frame_len = f->frame_len;
            slot->timestamp_ns = f->timestamp_ns;
            /* 发布：让 consumer 知道这个 slot 满了 */
            __atomic_store_n(&slot->seq, t + 1 + (r->mask + 1), __ATOMIC_RELEASE);
            return 0;
        }
        /* CAS 失败：别的 producer 抢了 slot，重新读 tail 重试 */
        t = expected_t;
    }
}

/* 消费者：worker 批量取，返回取到的数量 */
static inline int ring_pop_batch(hep_ring_t *r, hep_frame_t *out, int max) {
    int n = 0;
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    for (int i = 0; i < max; ++i) {
        hep_slot_t *slot = &r->slots[h & r->mask];
        uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        if (seq != h + 1 + (r->mask + 1)) break; /* slot 还没满 */
        memcpy(out[i].frame, slot->frame, slot->frame_len);
        out[i].frame_len = slot->frame_len;
        out[i].timestamp_ns = slot->timestamp_ns;
        /* slot 归位 */
        __atomic_store_n(&slot->seq, h + 1, __ATOMIC_RELEASE);
        ++h;
        ++n;
    }
    if (n > 0) __atomic_store_n(&r->head, h, __ATOMIC_RELEASE);
    return n;
}

/* ===== HEP3 真实 chunks 协议编码（network byte order，强制！）=====
 *
 * HEP3 帧格式：
 *   magic "HEP3" (4B, not null-terminated)
 *   total_len   (2B, htons) = 所有 chunks 的总长度（不含 magic+total_len 本身）
 *   chunks[]    — 每个 chunk:
 *     vendor_id    (2B, htons=0x0000 IANA default)
 *     type_id      (2B, htons, 见 hep_sink.h HEP3_CHUNK_*)
 *     chunk_len    (2B, htons, = data_len，不含 chunk 自己的 6B 头)
 *     data         (chunk_len B)
 *
 * IPv4 RTP 帧 = 11 个 chunks，header overhead ≈ 104B + payload
 */

/* 辅助：序列化一个 chunk，返回写入的字节数（6 + data_len） */
static inline size_t chunk_write(uint8_t *buf,
                                 uint16_t vendor_id,
                                 uint16_t type_id,
                                 const void *data,
                                 uint16_t data_len)
{
    uint16_t v_be = htons(vendor_id);
    uint16_t t_be = htons(type_id);
    uint16_t l_be = htons(data_len);
    memcpy(buf, &v_be, 2);
    memcpy(buf + 2, &t_be, 2);
    memcpy(buf + 4, &l_be, 2);
    if (data_len > 0) memcpy(buf + 6, data, data_len);
    return (size_t)(6 + data_len);
}

/* 主编码函数：序列化完整 HEP3 chunks 帧到 buf
 * 返回完整帧长度（magic + total_len + chunks + 内联 PAYLOAD chunk）
 * buf_size = 输出缓冲上限；编码前先预计算，不满足返回 0
 * （返回 0 = 帧太大装不下，调用方应丢弃） */
static inline size_t hep3_encode_chunks(
    uint8_t *buf,                 /* 输出缓冲 */
    size_t buf_size,              /* 缓冲上限，防止 overflow */
    uint32_t capture_id,
    uint8_t  ip_version,
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port,
    uint8_t  protocol,
    uint64_t ts_ns,
    uint8_t  payload_type,
    const uint8_t *payload, size_t payload_len,
    int include_payload_chunk)
{
    /* Step 1: 预计算 total_chunks_len，同时做边界检查 */
    size_t total_chunks_len = 0;
    total_chunks_len += 6 + 1;                            /* IP_VERSION */
    total_chunks_len += 6 + (ip_version == HEP3_IPV4 ? 4 : 16); /* SRC_IP */
    total_chunks_len += 6 + (ip_version == HEP3_IPV4 ? 4 : 16); /* DST_IP */
    total_chunks_len += 6 + 2;                            /* SRC_PORT */
    total_chunks_len += 6 + 2;                            /* DST_PORT */
    total_chunks_len += 6 + 1;                            /* PROTOCOL */
    total_chunks_len += 6 + 4;                            /* CAPTURE_ID */
    total_chunks_len += 6 + 8;                            /* TIME_SEC */
    total_chunks_len += 6 + 4;                            /* TIME_USEC */
    total_chunks_len += 6 + 1;                            /* PAYLOAD_TYPE */
    if (include_payload_chunk)
        total_chunks_len += 6 + payload_len;              /* PAYLOAD（内联） */

    /* 完整帧 = 6B 协议头（magic+total_len） + total_chunks_len */
    size_t frame_size = 6 + total_chunks_len;
    if (frame_size > buf_size) return 0;                  /* 帧太大 → 让调用方丢弃 */

    /* Step 2: 写 magic + total_len */
    uint8_t *p = buf;
    memcpy(p, "HEP3", 4); p += 4;
    uint16_t tl_be = htons((uint16_t)total_chunks_len);
    memcpy(p, &tl_be, 2); p += 2;

    /* Step 3: 逐 chunk 写入 */
    /* IP_VERSION */
    uint8_t ipv = ip_version;
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_IP_VERSION, &ipv, 1);

    /* SRC_IP / DST_IP（IPv4 仅） */
    uint32_t sip_be = htonl(src_ip);
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_SRC_IP, &sip_be, 4);
    uint32_t dip_be = htonl(dst_ip);
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_DST_IP, &dip_be, 4);

    /* SRC_PORT / DST_PORT */
    uint16_t sp_be = htons(src_port);
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_SRC_PORT, &sp_be, 2);
    uint16_t dp_be = htons(dst_port);
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_DST_PORT, &dp_be, 2);

    /* PROTOCOL */
    uint8_t proto = protocol;
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_PROTOCOL, &proto, 1);

    /* CAPTURE_ID */
    uint32_t cid_be = htonl(capture_id);
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_CAPTURE_ID, &cid_be, 4);

    /* TIME_SEC + TIME_USEC（拆 ns → sec + usec） */
    uint64_t sec_ns = 1000000000ULL;
    uint64_t sec = ts_ns / sec_ns;
    uint32_t usec = (uint32_t)((ts_ns % sec_ns) / 1000ULL);
    uint64_t sec_be = htobe64(sec);
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_TIME_SEC, &sec_be, 8);
    uint32_t usec_be = htonl(usec);
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_TIME_USEC, &usec_be, 4);

    /* PAYLOAD_TYPE */
    uint8_t pt = payload_type;
    p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_PAYLOAD_TYPE, &pt, 1);

    /* PAYLOAD（如果内联。当前实现是 header + 独立 payload 的 2-iov sendmmsg，
     * 所以这里不内联 payload chunk——payload 作为 UDP 包的第二段） */
    if (include_payload_chunk && payload_len > 0) {
        p += chunk_write(p, HEP3_VENDOR_IANA, HEP3_CHUNK_PAYLOAD,
                         payload, (uint16_t)payload_len);
    }

    return (size_t)(p - buf);  /* 完整 header 长度 */
}

/* ===== 全局状态 ===== */

#define HEP_MAX_WORKERS 4
#define HEP_MAX_RINGS   4     /* 每 worker 一个 ring（4×4=16 并发 producer 足够） */
#define HEP_MAX_RING_SIZE (1 << 13)  /* 8192 per ring：256B header + 1500B payload ≈ 1756B/slot × 8192 × 4 ≈ 56MB，合理 */

static hep_cfg_t   g_cfg;
static hep_ring_t  g_rings[HEP_MAX_RINGS];
static int         g_nrings;
static int         g_stop = 0;

/* worker 线程：从 ring 取帧 → sendmmsg */
static pthread_t   g_workers[HEP_MAX_WORKERS];
static int         g_nworkers;

/* metrics */
static volatile uint64_t g_frames_sent;
static volatile uint64_t g_frames_dropped_ring;
static volatile uint64_t g_frames_dropped_send;

/* ===== media_bug 回调（热路径！必须无锁无阻塞）===== */

/* frame 是栈上临时对象，必须立即 memcpy 到 slot */
void hep_sink_on_frame(media_bug_frame_t *frame, void *ud) {
    (void)ud;
    if (g_stop) return;

    uint32_t pt;
    switch (frame->type) {
        case BUG_RAW_RTP:      pt = HEP3_PT_RTP;  break;
        case BUG_RTCP_MIRROR:  pt = HEP3_PT_RTCP; break;
        case BUG_SIPCAPTURE_TAP: pt = HEP3_PT_SIP; break;
        default: return;
    }

    /* 选 ring：按 frame->call_key 哈希（b2b_call_t* 指针值） */
    uintptr_t h = (uintptr_t)frame->call_key;
    uint32_t ri = (uint32_t)(h & (g_nrings - 1));

    /* 构造 hep_frame_t（栈上），然后 ring_push */
    hep_frame_t f;
    uint16_t sp = (uint16_t)frame->src_port;
    uint16_t dp = (uint16_t)frame->dst_port;

    /* 真实 HEP3 chunks 协议编码：完整帧（magic+total_len+10 chunks+内联 PAYLOAD）
     * 编码前先预计算 total_chunks_len，超过 buf_size 返回 0 → 丢帧（不截断） */
    f.frame_len = hep3_encode_chunks(
        f.frame, HEP3_MAX_FRAME_LEN,
        g_cfg.capture_id,
        HEP3_IPV4,
        frame->src_ip, sp,
        frame->dst_ip, dp,
        HEP3_UDP,
        frame->timestamp_ns,
        (uint8_t)pt,
        frame->data, frame->data_len,
        1);

    if (f.frame_len == 0) {
        /* 帧太大（通常 payload_len 超过 MTU 限制）→ 丢弃 */
        __atomic_fetch_add(&g_frames_dropped_ring, 1, __ATOMIC_RELAXED);
        return;
    }
    f.timestamp_ns = frame->timestamp_ns;

    if (ring_push(&g_rings[ri], &f) != 0) {
        __atomic_fetch_add(&g_frames_dropped_ring, 1, __ATOMIC_RELAXED);
    }
}

/* ===== worker：从 ring 批量取 → sendmmsg ===== */

static void *hep_worker(void *arg) {
    int ring_idx = (int)(intptr_t)arg;
    hep_ring_t *ring = &g_rings[ring_idx];

    /* 创建 UDP socket（每 worker 独立 socket） */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_err("hep", "worker %d socket failed errno=%d", ring_idx, errno);
        return NULL;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, g_cfg.server_host, &dst.sin_addr);
    dst.sin_port = htons(g_cfg.server_port);

    /* 堆分配 batch（64 × HEP3_MAX_FRAME_LEN × 字节）——避免栈溢出 */
    hep_frame_t *batch = (hep_frame_t *)calloc(64, sizeof(hep_frame_t));
    if (!batch) {
        log_err("hep", "worker %d batch alloc failed", ring_idx);
        close(fd);
        return NULL;
    }

    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        int n = ring_pop_batch(ring, batch, 64);
        if (n == 0) {
            usleep(100);
            continue;
        }

        int sent = 0;
        for (int i = 0; i < n; ++i) {
            ssize_t st = sendto(fd, batch[i].frame, batch[i].frame_len, 0,
                                (struct sockaddr *)&dst, sizeof(dst));
            if (st < 0) {
                __atomic_fetch_add(&g_frames_dropped_send, 1, __ATOMIC_RELAXED);
            } else {
                ++sent;
            }
        }
        __atomic_fetch_add(&g_frames_sent, (uint64_t)sent, __ATOMIC_RELAXED);
    }
    free(batch);
    close(fd);
    return NULL;
}

/* ===== 生命周期 ===== */

int hep_sink_create(const hep_cfg_t *cfg) {
    if (!cfg || cfg->server_host[0] == '\0' || cfg->capture_id == 0) return -1;
    g_cfg = *cfg;

    /* 决定 ring 数 = min(socket_pool, HEP_MAX_RINGS, 物理核) */
    g_nrings = cfg->socket_pool > 0 ? cfg->socket_pool : 8;
    if (g_nrings > HEP_MAX_RINGS) g_nrings = HEP_MAX_RINGS;
    if (g_nrings < 1) g_nrings = 1;

    int ring_size = cfg->ring_size > 0 ? cfg->ring_size : HEP_MAX_RING_SIZE;
    /* ring_size 必须 2 的幂 */
    int rs = 1;
    while (rs < ring_size) rs <<= 1;
    ring_size = rs;

    for (int i = 0; i < g_nrings; ++i) {
        if (ring_init(&g_rings[i], ring_size) != 0) {
            log_err("hep", "ring[%d] init failed", i);
            hep_sink_destroy();
            return -1;
        }
    }

    /* 注册为 media_bug 全局消费者 */
    static media_bug_consumer_t consumer;
    strncpy(consumer.name, "hep_sink", sizeof(consumer.name) - 1);
    consumer.type_mask = BUG_RAW_RTP | BUG_RTCP_MIRROR | BUG_SIPCAPTURE_TAP;
    consumer.cb = hep_sink_on_frame;
    consumer.ud = NULL;
    if (media_bug_register_global_consumer(&consumer) != 0) {
        log_err("hep", "register media_bug consumer failed");
        hep_sink_destroy();
        return -1;
    }

    /* 启动 worker 线程 */
    g_stop = 0;
    for (int i = 0; i < g_nrings; ++i) {
        pthread_create(&g_workers[i], NULL, hep_worker, (void *)(intptr_t)i);
    }
    g_nworkers = g_nrings;
    log_info("hep", "sink started → %s:%u capture_id=%u workers=%d",
             cfg->server_host, cfg->server_port, cfg->capture_id, g_nworkers);
    return 0;
}

void hep_sink_destroy(void) {
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < g_nworkers; ++i) {
        pthread_join(g_workers[i], NULL);
    }
    g_nworkers = 0;
    for (int i = 0; i < g_nrings; ++i) {
        ring_free(&g_rings[i]);
    }
    g_nrings = 0;
    log_info("hep", "sink stopped");
}

/* ===== OAM 指标 ===== */

uint64_t hep_frames_sent(void) { return __atomic_load_n(&g_frames_sent, __ATOMIC_RELAXED); }
uint64_t hep_frames_dropped(const char *reason) {
    if (!reason) return 0;
    if (strcmp(reason, "ring_full") == 0)
        return __atomic_load_n(&g_frames_dropped_ring, __ATOMIC_RELAXED);
    if (strcmp(reason, "send_error") == 0)
        return __atomic_load_n(&g_frames_dropped_send, __ATOMIC_RELAXED);
    return 0;
}
uint64_t hep_backlog_depth(void) {
    uint64_t total = 0;
    for (int i = 0; i < g_nrings; ++i) {
        uint32_t t = __atomic_load_n(&g_rings[i].tail, __ATOMIC_RELAXED);
        uint32_t h = __atomic_load_n(&g_rings[i].head, __ATOMIC_RELAXED);
        total += t - h;
    }
    return total;
}

/* ===== OAM 命令族 ===== */

int hep_cmd_status(char *out, size_t outsz) {
    return snprintf(out, outsz,
        "hep server=%s:%u capture_id=%u workers=%d stopped=%d\n"
        "frames_sent=%lu dropped_ring_full=%lu dropped_send_error=%lu backlog=%lu\n",
        g_cfg.server_host, g_cfg.server_port, g_cfg.capture_id,
        g_nworkers, __atomic_load_n(&g_stop, __ATOMIC_RELAXED),
        (unsigned long)hep_frames_sent(),
        (unsigned long)hep_frames_dropped("ring_full"),
        (unsigned long)hep_frames_dropped("send_error"),
        (unsigned long)hep_backlog_depth());
}

int hep_cmd_flush(char *out, size_t outsz) {
    /* P0：worker 已经在正常消费，flush 等同于等一小段时间 */
    usleep(10000);
    return snprintf(out, outsz, "hep backlog=%lu (flushed)\n",
                    (unsigned long)hep_backlog_depth());
}

int hep_cmd_reload(char *out, size_t outsz) {
    /* P0：不重建 socket（太复杂），只打印 "reload not supported P0" */
    return snprintf(out, outsz, "hep reload: P0 not supported — restart nsbc to change config\n");
}

/* ===== 模块 init / fini（REQ-ARCH-010）===== */

#include "cfg.h"
#include "app_ctx.h"

static int hep_init(struct nsbc_ctx *ctx) {
    if (!ctx) return NSBC_INVAL;

    hep_cfg_t cfg = {{0}, 0, 0, 0, 0, 0, 0, 0};
    cfg.server_port = 9060;  /* Homer 默认端口 */
    cfg.capture_id  = 0;     /* 0 = 未配置，禁用 */
    cfg.flush_batch = 32;
    cfg.flush_interval_ms = 5;
    cfg.socket_pool = 4;      /* 默认 4 worker（约 = CPU 核数 / 2） */
    cfg.ring_size = 8192;     /* 默认 8k per ring（65536 太大 → 935MB，8192×4≈56MB） */

    if (ctx->cfg) {
        const char *s;
        long lv;
        if (cfg_get_str(ctx->cfg, "hep.server", &s, NULL) == 0 && s) {
            snprintf(cfg.server_host, sizeof(cfg.server_host), "%s", s);
        }
        if (cfg_get_int(ctx->cfg, "hep.port", &lv, NULL) == 0 && lv > 0 && lv < 65536) {
            cfg.server_port = (uint16_t)lv;
        }
        if (cfg_get_int(ctx->cfg, "hep.capture_id", &lv, NULL) == 0 && lv > 0) {
            cfg.capture_id = (uint32_t)lv;
        }
        if (cfg_get_int(ctx->cfg, "hep.ring_size", &lv, NULL) == 0 && lv > 0) {
            cfg.ring_size = (int)lv;
        }
        if (cfg_get_int(ctx->cfg, "hep.socket_pool", &lv, NULL) == 0 && lv > 0 && lv <= 8) {
            cfg.socket_pool = (int)lv;
        }
    }

    if (cfg.capture_id == 0 || !cfg.server_host[0]) {
        log_info("hep", "sink not configured (hep.server / hep.capture_id missing) — disabled");
        return NSBC_OK;  /* 配置缺失不算 init 失败，skip */
    }

    int rc = hep_sink_create(&cfg);
    if (rc != 0) {
        log_warn("hep", "sink create failed rc=%d — hep push disabled", rc);
        return NSBC_OK;  /* hep 是可选功能，失败不阻塞启动 */
    }
    return NSBC_OK;
}

static void hep_fini(struct nsbc_ctx *ctx) {
    (void)ctx;
    hep_sink_destroy();
}

#include "module.h"
NSBC_MOD0_CAT(hep, 1, MOD_L_WORKER_READY, MOD_F_CALL_PATH, MOD_CAT_MEDIA_BUG, hep_init, hep_fini);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
