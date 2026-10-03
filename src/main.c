#include "server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s [-p port] [-m max_clients] [-o max_queue_bytes] [-h]\n"
            "  -p port             TCP port to listen on (default 65001)\n"
            "  -m max_clients      maximum simultaneous clients (default 1000)\n"
            "  -o max_queue_bytes  per-client output queue limit before a slow\n"
            "                      client is dropped (default 262144)\n"
            "  -h                  show this help\n",
            prog);
}

static bool parse_long(const char *s, long min, long max, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return false;
    *out = v;
    return true;
}

int main(int argc, char *argv[])
{
    struct server_config cfg = {
        .port        = "65001",
        .max_clients = 1000,
        .max_outbuf  = 256 * 1024,
    };
    long v;
    int opt;

    while ((opt = getopt(argc, argv, "p:m:o:h")) != -1) {
        switch (opt) {
        case 'p':
            if (!parse_long(optarg, 1, 65535, &v)) {
                fprintf(stderr, "%s: invalid port '%s'\n", argv[0], optarg);
                return EXIT_FAILURE;
            }
            cfg.port = optarg;
            break;
        case 'm':
            if (!parse_long(optarg, 1, 1000000, &v)) {
                fprintf(stderr, "%s: invalid max_clients '%s'\n", argv[0], optarg);
                return EXIT_FAILURE;
            }
            cfg.max_clients = (int)v;
            break;
        case 'o':
            if (!parse_long(optarg, 1024, 1L << 30, &v)) {
                fprintf(stderr, "%s: invalid max_queue_bytes '%s'\n", argv[0], optarg);
                return EXIT_FAILURE;
            }
            cfg.max_outbuf = (size_t)v;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], argv[optind]);
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    return server_run(&cfg) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
