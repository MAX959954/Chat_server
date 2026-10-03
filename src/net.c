#ifdef __linux__
#define _GNU_SOURCE  /* accept4() */
#endif

#include "net.h"
#include "log.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int net_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int net_listen(const char *port, int backlog)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    int r = getaddrinfo(NULL, port, &hints, &res);
    if (r != 0) {
        LOG_ERROR("getaddrinfo(port %s): %s", port, gai_strerror(r));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == -1) {
            LOG_ERRNO("socket");
            continue;
        }
        int yes = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == -1)
            LOG_ERRNO("setsockopt(SO_REUSEADDR)");

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(fd, backlog) == 0 &&
            net_set_nonblocking(fd) == 0 &&
            net_set_cloexec(fd) == 0)
            break;

        LOG_ERRNO("bind/listen");
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int net_accept(int listen_fd, struct sockaddr_storage *addr, socklen_t *len)
{
#ifdef __linux__
    return accept4(listen_fd, (struct sockaddr *)addr, len,
                   SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    int fd = accept(listen_fd, (struct sockaddr *)addr, len);
    if (fd != -1 && (net_set_nonblocking(fd) == -1 || net_set_cloexec(fd) == -1)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
#endif
}

bool net_is_loopback(const struct sockaddr_storage *addr)
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

void net_peer_name(const struct sockaddr_storage *addr, socklen_t len,
                   char *out, size_t out_size)
{
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    int r = getnameinfo((const struct sockaddr *)addr, len,
                        host, sizeof host, serv, sizeof serv,
                        NI_NUMERICHOST | NI_NUMERICSERV);
    if (r != 0) {
        LOG_ERROR("getnameinfo: %s", gai_strerror(r));
        snprintf(out, out_size, "unknown");
        return;
    }
    if (addr->ss_family == AF_INET6)
        snprintf(out, out_size, "[%s]:%s", host, serv);
    else
        snprintf(out, out_size, "%s:%s", host, serv);
}
