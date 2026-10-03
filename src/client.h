/* Per-connection state: identity, framing buffer, output queue, rate limit. */
#ifndef CHAT_CLIENT_H
#define CHAT_CLIENT_H

#include "buffer.h"
#include "protocol.h"
#include "ratelimit.h"

#include <stdbool.h>
#include <stddef.h>

enum {
    CLIENT_ADDR_SIZE   = 64,
    CLIENT_REASON_SIZE = 128
};

struct client {
    int                 fd;
    bool                is_local;     /* connected from a loopback address */
    bool                dead;         /* scheduled for disconnect */
    bool                want_write;   /* write readiness currently requested */
    bool                rate_warned;  /* RATE_LIMITED already sent this burst */
    char                addr[CLIENT_ADDR_SIZE];        /* "ip:port" */
    char                nick[PROTO_NAME_MAX + 1];
    char                room[PROTO_NAME_MAX + 1];
    char                quit_reason[CLIENT_REASON_SIZE];
    struct token_bucket rate;
    struct line_reader  in;
    struct buffer       out;
    struct client      *prev, *next;  /* intrusive list of all clients */
};

enum flush_result {
    FLUSH_DONE,     /* output queue is empty */
    FLUSH_PENDING,  /* kernel buffer full, wait for write readiness */
    FLUSH_ERROR     /* peer gone or socket error */
};

/* Takes ownership of fd. Returns NULL on allocation failure (fd untouched). */
struct client *client_create(int fd, const char *addr, bool is_local);

/* Closes the socket and frees the client. */
void client_destroy(struct client *c);

/* Queue bytes for sending. Returns -1 if the queue would exceed `limit`
 * bytes (a client that can't keep up) or memory runs out. */
int client_enqueue(struct client *c, const char *data, size_t len, size_t limit);

/* Write as much queued output as the socket accepts without blocking. */
enum flush_result client_flush(struct client *c);

/* Schedule a disconnect; the first reason given is the one reported. */
void client_kill(struct client *c, const char *reason);

#endif
