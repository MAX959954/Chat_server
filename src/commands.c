/*
 * Chat semantics: what each line from a client means.
 *
 * Every slash command gets exactly one reply, "OK ..." or "ERR ...".
 * Chat text is confirmed by the sender's own copy of the MSG event.
 * Events (MSG, PM, JOIN, PART, NICK, QUIT, INFO, ...) can arrive at any time.
 * See PROTOCOL.md.
 */
#include "server_internal.h"

#include "ratelimit.h"

#include <stdio.h>
#include <string.h>

static const char *const HELP[] = {
    "INFO Commands:",
    "INFO   <text>               send to everyone in your room",
    "INFO   /nick <name>         change your nickname",
    "INFO   /join <room>         switch to another room (created on demand)",
    "INFO   /list                users in your room",
    "INFO   /rooms               all rooms with user counts",
    "INFO   /msg <nick> <text>   private message",
    "INFO   /quit [reason]       leave the chat",
    "INFO   /shutdown [password] stop the server (localhost or admin password)",
    "INFO   //text               send text that starts with '/'",
};

/* Compare without exiting early on the first mismatch, so the time taken
 * does not reveal how many leading characters of a guess were right. */
static bool secure_equals(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned char)(a[i] ^ b[lb ? i % lb : 0]);
    return diff == 0;
}

/* ---- session ------------------------------------------------------------- */

void chat_on_connect(struct server *s, struct client *c)
{
    send_line(s, c, "WELCOME %s %s", c->nick, c->room);
    send_line(s, c, "INFO Welcome to chat_server, %s. Type /help for commands.", c->nick);
    send_room(s, c->room, c, "JOIN %s %s", c->room, c->nick);
}

void chat_on_disconnect(struct server *s, const char *nick, const char *room,
                        const char *reason)
{
    send_room(s, room, NULL, "QUIT %s %s", nick, reason);
}

/* ---- handlers -------------------------------------------------------------- */

static bool text_fits(struct server *s, struct client *c, const char *text)
{
    if (strlen(text) <= PROTO_MAX_TEXT)
        return true;
    send_err(s, c, ERR_TOO_LONG, "Message text is limited to %d bytes", PROTO_MAX_TEXT);
    return false;
}

static void cmd_message(struct server *s, struct client *c, char *text)
{
    if (!text_fits(s, c, text))
        return;
    protocol_sanitize(text);
    send_room(s, c->room, NULL, "MSG %s %s %s", c->room, c->nick, text);
    printf("[%s] %s: %s\n", c->room, c->nick, text);  /* transcript */
}

static void cmd_nick(struct server *s, struct client *c, const char *nick)
{
    if (!protocol_valid_name(nick)) {
        send_err(s, c, ERR_INVALID_NAME,
                 "Nicknames are 1-%d characters: letters, digits, '_' or '-', "
                 "starting with a letter", PROTO_NAME_MAX);
        return;
    }
    const struct client *owner = find_by_nick(s, nick);
    if (owner != NULL && owner != c) {  /* changing only the case is allowed */
        send_err(s, c, ERR_NICK_IN_USE, "Nickname %s is already taken", nick);
        return;
    }
    char old[sizeof c->nick];
    memcpy(old, c->nick, sizeof old);
    snprintf(c->nick, sizeof c->nick, "%s", nick);

    send_ok(s, c, "NICK %s", c->nick);
    if (strcmp(old, c->nick) != 0) {
        send_room(s, c->room, c, "NICK %s %s", old, c->nick);
        LOG_INFO("%s (%s) is now %s", old, c->addr, c->nick);
    }
}

static void cmd_join(struct server *s, struct client *c, const char *room)
{
    if (!protocol_valid_name(room)) {
        send_err(s, c, ERR_INVALID_NAME,
                 "Room names are 1-%d characters: letters, digits, '_' or '-', "
                 "starting with a letter", PROTO_NAME_MAX);
        return;
    }
    if (strcmp(room, c->room) != 0) {
        send_room(s, c->room, c, "PART %s %s", c->room, c->nick);
        snprintf(c->room, sizeof c->room, "%s", room);
        send_room(s, c->room, c, "JOIN %s %s", c->room, c->nick);
    }
    send_ok(s, c, "JOIN %s", c->room);
}

/* One USER line per member keeps every line short however big the room is. */
static void cmd_list(struct server *s, struct client *c)
{
    int count = 0;
    for (struct client *o = s->head; o != NULL; o = o->next) {
        if (!o->dead && strcmp(o->room, c->room) == 0) {
            send_line(s, c, "USER %s %s", c->room, o->nick);
            count++;
        }
    }
    send_ok(s, c, "LIST %s %d", c->room, count);
}

static void cmd_rooms(struct server *s, struct client *c)
{
    int rooms = 0;
    for (struct client *o = s->head; o != NULL; o = o->next) {
        if (o->dead)
            continue;
        /* count each room once, at its first member in the list */
        bool first = true;
        for (struct client *p = s->head; p != o; p = p->next) {
            if (!p->dead && strcmp(p->room, o->room) == 0) {
                first = false;
                break;
            }
        }
        if (!first)
            continue;
        int members = 0;
        for (struct client *p = o; p != NULL; p = p->next) {
            if (!p->dead && strcmp(p->room, o->room) == 0)
                members++;
        }
        send_line(s, c, "ROOM %s %d", o->room, members);
        rooms++;
    }
    send_ok(s, c, "ROOMS %d", rooms);
}

static void cmd_msg(struct server *s, struct client *c, const char *nick, char *text)
{
    struct client *to = find_by_nick(s, nick);
    if (to == NULL) {
        send_err(s, c, ERR_NO_SUCH_NICK, "No user named %s", nick);
        return;
    }
    if (!text_fits(s, c, text))
        return;
    protocol_sanitize(text);
    send_line(s, to, "PM %s %s", c->nick, text);
    send_ok(s, c, "MSG %s", to->nick);
}

static void cmd_quit(struct server *s, struct client *c, char *reason)
{
    protocol_sanitize(reason);
    send_ok(s, c, "QUIT");
    client_kill(c, reason[0] != '\0' ? reason : "quit");
}

static void cmd_shutdown(struct server *s, struct client *c, const char *password)
{
    bool by_password = password != NULL && s->cfg.admin_password != NULL &&
                       secure_equals(password, s->cfg.admin_password);
    if (!c->is_local && !by_password) {
        LOG_INFO("refused /shutdown from %s (%s)", c->nick, c->addr);
        send_err(s, c, ERR_FORBIDDEN,
                 "Shutdown requires a localhost connection or the admin password");
        return;
    }
    LOG_INFO("shutdown requested by %s (%s)%s", c->nick, c->addr,
             by_password ? " with admin password" : " from localhost");
    send_ok(s, c, "SHUTDOWN");
    s->stopping = true;
}

static void cmd_help(struct server *s, struct client *c)
{
    for (size_t i = 0; i < sizeof HELP / sizeof HELP[0]; i++)
        send_line(s, c, "%s", HELP[i]);
    send_ok(s, c, "HELP");
}

/* ---- dispatch ---------------------------------------------------------------- */

void chat_handle_line(struct server *s, struct client *c, char *line, size_t len)
{
    (void)len;
    if (line[0] == '\0')
        return;

    if (!tb_take(&c->rate, monotonic_ms())) {
        if (!c->rate_warned) {  /* one error per burst, not one per line */
            send_err(s, c, ERR_RATE_LIMITED,
                     "Too many messages, slow down (limit %.1f/s, burst %.0f)",
                     s->cfg.msg_rate, s->cfg.msg_burst);
            c->rate_warned = true;
        }
        return;
    }
    c->rate_warned = false;

    struct command cmd;
    protocol_parse(line, &cmd);
    if (cmd.usage != NULL) {
        send_err(s, c, ERR_BAD_REQUEST, "Usage: %s", cmd.usage);
        return;
    }

    switch (cmd.type) {
    case CMD_EMPTY:    break;
    case CMD_MESSAGE:  cmd_message(s, c, cmd.text);           break;
    case CMD_NICK:     cmd_nick(s, c, cmd.arg);               break;
    case CMD_JOIN:     cmd_join(s, c, cmd.arg);               break;
    case CMD_LIST:     cmd_list(s, c);                        break;
    case CMD_ROOMS:    cmd_rooms(s, c);                       break;
    case CMD_MSG:      cmd_msg(s, c, cmd.arg, cmd.text);      break;
    case CMD_QUIT:     cmd_quit(s, c, cmd.text);              break;
    case CMD_SHUTDOWN: cmd_shutdown(s, c, cmd.arg);           break;
    case CMD_HELP:     cmd_help(s, c);                        break;
    case CMD_UNKNOWN:
        protocol_sanitize(cmd.name);
        send_err(s, c, ERR_UNKNOWN_COMMAND, "Unknown command /%.32s, try /help", cmd.name);
        break;
    }
}
