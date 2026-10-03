/*
 * Wire protocol (see PROTOCOL.md for the full specification).
 *
 *   - Newline-delimited text; "\r\n" is accepted from clients.
 *   - Every line, in both directions, is at most PROTO_MAX_LINE bytes
 *     including the terminating '\n'. A longer client line is a protocol
 *     violation; the line_reader detects it.
 *   - Lines starting with '/' are commands, everything else is chat text.
 *
 * TCP is a byte stream, so the line_reader reassembles lines that arrive split
 * across several recv() calls, and splits recv() data that holds several lines.
 */
#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

enum {
    PROTO_MAX_LINE = 4096,  /* including the terminating '\n' */
    PROTO_MAX_TEXT = 3900,  /* chat / private message text, bytes */
    PROTO_NAME_MAX = 16     /* nicknames and room names */
};

#define PROTO_DEFAULT_ROOM "lobby"

/* ---- framing ------------------------------------------------------------ */

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
 * The line may be modified in place by the caller. The pointer stays valid
 * until the next call on this reader. */
enum lr_status line_reader_next(struct line_reader *lr, char **line, size_t *len);

/* ---- error codes ---------------------------------------------------------- */

enum proto_error {
    ERR_BAD_REQUEST     = 400,  /* wrong arguments for a command */
    ERR_FORBIDDEN       = 403,  /* /shutdown without permission */
    ERR_NO_SUCH_NICK    = 404,  /* /msg to an unknown nickname */
    ERR_NICK_IN_USE     = 409,  /* /nick already taken */
    ERR_TOO_LONG        = 413,  /* line or message text too long */
    ERR_UNKNOWN_COMMAND = 421,
    ERR_INVALID_NAME    = 422,  /* bad nickname or room name */
    ERR_RATE_LIMITED    = 429,
    ERR_SERVER_FULL     = 503
};

const char *proto_error_name(enum proto_error code);

/* ---- commands ------------------------------------------------------------- */

enum command_type {
    CMD_EMPTY,     /* blank line, ignored */
    CMD_MESSAGE,   /* chat text for the current room */
    CMD_NICK,      /* /nick <name> */
    CMD_JOIN,      /* /join <room> */
    CMD_LIST,      /* /list */
    CMD_ROOMS,     /* /rooms */
    CMD_MSG,       /* /msg <nick> <text> */
    CMD_QUIT,      /* /quit [reason] */
    CMD_SHUTDOWN,  /* /shutdown [password] */
    CMD_HELP,      /* /help */
    CMD_UNKNOWN    /* any other /command */
};

struct command {
    enum command_type type;
    char       *name;   /* command word without '/', e.g. "nick" */
    char       *arg;    /* first argument, or NULL */
    char       *text;   /* chat text, message body or reason; "" if none */
    const char *usage;  /* non-NULL if the arguments are wrong */
};

/* Parse a line in place (it is split with NUL bytes). */
void protocol_parse(char *line, struct command *cmd);

/* Nicknames and rooms: 1-16 chars of [A-Za-z0-9_-], starting with a letter. */
bool protocol_valid_name(const char *s);

/* Replace control characters (including ESC) with '?' so one client cannot
 * inject terminal escape sequences into another client's screen. */
void protocol_sanitize(char *s);

#endif
