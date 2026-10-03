/* Level-triggered poll() backend (portable POSIX fallback). */
#include "poller.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>

struct poller {
    struct pollfd *fds;    /* dense array passed to poll() */
    int            nfds;
    int            cap;
    int           *index;  /* index[fd] = position in fds, or -1 */
    int            index_cap;
};

static short to_poll(unsigned interest)
{
    short ev = 0;
    if (interest & POLLER_READ)
        ev |= POLLIN;
    if (interest & POLLER_WRITE)
        ev |= POLLOUT;
    return ev;
}

static unsigned from_poll(short ev)
{
    unsigned out = 0;
    if (ev & POLLIN)
        out |= POLLER_READ;
    if (ev & POLLOUT)
        out |= POLLER_WRITE;
    if (ev & (POLLHUP | POLLERR | POLLNVAL))
        out |= POLLER_ERROR;
    return out;
}

struct poller *poller_create(void)
{
    return calloc(1, sizeof(struct poller));
}

void poller_destroy(struct poller *p)
{
    if (p == NULL)
        return;
    free(p->fds);
    free(p->index);
    free(p);
}

static int slot_of(const struct poller *p, int fd)
{
    if (fd < 0 || fd >= p->index_cap)
        return -1;
    return p->index[fd];
}

int poller_add(struct poller *p, int fd, unsigned interest)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (slot_of(p, fd) != -1) {
        errno = EEXIST;
        return -1;
    }
    if (fd >= p->index_cap) {
        int cap = p->index_cap ? p->index_cap : 64;
        while (cap <= fd)
            cap *= 2;
        int *idx = realloc(p->index, (size_t)cap * sizeof *idx);
        if (idx == NULL)
            return -1;
        for (int i = p->index_cap; i < cap; i++)
            idx[i] = -1;
        p->index = idx;
        p->index_cap = cap;
    }
    if (p->nfds == p->cap) {
        int cap = p->cap ? p->cap * 2 : 64;
        struct pollfd *fds = realloc(p->fds, (size_t)cap * sizeof *fds);
        if (fds == NULL)
            return -1;
        p->fds = fds;
        p->cap = cap;
    }
    p->fds[p->nfds].fd = fd;
    p->fds[p->nfds].events = to_poll(interest);
    p->fds[p->nfds].revents = 0;
    p->index[fd] = p->nfds++;
    return 0;
}

int poller_modify(struct poller *p, int fd, unsigned interest)
{
    int i = slot_of(p, fd);
    if (i == -1) {
        errno = ENOENT;
        return -1;
    }
    p->fds[i].events = to_poll(interest);
    return 0;
}

int poller_remove(struct poller *p, int fd)
{
    int i = slot_of(p, fd);
    if (i == -1) {
        errno = ENOENT;
        return -1;
    }
    p->fds[i] = p->fds[--p->nfds];  /* swap-remove keeps the array dense */
    p->index[p->fds[i].fd] = i;
    p->index[fd] = -1;
    return 0;
}

int poller_wait(struct poller *p, struct poller_event *events, int max,
                int timeout_ms)
{
    if (max <= 0) {
        errno = EINVAL;
        return -1;
    }
    int r = poll(p->fds, (nfds_t)p->nfds, timeout_ms);
    if (r <= 0)
        return r;

    int n = 0;
    for (int i = 0; i < p->nfds && n < max; i++) {
        if (p->fds[i].revents == 0)
            continue;
        events[n].fd = p->fds[i].fd;
        events[n].events = from_poll(p->fds[i].revents);
        n++;
    }
    return n;  /* level-triggered: anything not reported now comes back next call */
}

const char *poller_backend(void)
{
    return "poll (level-triggered)";
}
