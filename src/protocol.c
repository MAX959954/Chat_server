#include "protocol.h"

#include <string.h>

void line_reader_init(struct line_reader *lr)
{
    lr->len = 0;
    lr->pos = 0;
}

/* Move the unread tail to the front of the buffer. */
static void compact(struct line_reader *lr)
{
    if (lr->pos == 0)
        return;
    memmove(lr->buf, lr->buf + lr->pos, lr->len - lr->pos);
    lr->len -= lr->pos;
    lr->pos = 0;
}

size_t line_reader_space(struct line_reader *lr, char **dst)
{
    compact(lr);
    *dst = lr->buf + lr->len;
    return sizeof lr->buf - lr->len;
}

void line_reader_commit(struct line_reader *lr, size_t n)
{
    lr->len += n;
}

enum lr_status line_reader_next(struct line_reader *lr, char **line, size_t *len)
{
    char  *start = lr->buf + lr->pos;
    size_t avail = lr->len - lr->pos;
    char  *nl = memchr(start, '\n', avail);

    if (nl == NULL) {
        compact(lr);
        return lr->len == sizeof lr->buf ? LR_TOO_LONG : LR_AGAIN;
    }

    size_t n = (size_t)(nl - start);
    lr->pos += n + 1;
    if (n > 0 && start[n - 1] == '\r')
        n--;
    start[n] = '\0';

    *line = start;
    *len = n;
    return LR_LINE;
}

struct command protocol_parse(const char *line)
{
    struct command cmd = { CMD_MESSAGE, line };

    if (line[0] == '\0')
        cmd.type = CMD_EMPTY;
    else if (line[0] == '/') {
        if (strcmp(line, "/quit") == 0)
            cmd.type = CMD_QUIT;
        else if (strcmp(line, "/shutdown") == 0)
            cmd.type = CMD_SHUTDOWN;
        else
            cmd.type = CMD_UNKNOWN;
    }
    return cmd;
}
