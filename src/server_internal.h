/*
 * Shared between the event loop (server.c) and the command handlers
 * (commands.c). Not part of the public server API.
 */
#ifndef CHAT_SERVER_INTERNAL_H
#define CHAT_SERVER_INTERNAL_H

#include "client.h"
#include "log.h"
#include "poller.h"
#include "protocol.h"
#include "server.h"

#include <stdbool.h>
#include <stddef.h>

struct server {
    struct server_config cfg;
    struct poller  *poller;
    int             listen_fd;
    int             spare_fd;      /* reserved fd for EMFILE recovery */
    int             sig_pipe[2];   /* self-pipe: signal handler -> event loop */
    struct client **by_fd;         /* fd -> client, grown on demand */
    size_t          by_fd_cap;
    struct client  *head;          /* list of all clients */
    int             nclients;
    unsigned        guest_counter; /* for default nicknames */
    bool            stopping;
};

/* ---- output (server.c) -----------------------------------------------------
 * All of these only queue data and try a non-blocking send; they never block
 * and never free a client (failures go through client_kill()). Lines are
 * formatted with printf syntax and get the trailing '\n' appended. */

void send_raw(struct server *s, struct client *c, const char *data, size_t len);
void send_line(struct server *s, struct client *c, const char *fmt, ...) CHAT_PRINTF(3, 4);
void send_ok(struct server *s, struct client *c, const char *fmt, ...) CHAT_PRINTF(3, 4);
void send_err(struct server *s, struct client *c, enum proto_error code,
              const char *fmt, ...) CHAT_PRINTF(4, 5);

/* To every live client in `room` (or every client if room is NULL),
 * except `except` (may be NULL). */
void send_room(struct server *s, const char *room, const struct client *except,
               const char *fmt, ...) CHAT_PRINTF(4, 5);

/* Case-insensitive nickname lookup among live clients. */
struct client *find_by_nick(struct server *s, const char *nick);

/* ---- session (commands.c) --------------------------------------------------- */

void chat_on_connect(struct server *s, struct client *c);
void chat_on_disconnect(struct server *s, const char *nick, const char *room,
                        const char *reason);
void chat_handle_line(struct server *s, struct client *c, char *line, size_t len);

#endif
