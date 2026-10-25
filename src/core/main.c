/*
 * NSBC core daemon entry — FreeSWITCH-style module loader + reactor main loop.
 * Copyright 2024-2026 李浩 (Li Hao), 上海宁卫信息技术有限公司
 * Contact: 1354608370@qq.com
 *
 * Licensed under the Apache License, Version 2.0. See LICENSE in project root.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "app_ctx.h"
#include "cfg.h"
#include "cfg_schema.h"
#include "log.h"
#include "module.h"
#include "nsbcd_signal.h"
#include "playback_engine.h"
#include "reactor.h"
#include "security.h"       /* REQ-EXT-API-005: security_set_yaml_path */
#include "srtp_session.h"

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#define NSBC_VERSION "1.0.0"
#define LEGACY_CONFIG "/etc/nsbc/nsbc.yaml"

/* 获取当前进程的可执行文件目录：readlink /proc/self/exe → dirname。
 * 成功写出不含末尾 '/' 的绝对路径，失败返回 0（主流程 fallback）。 */
static int nsbc_exe_dir(char *buf, size_t bufsz)
{
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return 0;
    self[n] = '\0';
    char *dup = strdup(self);
    if (!dup)
        return 0;
    char *d = dirname(dup);
    if (strlen(d) >= bufsz) {
        free(dup);
        return 0;
    }
    snprintf(buf, bufsz, "%s", d);
    free(dup);
    return 1;
}

/* 在 cfg 加载后，对路径类键做相对路径 resolve：若值不以 '/' 开头，
 * 拼接到 cfg 文件所在目录。键清单固定（schema 硬编码，不跨模块动态发现）。 */
static void nsbc_resolve_cfg_paths(cfg_t *cfg, const char *cfgpath)
{
    const char *keys[] = {
        "log.path", "log.audit_path", "oam.control_sock",
        "system.config_snapshot_dir", NULL};
    static const char *pseudo[] = {  /* 含 scheme 的伪路径：unix:/var/run/... */
        "oam.control_sock", NULL};
    char cfg_dir[PATH_MAX];
    char *dup = strdup(cfgpath);
    if (!dup)
        return;
    char *d = dirname(dup);
    snprintf(cfg_dir, sizeof(cfg_dir), "%s", d);
    free(dup);
    const char *val = NULL;
    for (int i = 0; keys[i]; ++i) {
        if (cfg_get_str(cfg, keys[i], &val, NULL) != NSBC_OK || !val)
            continue;
        /* 跳过含 ':' 的伪路径（unix: 之类）——已带 scheme */
        int skip_pseudo = 0;
        for (int j = 0; pseudo[j]; ++j)
            if (strcmp(keys[i], pseudo[j]) == 0) { skip_pseudo = 1; break; }
        if (skip_pseudo && strchr(val, ':'))
            continue;
        if (val[0] == '/')
            continue;  /* 已是绝对路径 */
        char resolved[PATH_MAX];
        snprintf(resolved, sizeof(resolved), "%s/%s", cfg_dir, val);
        cfg_set(cfg, CFG_L_RUNTIME, keys[i], resolved);
    }
}

/* 各业务模块在 builtin.c 汇总（静态链接，REQ-ARCH-011） */
extern const nsbc_module_t *const nsbc_builtin_modules[];

/* ------------------------------------------------------------------ */
/* 时间轮 + RCU 推进线程（REQ-ARCH-007/001，单调时钟，禁用 timerfd）    */
/* ------------------------------------------------------------------ */
typedef struct {
    nsbc_ctx_t *ctx;
    pthread_t thr;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int stop;
} ticker_t;

static void *ticker_main(void *p)
{
    ticker_t *t = (ticker_t *)p;
    log_thread_attach();
    uint64_t tick = 100;
    long tick_l = 100;
    cfg_get_int(t->ctx->cfg, "core.timer_tick_ms", &tick_l, NULL);
    tick = (uint64_t)tick_l;
    uint64_t now = nsbc_now_ms();
    unsigned since_rcu = 0;
    pthread_mutex_lock(&t->lock);
    while (!t->stop) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        uint64_t target = ms + tick;
        ts.tv_sec = target / 1000;
        ts.tv_nsec = (long)(target % 1000) * 1000000;
        pthread_cond_timedwait(&t->cond, &t->lock, &ts);
        if (t->stop)
            break;
        uint64_t cur = nsbc_now_ms();
        if (cur <= now)
            continue;
        (void)tw_advance(t->ctx->tw, cur);
        now = cur;
        if (++since_rcu >= 10) { /* ~1s 推进一个 RCU 宽限检测 */
            since_rcu = 0;
            wpool_rcu_advance(t->ctx->wpool);
        }
    }
    pthread_mutex_unlock(&t->lock);
    log_thread_detach();
    return NULL;
}

static void ticker_start(ticker_t *t, nsbc_ctx_t *ctx)
{
    pthread_condattr_t ca;
    t->ctx = ctx;
    t->stop = 0;
    pthread_mutex_init(&t->lock, NULL);
    /* ticker_main 用 CLOCK_MONOTONIC 填充 abstime（REQ-ARCH-007），
     * cond 必须同步设置为单调时钟——默认 CLOCK_REALTIME 会把单调
     * 毫秒值当墙钟解释成"早已过去"，timedwait 立即 ETIMEDOUT，
     * 循环退化为空转烧满 CPU（远端 8 核验证时实测捕获）。 */
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&t->cond, &ca);
    pthread_condattr_destroy(&ca);
    pthread_create(&t->thr, NULL, ticker_main, t);
}
static void ticker_stop(ticker_t *t)
{
    pthread_mutex_lock(&t->lock);
    t->stop = 1;
    pthread_cond_signal(&t->cond);
    pthread_mutex_unlock(&t->lock);
    pthread_join(t->thr, NULL);
    pthread_cond_destroy(&t->cond);
    pthread_mutex_destroy(&t->lock);
}

/* ------------------------------------------------------------------ */
/* 信号回调（reactor 线程上下文）                                       */
/* ------------------------------------------------------------------ */
static nsbc_ctx_t *g_ctx;
static const char *g_cfgpath;

/* 前向声明：signaling.c — SIGHUP reload 证书续期时调用 */
int sig_reload_tls_certs(struct nsbc_ctx *ctx);

/* reload/UDS 线程取全局上下文（app_ctx.h 声明）。
 * main 栈变量，进程生命周期内有效；启动前/退出后为 NULL。 */
nsbc_ctx_t *nsbc_get_ctx(void)
{
    return g_ctx;
}

/* 配置变更事件桥（REQ-CFG-012）：cfg 事件 → OAM 结构化事件（CFG 域，
 * 经 ctx->event_fn 入 obs 事件环/订阅）。oam 模块 start 前 event_fn 为
 * NULL，只做事件丢弃；白名单热更项的"应用动作"也收口于此（单一真源：
 * SIGHUP/control config.reload 与 config.set 同源，禁止各入口自行重应用）。 */
static void on_cfg_event(cfg_event_t ev, const char *detail, void *ud)
{
    nsbc_ctx_t *c = (nsbc_ctx_t *)ud;
    static const char *names[] = {
        "CFG_RELOAD_SUCCESS", "CFG_RELOAD_FAILED",
        "CFG_ROLLBACK", "CFG_CHANGED",
    };
    const char *name = ((unsigned)ev < sizeof(names) / sizeof(names[0]))
                           ? names[ev]
                           : "CFG_UNKNOWN";
    /* SUCCESS=文件事务 reload；CHANGED=runtime 单键 set；ROLLBACK=快照
     * 回滚；三条生效路径的缓存项重应用都只在此单点（当前仅 log.level
     * 有订阅者）。 */
    if ((ev == CFG_EV_RELOAD_SUCCESS || ev == CFG_EV_CHANGED ||
         ev == CFG_EV_ROLLBACK) && c && c->cfg) {
        long lv = 4; /* LOG_WARN：生产环境默认 WARN（>=4），避免每 INVITE 一条 INFO 打爆 IO */
        cfg_get_int(c->cfg, "log.level", &lv, NULL);
        log_set_level((log_level_t)lv);
    }
    if (c && c->event_fn)
        c->event_fn(5, name, "%s", detail ? detail : "-");  /* NOTICE 级：配置变更值得留意 */
}

static void on_signal(int fd, uint32_t ev, void *arg)
{
    (void)fd;
    (void)ev;
    (void)arg;
    unsigned char b[16];
    int n = read(nsbcd_signal_fd(), b, sizeof(b));  /* 先手动读，避免 nsbcd_signal_ack 把 pipe 空了再 reload 检查 */
    (void)n;
    if (nsbcd_signal_reload_pending()) {
        nsbcd_signal_reload_clear();
        const char *bad = NULL;
        int rc = cfg_reload_file(g_ctx->cfg, g_cfgpath, &bad);
        if (rc == NSBC_OK) {
            /* log.level 重应用已在 on_cfg_event 收口（与 control 同源） */
            log_audit("system", "config-reload", "SIGHUP reload success");
            llog(LOG_NOTICE, "core", "config reloaded: %s", g_cfgpath);
            /* TLS 证书热重载：nsbc-web 每小时更新 cert.pem 后 kill SIGHUP，
             * 这里检测到 reload 后同步刷新 SIP TLS + WebRTC WSS 的 ctx */
            sig_reload_tls_certs(g_ctx);
        } else {
            llog(LOG_WARN, "core", "reload rejected rc=%d bad=%s", rc,
                 bad ? bad : "-");
        }
    }
    if (nsbcd_signal_stop_pending() && !ctx_stopping(g_ctx)) {
        llog(LOG_NOTICE, "core", "SIGTERM/SIGINT: graceful drain begin");
        log_audit("system", "shutdown-begin", "signal received");
        ctx_request_stop(g_ctx);
    }
}

/* ------------------------------------------------------------------ */
/* 启动装配                                                             */
/* ------------------------------------------------------------------ */
static int build_arenas(nsbc_ctx_t *c)
{
    static const struct {
        const char *key;
        mem_domain_t dom;
        size_t dflt_mb;
    } map[] = {
        {"mem.signal_mb", MEM_DOMAIN_SIGNAL, 512},
        {"mem.packet_mb", MEM_DOMAIN_PACKET, 1024},
        {"mem.media_mb", MEM_DOMAIN_MEDIA, 512},
        {"mem.transcode_mb", MEM_DOMAIN_TRANSCODE, 256},
        {"mem.record_mb", MEM_DOMAIN_RECORD, 256},
    };
    int hp = 0, guard = 0;
    cfg_get_bool(c->cfg, "mem.hugepage", &hp, NULL);
    cfg_get_bool(c->cfg, "mem.guard_band", &guard, NULL);
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        long mb = (long)map[i].dflt_mb;
        cfg_get_int(c->cfg, map[i].key, &mb, NULL);
        mem_arena_cfg_t ac = {map[i].dom, (size_t)mb * 1024 * 1024, hp,
                              guard};
        int rc = mem_arena_create(&ac, &c->arena[map[i].dom]);
        if (rc != NSBC_OK) {
            llog(LOG_CRIT, "core", "arena %s create failed rc=%d",
                 mem_domain_name(map[i].dom), rc);
            return rc;
        }
    }
    return NSBC_OK;
}

/* ------------------------------------------------------------------ */
/* 离线子命令：config migrate（REQ-CFG-006）                            */
/* ------------------------------------------------------------------ */
/* 注：SRS 命名为 nsbc-cli；当前单二进制工程由 nsbcd 承担同一库入口
 * （与 -T 同模式），将来 nsbc-cli 薄壳直接复用 cfg_migrate_file()。
 * argv[0] 为 "migrate" 占位，选项从 argv[1] 开始。 */
static int run_cfg_migrate(int argc, char **argv)
{
    const char *path = LEGACY_CONFIG;
    const char *from = NULL;
    const char *to = NULL;
    static const struct option lopts[] = {
        {"config", required_argument, NULL, 'c'},
        {"from", required_argument, NULL, 'f'},
        {"to", required_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};
    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "c:f:t:h", lopts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            path = optarg;
            break;
        case 'f':
            from = optarg;
            break;
        case 't':
            to = optarg;
            break;
        default:
            fprintf(stderr,
                    "usage: nsbcd config migrate -c <file> --from <M.N>"
                    " --to <M.N>\n");
            return (opt == 'h') ? 0 : 2;
        }
    }
    if (!from || !to) {
        fprintf(stderr, "migrate: --from and --to are required\n");
        return 2;
    }
    /* 与主启动同口径的 strict cfg（schema/默认/环境）；不预加载目标文件：
     * 它可能属于另一 schema 主版本，解析校验在迁移内部完成。
     * 备份目录可用 NSBC_SYSTEM__CONFIG_SNAPSHOT_DIR 覆盖。 */
    cfg_t *cfg = NULL;
    if (cfg_create(&cfg, 0 /* relaxed P0 */,
                   getenv("NSBC_DEV_ALLOW_PLAIN") ? 1 : 0) != NSBC_OK) {
        fprintf(stderr, "cfg_create failed rc=%d\n", NSBC_ERR);
        return 1;
    }
    cfg_schema_register_core(cfg);
    cfg_load_defaults(cfg);
    cfg_apply_env(cfg, "NSBC_");
    char backup[256];
    int rc = cfg_migrate_file(cfg, path, from, to, backup, sizeof(backup));
    if (rc == NSBC_OK)
        printf("migrated %s: %s -> %s\nbackup: %s\n", path, from, to,
               backup);
    else
        fprintf(stderr, "migrate failed: %s rc=%d\n", path, rc);
    cfg_destroy(cfg);
    return rc == NSBC_OK ? 0 : 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "nsbcd %s — NSBC core daemon\n"
            "  -c, --config <path>   配置文件（默认 <exe>/conf/nsbc.yaml\n"
            "                          或 %s）\n"
            "  -T, --check-config    仅校验配置（两阶段校验后退出）\n"
            "  -V, --version         打印版本退出\n"
            "  -h, --help            本帮助\n"
            "  config migrate -c <file> --from <M.N> --to <M.N>\n"
            "                        离线迁移配置 schema（REQ-CFG-006）\n",
            NSBC_VERSION, LEGACY_CONFIG);
    (void)argv0;
}

int main(int argc, char **argv)
{
    /* 离线子命令（不进启动序列；主版本不符时守护进程拒绝启动，迁移必须
     * 能独立运行，故不走 control socket）。 */
    if (argc >= 3 && strcmp(argv[1], "config") == 0 &&
        strcmp(argv[2], "migrate") == 0)
        return run_cfg_migrate(argc - 2, argv + 2);

    char cfgpath_buf[PATH_MAX];
    /* 默认路径链：exe_dir/conf/nsbc.yaml → LEGACY_CONFIG → 内置默认
     * 命令行 -c 覆盖一切 */
    const char *cfgpath = NULL;
    int cfgpath_probed = 0;
    if (argc < 2) {
        /* 无参数：自动探测 */
        char exe_dir[PATH_MAX];
        if (nsbc_exe_dir(exe_dir, sizeof(exe_dir))) {
            snprintf(cfgpath_buf, sizeof(cfgpath_buf),
                     "%s/conf/nsbc.yaml", exe_dir);
            cfgpath = cfgpath_buf;
            FILE *f = fopen(cfgpath, "re");
            if (f) {
                fclose(f);
                cfgpath_probed = 1;
                fprintf(stderr, "[nsbcd] found config: %s\n", cfgpath);
            }
        }
        if (!cfgpath_probed) {
            cfgpath = LEGACY_CONFIG;
            FILE *f = fopen(cfgpath, "re");
            if (f) { fclose(f); cfgpath_probed = 1; }
        }
    }
    int check_only = 0;
    static const struct option lopts[] = {
        {"config", required_argument, NULL, 'c'},
        {"check-config", no_argument, NULL, 'T'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};
    int opt;
    while ((opt = getopt_long(argc, argv, "c:TVh", lopts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            cfgpath = optarg;
            cfgpath_probed = 1;
            break;
        case 'T':
            check_only = 1;
            break;
        case 'V':
            printf("nsbcd %s\n", NSBC_VERSION);
            return 0;
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 2;
        }
    }

    /* 阶段零：日志先到 stderr，配置读成功后重定向 */
    if (log_init(NULL, LOG_INFO, 512) != NSBC_OK) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }
    log_thread_attach();

    cfg_t *cfg = NULL;
    int rc = cfg_create(&cfg, 0 /* relaxed P0 */,
                        getenv("NSBC_DEV_ALLOW_PLAIN") ? 1 : 0);
    if (rc != NSBC_OK) {
        fprintf(stderr, "cfg_create failed rc=%d\n", rc);
        return 1;
    }
    cfg_schema_register_core(cfg);
    cfg_load_defaults(cfg);

    FILE *probe = cfgpath_probed ? fopen(cfgpath, "re") : NULL;
    if (probe) {
        fclose(probe);
        rc = cfg_load_file(cfg, cfgpath);
        if (rc != NSBC_OK) {
            fprintf(stderr, "config load failed: %s rc=%d\n", cfgpath, rc);
            return 1;
        }
        /* 相对路径 resolve：cfg 文件内路径类键若不是绝对路径，
         * 拼到 cfg 文件所在目录。 */
        nsbc_resolve_cfg_paths(cfg, cfgpath);
        llog(LOG_NOTICE, "core", "config loaded: %s", cfgpath);
        /* REQ-EXT-API-005: 把 yaml 路径注入 security 模块 ——
         * 后续 Web push → security.blacklist.reload 成功后自动写回 YAML */
        security_set_yaml_path(cfgpath);
    } else if (cfgpath_probed) {
        fprintf(stderr, "config not found: %s\n", cfgpath);
        return 1;
    } else {
        llog(LOG_NOTICE, "core", "no config file found, using built-in defaults");
    }
    cfg_apply_env(cfg, "NSBC_");

    /* REQ-CFG-006：配置 schema 版本闸门（-T/正常启动同口径，先于动态
     * 校验）。文件缺字段时由 schema 默认值 "1.0" 兜底；主版本不符拒绝
     * 启动并给出迁移命令；同主版本、文件次版本更新仅 WARN。 */
    const char *fver = NULL;
    (void)cfg_get_str(cfg, "config_version", &fver, NULL);
    int newer_minor = 0;
    int vrc = cfg_version_check(fver, &newer_minor);
    if (vrc != NSBC_OK) {
        fprintf(stderr,
                "config schema version rejected: have=%s build=%s rc=%d\n"
                "migrate with: nsbcd config migrate -c %s --from %s"
                " --to %s\n",
                fver ? fver : "(missing)", CFG_SCHEMA_VERSION, vrc,
                cfgpath, fver ? fver : "1.0", CFG_SCHEMA_VERSION);
        return 1;
    }
    if (newer_minor)
        llog(LOG_WARN, "core",
             "config schema minor %s newer than build supports %s",
             fver, CFG_SCHEMA_VERSION);

    /* 阶段二动态校验：共享段后端可用性（REQ-CFG-005） */
    const char *fail_item = NULL;
    if (cfg_dynamic_precheck(cfg, &fail_item) != NSBC_OK) {
        llog(LOG_CRIT, "core", "dynamic precheck failed: %s", fail_item);
        return 1;
    }

    if (check_only) {
        char snap[8192];
        cfg_export_yaml(cfg, snap, sizeof(snap), 1);
        printf("config OK (%s)\n%s", cfgpath, snap);
        log_thread_detach();
        log_stop();
        cfg_destroy(cfg);
        return 0;
    }

    /* 日志重定向到配置路径。cfg_get_str 返回单个 __thread 临时窗，连续
     * 两次取值会互相覆盖，必须各自拷贝后再用。log.level 是枚举字符串，
     * 不能用 cfg_get_int（strtol("info")=0 会把级别压成 EMERG）。 */
    long lv = LOG_INFO;
    const char *sp = NULL;
    if (cfg_get_str(cfg, "log.level", &sp, NULL) == NSBC_OK && sp) {
        static const char *const lvnames[] = {
            "emerg", "alert", "crit", "err", "warn",
            "notice", "info", "debug"};
        for (size_t li = 0; li < sizeof(lvnames) / sizeof(lvnames[0]); ++li) {
            if (strcasecmp(sp, lvnames[li]) == 0) {
                lv = (long)li;
                break;
            }
        }
    }
    char logpath[512] = "/var/log/nsbc/nsbc.log";
    char auditpath[512] = "/var/log/nsbc/audit.log";
    if (cfg_get_str(cfg, "log.path", &sp, NULL) == NSBC_OK && sp && *sp)
        cfg_resolve_path(cfg, sp, logpath, sizeof(logpath));
    if (cfg_get_str(cfg, "log.audit_path", &sp, NULL) == NSBC_OK && sp && *sp)
        cfg_resolve_path(cfg, sp, auditpath, sizeof(auditpath));
    if (log_init(logpath, (log_level_t)lv, 512) != NSBC_OK) {
        fprintf(stderr, "log reopen failed: %s\n", logpath);
        return 1;
    }
    /* size-based self-rotate: default 50MB per file, keep 5 copies
     * (nsbcd.log.1 ~ nsbcd.log.5). No external logrotate dependency — the
     * logger thread handles all rollover internally after each fsync. */
    log_set_rotate(50LL * 1024LL * 1024LL, 5);
    fprintf(stderr, "[nsbcd] logging to %s (self-rotate 50MB × 5)\n", logpath);
    llog(LOG_INFO, "core", "nsbcd %s starting, logfile=%s", NSBC_VERSION, logpath);
    log_audit_open(auditpath);
    log_audit("system", "boot", "nsbcd %s starting cfg=%s", NSBC_VERSION,
              cfgpath);
    llog(LOG_NOTICE, "core", "nsbcd %s starting", NSBC_VERSION);

    /* 启动脱敏有效配置快照（REQ-CFG-006，留最近 N 份）；不可写只 WARN。 */
    char snap_name[128];
    int snap_rc = cfg_snapshot_save(cfg, snap_name, sizeof(snap_name));
    if (snap_rc == NSBC_OK)
        llog(LOG_NOTICE, "core", "config snapshot saved: %s", snap_name);
    else
        llog(LOG_WARN, "core", "config snapshot save skipped rc=%d", snap_rc);

    /* ---- 装配上下文 ---- */
    nsbc_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;
    ctx.config_file = cfgpath;
    g_ctx = &ctx;
    g_cfgpath = cfgpath;
    /* REQ-CFG-012：接线必须在模块 start（event_fn 挂载）与任何 reload 之前 */
    cfg_set_event_cb(cfg, on_cfg_event, &ctx);

    /* 信号（在创建线程/套接字前屏蔽 SIGPIPE） */
    if (nsbcd_signal_init() != NSBC_OK) {
        llog(LOG_CRIT, "core", "signal init failed");
        return 1;
    }

    /* PID 文件：/var/run/nsbc/nsbcd.pid — nsbc-web TLS 续期时 kill SIGHUP 用 */
    {
        const char *pidfile = "/var/run/nsbc/nsbcd.pid";
        const char *piddir  = "/var/run/nsbc";
        /* 确保目录存在（systemd 可能自动建；独立启动需自己 mkdir）*/
        mkdir(piddir, 0755);
        FILE *pf = fopen(pidfile, "we");
        if (pf) {
            fprintf(pf, "%ld\n", (long)getpid());
            fclose(pf);
            llog(LOG_INFO, "core", "pid written: %s", pidfile);
        } else {
            llog(LOG_WARN, "core", "pid file write failed %s (errno=%d): nsbc-web cannot trigger TLS reload",
                 pidfile, errno);
        }
    }

    if (build_arenas(&ctx) != NSBC_OK) {
        llog(LOG_CRIT, "core", "build_arenas failed");
        fprintf(stderr, "build_arenas failed\n");
        return 1;
    }

    ctx.rcu = mem_rcu_create();
    if (!ctx.rcu) {
        llog(LOG_CRIT, "core", "rcu create failed");
        return 1;
    }

    long nworkers = 0;
    cfg_get_int(cfg, "core.workers", &nworkers, NULL);
    if (nworkers <= 0)
        nworkers = sysconf(_SC_NPROCESSORS_ONLN);
    if (nworkers < 1)
        nworkers = 1;
    long qcap = 4096;
    cfg_get_int(cfg, "core.worker_queue", &qcap, NULL);
    if (wpool_create(&ctx.wpool, (unsigned)nworkers, (uint32_t)qcap,
                     ctx.rcu) != NSBC_OK) {
        llog(LOG_CRIT, "core", "worker pool create failed");
        return 1;
    }

    ctx.tw = calloc(1, sizeof(tw_t));
    tw_init(ctx.tw, 100, nsbc_now_ms());

    /* §8 ctx 槽位绑定：playback_engine 注册 pb_stop_if_playing
     * dp_sleep 在 dp_ctx_init 时自行绑 dp_sleep_cancel/dp_sleep_active */
    pb_bind_ctx_slots(&ctx);

    ctx.re = reactor_create(1024);
    if (!ctx.re) {
        llog(LOG_CRIT, "core", "reactor create failed");
        return 1;
    }
    int sfd = nsbcd_signal_fd();
    int src = reactor_add(ctx.re, sfd, EPOLLIN, on_signal, NULL);
    llog(LOG_INFO, "core", "signal fd=%d reactor_add rc=%d", sfd, src);
    if (src != NSBC_OK) {
        llog(LOG_CRIT, "core", "signal fd reactor_add failed rc=%d — shutdown won't work", src);
    }

    ticker_t tick;
    ticker_start(&tick, &ctx);

    /* ---- libsrtp2 全局初始化（必须在任何 srtp_create 之前！） ---- */
    if (nsbc_srtp_global_init() != NSBC_OK) {
        llog(LOG_CRIT, "core", "nsbc_srtp_global_init failed — libsrtp2 not available");
        return 1;
    }
    llog(LOG_INFO, "core", "libsrtp2 global init OK");

    /* ---- 模块分层初始化（REQ-ARCH-010） ----
     * 编译时 configure 分两组：
     *   STATIC  —— REQUIRED / CALL_PATH → 进 builtin_modules.gen.inc（nsbc_builtin_modules 数组）
     *   DYNAMIC —— 非 REQUIRED/CALL_PATH → 编 build/mod/mod_xxx.so，mod.load 列路径
     * 运行时注册顺序：register_builtin(STATIC) → load_all(DYNAMIC) → disable 过滤 → init_level */
    nsbc_modmgr_t *mm = NULL;
    modmgr_create(&mm);
    modmgr_set_global(mm);   /* Phase 5 CLI: OAM show modules 等命令访问 */
    rc = modmgr_register_builtin(mm, nsbc_builtin_modules);
    if (rc != NSBC_OK) {
        llog(LOG_CRIT, "core", "register_builtin failed rc=%d — REQUIRED modules missing", rc);
        goto shutdown_failed;
    }
    llog(LOG_INFO, "core", "registered %u STATIC modules (builtin)", modmgr_count(mm));

    /* REQ-ARCH-010：动态模块加载（configure 生成 mod.load）。
     * 路径候选：exe_dir/mod/mod.load → cfg core.mods_load_path → /etc/nsbc/mod/mod.load
     * mod.load 不存在是正常的（0 个动态模块），不报错。 */
    {
        const char *candidates[4];
        int ncand = 0;
        /* 候选1：可执行文件所在目录的 mod/mod.load（install 后 nsbcd 和 mod/ 放一起） */
        char exe_mod_load[1024];
        {
            ssize_t rsz = readlink("/proc/self/exe", exe_mod_load, sizeof(exe_mod_load) - 1);
            if (rsz > 0) {
                exe_mod_load[rsz] = '\0';
                char *slash = strrchr(exe_mod_load, '/');
                if (slash) {
                    *(slash + 1) = '\0';
                    snprintf(exe_mod_load + (slash - exe_mod_load) + 1,
                             sizeof(exe_mod_load) - (slash - exe_mod_load) - 1,
                             "mod/mod.load");
                    candidates[ncand++] = exe_mod_load;
                }
            }
        }
        /* 候选2：nsbc.yaml 里 core.mods_load_path（显式配置） */
        const char *cfg_mod_load = NULL;
        cfg_get_str(cfg, "core.mods_load_path", &cfg_mod_load, NULL);
        if (cfg_mod_load && cfg_mod_load[0]) candidates[ncand++] = cfg_mod_load;
        /* 候选3：开发时 build/mod/mod.load（configure 生成，和 .so 同目录） */
        candidates[ncand++] = "build/mod/mod.load";
        /* 候选4：旧路径兼容 */
        candidates[ncand++] = "build/mod.load";
        /* 候选5：系统安装路径 */
        candidates[ncand++] = "/etc/nsbc/mod/mod.load";

        for (int i = 0; i < ncand; ++i) {
            if (candidates[i] && access(candidates[i], R_OK) == 0) {
                llog(LOG_INFO, "core", "loading dynamic modules from: %s", candidates[i]);
                int rc_ld = modmgr_load_all(mm, candidates[i]);
                if (rc_ld == NSBC_NOENT) {
                    llog(LOG_INFO, "core", "mod.load empty → 0 dynamic modules");
                } else if (rc_ld != NSBC_OK) {
                    /* 非 REQUIRED .so 加载失败 → 不 fail-fast（configure 依赖校验已保证正确性，
                     * 加载失败就是 .so 文件丢失/ABI 不匹配 → 运维问题，记日志继续） */
                    llog(LOG_WARN, "core",
                         "mod.load rc=%d — 有 .so 加载失败，继续（REQUIRED 语义见模块 flags）",
                         rc_ld);
                }
                break; /* 命中一个就停 */
            }
        }
    }
    llog(LOG_INFO, "core", "total %u modules registered", modmgr_count(mm));

    /* REQ-ARCH-010：YAML modules.disable / enable 过滤。
     * disable 列表 → 命中的模块标记 ST_DISABLED（REQUIRED 模块禁 disable → fail-fast）。
     * enable 白名单 → 只留白名单里的（REQUIRED 永远强制 enable）；enable 缺失 = 除 disable 外全 enable。
     * 优先级：白名单（enable）先算 → 再套黑名单（disable）。 */
    {
        char **dis = NULL; int nd = 0;
        char **en  = NULL; int ne = 0;
        int rc_mods = cfg_get_module_list(cfg, &dis, &nd, &en, &ne);
        if (rc_mods == NSBC_OK) {
            llog(LOG_INFO, "core", "modules config: %d disable / %d enable", nd, ne);
        } else {
            llog(LOG_INFO, "core", "modules: no modules section in YAML → all enabled");
        }

        /* enable 白名单：有 enable 列表时，不在白名单里的普通模块（非 REQUIRED）→ disable */
        if (ne > 0) {
            for (unsigned i = 0; i < modmgr_count(mm); ++i) {
                const nsbc_module_t *m = modmgr_get_nth(mm, i);
                if (!m || (m->flags & MOD_F_REQUIRED)) continue;
                int in_white = 0;
                for (int j = 0; j < ne; ++j) {
                    if (strcmp(m->name, en[j]) == 0) { in_white = 1; break; }
                }
                if (!in_white) {
                    llog(LOG_INFO, "core", "modules: '%s' not in enable whitelist → disabled", m->name);
                    modmgr_disable(mm, m->name); /* 非 REQUIRED → 直接 DISABLED */
                }
            }
        }

        /* disable 黑名单：逐模块调 modmgr_disable（REQUIRED → PERM_DENIED → fail-fast） */
        for (int i = 0; i < nd; ++i) {
            int rc_d = modmgr_disable(mm, dis[i]);
            if (rc_d == NSBC_PERM_DENIED) {
                llog(LOG_CRIT, "core",
                     "modules.disable 禁止禁用 REQUIRED 模块 '%s' — fail-fast",
                     dis[i]);
                rc = NSBC_PERM_DENIED;
                goto shutdown_failed;
            }
        }

        for (int i = 0; i < nd; ++i) free(dis[i]);
        free(dis);
        for (int i = 0; i < ne; ++i) free(en[i]);
        free(en);
    }

    static const mod_level_t levels[] = {
        MOD_L_EARLY, MOD_L_MEM_READY, MOD_L_NET_READY,
        MOD_L_WORKER_READY, MOD_L_FINAL};
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
        rc = modmgr_init_level(mm, levels[i], &ctx);
        if (rc != NSBC_OK) {
            llog(LOG_CRIT, "core", "module init level %d failed rc=%d",
                 levels[i], rc);
            goto shutdown_failed;
        }
    }
    rc = modmgr_start_all(mm, &ctx);
    if (rc != NSBC_OK)
        goto shutdown_failed;
    log_audit("system", "boot-complete", "modules started, accepting traffic");

    /* ---- 主事件循环：reactor + 信号；超时由时间轮决定 ---- */
    while (!ctx_stopping(&ctx)) {
        uint64_t w = tw_next_wait_ms(ctx.tw);
        int timeout = (w == UINT64_MAX) ? 1000 : (int)(w > 1000 ? 1000 : w);
        reactor_wait(ctx.re, timeout);
    }

    /* ---- 优雅停机：先摘流量，再 drain ---- */
    modmgr_stop_all(mm, &ctx);
    reactor_stop(ctx.re);
    long drain_ms = 30000;
    cfg_get_int(cfg, "core.drain_max_ms", &drain_ms, NULL);
    uint64_t deadline = nsbc_now_ms() + (uint64_t)drain_ms;
    while (sat_read(ctx.active_calls) > 0 && nsbc_now_ms() < deadline)
        usleep(100 * 1000);
    if (sat_read(ctx.active_calls) > 0)
        llog(LOG_WARN, "core", "drain timeout, %ld calls still active",
             (long)sat_read(ctx.active_calls));
    log_audit("system", "shutdown-complete", "drain finished");

    modmgr_fini_all(mm, &ctx);
    nsbc_srtp_global_shutdown();
    ticker_stop(&tick);
    tw_fini(ctx.tw);
    free(ctx.tw);
    wpool_destroy(ctx.wpool);
    mem_rcu_destroy(ctx.rcu);
    for (int i = 0; i < MEM_DOMAIN__COUNT; ++i)
        mem_arena_destroy(ctx.arena[i]);
    modmgr_destroy(mm);
    reactor_destroy(ctx.re);
    log_thread_detach();
    log_stop();
    cfg_destroy(cfg);
    unlink("/var/run/nsbc/nsbcd.pid");
    return 0;

shutdown_failed:
    log_audit("system", "boot-failed", "rc=%d", rc);
    modmgr_stop_all(mm, &ctx); /* 已 STARTED 的模块先摘流量 */
    modmgr_fini_all(mm, &ctx);
    nsbc_srtp_global_shutdown();
    ticker_stop(&tick);
    tw_fini(ctx.tw);
    free(ctx.tw);
    wpool_destroy(ctx.wpool);
    mem_rcu_destroy(ctx.rcu);
    for (int i = 0; i < MEM_DOMAIN__COUNT; ++i)
        mem_arena_destroy(ctx.arena[i]);
    modmgr_destroy(mm);
    reactor_destroy(ctx.re);
    log_thread_detach();
    log_stop();
    cfg_destroy(cfg);
    unlink("/var/run/nsbc/nsbcd.pid");
    return 1;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
