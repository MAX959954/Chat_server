#ifndef CHAT_SERVER_H
#define CHAT_SERVER_H

#include <stddef.h>

struct server_config {
    const char *port;
    int         max_clients;
    size_t      max_outbuf;      /* per-client output queue limit, bytes */
    double      msg_rate;        /* sustained messages per second per client */
    double      msg_burst;       /* messages allowed in a burst */
    const char *admin_password;  /* enables /shutdown <password>; NULL = off */
};

/* Runs until /shutdown, SIGINT or SIGTERM.
 * Returns 0 on clean shutdown, -1 if the server could not start. */
int server_run(const struct server_config *cfg);

#endif
