/* Edge-triggered epoll backend (Linux). */
#include "poller.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

struct poller {
    int                 epfd;
    struct epoll_event *events;
    int                 cap;
};

static uint32_t to_epoll(unsigned interest)
{
    /* EPOLLRDHUP: report a peer's shutdown as readable, recv() then returns 0 */
    uint32_t ev = EPOLLET | EPOLLRDHUP;
    if (interest & POLLER_READ)
        ev |= EPOLLIN;
    if (interest & POLLER_WRITE)
        ev |= EPOLLOUT;
    return ev;
}

static unsigned from_epoll(uint32_t ev)
{
    unsigned out = 0;
    if (ev & (EPOLLIN | EPOLLRDHUP))
        out |= POLLER_READ;
    if (ev & EPOLLOUT)
        out |= POLLER_WRITE;
    if (ev & (EPOLLHUP | EPOLLERR))
        out |= POLLER_ERROR;
    return out;
}

struct poller *poller_create(void)
{
    struct poller *p = calloc(1, sizeof *p);
    if (p == NULL)
        return NULL;
    p->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (p->epfd == -1) {
        free(p);
        return NULL;
    }
    return p;
}

void poller_destroy(struct poller *p)
{
    if (p == NULL)
        return;
    close(p->epfd);
    free(p->events);
    free(p);
}

static int ctl(struct poller *p, int op, int fd, unsigned interest)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = to_epoll(interest);
    ev.data.fd = fd;
    return epoll_ctl(p->epfd, op, fd, &ev);
}

int poller_add(struct poller *p, int fd, unsigned interest)
{
    return ctl(p, EPOLL_CTL_ADD, fd, interest);
}

int poller_modify(struct poller *p, int fd, unsigned interest)
{
    return ctl(p, EPOLL_CTL_MOD, fd, interest);
}

int poller_remove(struct poller *p, int fd)
{
    return ctl(p, EPOLL_CTL_DEL, fd, 0);  /* non-NULL event for old kernels */
}

int poller_wait(struct poller *p, struct poller_event *events, int max,
                int timeout_ms)
{
    if (max <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (max > p->cap) {
        struct epoll_event *e = realloc(p->events, (size_t)max * sizeof *e);
        if (e == NULL)
            return -1;
        p->events = e;
        p->cap = max;
    }

    int n = epoll_wait(p->epfd, p->events, max, timeout_ms);
    for (int i = 0; i < n; i++) {
        events[i].fd = p->events[i].data.fd;
        events[i].events = from_epoll(p->events[i].events);
    }
    return n;
}

const char *poller_backend(void)
{
    return "epoll (edge-triggered)";
}
