#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reactor.h"
#include "nsbc_atomic.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    reactor_cb_fn cb;
    void *arg;
} slot_t;

struct reactor {
    int epfd;
    int max_events;
    struct epoll_event *events;
    slot_t *slots;     /* fd → 回调（fd 直查，容量=fd 高水位） */
    int slots_cap;
    nsbc_atomic_t stop;
};

reactor_t *reactor_create(int max_events)
{
    reactor_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->max_events = max_events > 0 ? max_events : 256;
    r->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epfd < 0) {
        free(r);
        return NULL;
    }
    r->events = calloc((size_t)r->max_events, sizeof(struct epoll_event));
    r->slots_cap = 1024;
    r->slots = calloc((size_t)r->slots_cap, sizeof(slot_t));
    if (!r->events || !r->slots) {
        close(r->epfd);
        free(r->events);
        free(r->slots);
        free(r);
        return NULL;
    }
    return r;
}

void reactor_destroy(reactor_t *r)
{
    if (!r)
        return;
    close(r->epfd);
    free(r->events);
    free(r->slots);
    free(r);
}

static int slot_ensure(reactor_t *r, int fd)
{
    if (fd < 0)
        return NSBC_INVAL;
    if (fd < r->slots_cap)
        return NSBC_OK;
    int nc = r->slots_cap;
    while (nc <= fd)
        nc *= 2;
    slot_t *ns = realloc(r->slots, (size_t)nc * sizeof(slot_t));
    if (!ns)
        return NSBC_NOMEM;
    memset(ns + r->slots_cap, 0,
           (size_t)(nc - r->slots_cap) * sizeof(slot_t));
    r->slots = ns;
    r->slots_cap = nc;
    return NSBC_OK;
}

int reactor_add(reactor_t *r, int fd, uint32_t events, reactor_cb_fn cb,
                void *arg)
{
    if (slot_ensure(r, fd) != NSBC_OK)
        return NSBC_NOMEM;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(r->epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return NSBC_ERR;
    r->slots[fd].cb = cb;
    r->slots[fd].arg = arg;
    return NSBC_OK;
}

int reactor_mod(reactor_t *r, int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(r->epfd, EPOLL_CTL_MOD, fd, &ev) < 0 ? NSBC_ERR
                                                           : NSBC_OK;
}

int reactor_del(reactor_t *r, int fd)
{
    if (epoll_ctl(r->epfd, EPOLL_CTL_DEL, fd, NULL) < 0)
        return NSBC_ERR;
    if (fd >= 0 && fd < r->slots_cap) {
        r->slots[fd].cb = NULL;
        r->slots[fd].arg = NULL;
    }
    return NSBC_OK;
}

void reactor_stop(reactor_t *r) { sat_store_ptr(&r->stop, (void *)1); }
int reactor_stopping(const reactor_t *r) { return sat_read(((reactor_t *)r)->stop) != 0; }

int reactor_wait(reactor_t *r, int timeout_ms)
{
    int n;
    do {
        n = epoll_wait(r->epfd, r->events, r->max_events, timeout_ms);
    } while (n < 0 && errno == EINTR && !reactor_stopping(r));
    if (n < 0)
        return reactor_stopping(r) ? 0 : NSBC_ERR;
    for (int i = 0; i < n; ++i) {
        int fd = r->events[i].data.fd;
        if (fd >= r->slots_cap || !r->slots[fd].cb)
            continue;
        r->slots[fd].cb(fd, r->events[i].events, r->slots[fd].arg);
    }
    return n;
}
