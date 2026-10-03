/*
 * I/O readiness notification, with two interchangeable backends chosen at
 * build time (CMake option CHAT_POLLER):
 *
 *   poller_epoll.c  Linux epoll, edge-triggered
 *   poller_poll.c   POSIX poll(), level-triggered (portable fallback)
 *
 * Callers must work with both semantics: on a readiness event, read until
 * EAGAIN and write until EAGAIN (or nothing is left), and request
 * POLLER_WRITE only while there is queued output.
 */
#ifndef CHAT_POLLER_H
#define CHAT_POLLER_H

enum {
    POLLER_READ  = 1u << 0,
    POLLER_WRITE = 1u << 1,
    POLLER_ERROR = 1u << 2   /* hang-up or socket error; read to find out */
};

struct poller_event {
    int      fd;
    unsigned events;
};

struct poller;

struct poller *poller_create(void);
void           poller_destroy(struct poller *p);

int poller_add(struct poller *p, int fd, unsigned interest);
int poller_modify(struct poller *p, int fd, unsigned interest);
int poller_remove(struct poller *p, int fd);

/* Returns the number of events stored (0 on timeout) or -1 with errno set. */
int poller_wait(struct poller *p, struct poller_event *events, int max,
                int timeout_ms);

const char *poller_backend(void);

#endif
