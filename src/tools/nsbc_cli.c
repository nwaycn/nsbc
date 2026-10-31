/* nsbc_cli.c — NSBC 专业命令行客户端
 *
 * 设计参考 FreeSWITCH fs_cli（libs/esl/fs_cli.c）的成功之处：
 *   1) 启动大 Banner：ASCII art logo + 版本 + 作者 + 版权
 *   2) Ctrl-C = 清空当前行（不退出！）、Ctrl-D = 退出（fs_cli 风格）
 *   3) 帮助系统：help 分类列出所有命令，help <cmd> 显示详细
 *   4) 变量替换：$focus / $profile 在命令中展开
 *   5) 自动订阅 obs_sub：log + events + cdr 实时推送
 *   6) ANSI 彩色日志输出（非 TTY 自动关闭）
 *   7) Tab 补全 + 历史（~/.nsbc_cli_history）
 *   8) SIP trace 一键抓包（Ctrl-C 停止，不退出 REPL）
 *
 * 架构（fs_cli 风格，零线程零锁）：
 *   readline event hook → poll socket → recv → 帧分类 → 过滤 → 着色
 *   主线程 readline() REPL — 本地命令拦截 → 其他发往服务端
 *   SIGINT handler：Ctrl-C 清行，sip-trace 模式设标志中断阻塞
 *   所有 socket I/O 主循环驱动，无 pthread，无 race condition
 *
 * Author: 李浩 (Lihao) <1354608370@qq.com>
 * Company: 上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
 * License: Apache License 2.0
 * Copyright (c) 2024-2026 All Rights Reserved.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

#if defined(__has_include)
#  if __has_include(<editline/readline.h>)
#    include <editline/readline.h>
#  elif __has_include(<readline/readline.h>)
#    include <readline/readline.h>
#    include <readline/history.h>
#  endif
#else
#  include <editline/readline.h>
#endif

/* ================================================================
 * 常量
 * ================================================================ */
#define NSBC_VERSION     "2.6.0"
#define NSBC_BUILD_DATE  __DATE__
#define NSBC_BUILD_TIME  __TIME__
#define RECV_BUF         (64 * 1024)
#define CLI_PROMPT       "nsbc> "
#define FILTER_MAX       8
#define TAG_MAXLEN       32
#define PATTERN_MAX      256
#define CMD_HELP_MAX     160

/* ================================================================
 * ANSI 颜色（非 TTY 自动关闭）
 * ================================================================ */
#define C_RESET    "\033[0m"
#define C_BOLD     "\033[1m"
#define C_DIM      "\033[2m"
#define C_EMERG    "\033[41;37m"
#define C_ALERT    "\033[31;5m"
#define C_CRIT     "\033[1;31m"
#define C_ERR      "\033[31m"
#define C_WARN     "\033[33m"
#define C_NOTICE   "\033[36m"
#define C_INFO     "\033[0m"
#define C_DEBUG    "\033[90m"
#define C_LOG_TAG  "\033[90m"      /* [log] */
#define C_EVENTS   "\033[35m"
#define C_CDR      "\033[32m"
#define C_CMD_OK   "\033[32m"
#define C_CMD_ERR  "\033[31m"
#define C_BANNER   "\033[1;36m"    /* Banner 主色 青色粗体 */
#define C_BANNER2  "\033[36m"      /* Banner 副色 */
#define C_BANNER3  "\033[33m"      /* 作者名黄色 */
#define C_SIP_TX   "\033[1;32m"    /* siptrace TX 绿色粗体 */
#define C_SIP_RX   "\033[1;33m"    /* siptrace RX 黄色粗体 */
#define C_SIP_META "\033[90m"      /* siptrace 元数据灰色（源地址/长度） */

static const char *g_level_color[] = {
    C_EMERG, C_ALERT, C_CRIT, C_ERR, C_WARN, C_NOTICE, C_INFO, C_DEBUG
};
static const char *g_level_name[] = {
    "EMERG ", "ALERT ", "CRIT  ", "ERR   ", "WARN  ", "NOTICE", "INFO  ", "DEBUG "
};

/* ================================================================
 * 帧类型
 * ================================================================ */
typedef enum {
    FRAME_UNKNOWN = 0,
    FRAME_LOG,
    FRAME_EVENTS,
    FRAME_CDR,
    FRAME_TRACE,      /* siptrace 帧: "siptrace TX->" 或 "siptrace <-RX" 开头 */
    FRAME_METRICS,
    FRAME_CMD,
    FRAME_OTHER,
} frame_kind_t;

/* ================================================================
 * 命令分类（help 显示用）
 * ================================================================ */
typedef enum {
    CMD_LOCAL   = 0,   /* CLI 本地拦截，不发往服务端 */
    CMD_SHOW    = 1,   /* L1 只读查询 */
    CMD_CONFIG  = 2,   /* 配置管理 */
    CMD_RELOAD  = 3,   /* 数据 reload */
    CMD_CALL    = 4,   /* 呼叫级命令 */
    CMD_GLOBAL  = 5,   /* 全局命令 */
} cmd_group_t;

/* ================================================================
 * 全局状态
 * ================================================================ */
static int   g_fd            = -1;
static int   g_recv_alive    = 1;
static int   g_use_color     = 1;
static time_t g_start_time   = 0;

/* P1 过滤器 */
static int   g_log_level_filter = 0;
static char  g_tag_filter[FILTER_MAX][TAG_MAXLEN];
static int   g_tag_count = 0;
static char  g_pattern_filter[PATTERN_MAX] = "";
static int   g_events_on = 1;
static int   g_cdr_on    = 1;
static int   g_log_on    = 1;

/* P2 sip-trace 模式 */
static int   g_sip_trace_mode = 0;
static volatile sig_atomic_t g_interrupt = 0;

/* P2 sip-trace Call-ID 过滤器（非空时只显示匹配的 Call-ID 的包） */
static char  g_callid_filter[128] = "";

/* ================================================================
 * siptrace 包级缓冲状态机
 * siptrace 一个包跨多行：包头行(tag=siptrace) → 多行 SIP body → 下一个包头
 * Call-ID 在 body 中间，必须先收完整包再过滤
 * ================================================================ */
#define SIP_PKT_BUF 16384
static char  g_sip_pkt[SIP_PKT_BUF];
static size_t g_sip_pkt_len = 0;
static int   g_in_sip_pkt = 0;         /* 正在缓冲一个 siptrace 包 */
static char  g_sip_pkt_callid[128] = "";

/* 判断一行是不是 siptrace 包头（tag=="siptrace" + TX-> 或 <-RX） */
static int is_sip_trace_header(const char *line, size_t len)
{
    if (len < 20) return 0;
    if (strncmp(line, "siptrace ", 9) != 0) return 0;
    const char *d = line + 9;
    if (strncmp(d, "TX->", 4) == 0 || strncmp(d, "<-RX", 4) == 0) return 1;
    return 0;
}

/* 从一个完整 siptrace 包缓冲里提取 Call-ID: 后面的值 */
static void sip_pkt_extract_callid(void)
{
    g_sip_pkt_callid[0] = '\0';
    const char *p = g_sip_pkt, *end = g_sip_pkt + g_sip_pkt_len;
    while (p < end) {
        const char *c = (const char *)memmem(p, (size_t)(end - p), "Call-ID:", 8);
        if (!c) break;
        c += 8;
        while (c < end && *c == ' ') c++;
        const char *nl = memchr(c, '\n', (size_t)(end - c));
        const char *cr = memchr(c, '\r', (size_t)(end - c));
        const char *cut = nl ? nl : (cr ? cr : end);
        size_t clen = (size_t)(cut - c);
        if (clen >= sizeof(g_sip_pkt_callid)) clen = sizeof(g_sip_pkt_callid) - 1;
        memcpy(g_sip_pkt_callid, c, clen);
        g_sip_pkt_callid[clen] = '\0';
        return;
    }
}

/* flush 一个完整 siptrace 包：过滤 → 视觉分隔 → 彩色输出 */
static void sip_pkt_flush(void)
{
    if (g_sip_pkt_len == 0) return;

    /* 提取 Call-ID */
    sip_pkt_extract_callid();

    /* Call-ID 过滤 —— 设置了 g_callid_filter 时只显示匹配的包 */
    if (g_callid_filter[0] && g_sip_pkt_callid[0]) {
        if (strcasestr(g_sip_pkt_callid, g_callid_filter) == NULL) {
            /* 不匹配，丢弃（用户说的"太多包"场景自动降噪） */
            g_sip_pkt_len = 0;
            g_in_sip_pkt = 0;
            return;
        }
    }

    /* 视觉分隔：包前空行 + 彩色方向箭头 + Call-ID 标签 */
    write(STDOUT_FILENO, "\n", 1);

    /* 解析包头方向 */
    char *first_nl = memchr(g_sip_pkt, '\n', g_sip_pkt_len);
    if (!first_nl) first_nl = g_sip_pkt + g_sip_pkt_len;
    size_t hdrlen = (size_t)(first_nl - g_sip_pkt);
    char dir_color[16];
    if (strncmp(g_sip_pkt + 9, "TX->", 4) == 0) {
        strncpy(dir_color, g_use_color ? C_SIP_TX : "", sizeof(dir_color) - 1);
        dir_color[sizeof(dir_color) - 1] = '\0';
    } else {
        strncpy(dir_color, g_use_color ? C_SIP_RX : "", sizeof(dir_color) - 1);
        dir_color[sizeof(dir_color) - 1] = '\0';
    }

    /* 包头：彩色方向 + 灰色元数据 + Call-ID 标签 */
    if (g_use_color && dir_color[0]) {
        write(STDOUT_FILENO, dir_color, strlen(dir_color));
        write(STDOUT_FILENO, g_sip_pkt, hdrlen);
        write(STDOUT_FILENO, C_RESET, 4);
    } else {
        write(STDOUT_FILENO, g_sip_pkt, hdrlen);
    }

    /* Call-ID 标签（灰色）—— 即使 body 很长也能一眼看到这包属于哪路 */
    if (g_sip_pkt_callid[0]) {
        char label[256];
        int n = snprintf(label, sizeof(label),
            "  %s[Call-ID: %s]%s\n",
            g_use_color ? C_SIP_META : "",
            g_sip_pkt_callid,
            g_use_color ? C_RESET : "");
        if (n > 0) write(STDOUT_FILENO, label, (size_t)n);
    } else {
        write(STDOUT_FILENO, "\n", 1);
    }

    /* SIP body：包头后直到末尾 */
    size_t body_off = hdrlen;
    /* 如果 Call-ID 已经打了标签，header 行的 \n 我们已经自己打了，
     * 现在 body 从 header 后第一行开始 */
    if (body_off < g_sip_pkt_len) {
        /* 跳过 header 行末尾的 \n 让 body 从第一行开始 */
        body_off++;
        if (body_off < g_sip_pkt_len)
            write(STDOUT_FILENO, g_sip_pkt + body_off,
                  g_sip_pkt_len - body_off);
    }

    g_sip_pkt_len = 0;
    g_in_sip_pkt = 0;
}

/* 变量替换状态（focus） */
static char  g_focus[64] = "";

/* ================================================================
 * 启动 Banner（fs_cli 风格大 logo）
 * 只在 TTY + use_color 时显示 ASCII art + 作者名
 * ================================================================ */
static void print_banner(void)
{
    int c = g_use_color;
    const char *B  = c ? C_BANNER  : "";
    const char *B2 = c ? C_BANNER2 : "";
    const char *B3 = c ? C_BANNER3 : "";
    const char *R  = c ? C_RESET   : "";

    /* 7 行 ASCII art logo — NWAY（每个字母 10 字符宽 × 7 行） */
    printf("%s", B);

    printf("██    ██   ███████   ██████    ██████\n");
    printf("███   ██   ██        ██   ██   ██    \n");
    printf("████  ██   ███████   ██████    ██    \n");
    printf("██ ██ ██        ██   ██   ██   ██    \n");
    printf("██  ████   ███████   ██████    ██████\n");

    printf("%s", R);

    printf("%s%s", B2, "    NSBC  Nway Session Border Controller\n");
    printf("    Version %s  (built %s %s)%s\n", NSBC_VERSION, NSBC_BUILD_DATE, NSBC_BUILD_TIME, R);
    printf("\n");

    printf("    %sby 李浩 (Lihao) <1354608370@qq.com>%s\n", B3, R);
    printf("    %s上海宁卫信息技术有限公司 (China Shanghai Nway Information Technology Co., Ltd.)%s\n", B3, R);
    printf("    Copyright © 2011-2026 · Apache License 2.0\n\n");
    fflush(stdout);
}

/* ================================================================
 * SIGINT 处理 — Ctrl-C 清行/中断 trace（不退出）
 * ================================================================ */
static void handle_sigint(int sig)
{
    (void)sig;
    g_interrupt = 1;
    if (g_sip_trace_mode) return;
#if defined(RL_READLINE_VERSION) || defined(HAVE_READLINE)
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
#endif
}

static void handle_sigpipe(int sig)  { (void)sig; }
static void handle_sigwinch(int sig){ (void)sig; }

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = handle_sigint;   sigaction(SIGINT,  &sa, NULL);
    sa.sa_handler = handle_sigpipe;  sigaction(SIGPIPE, &sa, NULL);
    sa.sa_handler = handle_sigwinch; sigaction(SIGWINCH, &sa, NULL);
}

/* ================================================================
 * ANSI 检测
 * ================================================================ */
static void detect_color(void)
{
    if (!isatty(STDOUT_FILENO)) g_use_color = 0;
}

/* ================================================================
 * 帧分类 + llog 级别解析
 * level_chars[] = "EACEWNID"
 *   0=EMERG 'E'  1=ALERT 'A'  2=CRIT 'C'  3=ERR 'E' (同字母)
 *   4=WARN  'W'  5=NOTIC 'N'  6=INFO 'I'  7=DEBUG 'D'
 * ================================================================ */
static frame_kind_t classify_frame(const char *s, size_t len)
{
    /* 优先按 [TOPIC] 前缀精确分类（obs_sub_publish 统一添加，
     * obs_topic_name 返回小写 → 必须用 strncasecmp 匹配） */
    if (len >= 9  && strncasecmp(s, "[EVENTS]", 8)  == 0) return FRAME_EVENTS;
    if (len >= 6  && strncasecmp(s, "[LOG]", 5)    == 0) return FRAME_LOG;
    if (len >= 8  && strncasecmp(s, "[TRACE]", 7)  == 0) return FRAME_TRACE;
    if (len >= 6  && strncasecmp(s, "[CDR]", 5)    == 0) return FRAME_CDR;
    if (len >= 9  && strncasecmp(s, "[METRICS]", 8)== 0) return FRAME_METRICS;

    /* nsbc> prompt / OK / ERR —— 命令响应帧（服务端 obs_control 直接写，不走 obs_sub） */
    if ((len >= 2 && strncmp(s, "OK", 2) == 0) ||
        (len >= 3 && strncmp(s, "ERR", 3) == 0) ||
        (len >= 6 && strncmp(s, "nsbc> ", 6) == 0)) return FRAME_CMD;

    /* banner / 其他 */
    return FRAME_OTHER;
}

static int parse_log_level(const char *s, size_t len, int *out_level,
                           char *out_tag, size_t out_tag_sz)
{
    if (len < 24) return -1;
    const char *ts_end = strchr(s, ' ');
    if (!ts_end) return -1;
    ts_end++;
    if (ts_end >= s + len) return -1;

    char ltr = *ts_end;
    int lvl = -1;
    switch (ltr) {
        case 'A': lvl = 1; break;   /* ALERT */
        case 'C': lvl = 2; break;   /* CRIT */
        case 'E': lvl = 3; break;   /* ERR (EMERG 也是 E — 无法精确区分) */
        case 'W': lvl = 4; break;   /* WARN */
        case 'N': lvl = 5; break;   /* NOTICE */
        case 'I': lvl = 6; break;   /* INFO */
        case 'D': lvl = 7; break;   /* DEBUG */
        default:  return -1;
    }
    if (out_level) *out_level = lvl;
    if (out_tag && out_tag_sz > 0) {
        const char *tag_start = ts_end + 1;
        while (*tag_start == ' ' && tag_start < s + len) tag_start++;
        size_t tl = 0;
        while (tag_start[tl] && tag_start[tl] != ' ' &&
               tag_start < s + len && tl < out_tag_sz - 1) tl++;
        memcpy(out_tag, tag_start, tl);
        out_tag[tl] = '\0';
    }
    return 0;
}

/* ================================================================
 * 过滤器检查
 * ================================================================ */
static int filter_pass(frame_kind_t kind, int level, const char *tag,
                       const char *line)
{
    /* 服务端 obs_control.c 在每个命令响应末尾追加 "nsbc> "（C 端 prompt）。
     * CLI 自己有 readline 管理 prompt，这里静默丢弃避免双 prompt 粘在一起 */
    if (kind == FRAME_CMD && line && strncmp(line, "nsbc> ", 6) == 0)
        return 0;

    if (kind == FRAME_LOG) {
        if (!g_log_on) return 0;
        if (g_sip_trace_mode && (!tag || strcmp(tag, "siptrace") != 0)) return 0;
        if (level >= 0 && g_log_level_filter > 0 && level > g_log_level_filter) return 0;
        if (g_tag_count > 0 && tag) {
            int hit = 0;
            for (int i = 0; i < g_tag_count; ++i)
                if (strcmp(g_tag_filter[i], tag) == 0) { hit = 1; break; }
            if (!hit) return 0;
        }
    } else if (kind == FRAME_EVENTS) {
        if (!g_events_on || g_sip_trace_mode) return 0;
    } else if (kind == FRAME_CDR) {
        if (!g_cdr_on || g_sip_trace_mode) return 0;
    }
    if (g_pattern_filter[0] && line)
        if (strcasestr(line, g_pattern_filter) == NULL) return 0;
    return 1;
}

/* ================================================================
 * 着色输出
 * ================================================================ */
static void write_colored(frame_kind_t kind, int level,
                           const char *line, size_t len)
{
    if (!g_use_color || kind == FRAME_UNKNOWN || kind == FRAME_CMD) {
        write(STDOUT_FILENO, line, len);
        write(STDOUT_FILENO, "\n", 1);
        return;
    }
    const char *color = NULL;
    const char *prefix = "";
    if (kind == FRAME_LOG) {
        color = (level >= 0 && level <= 7) ? g_level_color[level] : NULL;
        /* 级别名作为 prefix —— 让 WARN/ERR/INFO 一眼可见 */
        prefix = (level >= 0 && level <= 7) ? g_level_name[level] : "LOG   ";
    } else if (kind == FRAME_EVENTS) {
        /* obs_event 帧格式: 2026-xxx [INFO] CALL_INCOMING ...
         * obs_core.c ev_level_name[] 有的有尾随空格（"ERR "）
         * → 匹配 ln 开头 = 级别名，跳过尾部空格，接着必须是 ']' */
        int ev_lvl = -1;
        const char *ev_lvl_name[] = {"EMERG", "ALERT", "CRIT", "ERR",
                                     "WARN", "NOTIC", "INFO", "DEBUG"};
        /* 找 " [" 位置 → 级别名 ≤6 字符 */
        const char *b = line;
        const char *bend = line + len;
        const char *lb = memchr(b, '[', (size_t)(bend - b));
        if (lb && (size_t)(bend - lb) >= 4) {
            const char *ln = lb + 1;
            for (int i = 0; i <= 7; ++i) {
                size_t nl = strlen(ev_lvl_name[i]);
                if ((size_t)(bend - ln) <= nl) continue;
                if (strncmp(ln, ev_lvl_name[i], nl) != 0) continue;
                /* 跳过尾随空格 → 必须是 ']' */
                const char *p = ln + nl;
                while (p < bend && *p == ' ') ++p;
                if (p < bend && *p == ']') {
                    ev_lvl = i; break;
                }
            }
        }
        color = (ev_lvl >= 0 && ev_lvl <= 7) ? g_level_color[ev_lvl] : C_EVENTS;
        prefix = (ev_lvl >= 0 && ev_lvl <= 7) ? g_level_name[ev_lvl] : "EVENT ";
    } else if (kind == FRAME_CDR) {
        color = C_CDR;  prefix = "CDR   ";
    } else if (kind == FRAME_METRICS) {
        color = C_DIM;  prefix = "METRIC";
    }
    if (color) {
        write(STDOUT_FILENO, color, strlen(color));
        write(STDOUT_FILENO, prefix, strlen(prefix));
        write(STDOUT_FILENO, line, len);
        write(STDOUT_FILENO, C_RESET, 4);
    } else {
        write(STDOUT_FILENO, prefix, strlen(prefix));
        write(STDOUT_FILENO, line, len);
    }
    write(STDOUT_FILENO, "\n", 1);
}

/* ================================================================
 * 主循环接收缓冲区（readline event hook 轮询使用）
 * 单线程访问，无需锁 — 主循环 poll + recv，同时处理所有帧
 * ================================================================ */
static char g_recv_buf[RECV_BUF + 1];
static size_t g_recv_pending = 0;  /* 未完整的最后一段残留 */

/* 处理完整帧：切行 → siptrace 包级缓冲 / 普通帧 → 过滤 → 着色
 * siptrace 跨多行，必须先收完整包再按 Call-ID 过滤 */
static void process_recv_buffer(void)
{
    char *start = g_recv_buf, *nl;
    while ((nl = memchr(start, '\n', g_recv_buf + g_recv_pending - start)) != NULL) {
        *nl = '\0';
        size_t line_len = (size_t)(nl - start);
        if (line_len > 0 && start[line_len - 1] == '\r')
            start[--line_len] = '\0';

        /* ★ 双重剥离：
         * 1) 先剥服务端 nsbc> prompt（可能多个粘在一起：dispatch 末尾
         *    ctl_printf("nsbc> ") 和 CLI readline prompt 双写；如果此时
         *    obs_sub 有帧待发 → "nsbc> [events] CALL_INCOMING" 粘一起，
         *    classify_frame 会把它当命令响应静默丢弃）
         * 2) 再剥 obs_sub_publish 加的 [topic] 前缀 */
        size_t skip = 0;
        while (line_len - skip >= 6 && memcmp(start + skip, "nsbc> ", 6) == 0)
            skip += 6;
        if (line_len - skip >= 6 && start[skip] == '[') {
            if      (strncasecmp(start + skip, "[EVENTS] ", 9)  == 0) skip += 9;
            else if (strncasecmp(start + skip, "[METRICS] ", 9) == 0) skip += 9;
            else if (strncasecmp(start + skip, "[TRACE] ", 8)   == 0) skip += 8;
            else if (strncasecmp(start + skip, "[LOG] ", 6)     == 0) skip += 6;
            else if (strncasecmp(start + skip, "[CDR] ", 6)     == 0) skip += 6;
        }
        char *body = start + skip;
        size_t body_len = line_len - skip;

        /* classify_frame 用剥掉 nsbc> 但还可能带 [topic] 的行判型
         * — 这样 "nsbc> [events] CALL_INCOMING" 不会被误判为 FRAME_CMD */
        char *classify_line = start;
        size_t classify_len = line_len;
        if (skip >= 6 && memcmp(start, "nsbc> ", 6) == 0) {
            classify_line = start + 6;  /* 剥一层 nsbc> 就行，classify 不需要剥 [topic] */
            classify_len = line_len - 6;
        }

        if (body_len > 0 && is_sip_trace_header(body, body_len)) {
            /* flush 上一个包（如果还在收） */
            sip_pkt_flush();
            /* 开始新包：包头 + \n + 后续 SIP 头，整包缓冲 */
            g_in_sip_pkt = 1;
            if (body_len + 1 < SIP_PKT_BUF) {
                memcpy(g_sip_pkt, body, body_len);
                g_sip_pkt[body_len] = '\n';
                g_sip_pkt_len = body_len + 1;
            } else {
                g_in_sip_pkt = 0;
            }
        } else if (g_in_sip_pkt && g_sip_pkt_len > 0) {
            /* ★ end-of-packet 检测：如果这行不像 SIP 内容，说明上一个包结束了
             * SIP 消息的行要么是 SIP/2.0 响应 / 方法名开头的请求，要么是
             * Key: Value 格式的 header，要么是空行。以下特征明确不是 SIP：
             *   - 以 '20' 开头（时间戳 YYYY-MM-DD）
             *   - 以 "nsbc>" 开头（服务端 prompt）
             *   - 以 "OK " 或 "ERR " 开头（命令响应）
             * 一旦命中，flush siptrace 包，这行走下面的正常 classify→filter→display */
            int non_sip = 0;
            if (body_len >= 2 && body[0] == '2' && body[1] == '0') non_sip = 1;
            else if (body_len >= 5 && strncmp(body, "nsbc>", 5) == 0) non_sip = 1;
            else if (body_len >= 3 && strncmp(body, "OK ", 3) == 0) non_sip = 1;
            else if (body_len >= 4 && strncmp(body, "ERR ", 4) == 0) non_sip = 1;
            if (non_sip) {
                sip_pkt_flush();  /* 会置 g_in_sip_pkt=0 和 g_sip_pkt_len=0 */
                /* fall through 到下面的正常处理（goto label） */
                goto process_normal_line;
            }
            /* 是 SIP 内容，继续吸收到 siptrace 包 */
            size_t add = body_len + 1;
            if (g_sip_pkt_len + add < SIP_PKT_BUF) {
                memcpy(g_sip_pkt + g_sip_pkt_len, body, body_len);
                g_sip_pkt[g_sip_pkt_len + body_len] = '\n';
                g_sip_pkt_len += add;
            } else {
                sip_pkt_flush();
            }
        } else if (body_len > 0) {
        process_normal_line:
            frame_kind_t kind = classify_frame(classify_line, classify_len);
            int lvl = -1; char tag[TAG_MAXLEN] = {0};
            if (kind == FRAME_LOG)
                parse_log_level(body, body_len, &lvl, tag, sizeof(tag));
            if (filter_pass(kind, lvl, tag, body))
                write_colored(kind, lvl, body, body_len);
        }
        start = nl + 1;
    }
    g_recv_pending = g_recv_buf + g_recv_pending - start;
    if (g_recv_pending > 0 && start != g_recv_buf)
        memmove(g_recv_buf, start, g_recv_pending);
}

/* event hook 退出前 flush 最后一个未完成的 siptrace 包
 * （连接关闭时 event_hook 返回 1，主循环退出） */
static void sip_pkt_flush_pending(void)
{
    if (g_in_sip_pkt && g_sip_pkt_len > 0) sip_pkt_flush();
}

/* readline event hook — readline 等待用户输入时被周期调用
 * 这里 poll + recv 处理所有服务端推送，主循环驱动渲染，无线程 race
 * 返回 0 = 继续等待，非 0 = 触发 readline 返回（Ctrl-D/EINTR） */
static int cli_rl_event_hook(void)
{
    if (!g_recv_alive || g_fd < 0) { sip_pkt_flush_pending(); return 1; }

    /* ⚠️ timeout=5ms 而非 0！这是之前 CPU 飙 260% 的另一根因：
     * poll(0) 立即返回 → event_hook 每次进来都不阻塞 → readline
     * 内部 loop 以最高频率反复调 event_hook → 纯 busy-wait 烧 CPU。
     * 5ms 足够让 UDS 帧及时到达（siptrace 包不会堆积），
     * 又能让没数据时 poll 短暂阻塞让出 CPU（fs_cli 同写法）。 */
    struct pollfd p = { g_fd, POLLIN, 0 };
    int np = poll(&p, 1, 5);  /* 5ms 阻塞 */
    if (np < 0) return 0;
    if (np == 0) return 0;     /* 超时没数据，继续 readline 等用户输入 */
    if (p.revents & (POLLHUP | POLLERR)) {
        fprintf(stderr, "\n%s[nsbc] socket hangup%s\n",
                g_use_color ? C_CRIT : "", g_use_color ? C_RESET : "");
        g_recv_alive = 0;
        sip_pkt_flush_pending();
        return 1;
    }

    /* 一次性尽可能多 recv — 大并发日志峰值场景下减少系统调用次数 */
    while (1) {
        ssize_t n = recv(g_fd, g_recv_buf + g_recv_pending,
                         sizeof(g_recv_buf) - 1 - g_recv_pending, 0);
        if (n == 0) {
            fprintf(stderr, "\n[nsbc] connection closed\n");
            g_recv_alive = 0;
            sip_pkt_flush_pending();
            return 1;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fprintf(stderr, "\nrecv error: %s\n", strerror(errno));
            g_recv_alive = 0;
            sip_pkt_flush_pending();
            return 1;
        }
        g_recv_pending += (size_t)n;
        g_recv_buf[g_recv_pending] = '\0';
        /* 如果 buffer 快满了，先处理已完整的帧再继续 recv */
        if (g_recv_pending >= sizeof(g_recv_buf) / 2) {
            process_recv_buffer();
            if (g_recv_pending == 0) break;  /* buffer 空了，poll 再等下一批 */
        }
        /* 再 poll 一次看有没有更多数据（零成本批处理 — 用 1ms 而非 0
         * 避免无数据时 while 空转烧 CPU） */
        struct pollfd p2 = { g_fd, POLLIN, 0 };
        if (poll(&p2, 1, 1) == 0) break;  /* 没了，出去处理 */
    }

    process_recv_buffer();

    /* readline 重绘：服务端输出覆盖了当前行，readline 不知道要恢复 prompt。
     * event_hook 跑在 readline 主线程里，安全；sip-trace 阻塞模式下跳过
     * （repl_sip_trace 自己管输出，不跑 readline）。 */
#if defined(RL_READLINE_VERSION) || defined(HAVE_READLINE)
    if (!g_sip_trace_mode && g_recv_alive) {
        rl_on_new_line();
        rl_replace_line("", 0);
        rl_redisplay();
    }
#endif

    return 0;  /* 继续 readline 等待 */
}

/* ================================================================
 * 预 drain nsbcd banner — 同步收直到看到 prompt 为止
 * 这样不会让 recv_thread/event_hook 去碰 banner，避免 prompt 粘在一起
 * ================================================================ */
static int pre_drain_banner(void)
{
    size_t pending = 0;
    time_t start = time(NULL);
    while (pending < sizeof(g_recv_buf) - 1) {
        if (time(NULL) - start > 5) return -1;
        struct pollfd p = { g_fd, POLLIN, 0 };
        if (poll(&p, 1, 300) <= 0) continue;
        ssize_t n = recv(g_fd, g_recv_buf + pending,
                         sizeof(g_recv_buf) - 1 - pending, 0);
        if (n <= 0) return -1;
        pending += (size_t)n;
        g_recv_buf[pending] = '\0';
        /* 看到 nsbc> 说明 banner 完整了 */
        if (strstr(g_recv_buf, "nsbc> ") != NULL) break;
    }
    g_recv_pending = pending;
    /* 直接输出 banner 原文（不经过 filter） */
    if (pending > 0) {
        write(STDOUT_FILENO, g_recv_buf, pending);
        /* 如果 banner 末尾没换行，补一个，让 readline 的 prompt 在新行 */
        if (g_recv_buf[pending - 1] != '\n')
            write(STDOUT_FILENO, "\n", 1);
        fflush(stdout);
        /* 清空 buffer — banner 不进入后续帧处理 */
        g_recv_pending = 0;
        g_recv_buf[0] = '\0';
    }
    return 0;
}

/* ================================================================
 * UDS 发送助手（自带换行 + 截断保护）
 * ================================================================ */
static int uds_send(const char *fmt, ...)
{
    if (g_fd < 0) return -1;
    char line[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof(line) - 2) n = sizeof(line) - 3;
    line[n] = '\n'; line[n + 1] = '\0';
    if (send(g_fd, line, (size_t)n + 1, 0) != n + 1) return -1;
    return 0;
}

/* ================================================================
 * 变量替换：$focus / $profile / $cli_version 等
 * 返回 strdup 的新串（调用方 free）
 * ================================================================ */
static char *expand_vars(const char *input)
{
    if (!input || *input == '\0') return strdup(input ? "" : "");
    size_t il = strlen(input);
    char *out = malloc(il + 256);  /* 保守预估 */
    if (!out) return strdup(input);
    size_t op = 0;
    for (size_t i = 0; i < il; ++i) {
        if (input[i] == '$' && input[i+1]) {
            const char *end = input + i + 1;
            while (*end && (isalnum((unsigned char)*end) || *end == '_')) end++;
            size_t vlen = (size_t)(end - input - 1);
            char vname[64];
            if (vlen >= sizeof(vname)) vlen = sizeof(vname) - 1;
            memcpy(vname, input + i + 1, vlen); vname[vlen] = '\0';
            const char *val = "";
            if (strcmp(vname, "focus") == 0)         val = g_focus;
            else if (strcmp(vname, "cli_version") == 0) val = NSBC_VERSION;
            else if (strcmp(vname, "cli") == 0)      val = "nsbc_cli";
            else if (strcmp(vname, "home") == 0)     val = getenv("HOME") ? getenv("HOME") : "";
            else {
                /* 未知变量保留原样：$foo → $foo */
                out[op++] = '$';
                continue;
            }
            for (const char *p = val; *p; ++p) {
                if (op + 1 >= il + 256) break;
                out[op++] = *p;
            }
            i += vlen;
        } else {
            out[op++] = input[i];
        }
    }
    out[op] = '\0';
    return out;
}

/* ================================================================
 * 帮助系统 — 分类显示所有命令
 * help / help local / help show / help call / help reload 等
 * 每个命令有 NAME / CATEGORY / 帮助文本
 * ================================================================ */
struct cmd_entry {
    const char   *name;
    cmd_group_t   group;
    const char   *help;
};

static const struct cmd_entry g_cmds_all[] = {
    /* === LOCAL — CLI 本地拦截 === */
    { "help",           CMD_LOCAL,   "显示帮助（help <cmd> 看详细）" },
    { "status",         CMD_LOCAL,   "查看当前过滤器/日志级别/sip-trace 状态" },
    { "ver",            CMD_LOCAL,   "版本 + 运行时间 + 连接信息" },
    { "color on|off",   CMD_LOCAL,   "切换 ANSI 颜色输出（非 TTY 自动关闭）" },
    { "log filter ...", CMD_LOCAL,   "只显示指定 tag 的日志（空格分隔）" },
    { "log level <0-7>",CMD_LOCAL,   "本地日志级别闸门（0=全部 7=只DEBUG+）" },
    { "log grep <pat>", CMD_LOCAL,   "只显示包含 pattern 的行（子串匹配）" },
    { "log status",     CMD_LOCAL,   "查看当前过滤器设置" },
    { "log on|off",     CMD_LOCAL,   "显示/隐藏 [log] 帧" },
    { "events on|off",  CMD_LOCAL,   "显示/隐藏 [events] 帧" },
    { "cdr on|off",     CMD_LOCAL,   "显示/隐藏 [cdr] 帧" },
    { "sip-trace",      CMD_LOCAL,   "一键 SIP 包抓包（Ctrl-C 停止，自动 sofia siptrace off）" },
    { "sip-filter <pat>",CMD_LOCAL,  "按 Call-ID 子串过滤 siptrace 包（sip-filter clear 清除）" },
    { "focus <call-id>",CMD_LOCAL,   "聚焦通话（$focus 变量自动展开）" },
    { "focus clear",    CMD_LOCAL,   "清除聚焦" },
    { "quit / exit",    CMD_LOCAL,   "退出 CLI（Ctrl-D 空行等价）" },

    /* === SHOW — L1 只读查询 === */
    { "show calls",         CMD_SHOW,   "当前所有通话" },
    { "show channels",      CMD_SHOW,   "通道（calls 别名）" },
    { "show registrations", CMD_SHOW,   "在线注册分机" },
    { "show apps",          CMD_SHOW,   "注册的应用" },
    { "show event_bus",     CMD_SHOW,   "事件总线状态" },
    { "show codecs",        CMD_SHOW,   "支持的编解码器" },
    { "show cdr_wal",       CMD_SHOW,   "CDR 预写日志" },
    { "show endpoints",     CMD_SHOW,   "已注册分机详情" },
    { "show modules",       CMD_SHOW,   "已加载模块" },
    { "show ai_backends",   CMD_SHOW,   "AI 后端状态" },
    { "stats",              CMD_SHOW,   "系统综合状态" },
    { "cdr.recent",         CMD_SHOW,   "最近 CDR 记录" },
    { "cdr.flush",          CMD_SHOW,   "刷 CDR 到磁盘" },

    /* === CONFIG — 配置管理 === */
    { "config.get <key>",       CMD_CONFIG, "读配置值" },
    { "config.set <key> <val>", CMD_CONFIG, "写配置值" },
    { "config.validate",        CMD_CONFIG, "校验当前配置" },
    { "config.export",          CMD_CONFIG, "导出全部配置（JSON）" },
    { "config.snapshots",       CMD_CONFIG, "列配置快照历史" },
    { "config.rollback <id>",   CMD_CONFIG, "回滚到指定快照" },
    { "config.reload",          CMD_CONFIG, "从磁盘重新加载" },
    { "config.save",            CMD_CONFIG, "保存当前到磁盘" },

    /* === RELOAD — 数据 reload === */
    { "extensions.reload <json>",       CMD_RELOAD, "分机表全量替换" },
    { "gateways.reload <json>",         CMD_RELOAD, "网关表" },
    { "trunks.reload <json>",           CMD_RELOAD, "Trunk 表" },
    { "dialplan-rules.reload <json>",   CMD_RELOAD, "拨号规则表" },
    { "profiles.reload <json>",         CMD_RELOAD, "SIP Profile 表（自动同步监听端口）" },
    { "profiles.list",                  CMD_RELOAD, "当前加载的 Profile（L1）" },
    { "queues.reload <json>",           CMD_RELOAD, "ACD 队列表" },
    { "security.blacklist.reload <json>",CMD_RELOAD, "安全黑名单 + fence 配置" },
    { "ivr.reload <json>",              CMD_RELOAD, "IVR 菜单表" },

    /* === CALL — 呼叫级命令 === */
    { "uuid_kill <call-id> [cause]",    CMD_CALL,   "强制挂断" },
    { "uuid_cancel <call-id> [leg]",    CMD_CALL,   "CANCEL 早期 INVITE" },
    { "uuid_hold <call-id> on|off",     CMD_CALL,   "hold / resume" },
    { "uuid_mute <call-id> on|off [leg]",CMD_CALL,  "mute / resume" },
    { "uuid_vad_start <call-id> [thr]", CMD_CALL,   "启动 VAD 监控" },
    { "uuid_vad_stop <call-id>",        CMD_CALL,   "停止 VAD 监控" },
    { "uuid_transfer <call-id> <dest>", CMD_CALL,   "盲转 REFER" },
    { "uuid_displace <call-id>",        CMD_CALL,   "B-leg answer + A-leg unhold" },
    { "uuid_park <call-id>",            CMD_CALL,   "hold 双腿暂停媒体" },
    { "uuid_unpark <call-id>",          CMD_CALL,   "unhold 双腿恢复" },
    { "uuid_deflect <call-id> <dest>",  CMD_CALL,   "盲转别名" },
    { "uuid_wait_for_answer <id> [ms]", CMD_CALL,   "阻塞等待 B-leg 200 OK" },
    { "uuid_bridge <id1> <id2>",        CMD_CALL,   "桥接两呼叫" },
    { "uuid_broadcast <id> <audio>",    CMD_CALL,   "注入音频" },
    { "uuid_break <call-id>",           CMD_CALL,   "打断媒体流" },
    { "uuid_record <id> start|stop [p]",CMD_CALL,   "录制控制" },
    { "uuid_answer <call-id>",          CMD_CALL,   "200 OK 应答" },
    { "uuid_send_dtmf <id> <digits>",   CMD_CALL,   "发送 DTMF 序列" },
    { "uuid_setvar <id> <var> <val>",   CMD_CALL,   "设置通道变量" },
    { "uuid_getvar <id> <var>",         CMD_CALL,   "读通道变量（L1）" },
    { "uuid_dump <call-id>",            CMD_CALL,   "通话 dump（L1）" },
    { "hangup <call-id> [cause]",       CMD_CALL,   "挂断（uuid_kill 别名）" },
    { "originate <call-url> [&app]",    CMD_CALL,   "发起外呼" },
    { "send_msg <id> <body>",           CMD_CALL,   "对话内发送 MESSAGE" },

    /* === GLOBAL — 全局诊断 === */
    { "sofia siptrace on|off",              CMD_GLOBAL, "SIP 包 trace（自动升 loglevel + 订阅推送）" },
    { "sofia global siptrace on|off",       CMD_GLOBAL, "全写等价" },
    { "sofia profile <name> rescan",        CMD_GLOBAL, "FreeSWITCH 别名 → not-supported（用 profiles.reload）" },
    { "console loglevel <0-7|name>",        CMD_GLOBAL, "动态切换日志级别（服务端）" },
    { "console siptrace on|off",            CMD_GLOBAL, "简写等价 sofia siptrace" },
    { "media_bug purge_all",                CMD_GLOBAL, "强制清理所有 media_bug" },
    { "hep status|flush|reload",            CMD_GLOBAL, "HEP3 推流控制" },
    { "mod.load <name>",                    CMD_GLOBAL, "动态加载 mod_<name>.so（L2）" },
    { "mod.unload <name>",                  CMD_GLOBAL, "stop+fini+dlclose 模块（L2）" },
    { "mod.reload <name>",                  CMD_GLOBAL, "一键 unload+load+init+start（L2）" },
    { "security.ban.list [sub]",            CMD_GLOBAL, "封禁表查询（L1）" },
    { "security.ban.unban <ip>",            CMD_GLOBAL, "解封 IP（L2）" },
    { "security.blacklist.reload <json>",   CMD_GLOBAL, "静态黑名单同步（L2）" },
    { "test_push_cdr",                      CMD_GLOBAL, "模拟 CDR 验证 write-through" },
    { NULL, (cmd_group_t)0, NULL }
};

static const char *group_name(cmd_group_t g)
{
    switch (g) {
        case CMD_LOCAL:  return "  [CLI本地]  ";
        case CMD_SHOW:   return "  [只读查询]  ";
        case CMD_CONFIG: return "  [配置管理]  ";
        case CMD_RELOAD: return "  [数据重载]  ";
        case CMD_CALL:   return "  [呼叫控制]  ";
        case CMD_GLOBAL: return "  [全局诊断]  ";
        default:         return "  [???]       ";
    }
}

static void repl_help(const char *rest)
{
    /* help — 显示全部，按组分类 */
    if (!rest || rest[0] == '\0') {
        printf("nsbc_cli %s — NSBC 专业命令行客户端\n\n", NSBC_VERSION);
        printf("%s========= 命令分类 =========%s\n", g_use_color ? C_BANNER2 : "", g_use_color ? C_RESET : "");

        cmd_group_t cur = (cmd_group_t)-1;
        for (int i = 0; g_cmds_all[i].name; ++i) {
            if (g_cmds_all[i].group != cur) {
                cur = g_cmds_all[i].group;
                printf("%s\n%s%s\n",
                       g_use_color ? C_BOLD : "",
                       group_name(cur),
                       g_use_color ? C_RESET : "");
            }
            printf("  %-32s %s\n", g_cmds_all[i].name, g_cmds_all[i].help);
        }

        printf("\n%s========= 快捷键 =========%s\n",
               g_use_color ? C_BANNER2 : "", g_use_color ? C_RESET : "");
        printf("  Ctrl-C  清空当前行（不退出！）\n");
        printf("  Ctrl-D  退出 CLI\n");
        printf("  Tab     命令补全\n");
        printf("  ↑↓      历史命令\n");
        printf("  $focus  当前聚焦 call-id 自动展开\n");
        printf("\n  查看详细帮助: help <命令名>\n\n");
        return;
    }
    /* help <cmd> — 搜索并显示 */
    const char *p = rest;
    while (*p == ' ') p++;
    int found = 0;
    for (int i = 0; g_cmds_all[i].name; ++i) {
        if (strcasecmp(g_cmds_all[i].name, p) == 0 ||
            (strncmp(g_cmds_all[i].name, p, strlen(p)) == 0 &&
             (g_cmds_all[i].name[strlen(p)] == ' ' ||
              g_cmds_all[i].name[strlen(p)] == '\0'))) {
            printf("%s%s%s\n",
                   g_use_color ? C_BOLD : "", g_cmds_all[i].name,
                   g_use_color ? C_RESET : "");
            printf("%s\n", g_cmds_all[i].help);
            printf("%s%s\n",
                   g_use_color ? C_DIM : "",
                   g_cmds_all[i].group == CMD_LOCAL ? "(CLI 本地拦截，不发往服务端)" :
                   "(发往 nsbcd 服务端执行)");
            found = 1;
            break;
        }
    }
    if (!found) printf("ERR unknown command '%s'\n", p);
}

/* ================================================================
 * 过滤器本地命令
 * ================================================================ */
static void repl_log_filter(const char *rest)
{
    if (!rest || rest[0] == '\0') {
        g_tag_count = 0;
        printf("log filter cleared (show all tags)\n"); return;
    }
    g_tag_count = 0;
    const char *p = rest;
    while (*p && g_tag_count < FILTER_MAX) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        const char *end = p;
        while (*end && *end != ' ') end++;
        size_t tl = (size_t)(end - p);
        if (tl >= TAG_MAXLEN) tl = TAG_MAXLEN - 1;
        memcpy(g_tag_filter[g_tag_count], p, tl);
        g_tag_filter[g_tag_count][tl] = '\0';
        g_tag_count++;
        p = end;
    }
    printf("log filter set: %d tag(s)\n", g_tag_count);
}

static void repl_log_level(const char *rest)
{
    const char *names[] = {"emerg","alert","crit","err","warn","notice","info","debug"};
    if (!rest || rest[0] == '\0') {
        printf("log level %d (0=all)\n", g_log_level_filter); return;
    }
    const char *p = rest;
    while (*p == ' ') p++;
    int lvl = -1;
    if (isdigit((unsigned char)*p)) lvl = *p - '0';
    else for (int i = 0; i <= 7; ++i)
         if (strcasecmp(p, names[i]) == 0) { lvl = i; break; }
    if (lvl < 0 || lvl > 7) {
        printf("ERR invalid level '%s' (0-7 or emerg/alert/crit/err/warn/notice/info/debug)\n", p);
        return;
    }
    g_log_level_filter = lvl;
    printf("log level set to %d (%s and above)\n", lvl, lvl ? names[lvl] : "ALL");
}

static void repl_log_grep(const char *rest)
{
    if (!rest || rest[0] == '\0') {
        g_pattern_filter[0] = '\0';
        printf("log grep cleared\n"); return;
    }
    strncpy(g_pattern_filter, rest, sizeof(g_pattern_filter) - 1);
    g_pattern_filter[sizeof(g_pattern_filter) - 1] = '\0';
    size_t pl = strlen(g_pattern_filter);
    while (pl > 0 && (g_pattern_filter[pl-1] == ' ' || g_pattern_filter[pl-1] == '\t'))
        g_pattern_filter[--pl] = '\0';
    printf("log grep set: '%s'\n", g_pattern_filter);
}

static void repl_show_status(void)
{
    int c = g_use_color;
    printf("%s=== nsbc_cli status ===%s\n", c ? C_BOLD : "", c ? C_RESET : "");
    printf("  version=%s  build=%s %s\n", NSBC_VERSION, NSBC_BUILD_DATE, NSBC_BUILD_TIME);
    printf("  uptime=%lds\n", (long)(time(NULL) - g_start_time));
    printf("  sock=/var/run/nsbc/control.sock  connected=%s\n", g_fd >= 0 ? "yes" : "no");
    printf("  color=%s  tty=%s\n", g_use_color ? "on" : "off", isatty(STDOUT_FILENO) ? "yes" : "no");
    printf("\n");
    printf("  log_level_filter=%d (0=all)\n", g_log_level_filter);
    printf("  tags=[%d] ", g_tag_count);
    for (int i = 0; i < g_tag_count; ++i)
        printf("%s%s", i ? "," : "", g_tag_filter[i]);
    if (g_tag_count == 0) printf("(none)");
    printf("\n  pattern='%s'\n", g_pattern_filter[0] ? g_pattern_filter : "(none)");
    printf("  log=%s  events=%s  cdr=%s\n",
           g_log_on ? "on" : "off", g_events_on ? "on" : "off", g_cdr_on ? "on" : "off");
    printf("  sip_trace_mode=%d  focus='%s'\n",
           g_sip_trace_mode, g_focus[0] ? g_focus : "(none)");
}

static void repl_focus(const char *rest)
{
    if (!rest || rest[0] == '\0' || strcmp(rest, "clear") == 0) {
        g_focus[0] = '\0';
        printf("focus cleared\n");
        return;
    }
    strncpy(g_focus, rest, sizeof(g_focus) - 1);
    g_focus[sizeof(g_focus) - 1] = '\0';
    printf("focus set to '%s' ($focus expands to this)\n", g_focus);
}

/* ================================================================
 * P2 sip-trace 一键抓包（阻塞但 Ctrl-C 不退出 REPL）
 * ================================================================ */
static void repl_sip_trace(void)
{
    g_sip_trace_mode = 1;
    g_interrupt = 0;
    /* 自动订阅 trace topic — 与 log 独立，不污染正常日志流 */
    uds_send("sub trace");
    uds_send("sofia siptrace on");

    int c = g_use_color;
    printf("\n%s== SIP trace ON — Ctrl-C to stop ==%s\n",
           c ? C_BOLD : "", c ? C_RESET : "");
    printf("   trace topic subscribed (siptrace packets only)\n");
    if (g_callid_filter[0])
        printf("   Call-ID filter: '%s'\n", g_callid_filter);
    printf("\n");
    fflush(stdout);

    int elapsed = 0;
    while (elapsed < 120 && !g_interrupt && g_recv_alive) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        elapsed++;
    }

    g_sip_trace_mode = 0;
    uds_send("sofia siptrace off");
    uds_send("unsub trace");   /* 清理订阅，后续不再收 trace 帧 */
    g_interrupt = 0;

    printf("\n%s== SIP trace OFF (duration %ds) ==%s\n\n",
           c ? C_BOLD : "", elapsed / 10, c ? C_RESET : "");
    fflush(stdout);
}

/* ================================================================
 * Tab 补全表（含新增命令）
 * ================================================================ */
static const char *g_cmds[] = {
    "help", "quit", "exit", "ver", "version", "status",
    "show", "log", "config.get", "config.set", "config.validate",
    "config.export", "config.snapshots", "config.rollback", "config.reload",
    "cdr.recent", "cdr.flush",
    "profiles.reload", "profiles.list", "extensions.reload",
    "gateways.reload", "trunks.reload", "dialplan-rules.reload",
    "queues.reload", "security.blacklist.reload",
    "uuid_getvar", "uuid_kill", "uuid_hold", "uuid_transfer",
    "uuid_bridge", "uuid_broadcast", "uuid_break", "uuid_record",
    "uuid_answer", "uuid_send_dtmf", "uuid_setvar", "uuid_dump",
    "originate", "sofia", "send_msg", "hep",
    "media_bug", "uuid_media_bug", "test_push_cdr",
    "console", "events", "cdr", "sip-trace", "sip-filter",
    "sub", "unsub", "focus", "color",
    "mod.load", "mod.unload", "mod.reload",
    "security.ban.list", "security.ban.unban", "security.ban.clear",
    "security.blacklist.list", "security.blacklist.reload", "security.cfg.reload",
    "profiles.list", "trunks.list", "dialplan.show",
    "bugs", "bug.add", "bug.del", "bug.list",
    NULL
};

/* Tab 补全：子命令表 */
struct subcmd_entry {
    const char  *prefix;       /* 用户已输入的前缀（如 "show "） */
    const char **candidates;   /* 候选补全列表（NULL 结尾） */
};

static const struct subcmd_entry g_subcmds[] = {
    { "show ",         (const char *[]){ "calls", "channels", "registrations", "rates",
                                          "balances", "transcode", "cdr", NULL } },
    { "log ",          (const char *[]){ "filter", "level", "grep", "on", "off", "status", NULL } },
    { "sofia ",        (const char *[]){ "siptrace", "global", "profile", NULL } },
    { "sofia global ", (const char *[]){ "siptrace", NULL } },
    { "sofia profile ",(const char *[]){ "rescan", "restart", NULL } },
    { "console ",      (const char *[]){ "loglevel", "siptrace", NULL } },
    { "uuid_hold ",    (const char *[]){ "on", "off", NULL } },
    { "uuid_record ",  (const char *[]){ "start", "stop", NULL } },
    { "uuid_media_bug ",(const char *[]){ "start", "stop", "list", NULL } },
    { "media_bug ",    (const char *[]){ "purge_all", NULL } },
    { "hep ",          (const char *[]){ "status", "flush", "reload", NULL } },
    { "events ",       (const char *[]){ "on", "off", NULL } },
    { "cdr ",          (const char *[]){ "on", "off", NULL } },
    { "focus ",        (const char *[]){ "clear", NULL } },
    { "color ",        (const char *[]){ "on", "off", NULL } },
    { "sub ",          (const char *[]){ "log", "events", "cdr", "metrics", NULL } },
    { NULL, (const char *[]){ NULL } }
};

static char *cmd_compgen(const char *text, int state)
{
    static int list_idx, len;
    static int  use_subcmd;
    static const char *sub_prefix;
    if (state == 0) {
        const char *buf = rl_line_buffer;
        int plen = rl_point;
        char already[256];
        if (plen >= (int)sizeof(already)) plen = (int)sizeof(already) - 1;
        memcpy(already, buf, plen);
        already[plen] = '\0';
        use_subcmd = 0; sub_prefix = NULL;
        for (int i = 0; g_subcmds[i].prefix; ++i) {
            if (strncmp(already, g_subcmds[i].prefix,
                        strlen(g_subcmds[i].prefix)) == 0) {
                use_subcmd = 1; sub_prefix = g_subcmds[i].prefix;
                len = (int)strlen(text); list_idx = 0; break;
            }
        }
        if (!use_subcmd) {
            if (strchr(already, ' ')) return NULL;
            len = (int)strlen(text); list_idx = 0;
        }
    }
    if (use_subcmd) {
        for (int i = 0; g_subcmds[i].prefix; ++i) {
            if (strcmp(g_subcmds[i].prefix, sub_prefix) == 0) {
                while (g_subcmds[i].candidates[list_idx]) {
                    const char *cand = g_subcmds[i].candidates[list_idx++];
                    if (strncmp(cand, text, len) == 0) return strdup(cand);
                }
                return NULL;
            }
        }
        return NULL;
    }
    while (g_cmds[list_idx]) {
        const char *cand = g_cmds[list_idx++];
        if (strncmp(cand, text, len) == 0) return strdup(cand);
    }
    return NULL;
}

/* readline 8.x 推荐的 Tab 补全入口 —— 完全接管，不会 fallback 到 history
 * search / filename completion（旧 rl_completion_entry_function 会让 readline
 * 在没有匹配候选时偷偷跑 history search，这就是 "输入 h 自动补成 show registrations"
 * 的根因）。
 * 返回 NULL 表示"没有候选"（Tab 静默，不触发任何历史/文件名猜测）。 */
static char **cli_attempted_completion(const char *text, int start, int end)
{
    (void)end;
    /* 只补第一个 token（主命令）或子命令；其他位置返回 NULL */
    if (start > 0 && rl_line_buffer[start - 1] != ' ')
        return NULL;
    return rl_completion_matches(text, cmd_compgen);
}

/* ================================================================
 * YAML / UDS 辅助
 * ================================================================ */
static int parse_ctrl_sock(const char *cfgpath, char *out, size_t outsz)
{
    FILE *f = fopen(cfgpath, "re");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' ' || line[len-1] == '\t'))
            line[--len] = '\0';
        char *p = strstr(line, "control_sock");
        if (!p) continue;
        p = strchr(p, ':');
        if (!p) continue;
        p++; while (*p == ' ' || *p == '\t') p++;
        if (*p == '"' || *p == '\'') {
            char q = *p++; size_t l = strlen(p);
            if (l > 0 && p[l-1] == q) p[l-1] = '\0';
        }
        if (strncmp(p, "unix:", 5) == 0) p += 5;
        if (*p == '\0') continue;
        if (strlen(p) >= outsz) continue;
        snprintf(out, outsz, "%s", p); fclose(f); return 1;
    }
    fclose(f); return 0;
}

static int resolve_sock_path(char *out, size_t outsz,
                             const char *cli_cfg, const char *cli_sock)
{
    if (cli_sock) { snprintf(out, outsz, "%s", cli_sock); return 1; }
    if (cli_cfg) return parse_ctrl_sock(cli_cfg, out, outsz);
    snprintf(out, outsz, "/var/run/nsbc/control.sock"); return 1;
}

static int connect_uds(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un un; memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&un, sizeof(un)) != 0) {
        close(fd); return -1;
    }
    return fd;
}

/* ================================================================
 * run_repl — fs_cli 风格 REPL
 * ================================================================ */
static void run_repl(int fd)
{
    g_fd = fd; g_recv_alive = 1;
    g_start_time = time(NULL);

    install_signal_handlers();
    detect_color();

    /* === 启动 Banner === */
    print_banner();

    /* === pre-drain banner ===
     * 同步收 nsbcd 开头的 banner 直到看到 nsbc>，原文输出后清空 buffer。
     * 这样 banner 由主循环直接 write，不经过 event_hook 过滤，
     * 也不会跟 readline 的 prompt 粘在一起。 */
    pre_drain_banner();

    /* === readline 设置 + event hook ===
     * 不再开 pthread — event_hook 在 readline 等待输入时 poll socket，
     * 零线程、零锁、零 race condition（fs_cli 架构）。 */
    rl_readline_name = "nsbc";
    /* ★ 关键：用 rl_attempted_completion_function 而非旧式
     * rl_completion_entry_function —— 前者完全接管 Tab，不会 fallback 到
     * readline 默认的 history search / filename completion（那就是"输入 h
     * 自动补全成 show registrations"的根因：readline 内部 history 补全
     * 抢了命令补全的活）。 */
    rl_attempted_completion_function = cli_attempted_completion;
    rl_event_hook = cli_rl_event_hook;

    /* 禁用 history expansion（!号展开历史命令）—— CLI 命令里 ! 合法，
     * 用户输入 !security 不该被 readline 当历史展开 */
    using_history();
    stifle_history(500);  /* 内存只保留最近 500 条，防历史膨胀 */

    char histpath[PATH_MAX];
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(histpath, sizeof(histpath), "%s/.nsbc_cli_history", home);
        read_history(histpath);
    } else { histpath[0] = '\0'; }

    /* 自动订阅 — sub 命令一次发多个 topic（逗号分隔），避免两次 send
     * 之间 kernel buffer 满导致 EAGAIN 丢第二次 */
    uds_send("sub log,events");
    fprintf(stderr, "%s[nsbc_cli] auto-subscribed: log events%s\n",
            g_use_color ? C_DIM : "", g_use_color ? C_RESET : "");
    fprintf(stderr, "%s[nsbc_cli] Ctrl-C clears line · Ctrl-D to quit · 'help' for commands%s\n",
            g_use_color ? C_DIM : "", g_use_color ? C_RESET : "");

    while (g_recv_alive) {
        g_interrupt = 0;
        char *line = readline(CLI_PROMPT);
        if (!line) {
            fprintf(stderr, "%s[nsbc] bye%s\n",
                    g_use_color ? C_DIM : "", g_use_color ? C_RESET : "");
            break;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '  || line[len-1] == '\t'))
            line[--len] = '\0';
        if (len == 0) { free(line); continue; }

        add_history(line);

        /* === 本地命令拦截 === */
        int handled = 0;

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            handled = 1; uds_send("quit");
            struct timespec ts = { 0, 300 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            free(line); break;
        }
        if (strcmp(line, "ver") == 0 || strcmp(line, "version") == 0 || strcmp(line, "about") == 0) {
            handled = 1; repl_show_status();
        }
        if (strcmp(line, "status") == 0) { handled = 1; repl_show_status(); }
        if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            handled = 1; repl_help(NULL);
        } else if (strncmp(line, "help ", 5) == 0) {
            handled = 1; repl_help(line + 5);
        }
        if (strcmp(line, "sip-trace") == 0) {
            handled = 1; free(line); repl_sip_trace(); continue;
        }
        /* sip-filter <call-id-pattern> — 按 Call-ID 过滤 siptrace 包
         * sip-filter clear — 清除过滤器
         * 也可以先 sip-filter 再 sip-trace，或者 sip-trace on 后再 sip-filter */
        if (strcmp(line, "sip-filter") == 0) {
            handled = 1;
            if (g_callid_filter[0])
                printf("sip-filter: Call-ID filter = '%s'\n", g_callid_filter);
            else
                printf("sip-filter: (no filter — showing all siptrace packets)\n");
        } else if (strncmp(line, "sip-filter ", 11) == 0) {
            handled = 1;
            const char *pat = line + 11;
            while (*pat == ' ') pat++;
            if (strcasecmp(pat, "clear") == 0 || pat[0] == '\0') {
                g_callid_filter[0] = '\0';
                printf("sip-filter: cleared (showing all)\n");
            } else {
                size_t plen = strlen(pat);
                if (plen >= sizeof(g_callid_filter)) plen = sizeof(g_callid_filter) - 1;
                memcpy(g_callid_filter, pat, plen);
                g_callid_filter[plen] = '\0';
                printf("sip-filter: Call-ID filter = '%s'  (case-insensitive substring match)\n", g_callid_filter);
            }
        }
        if (strcmp(line, "events on") == 0) { handled = 1; g_events_on = 1; printf("events on\n"); }
        else if (strcmp(line, "events off") == 0) { handled = 1; g_events_on = 0; printf("events off (hidden)\n"); }
        if (strcmp(line, "cdr on") == 0) { handled = 1; g_cdr_on = 1; printf("cdr on\n"); }
        else if (strcmp(line, "cdr off") == 0) { handled = 1; g_cdr_on = 0; printf("cdr off (hidden)\n"); }
        if (strcmp(line, "log on") == 0) { handled = 1; g_log_on = 1; printf("log on\n"); }
        else if (strcmp(line, "log off") == 0) { handled = 1; g_log_on = 0; printf("log off (hidden)\n"); }
        if (strcmp(line, "color on") == 0) { handled = 1; g_use_color = 1; printf("color on\n"); }
        else if (strcmp(line, "color off") == 0) { handled = 1; g_use_color = 0; printf("color off\n"); }
        if (strcmp(line, "focus clear") == 0) { handled = 1; repl_focus("clear"); }
        else if (strncmp(line, "focus ", 6) == 0) { handled = 1; repl_focus(line + 6); }
        else if (strcmp(line, "focus") == 0) { handled = 1; repl_focus(NULL); }

        if (strncmp(line, "log filter", 10) == 0)       { handled = 1; repl_log_filter(line + 10); }
        else if (strncmp(line, "log level", 9) == 0)    { handled = 1; repl_log_level(line + 9); }
        else if (strncmp(line, "log grep", 8) == 0)     { handled = 1; repl_log_grep(line + 8); }
        else if (strcmp(line, "log status") == 0)       { handled = 1; repl_show_status(); }

        if (!handled) {
            /* === 变量替换 === */
            char *expanded = expand_vars(line);
            if (expanded) {
                uds_send("%s", expanded);
                free(expanded);
            } else {
                uds_send("%s", line);
            }
        }
        free(line);
    }

    g_recv_alive = 0;
    if (histpath[0]) write_history(histpath);
}

/* ================================================================
 * run_once — 单次命令模式（-e），不启订阅/着色/Banner
 * ================================================================ */
static int run_once(int fd, const char *cmd)
{
    char buf[RECV_BUF];
    int partial = 0;
    time_t start = time(NULL);
    while (partial == 0 || !strstr(buf, "nsbc> ")) {
        if (time(NULL) - start > 3) { fprintf(stderr, "timeout waiting for banner\n"); return 1; }
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 500) <= 0) continue;
        ssize_t n = recv(fd, buf + partial, sizeof(buf) - 1 - partial, 0);
        if (n <= 0) return 1;
        partial += (int)n; buf[partial] = '\0';
    }
    char *prompt = strstr(buf, "\nnsbc> ");
    if (prompt) { fwrite(buf, 1, (size_t)(prompt - buf + 1), stdout); fflush(stdout); }

    char *dup = strdup(cmd);
    char *saveptr = NULL;
    char *tok = strtok_r(dup, ";", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        char *e = tok + strlen(tok);
        while (e > tok && *(e-1) == ' ') *--e = '\0';
        if (*tok) {
            char *expanded = expand_vars(tok);
            const char *send_this = expanded ? expanded : tok;
            send(fd, send_this, strlen(send_this), 0);
            send(fd, "\n", 1, 0);
            free(expanded);

            char rb[RECV_BUF]; int rp = 0;
            start = time(NULL);
            while (1) {
                if (time(NULL) - start > 10) break;
                struct pollfd p = {fd, POLLIN, 0};
                if (poll(&p, 1, 500) <= 0) continue;
                ssize_t n = recv(fd, rb + rp, sizeof(rb) - 1 - rp, 0);
                if (n <= 0) break;
                rp += (int)n; rb[rp] = '\0';
                if (rp >= (int)sizeof(rb) - 1) {
                    fwrite(rb, 1, (size_t)rp, stdout); fflush(stdout); rp = 0;
                }
                if (strstr(rb, "\nnsbc> ") || strstr(rb, "bye")) break;
            }
            if (rp > 0) {
                char *pp = strstr(rb, "\nnsbc> ");
                if (pp) fwrite(rb, 1, (size_t)(pp - rb + 1), stdout);
                else    fwrite(rb, 1, (size_t)rp, stdout);
                fflush(stdout);
            }
        }
        tok = strtok_r(NULL, ";", &saveptr);
    }
    free(dup);
    return 0;
}

/* ================================================================
 * main
 * ================================================================ */
int main(int argc, char **argv)
{
    const char *cli_cfg = NULL;
    const char *cli_sock = NULL;
    const char *cli_exec = NULL;

    static const struct option lopts[] = {
        {"sock",    required_argument, NULL, 's'},
        {"config",  required_argument, NULL, 'c'},
        {"exec",    required_argument, NULL, 'e'},
        {"version", no_argument,       NULL, 'V'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}};
    int opt;
    while ((opt = getopt_long(argc, argv, "s:c:e:Vh", lopts, NULL)) != -1) {
        switch (opt) {
        case 's': cli_sock = optarg; break;
        case 'c': cli_cfg = optarg; break;
        case 'e': cli_exec = optarg; break;
        case 'V':
            printf("nsbc_cli %s — by 李浩 (Lihao) · Shanghai Nway\n", NSBC_VERSION);
            return 0;
        default:
            fprintf(stderr,
                    "nsbc_cli %s — NSBC 专业命令行客户端\n"
                    "  Author: 李浩 (Lihao) <1354608370@qq.com>\n"
                    "  Company: 上海宁卫信息技术有限公司\n\n"
                    "  -s, --sock <path>     UDS path (default /var/run/nsbc/control.sock)\n"
                    "  -c, --config <yaml>   从 nsbc.yaml 解析 control_sock\n"
                    "  -e, --exec <cmd>      单次命令（支持 ; 分隔多命令）\n"
                    "  -V, --version         版本\n"
                    "  -h, --help            帮助\n\n"
                    "  Ctrl-C 清空当前行（不退出！） · Ctrl-D 退出\n"
                    "  启动自动订阅 log + events\n"
                    "  输入 'help' 查看所有命令\n",
                    NSBC_VERSION);
            return (opt == 'h') ? 0 : 2;
        }
    }

    char sock_path[PATH_MAX];
    if (!resolve_sock_path(sock_path, sizeof(sock_path), cli_cfg, cli_sock)) {
        fprintf(stderr, "cannot resolve control socket path\n"); return 1;
    }
    if (strncmp(sock_path, "unix:", 5) == 0)
        memmove(sock_path, sock_path + 5, strlen(sock_path) - 5 + 1);

    fprintf(stderr, "%s[nsbc_cli] connecting to %s ...%s\n",
            isatty(STDERR_FILENO) ? "\033[2m" : "",
            sock_path,
            isatty(STDERR_FILENO) ? "\033[0m" : "");
    int fd = connect_uds(sock_path);
    if (fd < 0) {
        fprintf(stderr, "connect failed: %s\n", strerror(errno)); return 1;
    }

    if (cli_exec) {
        int rc = run_once(fd, cli_exec);
        close(fd); return rc;
    }
    run_repl(fd);
    close(fd); return 0;
}

/*
    Author:       李浩 (Lihao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
