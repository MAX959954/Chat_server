/*
 * Console client for chat_server.
 *
 *   chat_client [-r] [host [port]]
 *
 * One thread, poll() on stdin and the socket: whatever you type is sent to
 * the server line by line, and server lines are pretty-printed as they arrive
 * (-r prints them raw). End of input (Ctrl+D or the end of a pipe) sends /quit.
 */
#include "buffer.h"
#include "protocol.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int r = getaddrinfo(host, port, &hints, &res);
    if (r != 0) {
        fprintf(stderr, "%s:%s: %s\n", host, port, gai_strerror(r));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL && fd == -1; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd != -1 && connect(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
            close(fd);
            fd = -1;
        }
    }
    if (fd == -1)
        fprintf(stderr, "cannot connect to %s:%s: %s\n", host, port, strerror(errno));
    freeaddrinfo(res);
    return fd;
}

static bool send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

/* Split off up to `max` space-separated fields; the last one keeps the rest. */
static int split(char *s, char **f, int max)
{
    int n = 0;
    while (*s != '\0' && n < max) {
        f[n++] = s;
        if (n == max)
            break;
        char *sp = strchr(s, ' ');
        if (sp == NULL)
            break;
        *sp = '\0';
        s = sp + 1;
    }
    return n;
}

static struct buffer users;  /* USER lines collected until OK LIST */

static void print_ok(char **f, int n)
{
    if (n >= 3 && strcmp(f[1], "LIST") == 0) {
        printf("* Users in #%s: %.*s\n", f[2],
               (int)buffer_size(&users), buffer_peek(&users));
        buffer_consume(&users, buffer_size(&users));
    } else if (n >= 2 && strcmp(f[1], "ROOMS") == 0) {
        /* ROOM lines were already printed */
    } else if (n >= 3 && strcmp(f[1], "NICK") == 0) {
        printf("* You are now known as %s\n", f[2]);
    } else if (n >= 3 && strcmp(f[1], "JOIN") == 0) {
        printf("* You are in #%s\n", f[2]);
    } else if (n >= 3 && strcmp(f[1], "MSG") == 0) {
        printf("* Private message delivered to %s\n", f[2]);
    } else if (n >= 2 && strcmp(f[1], "QUIT") == 0) {
        printf("* Bye\n");
    } else if (n >= 2 && strcmp(f[1], "HELP") == 0) {
        /* the INFO lines were the help */
    } else {
        printf("* OK %s\n", n >= 2 ? f[1] : "");
    }
}

static void print_line(char *line, bool raw)
{
    protocol_sanitize(line);  /* never echo control sequences to the terminal */
    if (raw) {
        puts(line);
        return;
    }
    /* MSG <room> <nick> <text> and ERR <code> <name> <text> have three fixed
     * fields before the free text, every other line at most two */
    bool four = strncmp(line, "MSG ", 4) == 0 || strncmp(line, "ERR ", 4) == 0 ||
                strncmp(line, "OK ", 3) == 0;
    char *f[4];
    char copy[PROTO_MAX_LINE];
    snprintf(copy, sizeof copy, "%s", line);
    int n = split(copy, f, four ? 4 : 3);
    const char *t = n > 0 ? f[0] : "";

    if (strcmp(t, "MSG") == 0 && n == 4)
        printf("[#%s] %s: %s\n", f[1], f[2], f[3]);
    else if (strcmp(t, "PM") == 0 && n >= 3)
        printf("[private] %s: %s\n", f[1], f[2]);
    else if (strcmp(t, "JOIN") == 0 && n == 3)
        printf("* %s joined #%s\n", f[2], f[1]);
    else if (strcmp(t, "PART") == 0 && n == 3)
        printf("* %s left #%s\n", f[2], f[1]);
    else if (strcmp(t, "NICK") == 0 && n == 3)
        printf("* %s is now known as %s\n", f[1], f[2]);
    else if (strcmp(t, "QUIT") == 0 && n >= 2)
        printf("* %s left the chat (%s)\n", f[1], n >= 3 ? f[2] : "");
    else if (strcmp(t, "WELCOME") == 0 && n >= 3)
        printf("* Connected as %s in #%s\n", f[1], f[2]);
    else if (strcmp(t, "USER") == 0 && n == 3) {
        if (buffer_size(&users) > 0)
            buffer_append(&users, ", ", 2);
        buffer_append(&users, f[2], strlen(f[2]));
    } else if (strcmp(t, "ROOM") == 0 && n == 3)
        printf("* #%s (%s %s)\n", f[1], f[2], strcmp(f[2], "1") == 0 ? "user" : "users");
    else if (strcmp(t, "INFO") == 0)
        printf("* %s\n", strlen(line) > 5 ? line + 5 : "");
    else if (strcmp(t, "OK") == 0)
        print_ok(f, n);
    else if (strcmp(t, "ERR") == 0 && n == 4)
        printf("! %s (%s %s)\n", f[3], f[1], f[2]);
    else
        puts(line);
}

int main(int argc, char *argv[])
{
    bool raw = false;
    int opt;
    while ((opt = getopt(argc, argv, "r")) != -1) {
        if (opt != 'r') {
            fprintf(stderr, "Usage: %s [-r] [host [port]]\n", argv[0]);
            return EXIT_FAILURE;
        }
        raw = true;
    }
    const char *host = optind < argc ? argv[optind] : "localhost";
    const char *port = optind + 1 < argc ? argv[optind + 1] : "65001";

    signal(SIGPIPE, SIG_IGN);
    int sock = connect_to(host, port);
    if (sock == -1)
        return EXIT_FAILURE;

    struct line_reader in;
    line_reader_init(&in);
    buffer_init(&users);

    struct pollfd fds[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = sock,         .events = POLLIN },
    };

    for (;;) {
        if (poll(fds, 2, -1) == -1) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* keyboard -> server */
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buf[1024];
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n > 0) {
                if (!send_all(sock, buf, (size_t)n))
                    break;
            } else if (n == 0 || errno != EINTR) {
                send_all(sock, "\n/quit\n", 7);  /* end of input: leave politely */
                fds[0].fd = -1;                  /* poll() ignores negative fds */
            }
        }

        /* server -> screen */
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char  *dst;
            size_t space = line_reader_space(&in, &dst);
            ssize_t n = recv(sock, dst, space, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                printf("* Disconnected from server\n");
                break;
            }
            line_reader_commit(&in, (size_t)n);

            char  *line;
            size_t len;
            enum lr_status st;
            while ((st = line_reader_next(&in, &line, &len)) == LR_LINE)
                print_line(line, raw);
            if (st == LR_TOO_LONG) {  /* should not happen: server lines are bounded */
                fputs("! Overlong line from server dropped\n", stdout);
                line_reader_init(&in);
            }
            fflush(stdout);
        }
    }

    buffer_free(&users);
    close(sock);
    return EXIT_SUCCESS;
}
