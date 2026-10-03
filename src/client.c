#include "client.h"
#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

struct client *client_create(int fd, const char *name, bool is_local)
{
    struct client *c = calloc(1, sizeof *c);
    if (c == NULL)
        return NULL;
    c->fd = fd;
    c->is_local = is_local;
    snprintf(c->name, sizeof c->name, "%s", name);
    line_reader_init(&c->in);
    buffer_init(&c->out);
    return c;
}

void client_destroy(struct client *c)
{
    if (c == NULL)
        return;
    close(c->fd);
    buffer_free(&c->out);
    free(c);
}

int client_enqueue(struct client *c, const char *data, size_t len, size_t limit)
{
    if (len > limit || buffer_size(&c->out) > limit - len)
        return -1;
    return buffer_append(&c->out, data, len);
}

enum flush_result client_flush(struct client *c)
{
    while (buffer_size(&c->out) > 0) {
        ssize_t n = send(c->fd, buffer_peek(&c->out), buffer_size(&c->out),
                         MSG_NOSIGNAL);
        if (n > 0) {
            buffer_consume(&c->out, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return FLUSH_PENDING;
        return FLUSH_ERROR;  /* EPIPE, ECONNRESET, ... */
    }
    return FLUSH_DONE;
}
