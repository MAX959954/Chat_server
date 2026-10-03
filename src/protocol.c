#include "protocol.h"

#include <string.h>

/* ---- framing ------------------------------------------------------------ */

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

/* ---- error codes ---------------------------------------------------------- */

const char *proto_error_name(enum proto_error code)
{
    switch (code) {
    case ERR_BAD_REQUEST:     return "BAD_REQUEST";
    case ERR_FORBIDDEN:       return "FORBIDDEN";
    case ERR_NO_SUCH_NICK:    return "NO_SUCH_NICK";
    case ERR_NICK_IN_USE:     return "NICK_IN_USE";
    case ERR_TOO_LONG:        return "TOO_LONG";
    case ERR_UNKNOWN_COMMAND: return "UNKNOWN_COMMAND";
    case ERR_INVALID_NAME:    return "INVALID_NAME";
    case ERR_RATE_LIMITED:    return "RATE_LIMITED";
    case ERR_SERVER_FULL:     return "SERVER_FULL";
    }
    return "ERROR";
}

/* ---- commands ------------------------------------------------------------- */

enum arg_spec {
    ARGS_NONE,       /* /list */
    ARGS_ONE,        /* /nick <name> */
    ARGS_OPT_ONE,    /* /shutdown [password] */
    ARGS_ONE_TEXT,   /* /msg <nick> <text> */
    ARGS_OPT_TEXT    /* /quit [reason] */
};

static const struct {
    const char       *name;
    enum command_type type;
    enum arg_spec     args;
    const char       *usage;
} COMMANDS[] = {
    { "nick",     CMD_NICK,     ARGS_ONE,      "/nick <name>" },
    { "join",     CMD_JOIN,     ARGS_ONE,      "/join <room>" },
    { "list",     CMD_LIST,     ARGS_NONE,     "/list" },
    { "rooms",    CMD_ROOMS,    ARGS_NONE,     "/rooms" },
    { "msg",      CMD_MSG,      ARGS_ONE_TEXT, "/msg <nick> <text>" },
    { "quit",     CMD_QUIT,     ARGS_OPT_TEXT, "/quit [reason]" },
    { "shutdown", CMD_SHUTDOWN, ARGS_OPT_ONE,  "/shutdown [password]" },
    { "help",     CMD_HELP,     ARGS_NONE,     "/help" },
};

static char *skip_spaces(char *p)
{
    while (*p == ' ')
        p++;
    return p;
}

/* Cut the first space-delimited word off *p; returns it (NULL if none). */
static char *next_word(char **p)
{
    char *s = skip_spaces(*p);
    if (*s == '\0') {
        *p = s;
        return NULL;
    }
    char *e = s;
    while (*e != '\0' && *e != ' ')
        e++;
    if (*e == ' ')
        *e++ = '\0';
    *p = e;
    return s;
}

void protocol_parse(char *line, struct command *cmd)
{
    static char empty[] = "";
    cmd->name = empty;
    cmd->arg = NULL;
    cmd->text = empty;
    cmd->usage = NULL;

    if (line[0] == '\0') {
        cmd->type = CMD_EMPTY;
        return;
    }
    if (line[0] != '/' || line[1] == '/') {
        /* plain text; a leading "//" sends a literal "/" */
        cmd->type = CMD_MESSAGE;
        cmd->text = line[0] == '/' ? line + 1 : line;
        return;
    }

    char *p = line + 1;
    char *word = next_word(&p);
    cmd->name = word ? word : empty;
    cmd->type = CMD_UNKNOWN;

    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        if (word == NULL || strcmp(word, COMMANDS[i].name) != 0)
            continue;

        cmd->type = COMMANDS[i].type;
        bool ok = true;
        switch (COMMANDS[i].args) {
        case ARGS_NONE:
            ok = *skip_spaces(p) == '\0';
            break;
        case ARGS_ONE:
            cmd->arg = next_word(&p);
            ok = cmd->arg != NULL && *skip_spaces(p) == '\0';
            break;
        case ARGS_OPT_ONE:
            cmd->arg = next_word(&p);
            ok = *skip_spaces(p) == '\0';
            break;
        case ARGS_ONE_TEXT:
            cmd->arg = next_word(&p);
            cmd->text = skip_spaces(p);
            ok = cmd->arg != NULL && cmd->text[0] != '\0';
            break;
        case ARGS_OPT_TEXT:
            cmd->text = skip_spaces(p);
            break;
        }
        if (!ok)
            cmd->usage = COMMANDS[i].usage;
        return;
    }
}

bool protocol_valid_name(const char *s)
{
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        char ch = s[n];
        bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        bool digit = ch >= '0' && ch <= '9';
        if (n == 0 ? !letter : !(letter || digit || ch == '_' || ch == '-'))
            return false;
        if (n >= PROTO_NAME_MAX)
            return false;
    }
    return n > 0;
}

void protocol_sanitize(char *s)
{
    for (; *s != '\0'; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '\t')
            *s = ' ';
        else if (ch < 0x20 || ch == 0x7f)
            *s = '?';
    }
}
