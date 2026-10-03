/*
 * Single-threaded event loop:
 *
 *   poller_wait() -> for each ready fd:
 *       listener    accept every pending connection
 *       signal pipe stop the loop (SIGINT / SIGTERM)
 *       client      flush queued output, then read and dispatch complete lines
 *   reap_dead()  close clients that failed during this iteration
 *
 * All sockets are non-blocking. Output to each client goes through its own
 * queue; a client whose queue exceeds max_outbuf is disconnected instead of
 * being allowed to stall everyone else (backpressure).
 *
 * Chat semantics (nicknames, rooms, commands) live in commands.c.
 */
#include "server_internal.h"

#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    BACKLOG           = 128,
    MAX_EVENTS        = 256,
    SHUTDOWN_FLUSH_MS = 1000  /* time given to deliver the goodbye message */
};

/* ------------------------------------------------------------------------ */
/* Signals: the handler only writes the signal number into a pipe that the  */
/* event loop watches, so all real work happens outside signal context.      */

static int g_signal_write_fd = -1;

static void on_signal(int signo)
{
    int saved = errno;
    unsigned char b = (unsigned char)signo;
    ssize_t r = write(g_signal_write_fd, &b, 1);  /* async-signal-safe */
    (void)r;  /* pipe full: a stop is already pending */
    errno = saved;
}

static int install_signals(struct server *s)
{
    if (pipe(s->sig_pipe) == -1) {
        LOG_ERRNO("pipe");
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if (net_set_nonblocking(s->sig_pipe[i]) == -1 ||
            net_set_cloexec(s->sig_pipe[i]) == -1) {
            LOG_ERRNO("fcntl(signal pipe)");
            return -1;
        }
    }
    g_signal_write_fd = s->sig_pipe[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_signal;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        LOG_ERRNO("sigaction");
        return -1;
    }
    /* writing to a vanished peer must return EPIPE, not kill the process */
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        LOG_ERRNO("sigaction(SIGPIPE)");
        return -1;
    }
    return 0;
}

static void restore_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    g_signal_write_fd = -1;
}

static void drain_signal_pipe(struct server *s)
{
    unsigned char sig[16];
    ssize_t n;
    while ((n = read(s->sig_pipe[0], sig, sizeof sig)) > 0) {
        LOG_INFO("received signal %d, shutting down", sig[0]);
        s->stopping = true;
    }
}

/* ------------------------------------------------------------------------ */
/* Client table                                                              */

static struct client *table_get(const struct server *s, int fd)
{
    if (fd < 0 || (size_t)fd >= s->by_fd_cap)
        return NULL;
    return s->by_fd[fd];
}

static int table_insert(struct server *s, struct client *c)
{
    size_t fd = (size_t)c->fd;
    if (fd >= s->by_fd_cap) {
        size_t cap = s->by_fd_cap ? s->by_fd_cap : 64;
        while (cap <= fd)
            cap *= 2;
        struct client **t = realloc(s->by_fd, cap * sizeof *t);
        if (t == NULL)
            return -1;
        memset(t + s->by_fd_cap, 0, (cap - s->by_fd_cap) * sizeof *t);
        s->by_fd = t;
        s->by_fd_cap = cap;
    }
    s->by_fd[fd] = c;

    c->prev = NULL;
    c->next = s->head;
    if (s->head)
        s->head->prev = c;
    s->head = c;
    s->nclients++;
    return 0;
}

static void table_remove(struct server *s, struct client *c)
{
    s->by_fd[c->fd] = NULL;
    if (c->prev)
        c->prev->next = c->next;
    else
        s->head = c->next;
    if (c->next)
        c->next->prev = c->prev;
    s->nclients--;
}

struct client *find_by_nick(struct server *s, const char *nick)
{
    for (struct client *c = s->head; c != NULL; c = c->next) {
        if (!c->dead && strcasecmp(c->nick, nick) == 0)
            return c;
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Output                                                                    */

static void set_write_interest(struct server *s, struct client *c, bool want)
{
    if (c->want_write == want)
        return;
    unsigned interest = POLLER_READ | (want ? POLLER_WRITE : 0u);
    if (poller_modify(s->poller, c->fd, interest) == -1) {
        LOG_ERRNO("poller_modify");
        client_kill(c, "server error");
        return;
    }
    c->want_write = want;
}

static void after_flush(struct server *s, struct client *c, enum flush_result r)
{
    switch (r) {
    case FLUSH_DONE:    set_write_interest(s, c, false);       break;
    case FLUSH_PENDING: set_write_interest(s, c, true);        break;
    case FLUSH_ERROR:   client_kill(c, "connection error");    break;
    }
}

void send_raw(struct server *s, struct client *c, const char *data, size_t len)
{
    if (c->dead)
        return;
    if (client_enqueue(c, data, len, s->cfg.max_outbuf) == -1) {
        LOG_INFO("%s (%s): output queue over %zu bytes, dropping slow client",
                 c->nick, c->addr, s->cfg.max_outbuf);
        client_kill(c, "too slow");
        return;
    }
    if (!c->want_write)  /* otherwise the socket is known to be full */
        after_flush(s, c, client_flush(c));
}

/* Format prefix + fmt into one protocol line (at most PROTO_MAX_LINE bytes,
 * '\n' included). Returns its length. */
static size_t vformat_line(char *out, const char *prefix, const char *fmt, va_list ap)
{
    size_t cap = PROTO_MAX_LINE - 1;  /* room for '\n' */
    int p = snprintf(out, cap + 1, "%s", prefix);
    size_t len = p < 0 ? 0 : (size_t)p;
    if (len > cap)
        len = cap;
    int n = vsnprintf(out + len, cap + 1 - len, fmt, ap);
    if (n > 0)
        len = len + (size_t)n > cap ? cap : len + (size_t)n;
    out[len++] = '\n';
    return len;
}

void send_line(struct server *s, struct client *c, const char *fmt, ...)
{
    char line[PROTO_MAX_LINE + 1];
    va_list ap;
    va_start(ap, fmt);
    size_t len = vformat_line(line, "", fmt, ap);
    va_end(ap);
    send_raw(s, c, line, len);
}

void send_ok(struct server *s, struct client *c, const char *fmt, ...)
{
    char line[PROTO_MAX_LINE + 1];
    va_list ap;
    va_start(ap, fmt);
    size_t len = vformat_line(line, "OK ", fmt, ap);
    va_end(ap);
    send_raw(s, c, line, len);
}

void send_err(struct server *s, struct client *c, enum proto_error code,
              const char *fmt, ...)
{
    char prefix[64];
    snprintf(prefix, sizeof prefix, "ERR %d %s ", (int)code, proto_error_name(code));
    char line[PROTO_MAX_LINE + 1];
    va_list ap;
    va_start(ap, fmt);
    size_t len = vformat_line(line, prefix, fmt, ap);
    va_end(ap);
    send_raw(s, c, line, len);
}

void send_room(struct server *s, const char *room, const struct client *except,
               const char *fmt, ...)
{
    char line[PROTO_MAX_LINE + 1];
    va_list ap;
    va_start(ap, fmt);
    size_t len = vformat_line(line, "", fmt, ap);
    va_end(ap);

    /* failures only mark clients dead, so the list is never modified here */
    for (struct client *c = s->head; c != NULL; c = c->next) {
        if (c != except && (room == NULL || strcmp(c->room, room) == 0))
            send_raw(s, c, line, len);
    }
}

/* ------------------------------------------------------------------------ */
/* Disconnects are deferred to the end of an event-loop iteration: closing   */
/* immediately would let accept() reuse the fd number while events for the  */
/* old connection are still waiting in the current batch.                    */

static void drop_client(struct server *s, struct client *c)
{
    poller_remove(s->poller, c->fd);
    table_remove(s, c);
    LOG_INFO("%s (%s) disconnected: %s (%d clients)",
             c->nick, c->addr, c->quit_reason, s->nclients);
    client_destroy(c);
}

static void reap_dead(struct server *s)
{
    bool again = true;
    while (again) {  /* announcing a disconnect can find more dead clients */
        again = false;
        struct client *c = s->head;
        while (c != NULL) {
            struct client *next = c->next;  /* senders never free */
            if (c->dead) {
                char nick[sizeof c->nick], room[sizeof c->room];
                char reason[sizeof c->quit_reason];
                memcpy(nick, c->nick, sizeof nick);
                memcpy(room, c->room, sizeof room);
                memcpy(reason, c->quit_reason, sizeof reason);
                drop_client(s, c);
                chat_on_disconnect(s, nick, room, reason);
                again = true;
            }
            c = next;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Accepting                                                                 */

/* Out of file descriptors: the pending connection would keep the listener
 * readable forever. Free the reserved fd, accept the connection only to close
 * it, then reserve the fd again. Returns false if no progress was possible. */
static bool shed_connection(struct server *s)
{
    if (s->spare_fd == -1)
        return false;
    close(s->spare_fd);
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd != -1)
        close(fd);
    s->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    LOG_ERROR("out of file descriptors, connection rejected");
    return fd != -1;
}

static void reject(int fd, enum proto_error code, const char *text)
{
    char line[256];
    int n = snprintf(line, sizeof line, "ERR %d %s %s\n",
                     (int)code, proto_error_name(code), text);
    if (n > 0) {
        ssize_t r = send(fd, line, (size_t)n, MSG_NOSIGNAL);  /* best effort */
        (void)r;
    }
    close(fd);
}

static void register_client(struct server *s, int fd,
                            const struct sockaddr_storage *addr, socklen_t len)
{
    char name[CLIENT_ADDR_SIZE];
    net_peer_name(addr, len, name, sizeof name);

    struct client *c = client_create(fd, name, net_is_loopback(addr));
    if (c == NULL) {
        LOG_ERROR("out of memory, rejecting %s", name);
        close(fd);
        return;
    }
    do {  /* default nickname: first free guestN */
        snprintf(c->nick, sizeof c->nick, "guest%u", ++s->guest_counter);
    } while (find_by_nick(s, c->nick) != NULL);
    snprintf(c->room, sizeof c->room, "%s", PROTO_DEFAULT_ROOM);
    tb_init(&c->rate, s->cfg.msg_rate, s->cfg.msg_burst, monotonic_ms());

    if (poller_add(s->poller, fd, POLLER_READ) == -1) {
        LOG_ERRNO("poller_add");
        client_destroy(c);
        return;
    }
    if (table_insert(s, c) == -1) {
        LOG_ERROR("out of memory, rejecting %s", name);
        poller_remove(s->poller, fd);
        client_destroy(c);
        return;
    }
    LOG_INFO("%s (%s) connected (%d clients)", c->nick, c->addr, s->nclients);
    chat_on_connect(s, c);
}

static void accept_clients(struct server *s)
{
    for (;;) {  /* edge-triggered: accept until the queue is empty */
        struct sockaddr_storage addr;
        socklen_t len = sizeof addr;  /* value-result: reset every call */

        int fd = net_accept(s->listen_fd, &addr, &len);
        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EMFILE || errno == ENFILE) {
                if (shed_connection(s))
                    continue;
                return;
            }
            LOG_ERRNO("accept");
            return;
        }
        if (s->nclients >= s->cfg.max_clients) {
            LOG_INFO("rejected connection: server full (%d clients)", s->nclients);
            reject(fd, ERR_SERVER_FULL, "Server is full, try again later");
            continue;
        }
        register_client(s, fd, &addr, len);
    }
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                   */

/* Dispatch every complete line. Returns false if reading should stop. */
static bool process_lines(struct server *s, struct client *c)
{
    for (;;) {
        char  *line;
        size_t len;
        switch (line_reader_next(&c->in, &line, &len)) {
        case LR_AGAIN:
            return true;
        case LR_TOO_LONG:
            send_err(s, c, ERR_TOO_LONG, "Line longer than %d bytes, disconnecting",
                     PROTO_MAX_LINE - 1);
            client_kill(c, "protocol error");
            return false;
        case LR_LINE:
            chat_handle_line(s, c, line, len);
            if (c->dead || s->stopping)
                return false;
            break;
        }
    }
}

static void handle_read(struct server *s, struct client *c)
{
    for (;;) {  /* edge-triggered: read until EAGAIN */
        char  *dst;
        size_t space = line_reader_space(&c->in, &dst);
        ssize_t n = recv(c->fd, dst, space, 0);

        if (n > 0) {
            line_reader_commit(&c->in, (size_t)n);
            if (!process_lines(s, c))
                return;
            continue;
        }
        if (n == 0) {                       /* orderly shutdown by peer */
            client_kill(c, "connection closed");
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            client_kill(c, "connection error");  /* ECONNRESET etc. */
        return;
    }
}

static void handle_client_event(struct server *s, struct client *c, unsigned ev)
{
    if (ev & POLLER_WRITE)
        after_flush(s, c, client_flush(c));
    if (!c->dead && (ev & (POLLER_READ | POLLER_ERROR)))
        handle_read(s, c);
}

/* ------------------------------------------------------------------------ */

static int event_loop(struct server *s)
{
    struct poller_event events[MAX_EVENTS];

    while (!s->stopping) {
        int n = poller_wait(s->poller, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            LOG_ERRNO("poller_wait");
            return -1;
        }
        for (int i = 0; i < n && !s->stopping; i++) {
            int fd = events[i].fd;
            if (fd == s->listen_fd) {
                accept_clients(s);
            } else if (fd == s->sig_pipe[0]) {
                drain_signal_pipe(s);
            } else {
                struct client *c = table_get(s, fd);
                if (c != NULL && !c->dead)
                    handle_client_event(s, c, events[i].events);
            }
        }
        reap_dead(s);
        fflush(stdout);
    }
    return 0;
}

static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - start->tv_sec) * 1000 +
           (now.tv_nsec - start->tv_nsec) / 1000000;
}

static bool output_pending(const struct server *s)
{
    for (const struct client *c = s->head; c != NULL; c = c->next) {
        if (!c->dead && buffer_size(&c->out) > 0)
            return true;
    }
    return false;
}

/* Tell everyone, give queued output a bounded time to drain, then close. */
static void graceful_shutdown(struct server *s)
{
    send_room(s, NULL, NULL, "INFO Server is shutting down");

    /* From here on only write readiness matters: watching input would make a
     * level-triggered poller spin on clients that are still sending. */
    poller_remove(s->poller, s->listen_fd);
    for (struct client *c = s->head; c != NULL; c = c->next) {
        if (!c->dead && buffer_size(&c->out) > 0 &&
            poller_modify(s->poller, c->fd, POLLER_WRITE) == 0) {
            c->want_write = true;
        } else {
            poller_remove(s->poller, c->fd);
            client_kill(c, "server shutdown");
        }
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    struct poller_event events[MAX_EVENTS];

    while (output_pending(s)) {
        long left = SHUTDOWN_FLUSH_MS - elapsed_ms(&start);
        if (left <= 0) {
            LOG_INFO("shutdown: undelivered output dropped");
            break;
        }
        int n = poller_wait(s->poller, events, MAX_EVENTS, (int)left);
        if (n == -1 && errno != EINTR)
            break;
        for (int i = 0; i < n; i++) {
            struct client *c = table_get(s, events[i].fd);
            if (c == NULL || c->dead)
                continue;
            if ((events[i].events & POLLER_ERROR) ||
                client_flush(c) != FLUSH_PENDING) {
                poller_remove(s->poller, c->fd);  /* done or failed */
                client_kill(c, "server shutdown");
            }
        }
    }
}

static void cleanup(struct server *s)
{
    while (s->head != NULL) {
        client_kill(s->head, "server shutdown");
        drop_client(s, s->head);
    }
    free(s->by_fd);
    restore_signals();
    for (int i = 0; i < 2; i++) {
        if (s->sig_pipe[i] != -1)
            close(s->sig_pipe[i]);
    }
    if (s->listen_fd != -1)
        close(s->listen_fd);
    if (s->spare_fd != -1)
        close(s->spare_fd);
    poller_destroy(s->poller);
}

int server_run(const struct server_config *cfg)
{
    struct server s;
    memset(&s, 0, sizeof s);
    s.cfg = *cfg;
    s.listen_fd = s.spare_fd = s.sig_pipe[0] = s.sig_pipe[1] = -1;

    int rc = -1;
    s.poller = poller_create();
    if (s.poller == NULL) {
        LOG_ERRNO("poller_create");
        goto out;
    }
    if (install_signals(&s) == -1)
        goto out;
    s.listen_fd = net_listen(cfg->port, BACKLOG);
    if (s.listen_fd == -1)
        goto out;
    s.spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);

    if (poller_add(s.poller, s.listen_fd, POLLER_READ) == -1 ||
        poller_add(s.poller, s.sig_pipe[0], POLLER_READ) == -1) {
        LOG_ERRNO("poller_add");
        goto out;
    }

    LOG_INFO("listening on port %s (backend: %s, max %d clients, "
             "rate %.1f msg/s burst %.0f, admin password %s)",
             cfg->port, poller_backend(), cfg->max_clients,
             cfg->msg_rate, cfg->msg_burst, cfg->admin_password ? "set" : "not set");

    rc = event_loop(&s);
    graceful_shutdown(&s);
    LOG_INFO("server stopped");
out:
    cleanup(&s);
    return rc;
}
