/* Thin helpers over BSD sockets. */
#ifndef CHAT_NET_H
#define CHAT_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

/* Linux suppresses SIGPIPE per call; elsewhere SIGPIPE is ignored globally. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Non-blocking, close-on-exec IPv4 listener with SO_REUSEADDR.
 * Returns the fd, or -1 (the reason is already logged). */
int  net_listen(const char *port, int backlog);

/* accept() that returns a non-blocking, close-on-exec socket. */
int  net_accept(int listen_fd, struct sockaddr_storage *addr, socklen_t *len);

int  net_set_nonblocking(int fd);
int  net_set_cloexec(int fd);

bool net_is_loopback(const struct sockaddr_storage *addr);

/* "1.2.3.4:5678" or "[::1]:5678"; "unknown" if the address can't be printed. */
void net_peer_name(const struct sockaddr_storage *addr, socklen_t len,
                   char *out, size_t out_size);

#endif
