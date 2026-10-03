/*
 * Single-threaded, select()-based multi-client TCP chat server.
 * POSIX only (Linux / WSL / macOS) - see README.md.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Linux suppresses SIGPIPE per call with MSG_NOSIGNAL; other systems rely on
 * SIGPIPE being ignored process-wide (done in main). */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
    NAME_SIZE        = 64,   /* fits any numeric IPv4/IPv6 address */
    MSG_SIZE         = 4096, /* max bytes accepted per recv() */
    BACKLOG          = 10,   /* pending connections passed to listen() */
    SEND_TIMEOUT_SEC = 2     /* a client that blocks send() longer is dropped */
};

static const char *const DEFAULT_PORT = "65001";

struct server {
    int    listen_fd;
    int    spare_fd;                      /* reserved fd for EMFILE recovery */
    int    max_fd;                        /* highest fd in `active` */
    fd_set active;                        /* listener + all live clients */
    bool   dead[FD_SETSIZE];              /* scheduled for disconnect */
    bool   is_local[FD_SETSIZE];          /* connected from loopback */
    char   name[FD_SETSIZE][NAME_SIZE];   /* display name, indexed by fd */
};

/* ------------------------------------------------------------------------ */

/* Send the whole buffer, retrying on partial writes and EINTR.
 * Returns false if the peer is gone or blocked longer than SEND_TIMEOUT_SEC. */
static bool send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;  /* EPIPE, ECONNRESET, EAGAIN (send timeout), ... */
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

/* A failed client is only marked here and closed later in reap_dead(), so fd
 * numbers are never closed (and reused by accept()) in the middle of a loop
 * that is iterating over them. */
static void mark_dead(struct server *s, int fd)
{
    s->dead[fd] = true;
}

static void broadcast(struct server *s, const char *msg, int except_fd)
{
    size_t len = strlen(msg);
    for (int fd = 0; fd <= s->max_fd; fd++) {
        if (fd == s->listen_fd || fd == except_fd)
            continue;
        if (!FD_ISSET(fd, &s->active) || s->dead[fd])
            continue;
        if (!send_all(fd, msg, len))
            mark_dead(s, fd);
    }
    fputs(msg, stdout);
}

/* Close every client marked dead and announce it. Announcing can itself
 * discover more dead clients, so repeat until nothing changes. */
static void reap_dead(struct server *s)
{
    bool again = true;
    while (again) {
        again = false;
        for (int fd = 0; fd <= s->max_fd; fd++) {
            if (!s->dead[fd])
                continue;
            s->dead[fd] = false;
            FD_CLR(fd, &s->active);
            close(fd);

            char msg[NAME_SIZE + 32];
            snprintf(msg, sizeof msg, "SERVER> %s disconnected\n", s->name[fd]);
            broadcast(s, msg, -1);
            again = true;
        }
    }
    while (s->max_fd > s->listen_fd && !FD_ISSET(s->max_fd, &s->active))
        s->max_fd--;
}

static bool is_loopback(const struct sockaddr_storage *addr)
{
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        return (ntohl(in->sin_addr.s_addr) >> 24) == 127;  /* 127.0.0.0/8 */
    }
    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
        return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
    }
    return false;
}

/* ------------------------------------------------------------------------ */

static int open_listener(const char *port)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;      /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */
    hints.ai_flags    = AI_PASSIVE;   /* bind to all interfaces */

    int r = getaddrinfo(NULL, port, &hints, &res);
    if (r != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(r));  /* not errno */
        return -1;
    }

    int fd = -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == -1) {
            perror("socket");
            continue;
        }
        /* allow an immediate restart while old connections sit in TIME_WAIT */
        int yes = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == -1)
            perror("setsockopt(SO_REUSEADDR)");

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, BACKLOG) == 0)
            break;

        perror("bind/listen");
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Out of file descriptors: the pending connection keeps the listener readable,
 * so select() would spin forever. Free the reserved fd, accept the connection
 * just to close it, then reserve the fd again. */
static void shed_connection(struct server *s)
{
    if (s->spare_fd != -1) {
        close(s->spare_fd);
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd != -1)
            close(fd);
    }
    s->spare_fd = open("/dev/null", O_RDONLY);
    fputs("SERVER> out of file descriptors, connection rejected\n", stderr);
}

static void accept_client(struct server *s)
{
    struct sockaddr_storage addr;     /* large enough for any address family */
    socklen_t addr_len = sizeof addr; /* value-result: reset on every call */

    int fd = accept(s->listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd == -1) {
        if (errno == EMFILE || errno == ENFILE)
            shed_connection(s);
        else if (errno != EINTR && errno != ECONNABORTED && errno != EAGAIN)
            perror("accept");
        return;  /* one failed accept must not take the server down */
    }

    /* select() cannot watch fds >= FD_SETSIZE; FD_SET on one is UB and it
     * would also overflow the per-fd tables in struct server. */
    if (fd >= FD_SETSIZE) {
        static const char full[] = "SERVER> Server is full, try again later\n";
        send_all(fd, full, sizeof full - 1);
        close(fd);
        return;
    }

    /* bound how long a non-reading client can stall the whole server */
    struct timeval tv = { .tv_sec = SEND_TIMEOUT_SEC, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == -1)
        perror("setsockopt(SO_SNDTIMEO)");

    int r = getnameinfo((struct sockaddr *)&addr, addr_len,
                        s->name[fd], sizeof s->name[fd],
                        NULL, 0, NI_NUMERICHOST);
    if (r != 0) {
        fprintf(stderr, "getnameinfo: %s\n", gai_strerror(r));
        snprintf(s->name[fd], sizeof s->name[fd], "unknown-%d", fd);
    }
    s->is_local[fd] = is_loopback(&addr);
    s->dead[fd] = false;

    FD_SET(fd, &s->active);
    if (fd > s->max_fd)
        s->max_fd = fd;

    char msg[NAME_SIZE + 128];
    snprintf(msg, sizeof msg,
             "SERVER> Welcome %s to the chat server\n"
             "SERVER> Type 'shutdown' to stop the server (localhost only)\n",
             s->name[fd]);
    if (!send_all(fd, msg, strlen(msg)))
        mark_dead(s, fd);

    snprintf(msg, sizeof msg, "SERVER> %s has joined the server\n", s->name[fd]);
    broadcast(s, msg, fd);
}

/* Strip trailing "\r\n" / "\n" so commands work from both nc and telnet. */
static void chomp(char *str)
{
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r'))
        str[--len] = '\0';
}

/* Returns true if the client asked for (and is allowed to request) shutdown. */
static bool handle_client(struct server *s, int fd)
{
    char buf[MSG_SIZE];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);  /* leave room for '\0' */
    if (n == 0) {                                   /* orderly close */
        mark_dead(s, fd);
        return false;
    }
    if (n < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            mark_dead(s, fd);
        return false;
    }
    buf[n] = '\0';

    char cmd[MSG_SIZE];
    memcpy(cmd, buf, (size_t)n + 1);
    chomp(cmd);

    if (strcmp(cmd, "shutdown") == 0) {
        if (s->is_local[fd])
            return true;
        static const char denied[] =
            "SERVER> 'shutdown' is only allowed from localhost\n";
        if (!send_all(fd, denied, sizeof denied - 1))
            mark_dead(s, fd);
        return false;
    }

    char out[NAME_SIZE + MSG_SIZE + 4];
    snprintf(out, sizeof out, "%s> %s", s->name[fd], buf);
    broadcast(s, out, -1);
    return false;
}

static void shutdown_server(struct server *s)
{
    broadcast(s, "SERVER> Shutdown issued; closing all connections\n", -1);
    for (int fd = 0; fd <= s->max_fd; fd++) {
        if (fd != s->listen_fd && FD_ISSET(fd, &s->active))
            close(fd);
    }
    close(s->listen_fd);
    if (s->spare_fd != -1)
        close(s->spare_fd);
}

/* ------------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char *port = argc > 1 ? argv[1] : DEFAULT_PORT;

    /* writing to a socket whose peer has gone away must return EPIPE,
     * not kill the whole process */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction(SIGPIPE)");
        return EXIT_FAILURE;
    }

    static struct server s;  /* ~70 KB of per-fd tables: keep it off the stack */
    s.listen_fd = open_listener(port);
    if (s.listen_fd == -1)
        return EXIT_FAILURE;
    if (s.listen_fd >= FD_SETSIZE) {
        fputs("listener fd exceeds FD_SETSIZE\n", stderr);
        return EXIT_FAILURE;
    }
    s.spare_fd = open("/dev/null", O_RDONLY);

    FD_ZERO(&s.active);
    FD_SET(s.listen_fd, &s.active);
    s.max_fd = s.listen_fd;

    printf("Chat server is listening on port %s...\n", port);
    fflush(stdout);

    bool done = false;
    while (!done) {
        fd_set readable = s.active;  /* select() overwrites its argument */

        if (select(s.max_fd + 1, &readable, NULL, NULL, NULL) == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        for (int fd = 0; fd <= s.max_fd && !done; fd++) {
            if (!FD_ISSET(fd, &readable))
                continue;
            if (fd == s.listen_fd)
                accept_client(&s);
            else if (!s.dead[fd])
                done = handle_client(&s, fd);
        }
        reap_dead(&s);
        fflush(stdout);
    }

    shutdown_server(&s);
    return EXIT_SUCCESS;
}
