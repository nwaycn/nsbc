/* nsbc_cli.c — nsbc 控制面客户端（fs_cli 风格 + libedit 补全/历史）
 *
 * 独立二进制，纯 POSIX：AF_UNIX connect 到 nsbcd 的 control socket，
 * 文本行命令 REPL。不链接任何核心库——只做 socket I/O。
 *
 * 架构：
 *   - pthread 后台线程做 socket recv → write(STDOUT_FILENO)
 *   - 主线程跑 readline() 循环（libedit 提供行编辑/历史/Tab 补全）
 *   - readline 阻塞期间后台线程照常把服务端输出刷到终端
 *
 * 用法：
 *   nsbc_cli                          ← 默认 /var/run/nsbc/control.sock（与 nsbcd 硬编码一致）
 *   nsbc_cli --sock /path/control.sock
 *   nsbc_cli --config /path/nsbc.yaml
 *   nsbc_cli -e "show calls"          ← 单次命令执行后退出
 *   nsbc_cli -e "show channels; log dump 10"
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ---- libedit 头文件兼容 ----
 * Linux 上 readline-dev 提供 <readline/readline.h>；
 * macOS/FreeBSD 的 libedit 提供 <editline/readline.h>。
 * 我们用 editline 优先，不行 fallback 到 readline。
 * libedit 自带 history API（add_history / using_history 等），
 * 不需要额外 include history.h。 */
#if defined(__has_include)
#  if __has_include(<editline/readline.h>)
#    include <editline/readline.h>
#  elif __has_include(<readline/readline.h>)
#    include <readline/readline.h>
#    include <readline/history.h>
#  else
#    warning "no editline/readline header found — build will fail unless NSBC_CLI_NO_EDITLINE=1"
#  endif
#else
/* 老编译器：先试 editline，再试 readline */
#  include <editline/readline.h>
#endif

#define NSBC_VERSION "2.6.0"
#define RECV_BUF     (64 * 1024)
#define RECV_LINE    1536
#define CLI_PROMPT   "nsbc> "

/* ---- 全局 socket fd + 退出标志（供主线程/recv 线程共享） ---- */
static int   g_fd        = -1;
static int   g_recv_alive  = 1;  /* 后台 recv 线程 1=活 0=停 */

/* ================================================================
 * 命令补全表（硬编码，匹配服务端 obs_control.c + cmd_registry 注册）
 * 两级：一级命令名 + 子命令补全（"show " → "calls/channels/registrations/..."）
 * ================================================================ */

/* 一级命令名（Tab 补全的第一组候选） */
static const char *g_cmds[] = {
    "help", "quit", "exit", "version", "stats", "events",
    "show", "log", "config.get", "config.set", "config.validate",
    "config.export", "config.snapshots", "config.rollback", "config.reload",
    "reload", "hangup", "cdr.recent", "cdr.flush",
    "profiles.reload", "profiles.list", "extensions.reload",
    "gateways.reload", "trunks.reload", "dialplan-rules.reload",
    "queues.reload",
    "uuid_getvar", "uuid_kill", "uuid_hold", "uuid_transfer",
    "uuid_bridge", "uuid_broadcast", "uuid_break", "uuid_record",
    "uuid_answer", "uuid_send_dtmf", "uuid_setvar", "uuid_dump",
    "originate", "sofia", "send_msg", "hep",
    "rate.reload", "rate.upsert", "rate.delete",
    "account.open", "account.topup",
    "media_bug", "uuid_media_bug", "test_push_cdr",
    NULL
};

/* 子命令补全表：<prefix, 候选列表>
 * 键入 "show " 时补全 calls/channels/registrations/rates/balances/transcode/cdr
 * 键入 "log " 时补全 dump/stats
 * 键入 "sofia " 时补全 profile
 * 等等。 */
struct subcmd_entry {
    const char *prefix;   /* 已输入的前缀（含空格） */
    const char *candidates[16];  /* 候选子命令，NULL 结尾 */
};

static const struct subcmd_entry g_subcmds[] = {
    { "show ", { "calls", "channels", "registrations", "rates", "balances",
                 "transcode", "cdr", NULL } },
    { "log ",  { "dump", "stats", NULL } },
    { "sofia ", { "profile", NULL } },
    { "sofia profile ", { "rescan", "restart", NULL } },
    { "uuid_hold ", { "on", "off", NULL } },
    { "uuid_record ", { "start", "stop", NULL } },
    { "uuid_media_bug ", { "start", "stop", "list", NULL } },
    { "media_bug ", { "purge_all", NULL } },
    { "hep ", { "status", "flush", "reload", NULL } },
    { "events ", { NULL } },   /* events [N] 没子候选 */
    { NULL, { NULL } }
};

/* ---- editline 补全回调 ----
 * rl_completion_entry_function 签名：char *(*)(const char *, int)
 * —— 返回值由调用方 free()，所以必须 strdup。
 * 参数：text=当前已输入的待补全前缀，state=0 第一次，>0 后续继续枚举 */
static char *cmd_compgen(const char *text, int state)
{
    static int list_idx, len;
    static int  use_subcmd;     /* 1=用 g_subcmds 候选，0=用 g_cmds */
    static const char *sub_prefix;

    if (state == 0) {
        const char *buf = rl_line_buffer;
        int plen = rl_point;
        char already[256];
        if (plen >= (int)sizeof(already)) plen = sizeof(already) - 1;
        memcpy(already, buf, plen);
        already[plen] = '\0';

        use_subcmd = 0;
        sub_prefix = NULL;
        for (int i = 0; g_subcmds[i].prefix; ++i) {
            if (strncmp(already, g_subcmds[i].prefix,
                        strlen(g_subcmds[i].prefix)) == 0) {
                use_subcmd = 1;
                sub_prefix = g_subcmds[i].prefix;
                len = (int)strlen(text);
                list_idx = 0;
                break;
            }
        }
        if (!use_subcmd) {
            const char *sp = strchr(already, ' ');
            if (sp) return NULL;  /* 已输入参数，不再补全命令名 */
            len = (int)strlen(text);
            list_idx = 0;
        }
    }

    if (use_subcmd) {
        for (int i = 0; g_subcmds[i].prefix; ++i) {
            if (strcmp(g_subcmds[i].prefix, sub_prefix) == 0) {
                while (g_subcmds[i].candidates[list_idx]) {
                    const char *cand = g_subcmds[i].candidates[list_idx++];
                    if (strncmp(cand, text, len) == 0)
                        return strdup(cand);
                }
                return NULL;
            }
        }
        return NULL;
    }

    while (g_cmds[list_idx]) {
        const char *cand = g_cmds[list_idx++];
        if (strncmp(cand, text, len) == 0)
            return strdup(cand);
    }
    return NULL;
}

/* ================================================================
 * YAML 解析（-c 覆盖用）/ UDS connect
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
        /* yaml 为嵌套风格（core:/oam: 段下缩进的 control_sock:），
         * 两个段值一致，命中任意一处即可 */
        char *p = strstr(line, "control_sock");
        if (!p) continue;
        p = strchr(p, ':');
        if (!p) continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            size_t l = strlen(p);
            if (l > 0 && p[l-1] == q) p[l-1] = '\0';
        }
        if (strncmp(p, "unix:", 5) == 0) p += 5;
        if (*p == '\0') continue;
        if (strlen(p) >= outsz) continue;
        snprintf(out, outsz, "%s", p);
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

static int resolve_sock_path(char *out, size_t outsz,
                             const char *cli_cfg, const char *cli_sock)
{
    if (cli_sock) { snprintf(out, outsz, "%s", cli_sock); return 1; }
    if (cli_cfg) return parse_ctrl_sock(cli_cfg, out, outsz);
    /* 默认与 nsbcd obs_control 硬编码对齐：统一 /var/run/nsbc/control.sock */
    snprintf(out, outsz, "/var/run/nsbc/control.sock");
    return 1;
}

static int connect_uds(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&un, sizeof(un)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ================================================================
 * 后台 recv 线程：socket → write(STDOUT_FILENO)
 * 这样主线程 readline() 阻塞时，服务端输出照样刷到终端。
 * ================================================================ */
static void *recv_thread(void *arg)
{
    (void)arg;
    char buf[RECV_BUF];
    while (g_recv_alive) {
        /* 用 poll 做超时，方便及时退出 */
        struct pollfd p = { g_fd, POLLIN, 0 };
        int np = poll(&p, 1, 200);  /* 200ms 超时 */
        if (np < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (np == 0) continue;
        if (p.revents & (POLLHUP | POLLERR)) {
            fprintf(stderr, "\n[nsbc] socket hangup\n");
            break;
        }
        ssize_t n = recv(g_fd, buf, sizeof(buf), 0);
        if (n == 0) {
            fprintf(stderr, "\n[nsbc] connection closed\n");
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fprintf(stderr, "\nrecv error: %s\n", strerror(errno));
            break;
        }
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    g_recv_alive = 0;
    return NULL;
}

/* ================================================================
 * readline 模式 REPL
 * ================================================================ */
static void run_repl(int fd)
{
    g_fd = fd;
    g_recv_alive = 1;

    /* 启动后台 recv 线程 */
    pthread_t tid;
    if (pthread_create(&tid, NULL, recv_thread, NULL) != 0) {
        fprintf(stderr, "cannot spawn recv thread: %s\n", strerror(errno));
        return;
    }

    /* 初始化 libedit 补全 */
    rl_readline_name = "nsbc";
    rl_completion_entry_function = cmd_compgen;
    /* 历史文件：~/.nsbc_cli_history */
    char histpath[PATH_MAX];
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(histpath, sizeof(histpath), "%s/.nsbc_cli_history", home);
        read_history(histpath);
    } else {
        histpath[0] = '\0';
    }

    /* 屏蔽 SIGPIPE（后台线程 recv 时对端关闭不会炸整个进程） */
    signal(SIGPIPE, SIG_IGN);

    while (g_recv_alive) {
        char *line = readline(CLI_PROMPT);
        if (!line) {
            /* EOF (Ctrl-D) */
            fprintf(stderr, "\n[nsbc] bye\n");
            break;
        }
        /* trim */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '  || line[len-1] == '\t'))
            line[--len] = '\0';

        if (len == 0) { free(line); continue; }

        /* 历史 */
        add_history(line);

        /* quit/exit 本地处理 */
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            send(fd, "quit\n", 5, 0);
            /* 让后台线程多 flush 一会儿 */
            struct timespec ts = { 0, 300 * 1000 * 1000 };  /* 300ms */
            nanosleep(&ts, NULL);
            free(line);
            break;
        }

        /* 发送 */
        send(fd, line, len, 0);
        send(fd, "\n", 1, 0);
        free(line);
    }

    g_recv_alive = 0;
    pthread_join(tid, NULL);

    if (histpath[0]) write_history(histpath);
}

/* ================================================================
 * 单次命令模式（-e）：保留原版实现，不启 editline
 * ================================================================ */
static int run_once(int fd, const char *cmd)
{
    char buf[RECV_BUF];
    int partial = 0;
    time_t start = time(NULL);
    while (partial == 0 || !strstr(buf, "nsbc> ")) {
        if (time(NULL) - start > 3) {
            fprintf(stderr, "timeout waiting for banner\n");
            return 1;
        }
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 500) <= 0) continue;
        ssize_t n = recv(fd, buf + partial, sizeof(buf) - 1 - partial, 0);
        if (n <= 0) return 1;
        partial += (int)n;
        buf[partial] = '\0';
    }
    char *prompt = strstr(buf, "\nnsbc> ");
    if (prompt) {
        fwrite(buf, 1, (size_t)(prompt - buf + 1), stdout);
        fflush(stdout);
    }

    char *dup = strdup(cmd);
    char *saveptr = NULL;
    char *tok = strtok_r(dup, ";", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        char *e = tok + strlen(tok);
        while (e > tok && *(e-1) == ' ') *--e = '\0';
        if (*tok) {
            send(fd, tok, strlen(tok), 0);
            send(fd, "\n", 1, 0);

            char rb[RECV_BUF];
            int rp = 0;
            start = time(NULL);
            while (1) {
                if (time(NULL) - start > 10) break;
                struct pollfd p = {fd, POLLIN, 0};
                if (poll(&p, 1, 500) <= 0) continue;
                ssize_t n = recv(fd, rb + rp, sizeof(rb) - 1 - rp, 0);
                if (n <= 0) break;
                rp += (int)n;
                rb[rp] = '\0';
                if (rp >= (int)sizeof(rb) - 1) {
                    fwrite(rb, 1, (size_t)rp, stdout);
                    fflush(stdout);
                    rp = 0;
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
        case 'V': printf("nsbc_cli %s\n", NSBC_VERSION); return 0;
        default:
            fprintf(stderr,
                    "nsbc_cli %s — nsbcd control socket client (libedit)\n"
                    "  -s, --sock <path>      UDS path\n"
                    "  -c, --config <yaml>    cfg 解析 oam.control_sock\n"
                    "  -e, --exec <cmd>       单次命令（支持 ; 分隔多命令）\n"
                    "  -V, --version          版本\n"
                    "REPL: Tab 补全、Ctrl-C 取消行、↑/↓ 历史（~/.nsbc_cli_history）\n"
                    "默认 UDS：/var/run/nsbc/control.sock（与 nsbcd 硬编码一致）\n",
                    NSBC_VERSION);
            return (opt == 'h') ? 0 : 2;
        }
    }

    char sock_path[PATH_MAX];
    if (!resolve_sock_path(sock_path, sizeof(sock_path), cli_cfg, cli_sock)) {
        fprintf(stderr, "cannot resolve control socket path\n");
        return 1;
    }

    if (strncmp(sock_path, "unix:", 5) == 0)
        memmove(sock_path, sock_path + 5, strlen(sock_path) - 5 + 1);

    fprintf(stderr, "[nsbc_cli] connecting to %s ...\n", sock_path);
    int fd = connect_uds(sock_path);
    if (fd < 0) {
        fprintf(stderr, "connect failed: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "[nsbc_cli] connected (Tab completion · ↑↓ history)\n");

    if (cli_exec) {
        int rc = run_once(fd, cli_exec);
        close(fd);
        return rc;
    }
    run_repl(fd);
    close(fd);
    return 0;
}
