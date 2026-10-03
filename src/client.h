/* Per-connection state: framing buffer for input, queue for output. */
#ifndef CHAT_CLIENT_H
#define CHAT_CLIENT_H

#include "buffer.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>

enum { CLIENT_NAME_SIZE = 64 };

struct client {
    int                fd;
    bool               is_local;    /* connected from a loopback address */
    bool               dead;        /* scheduled for disconnect */
    bool               want_write;  /* write readiness currently requested */
    char               name[CLIENT_NAME_SIZE];
    struct line_reader in;
    struct buffer      out;
    struct client     *prev, *next; /* intrusive list of all clients */
};

enum flush_result {
    FLUSH_DONE,     /* output queue is empty */
    FLUSH_PENDING,  /* kernel buffer full, wait for write readiness */
    FLUSH_ERROR     /* peer gone or socket error */
};

/* Takes ownership of fd. Returns NULL on allocation failure (fd untouched). */
struct client *client_create(int fd, const char *name, bool is_local);

/* Closes the socket and frees the client. */
void client_destroy(struct client *c);

/* Queue bytes for sending. Returns -1 if the queue would exceed `limit`
 * bytes (a client that can't keep up) or memory runs out. */
int client_enqueue(struct client *c, const char *data, size_t len, size_t limit);

/* Write as much queued output as the socket accepts without blocking. */
enum flush_result client_flush(struct client *c);

#endif
