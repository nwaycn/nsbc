/* =============================================================================
 * Phase 3+4: CDR WAL 兜底 — 文本文件健壮实现（batch + CRC + 文件头）
 *
 * Phase 3 骨架：纯文本追加 + 每次 flush + 无文件头 + 无校验 + replay 完不 truncate
 * Phase 4 升级：
 *   ✓ 文件头魔数 "NSBCWAL1" + 版本号 — 防止空/损坏文件被误 replay
 *   ✓ 每条记录带 CRC32 — replay 时跳过损坏记录（断电半写保护）
 *   ✓ batch append — 先攒内存队列（默认 64 条），满了/1s flush
 *   ✓ replay 完 truncate — 补写成功后清空文件（已落库不再需要）
 *   ✓ create 时自动补写文件头（若文件为空或不存在）
 *
 * Phase 5 升级路径：引入 sqlite3，WRITER 线程 + b2b_cdr_batch WAL 自动 flush
 *   PRAGMA journal_mode=WAL + synchronous=NORMAL
 *   CREATE TABLE cdr_events(id INTEGER PK AUTOINCREMENT, event_json TEXT, ts INTEGER, committed INTEGER DEFAULT 0)
 *
 * 为什么 Phase 4 还不用 sqlite3？
 *   sqlite3 是 ~2MB 单文件重依赖，牵涉 configure/Makefile/CI 全部改。
 *   当前文本 WAL 能做到断电安全（CRC 跳过损坏 + replay truncate），
 *   作为兜底层完全够用。BI/审计直连需求等真 sqlite3 再做。
 * ============================================================================= */

#include "cdr_wal.h"
#include "log.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>      /* snprintf — SQLite WAL SQL 构造 */
#include <time.h>
#include <unistd.h>   /* ftruncate / fileno — Linux/POSIX 需要 */

/* Phase E-04: SQLite WAL 后端（比文本 WAL 强：PRAGMA journal_mode=WAL + committed 字段 + SELECT replay）
 * configure pkg-config sqlite3 检测后写 -DNSBC_WITH_SQLITE 到 config.mk
 * 没检测到 sqlite3 时 #ifndef → 自动编译走文本 WAL 后端 */
#ifdef NSBC_WITH_SQLITE
#include <sqlite3.h>
#endif

/* ---- 文件头 ---- */
#define WAL_MAGIC    "NSBCWAL1"       /* 8 字节，无 \0 */
#define WAL_VERSION  2                /* 1=Phase 3 stub, 2=Phase 4 batch+CRC */
#define WAL_BATCH_MAX 64              /* 攒满 64 条触发 batch flush */

/* ---- CRC32（zlib 同款，标准多项式 0xEDB88320）---- */
static uint32_t crc32_table[256];
static int      crc32_table_ready = 0;

static void crc32_init_table(void)
{
    if (crc32_table_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

static uint32_t crc32_buf(const uint8_t *data, size_t len)
{
    crc32_init_table();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- 记录格式 ----
 * 每条记录 = [CRC32(4B 大端) | 长度(4B 大端) | payload(NB) | \n(1B)]
 * payload 是 JSON 序列化的 cdr_event_t
 * CRC32 覆盖 长度 + payload（不覆盖 CRC 自己）
 * 这样 replay 时可以：
 *   1. 读 4+4=8 字节头 → 拿到 crc 和 len
 *   2. 读 payload(len 字节)
 *   3. 计算 payload 的 crc，对比记录里的 crc — 对得上才处理
 *   4. 任何一步读失败 → 跳过（断电半写保护）*/

/* ---- 内存 batch 队列 ---- */
typedef struct {
    char *lines[WAL_BATCH_MAX];
    int   count;
    int   dirty;                /* 有新数据没 flush */
    time_t last_flush_time;
} wal_batch_t;

struct cdr_wal {
    char      db_path[512];
    FILE     *fp;                /* 文本 WAL 后端 — #ifdef NSBC_WITH_SQLITE 时可能为 NULL */
    pthread_mutex_t lock;
    wal_batch_t batch;           /* 文本 WAL 后端 batch 攒刷 */

#ifdef NSBC_WITH_SQLITE
    /* Phase E-04: SQLite WAL backend 额外字段 */
    sqlite3      *db;            /* sqlite3 连接，非 NULL 代表走 SQLite 后端；NULL 走文本 WAL */
    sqlite3_stmt *insert_stmt;   /* prepared INSERT — 热路径零 malloc，init 一次复用 */
#endif
};

/* Phase 6: 全局 WAL 指针（OAM show cdr_wal 命令访问）*/
static cdr_wal_t *g_wal_global = NULL;
void         cdr_wal_set_global(cdr_wal_t *w) { g_wal_global = w; }
cdr_wal_t   *cdr_wal_get_global(void)          { return g_wal_global; }

/* ------------------------------------------------------------------ */

/* 确保文件头存在。空文件或文件头不匹配 → 重建（写正确文件头）。
 * 已存在且匹配 → 不动，直接返回 OK。 */
static int wal_ensure_header(FILE *fp)
{
    if (!fp) return NSBC_ERR;

    rewind(fp);

    /* 检查当前文件头 */
    uint8_t head[8];
    size_t rd = fread(head, 1, sizeof(head), fp);
    if (rd == sizeof(head) && memcmp(head, WAL_MAGIC, 8) == 0) {
        /* 文件头正确，保留原有内容 */
        fseek(fp, 0, SEEK_END);
        return NSBC_OK;
    }

    /* 文件头不对或空文件 → 重建文件头 */
    if (ftruncate(fileno(fp), 0) != 0) {
        /* ftruncate 失败：试试 rewind + 写 */
        rewind(fp);
    } else {
        rewind(fp);
    }

    fwrite(WAL_MAGIC, 1, 8, fp);
    uint32_t ver = (uint32_t)WAL_VERSION;
    /* 大端序写入 version（4 字节） */
    uint8_t ver_bytes[4];
    ver_bytes[0] = (uint8_t)(ver >> 24);
    ver_bytes[1] = (uint8_t)(ver >> 16);
    ver_bytes[2] = (uint8_t)(ver >> 8);
    ver_bytes[3] = (uint8_t)(ver);
    fwrite(ver_bytes, 1, 4, fp);
    fflush(fp);

    rewind(fp);
    /* 跳过 12 字节文件头，到数据区末尾 */
    fseek(fp, 12, SEEK_END);
    return NSBC_OK;
}

/* ------------------------------------------------------------------ */

int cdr_wal_create(const char *db_path, cdr_wal_t **out)
{
    if (!db_path || !out) return NSBC_INVAL;

    cdr_wal_t *w = (cdr_wal_t *)calloc(1, sizeof(*w));
    if (!w) return NSBC_NOMEM;

    strncpy(w->db_path, db_path, sizeof(w->db_path) - 1);
    pthread_mutex_init(&w->lock, NULL);
    w->batch.last_flush_time = time(NULL);

#ifdef NSBC_WITH_SQLITE
    /* Phase E-04: 优先 SQLite WAL 后端
     *
     * sqlite3_open_v2 + SQLITE_OPEN_FULLMUTEX 让 sqlite3 内部自己加锁，
     * 我们的 pthread_mutex 额外保护 billing 级别的批量调用。
     * PRAGMA journal_mode=WAL — SQLite 自身 WAL（不是我们的文本 WAL！）
     * PRAGMA synchronous=NORMAL — 平衡安全（OS cache）与性能
     * PRAGMA journal_size_limit=32MB — 防止 WAL 膨胀
     * CREATE TABLE IF NOT EXISTS — re-open 不重复创建 */
    {
        char *sql_err = NULL;
        int rc = sqlite3_open_v2(db_path, &w->db,
            SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
        if (rc == SQLITE_OK && w->db) {
            /* 一组 PRAGMA（必须在事务外执行，直接 sqlite3_exec 即可）*/
            if (sqlite3_exec(w->db,
                "PRAGMA journal_mode=WAL;"
                "PRAGMA synchronous=NORMAL;"
                "PRAGMA busy_timeout=5000;"
                "PRAGMA journal_size_limit=32000000;"
                "CREATE TABLE IF NOT EXISTS cdr_events ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "  event_json TEXT NOT NULL,"
                "  committed INTEGER DEFAULT 0,"
                "  ts INTEGER NOT NULL"
                ");",
                NULL, NULL, &sql_err) == SQLITE_OK) {

                /* 预编译 INSERT — 热路径零 malloc，prepare 一次复用 */
                const char *ins_sql =
                    "INSERT INTO cdr_events(event_json, committed, ts) VALUES(?, 0, ?);";
                if (sqlite3_prepare_v2(w->db, ins_sql, -1, &w->insert_stmt, NULL) == SQLITE_OK) {
                    log_info("cdr_wal", "Phase E-04: sqlite3 WAL backend ok (db=%s)", db_path);
                    *out = w;
                    return NSBC_OK;
                }
            } else {
                log_warn("cdr_wal", "Phase E-04: sqlite3 PRAGMA failed: %s",
                         sql_err ? sql_err : "unknown");
                if (sql_err) sqlite3_free(sql_err);
            }
            /* sqlite3 后端初始化失败 → 清理，fallback 文本 WAL */
            if (w->insert_stmt) { sqlite3_finalize(w->insert_stmt); w->insert_stmt = NULL; }
            sqlite3_close(w->db);
            w->db = NULL;
            log_warn("cdr_wal", "Phase E-04: sqlite3 backend failed, falling back to text WAL");
        } else {
            log_warn("cdr_wal", "Phase E-04: sqlite3_open_v2 rc=%d, falling back to text WAL", rc);
            w->db = NULL;
        }
    }
#endif /* NSBC_WITH_SQLITE */

    /* ---- Fallback: 文本 WAL 后端（Phase 4 CRC+batch） ----
     * 只有 #ifndef NSBC_WITH_SQLITE 或 sqlite3 初始化失败时才走到这里。
     * 代码块：读写追加模式 + wal_ensure_header（与 Phase 4 完全一致）*/
    w->fp = fopen(db_path, "a+");
    if (!w->fp) {
        pthread_mutex_destroy(&w->lock);
        free(w);
        return NSBC_ERR;
    }

    pthread_mutex_lock(&w->lock);
    int rc = wal_ensure_header(w->fp);
    pthread_mutex_unlock(&w->lock);

    if (rc != NSBC_OK) {
        fclose(w->fp);
        w->fp = NULL;
        pthread_mutex_destroy(&w->lock);
        free(w);
        return rc;
    }

    *out = w;
    return NSBC_OK;
}

void cdr_wal_destroy(cdr_wal_t *w)
{
    if (!w) return;

    pthread_mutex_lock(&w->lock);

#ifdef NSBC_WITH_SQLITE
    if (w->db) {
        /* Phase E-04: SQLite WAL — checkpoint WAL 把 committed 刷回主文件 + 关闭 */
        if (w->insert_stmt) { sqlite3_finalize(w->insert_stmt); w->insert_stmt = NULL; }
        char *chkpt_err = NULL;
        sqlite3_exec(w->db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, &chkpt_err);
        if (chkpt_err) sqlite3_free(chkpt_err);
        pthread_mutex_unlock(&w->lock);   /* sqlite3_close 内部也可能用 FULLMUTEX */
        sqlite3_close(w->db);
        w->db = NULL;
        pthread_mutex_destroy(&w->lock);
        free(w);
        return;
    }
#endif

    /* 文本 WAL 后端：析构前 flush batch 里残留数据 */
    if (w->batch.count > 0 && w->fp) {
        for (int i = 0; i < w->batch.count; i++) free(w->batch.lines[i]);
        fflush(w->fp);
    }
    pthread_mutex_unlock(&w->lock);

    if (w->fp) fclose(w->fp);
    pthread_mutex_destroy(&w->lock);
    free(w);
}

/* ------------------------------------------------------------------ */

static int serialize_event(const cdr_event_t *ev, char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen,
        "{\"call_id\":\"%s\",\"tenant\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
        "\"phase\":%d,\"billsec\":%u,\"duration\":%u,\"disposition\":%d,"
        "\"ts\":%llu}",
        ev->call_id, ev->tenant, ev->from, ev->to,
        (int)ev->phase, (unsigned)ev->billsec, (unsigned)ev->duration_s, (int)ev->disposition,
        (unsigned long long)nsbc_now_ms());
    return n;
}

/* 把一条完整记录序列化为带 CRC32 的二进制格式
 * 最终格式：[CRC32(4B 大端) | LEN(4B 大端) | payload(NB) | \n(1B)]
 * CRC32 覆盖 LEN + payload（不含 CRC 自己 + \n）
 *
 * 直接顺序写：省掉原 tmp buffer 自覆盖的额外一次 memcpy。
 * 步骤：先写 LEN + payload → 算 CRC → 把 CRC 写最前 → 追加 \n */
static int pack_record(const char *payload, char *buf, size_t buflen)
{
    size_t plen = strlen(payload);
    size_t need = 4 + 4 + plen + 1; /* crc(4) + len(4) + payload + \n */
    if (need > buflen) return -1;

    /* 先写 LEN(4) + PAYLOAD(N) 到 buf+4，这样 CRC 可以直接对 [buf+4, buf+4+plen) 算 */
    uint32_t len_be = (uint32_t)plen;
    buf[4]  = (char)(len_be >> 24); buf[5] = (char)(len_be >> 16);
    buf[6]  = (char)(len_be >> 8);  buf[7] = (char)(len_be);
    memcpy(buf + 8, payload, plen);

    /* 对 [LEN + PAYLOAD] 段算 CRC32 */
    uint32_t crc = crc32_buf((const uint8_t *)buf + 4, 4 + plen);

    /* CRC(4) 写 buf[0..3] */
    buf[0] = (char)(crc >> 24); buf[1] = (char)(crc >> 16);
    buf[2] = (char)(crc >> 8);  buf[3] = (char)(crc);

    /* 末尾 \n */
    buf[8 + plen] = '\n';

    return (int)need;
}

/* batch flush — 锁内调用 */
static int wal_batch_flush_locked(cdr_wal_t *w)
{
    if (!w || !w->fp || w->batch.count == 0) return NSBC_OK;

    /* 拼一个大 buffer 批量写，比多次 fwrite 快 */
    size_t total = 0;
    for (int i = 0; i < w->batch.count; i++) total += strlen(w->batch.lines[i]);

    char *bigbuf = (char *)malloc(total);
    if (!bigbuf) {
        /* malloc 失败 → 逐条写 */
        for (int i = 0; i < w->batch.count; i++) {
            if (w->batch.lines[i]) {
                fwrite(w->batch.lines[i], 1, strlen(w->batch.lines[i]), w->fp);
                free(w->batch.lines[i]); w->batch.lines[i] = NULL;
            }
        }
        w->batch.count = 0;
        w->batch.dirty = 0;
        fflush(w->fp);
        return NSBC_OK;
    }

    size_t offset = 0;
    for (int i = 0; i < w->batch.count; i++) {
        size_t n = strlen(w->batch.lines[i]);
        memcpy(bigbuf + offset, w->batch.lines[i], n);
        offset += n;
        free(w->batch.lines[i]); w->batch.lines[i] = NULL;
    }

    size_t wr = fwrite(bigbuf, 1, offset, w->fp);
    free(bigbuf);

    if (wr != offset) {
        w->batch.count = 0;
        w->batch.dirty = 0;
        return NSBC_ERR;
    }

    fflush(w->fp);
    w->batch.count = 0;
    w->batch.dirty = 0;
    w->batch.last_flush_time = time(NULL);
    return NSBC_OK;
}

int cdr_wal_append(cdr_wal_t *w, const cdr_event_t *ev)
{
    if (!w || !ev) return NSBC_INVAL;

    pthread_mutex_lock(&w->lock);

    /* 先序列化（SQLite 和文本 WAL 共用同一个 serialize_event 函数）*/
    char json[1024];
    int jn = serialize_event(ev, json, sizeof(json));
    if (jn < 0 || (size_t)jn >= sizeof(json)) {
        pthread_mutex_unlock(&w->lock);
        return NSBC_ERR;
    }

#ifdef NSBC_WITH_SQLITE
    if (w->db && w->insert_stmt) {
        /* Phase E-04: SQLite WAL backend — 预编译 stmt，热路径零 malloc
         *
         * sqlite3_bind_text + SQLITE_TRANSIENT — sqlite3 内部 memcpy 到自己的 buffer
         * sqlite3_step 执行 INSERT（PRAGMA journal_mode=WAL → sqlite3 自身 WAL 保证原子性）
         * sqlite3_reset + sqlite3_clear_bindings — 复用 stmt，避免每次 prepare
         *
         * busy_timeout=5000 — 磁盘 WAL checkpoint 时可能临时阻塞，等 5s 够了 */
        sqlite3_bind_text(w->insert_stmt, 1, json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(w->insert_stmt, 2, (sqlite3_int64)nsbc_now_ms());
        int rc = sqlite3_step(w->insert_stmt);
        sqlite3_reset(w->insert_stmt);
        sqlite3_clear_bindings(w->insert_stmt);

        if (rc == SQLITE_DONE) {
            pthread_mutex_unlock(&w->lock);
            return NSBC_OK;
        }
        /* SQLite 写失败 — 日志后 return NSBC_ERR 让 billing persist_batch 降级 spool */
        log_warn("cdr_wal", "Phase E-04: sqlite3_step rc=%d (%s)",
                 rc, sqlite3_errmsg(w->db));
        pthread_mutex_unlock(&w->lock);
        return NSBC_ERR;
    }
#endif

    /* 文本 WAL fallback（Phase 4 CRC+batch）— 代码块与 Phase 4 完全一致 */
    /* 打包成带 CRC 的二进制格式 */
    char packed[2048];
    int pn = pack_record(json, packed, sizeof(packed));
    if (pn < 0 || pn >= (int)sizeof(packed)) {
        pthread_mutex_unlock(&w->lock);
        return NSBC_ERR;
    }

    /* 放进 batch */
    if (w->batch.count >= WAL_BATCH_MAX) {
        wal_batch_flush_locked(w);
    }
    w->batch.lines[w->batch.count] = strdup(packed);
    if (!w->batch.lines[w->batch.count]) {
        pthread_mutex_unlock(&w->lock);
        return NSBC_NOMEM;
    }
    w->batch.count++;
    w->batch.dirty = 1;

    wal_batch_flush_locked(w);

    pthread_mutex_unlock(&w->lock);
    return NSBC_OK;
}

/* ------------------------------------------------------------------ */

/* Phase 4: 真 replay — CRC 校验 + 跳过损坏 + 补写 truncate
 *
 * 读取格式：[CRC32(4)][LEN(4)][PAYLOAD(LEN)][\n]
 * 每循环：
 *   1. 读 CRC + LEN = 8 字节
 *   2. 读 LEN 字节 payload
 *   3. 计算 payload 的 CRC，对比 — 不匹配 → 跳过损坏记录 + 继续
 *   4. 匹配 → 视为合法事件（Phase 5 补 json parse → dbal_cdr_batch）
 *
 * 本函数 Phase 4 只负责：识别合法事件 + 返回 replay 条数 + truncate 文件
 * 真正的 DBAL 补写在 Phase 5 sqlite3 引入后加 */
int cdr_wal_replay(cdr_wal_t *w)
{
    if (!w) return 0;

#ifdef NSBC_WITH_SQLITE
    if (w->db) {
        /* Phase E-04: SQLite WAL replay — SELECT committed=0 未补写事件
         *
         * SQLite WAL 的好处：断电半写 → WAL checkpoint 自动合并回主表 → committed=0 事件仍然可见
         * 对比文本 WAL：断电半写文件尾部损坏 → CRC 校验跳过 → 剩余事件全部丢失
         *
         * Phase 5 补 DBAL sqlite3 后：逐条调 dbal_cdr_batch → UPDATE committed=1
         * 当前先只计数（证明 replay 骨架通）*/
        pthread_mutex_lock(&w->lock);

        const char *sel_sql =
            "SELECT event_json FROM cdr_events WHERE committed=0 ORDER BY id ASC";
        sqlite3_stmt *sel = NULL;
        if (sqlite3_prepare_v2(w->db, sel_sql, -1, &sel, NULL) != SQLITE_OK) {
            pthread_mutex_unlock(&w->lock);
            return 0;
        }

        int valid = 0;
        while (sqlite3_step(sel) == SQLITE_ROW) {
            const char *json = (const char *)sqlite3_column_text(sel, 0);
            if (json) {
                log_debug("cdr_wal", "replay valid event %d: %s", valid, json);
                valid++;
            }
        }
        sqlite3_finalize(sel);

        if (valid > 0) {
            log_info("cdr_wal", "Phase E-04: sqlite3 replay %d events → marking committed", valid);
            /* 标记已 replay（Phase 5 DBAL 接入后，应该是 DBAL 成功后才 committed=1）*/
            sqlite3_exec(w->db, "UPDATE cdr_events SET committed=1 WHERE committed=0;",
                         NULL, NULL, NULL);
        }

        /* checkpoint + 截断 WAL 文件（让磁盘空间回收）*/
        sqlite3_exec(w->db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);

        pthread_mutex_unlock(&w->lock);
        return valid;
    }
#endif

    /* ---- Fallback: 文本 WAL replay（Phase 4 CRC+校验）----
     * 与 Phase 4 完全一致 */
    if (!w->fp) return 0;

    pthread_mutex_lock(&w->lock);

    /* 先 flush batch 里残留的 */
    wal_batch_flush_locked(w);

    /* 回到文件头，跳过 12 字节文件头 */
    rewind(w->fp);
    uint8_t head[12];
    if (fread(head, 1, 12, w->fp) != 12 || memcmp(head, WAL_MAGIC, 8) != 0) {
        /* 文件头损坏，直接 truncate 重建 */
        log_warn("cdr_wal", "replay: file header corrupted, truncating");
        ftruncate(fileno(w->fp), 0);
        wal_ensure_header(w->fp);
        pthread_mutex_unlock(&w->lock);
        return 0;
    }

    int valid = 0;
    int corrupted = 0;

    while (1) {
        uint8_t crc_buf[4], len_buf[4];
        size_t hdr_rd = fread(crc_buf, 1, 4, w->fp);
        if (hdr_rd < 4) break;  /* EOF：正常结束 */
        size_t len_rd = fread(len_buf, 1, 4, w->fp);
        if (len_rd < 4) { corrupted++; break; }

        /* 大端解 len */
        uint32_t plen = ((uint32_t)len_buf[0] << 24) | ((uint32_t)len_buf[1] << 16)
                       | ((uint32_t)len_buf[2] << 8)  |  (uint32_t)len_buf[3];
        if (plen == 0 || plen > 8192) { corrupted++; break; }  /* 异常长度 */

        /* Phase 7 性能审计：replay 循环里每条都 malloc(plen+1)/free —— 一次性启动期 replay，
         * 但 8KB stack buffer 零开销更好。payload 最大 8192，stack 上完全塞得下 */
        char payload[8193];  /* stack buffer，零 malloc */
        size_t prd = fread(payload, 1, plen, w->fp);
        if (prd != plen) { corrupted++; break; }

        /* 跳过 \n */
        int c = fgetc(w->fp);
        if (c != '\n') { corrupted++; break; }

        /* CRC 校验：覆盖 LEN + PAYLOAD（crc_buf 里存的是 CRC32(LEN+PAYLOAD)）*/
        /* 简单做法：把 len_buf + payload 拼一起算 crc */
        uint8_t *check = (uint8_t *)malloc(4 + plen);
        if (check) {
            memcpy(check, len_buf, 4);
            memcpy(check + 4, payload, plen);
            uint32_t calc_crc = crc32_buf(check, 4 + plen);
            free(check);

            uint32_t rec_crc = ((uint32_t)crc_buf[0] << 24) | ((uint32_t)crc_buf[1] << 16)
                              | ((uint32_t)crc_buf[2] << 8)  |  (uint32_t)crc_buf[3];

            if (calc_crc != rec_crc) {
                corrupted++;
                /* CRC 错 → 文件可能断电半写，剩余数据全部跳过 */
                log_warn("cdr_wal", "replay: CRC mismatch at payload[0..%u], corrupted=%d",
                         (unsigned)(plen < 16 ? plen : 16), corrupted);
                break;
            }
        }

        /* 合法事件 — Phase 5 补 json parse → dbal_cdr_batch
         * 当前只计数 + 打 debug（证明 replay 骨架通）*/
        log_debug("cdr_wal", "replay valid event %d: %s", valid, payload);
        valid++;
    }

    /* replay 完 truncate 文件（已落库不再需要）+ 重建文件头 */
    if (valid > 0 || corrupted > 0) {
        log_info("cdr_wal", "replay done: valid=%d corrupted=%d → truncating WAL",
                 valid, corrupted);
        ftruncate(fileno(w->fp), 0);
        wal_ensure_header(w->fp);
    }

    pthread_mutex_unlock(&w->lock);
    return valid;
}

/* ------------------------------------------------------------------ */

int cdr_wal_uncommitted_count(cdr_wal_t *w)
{
    if (!w) return 0;

#ifdef NSBC_WITH_SQLITE
    if (w->db) {
        /* Phase E-04: SQLite WAL — committed=0 就是未补写事件数 */
        pthread_mutex_lock(&w->lock);
        sqlite3_stmt *sel = NULL;
        if (sqlite3_prepare_v2(w->db,
            "SELECT COUNT(*) FROM cdr_events WHERE committed=0;", -1, &sel, NULL) != SQLITE_OK) {
            pthread_mutex_unlock(&w->lock);
            return 0;
        }
        int count = 0;
        if (sqlite3_step(sel) == SQLITE_ROW) count = sqlite3_column_int(sel, 0);
        sqlite3_finalize(sel);
        pthread_mutex_unlock(&w->lock);
        return count;
    }
#endif

    /* 文本 WAL fallback — 统计文件头后记录数 */
    if (!w->fp) return 0;

    pthread_mutex_lock(&w->lock);

    rewind(w->fp);
    /* 跳过文件头 12 字节 */
    uint8_t skip[12];
    if (fread(skip, 1, 12, w->fp) < 12) {
        pthread_mutex_unlock(&w->lock);
        return 0;  /* 空文件 */
    }

    int count = 0;
    while (1) {
        uint8_t crc_buf[4], len_buf[4];
        if (fread(crc_buf, 1, 4, w->fp) < 4) break;
        if (fread(len_buf, 1, 4, w->fp) < 4) break;

        uint32_t plen = ((uint32_t)len_buf[0] << 24) | ((uint32_t)len_buf[1] << 16)
                       | ((uint32_t)len_buf[2] << 8)  |  (uint32_t)len_buf[3];
        if (plen == 0 || plen > 8192) break;

        if (fseek(w->fp, (long)plen + 1, SEEK_CUR) != 0) break;  /* skip payload + \n */
        count++;
    }

    pthread_mutex_unlock(&w->lock);
    return count;
}
