#include "server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s [-p port] [-m max_clients] [-o max_queue_bytes]\n"
            "          [-r msgs_per_sec] [-b burst] [-h]\n"
            "  -p port             TCP port to listen on (default 65001)\n"
            "  -m max_clients      maximum simultaneous clients (default 1000)\n"
            "  -o max_queue_bytes  per-client output queue limit before a slow\n"
            "                      client is dropped (default 262144)\n"
            "  -r msgs_per_sec     sustained message rate per client (default 5)\n"
            "  -b burst            messages allowed in a burst (default 10)\n"
            "  -h                  show this help\n"
            "\n"
            "Environment:\n"
            "  CHAT_ADMIN_PASSWORD  if set, '/shutdown <password>' works from any\n"
            "                       address (localhost can always shut down)\n",
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

static bool parse_double(const char *s, double min, double max, double *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !(v >= min && v <= max))
        return false;
    *out = v;
    return true;
}

static int bad_option(const char *prog, const char *what, const char *value)
{
    fprintf(stderr, "%s: invalid %s '%s'\n", prog, what, value);
    return EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
    struct server_config cfg = {
        .port           = "65001",
        .max_clients    = 1000,
        .max_outbuf     = 256 * 1024,
        .msg_rate       = 5.0,
        .msg_burst      = 10.0,
        .admin_password = NULL,
    };
    long l;
    double d;
    int opt;

    while ((opt = getopt(argc, argv, "p:m:o:r:b:h")) != -1) {
        switch (opt) {
        case 'p':
            if (!parse_long(optarg, 1, 65535, &l))
                return bad_option(argv[0], "port", optarg);
            cfg.port = optarg;
            break;
        case 'm':
            if (!parse_long(optarg, 1, 1000000, &l))
                return bad_option(argv[0], "max_clients", optarg);
            cfg.max_clients = (int)l;
            break;
        case 'o':
            if (!parse_long(optarg, 1024, 1L << 30, &l))
                return bad_option(argv[0], "max_queue_bytes", optarg);
            cfg.max_outbuf = (size_t)l;
            break;
        case 'r':
            if (!parse_double(optarg, 0.01, 100000.0, &d))
                return bad_option(argv[0], "msgs_per_sec", optarg);
            cfg.msg_rate = d;
            break;
        case 'b':
            if (!parse_double(optarg, 1.0, 100000.0, &d))
                return bad_option(argv[0], "burst", optarg);
            cfg.msg_burst = d;
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

    /* from the environment, not argv: command lines are visible in `ps` */
    const char *pw = getenv("CHAT_ADMIN_PASSWORD");
    if (pw != NULL && pw[0] != '\0')
        cfg.admin_password = pw;

    return server_run(&cfg) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
