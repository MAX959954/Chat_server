/*
 * Wire protocol: newline-delimited UTF-8/ASCII text.
 *
 *   - Every message is one line terminated by "\n" ("\r\n" is accepted).
 *   - A line longer than PROTO_MAX_LINE - 1 bytes is a protocol violation.
 *   - Lines starting with '/' are commands; everything else is chat text.
 *
 * TCP is a byte stream, so the line_reader reassembles lines that arrive split
 * across several recv() calls, and splits recv() data that holds several lines.
 */
#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <stddef.h>

enum { PROTO_MAX_LINE = 4096 };  /* including the terminating '\n' */

struct line_reader {
    size_t len;                  /* bytes stored in buf */
    size_t pos;                  /* start of the not-yet-returned data */
    char   buf[PROTO_MAX_LINE];
};

enum lr_status {
    LR_LINE,      /* a complete line was returned */
    LR_AGAIN,     /* need more bytes */
    LR_TOO_LONG   /* buffer full without a newline: protocol violation */
};

void line_reader_init(struct line_reader *lr);

/* Free space to recv() into; follow with line_reader_commit(). */
size_t line_reader_space(struct line_reader *lr, char **dst);
void   line_reader_commit(struct line_reader *lr, size_t n);

/* Return the next complete line, NUL-terminated, without "\n" / "\r\n".
 * The pointer stays valid until the next call on this reader. */
enum lr_status line_reader_next(struct line_reader *lr, char **line, size_t *len);

enum command_type {
    CMD_EMPTY,     /* blank line, ignored */
    CMD_MESSAGE,   /* chat text to broadcast */
    CMD_QUIT,      /* /quit */
    CMD_SHUTDOWN,  /* /shutdown */
    CMD_UNKNOWN    /* any other /command */
};

struct command {
    enum command_type type;
    const char       *text;  /* the whole line */
};

struct command protocol_parse(const char *line);

#endif
