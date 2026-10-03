# chat_server protocol, version 1

A line-based text protocol over TCP, readable and typeable with `nc` or `telnet`.

## Framing

- Every message is one line terminated by `\n`. The server accepts `\r\n` from clients
  and always sends `\n`.
- **Maximum line length is 4096 bytes including the newline, in both directions.**
  A client line that exceeds it is a protocol violation: the server replies
  `ERR 413` and closes the connection.
- Text is treated as bytes; UTF-8 passes through unchanged. Control characters
  (bytes `0x00`–`0x1F` and `0x7F`) in chat text, private messages and quit
  reasons are replaced with `?` before they are relayed, so a client cannot inject
  terminal escape sequences into other users' screens. Tabs become spaces.
- Empty lines are ignored.

## Names

Nicknames and room names: 1–16 characters from `A–Z a–z 0–9 _ -`, starting with
a letter. Nicknames are unique **case-insensitively** (`Bob` and `bob` conflict).

On connect a client gets the first free nickname `guestN` and is placed in the
room `lobby`. Rooms exist as long as someone is in them. A client is in exactly
one room at a time.

## Client to server

| Line | Meaning | Success reply |
|---|---|---|
| `<text>` | chat message to the current room | your own `MSG` event |
| `//<text>` | chat message that starts with `/` | your own `MSG` event |
| `/nick <name>` | change nickname | `OK NICK <name>` |
| `/join <room>` | move to a room (created on demand) | `OK JOIN <room>` |
| `/list` | members of the current room | `USER` lines, then `OK LIST <room> <count>` |
| `/rooms` | all rooms | `ROOM` lines, then `OK ROOMS <count>` |
| `/msg <nick> <text>` | private message | `OK MSG <nick>` |
| `/quit [reason]` | leave; the server closes the connection | `OK QUIT` |
| `/shutdown [password]` | stop the server | `OK SHUTDOWN` |
| `/help` | list commands | `INFO` lines, then `OK HELP` |

Commands are lowercase. Chat and private message text is limited to 3900 bytes.

**Every slash command gets exactly one final reply: `OK ...` or `ERR ...`.**
Lines that belong to the reply (`USER`, `ROOM`, `INFO`) come before it. Events
from other users can be interleaved at any point.

### `/shutdown` authorization

Allowed if either:
- the connection comes from a loopback address (`127.0.0.0/8`, `::1`), or
- the server was started with `CHAT_ADMIN_PASSWORD` set and the command carries
  that password: `/shutdown <password>`.

The password is compared in constant time but travels in plain text, so use it
only on trusted networks.

### Rate limit

Each connection has a token bucket (default: 10 messages burst, refilled at
5 per second; server options `-b` and `-r`). Every non-empty line costs one token.
A line that arrives with no token left is dropped. The first dropped line of a
burst gets `ERR 429`; further drops are silent until a line is accepted again.

## Server to client

### Events

| Line | Sent to | Meaning |
|---|---|---|
| `WELCOME <nick> <room>` | new client | connection accepted |
| `MSG <room> <nick> <text>` | room, sender included | chat message |
| `PM <from> <text>` | recipient | private message |
| `JOIN <room> <nick>` | room, except the joiner | someone entered the room |
| `PART <room> <nick>` | old room, except the leaver | someone moved to another room |
| `NICK <old> <new>` | room, except the renamed user | nickname change |
| `QUIT <nick> <reason>` | room | someone disconnected |
| `INFO <text>` | one or all clients | human-readable notice |

`QUIT` reasons set by the server: `quit` (plain `/quit`), `connection closed`,
`connection error`, `protocol error` (line too long), `too slow` (output queue
overflow), `server shutdown`; or the client's own `/quit` reason.

### Replies

```
OK <COMMAND> [details]
ERR <code> <NAME> <human-readable message>
USER <room> <nick>              (part of a /list reply)
ROOM <room> <count>             (part of a /rooms reply)
```

### Error codes

| Code | Name | When | Connection |
|---|---|---|---|
| 400 | `BAD_REQUEST` | wrong arguments; the message contains the usage | kept |
| 403 | `FORBIDDEN` | `/shutdown` not allowed | kept |
| 404 | `NO_SUCH_NICK` | `/msg` to an unknown nickname | kept |
| 409 | `NICK_IN_USE` | `/nick` already taken | kept |
| 413 | `TOO_LONG` | message text over 3900 bytes | kept |
| 413 | `TOO_LONG` | line over 4096 bytes | **closed** |
| 421 | `UNKNOWN_COMMAND` | unknown `/command` | kept |
| 422 | `INVALID_NAME` | bad nickname or room name | kept |
| 429 | `RATE_LIMITED` | token bucket empty | kept |
| 503 | `SERVER_FULL` | client limit (`-m`) reached; sent instead of `WELCOME` | **closed** |

### Flow control

The server never blocks on a client. Output for each connection is queued; if
a client stops reading and its queue grows beyond the limit (`-o`, default
256 KiB), it is disconnected with reason `too slow`.

### Shutdown

On `/shutdown`, `SIGINT` or `SIGTERM` every client receives
`INFO Server is shutting down`. Queued output then gets up to one second to be
delivered before the connections are closed.

## Example session

```
S: WELCOME guest1 lobby
S: INFO Welcome to chat_server, guest1. Type /help for commands.
C: /nick alice
S: OK NICK alice
C: hello
S: MSG lobby alice hello
C: /msg bob hi there
S: OK MSG bob
C: /list
S: USER lobby bob
S: USER lobby alice
S: OK LIST lobby 2
C: /nick bob
S: ERR 409 NICK_IN_USE Nickname bob is already taken
S: MSG lobby bob hi everyone          (an event from someone else)
C: /quit bye
S: OK QUIT
   (connection closed; the room receives "QUIT alice bye")
```
