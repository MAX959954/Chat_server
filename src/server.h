#ifndef CHAT_SERVER_H
#define CHAT_SERVER_H

#include <stddef.h>

struct server_config {
    const char *port;
    int         max_clients;
    size_t      max_outbuf;  /* per-client output queue limit, bytes */
};

/* Runs until /shutdown from localhost, SIGINT or SIGTERM.
 * Returns 0 on clean shutdown, -1 if the server could not start. */
int server_run(const struct server_config *cfg);

#endif
