#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nsbcd_signal.h"
#include "nsbc_atomic.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

static int g_pipe[2] = {-1, -1};
static nsbc_atomic_t g_stop;
static nsbc_atomic_t g_reload;

static void sig_handler(int sig)
{
    unsigned char b = (unsigned char)sig;
    int saved;
    (void)saved;
    if (sig == SIGTERM || sig == SIGINT)
        __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    else if (sig == SIGHUP)
        __atomic_store_n(&g_reload, 1, __ATOMIC_RELEASE);
    if (g_pipe[1] >= 0) {
        ssize_t r = write(g_pipe[1], &b, 1);
        (void)r;
    }
}

static int install(int sig)
{
    struct sigaction sa;
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    return sigaction(sig, &sa, NULL) == 0 ? NSBC_OK : NSBC_ERR;
}

int nsbcd_signal_init(void)
{
    if (pipe2(g_pipe, O_NONBLOCK | O_CLOEXEC) < 0)
        return NSBC_ERR;
    if (install(SIGTERM) != NSBC_OK || install(SIGINT) != NSBC_OK ||
        install(SIGHUP) != NSBC_OK)
        return NSBC_ERR;
    struct sigaction ign;
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ign.sa_flags = 0;
    sigaction(SIGPIPE, &ign, NULL);
    return NSBC_OK;
}

int nsbcd_signal_fd(void) { return g_pipe[0]; }

void nsbcd_signal_ack(void)
{
    unsigned char b[16];
    while (read(g_pipe[0], b, sizeof(b)) > 0) {
    }
}

int nsbcd_signal_stop_pending(void)
{
    return __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) != 0;
}
int nsbcd_signal_reload_pending(void)
{
    return __atomic_load_n(&g_reload, __ATOMIC_ACQUIRE) != 0;
}
void nsbcd_signal_reload_clear(void)
{
    __atomic_store_n(&g_reload, 0, __ATOMIC_RELEASE);
}
