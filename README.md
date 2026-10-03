# Chat Server (select()-based)

A single-threaded, multi-client TCP chat server written in C. Clients connect
over TCP, and any message one client sends is broadcast to every connected
client (including the sender, as an echo). Sending `shutdown` from a client on
localhost stops the server.

## Platform

POSIX only (Linux, WSL, macOS). The server uses BSD sockets, `select()` and
`unistd.h`, so it does not build with MSVC on Windows. On Windows, build and run
it inside WSL or Docker. CMake stops with a clear error on non-POSIX platforms.

## Build

```sh
cmake -B build
cmake --build build
```

or directly:

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -o chat_server CHAT_server.c
```

## Test

Integration tests (Linux/WSL, Python 3) start the server and check broadcast,
oversized messages, peer resets, slow clients, shutdown rules, fast restart,
fd exhaustion and the `FD_SETSIZE` limit:

```sh
ulimit -n 4096 && python3 tests/test_server.py ./build/chat_server
```

## Run

```sh
./build/chat_server          # listens on port 65001
./build/chat_server 7000     # custom port
```

Connect with `nc localhost 65001` (or `telnet localhost 65001`) from as many
terminals as you like.

## Design decisions

**`select()` over threads.** This server multiplexes all client sockets on a
single thread with `select()`, rather than spawning a thread per connection.
For a chat server, most sockets are idle most of the time. `select()` avoids
the cost and complexity of thread creation/teardown and needs no locking
around shared state (the connection table, the fd set), since only one thread
ever touches them. The tradeoff is that a slow client or an accidental
blocking call anywhere in the loop stalls every other client; a
thread-per-connection design trades that away for higher per-connection
overhead and mutex-protected shared state instead.

**Fixed-size client table indexed by fd, sized to `FD_SETSIZE`.** Each
client's state (display name, flags) is stored in arrays indexed directly by
file descriptor. `select()` cannot watch file descriptors `>= FD_SETSIZE`
(1024 on Linux), so the server explicitly rejects any accepted fd at or above
that limit with a "server is full" message instead of invoking undefined
behaviour in `FD_SET`. A production version would replace `select()` with
`poll()`/`epoll()` (no `FD_SETSIZE` ceiling) and the arrays with a dynamically
grown table.

**Deferred disconnects.** When a send or receive fails, the client is only
*marked* dead; it is closed at the end of the event-loop iteration. Closing
immediately would let `accept()` reuse that fd number while a loop is still
iterating over the old one. Announcing a disconnect can reveal further dead
clients, so reaping repeats until nothing changes.

**Text protocol, no framing.** Each `recv()` call is treated as one message.
This is simple but not correct in general: TCP is a byte stream, not a
message stream, so a single client message can arrive split across multiple
`recv()` calls, or multiple messages can arrive coalesced into one `recv()`.
There is no line-buffering layer yet. For interactive line-by-line chat over a
local network this rarely surfaces, but it is a known correctness gap.

## Robustness

- **No SIGPIPE crashes.** `SIGPIPE` is ignored and sends use `MSG_NOSIGNAL`, so
  writing to a client that has gone away returns `EPIPE` instead of killing the
  process.
- **Complete sends.** `send_all()` retries partial writes and `EINTR`; a client
  whose send fails is disconnected.
- **Bounded stall from slow clients.** Client sockets have a 2-second send
  timeout (`SO_SNDTIMEO`); a client that stops reading is dropped instead of
  blocking the server forever.
- **`accept()` failures are not fatal.** Transient errors are logged and the
  server keeps running. On fd exhaustion (`EMFILE`/`ENFILE`) the server frees a
  reserved descriptor, accepts and immediately closes the pending connection,
  then re-reserves it, so `select()` does not spin on a permanently readable
  listener.
- **Safe buffers.** `recv()` reads at most `sizeof(buffer) - 1` bytes so the
  terminating `'\0'` always fits; all formatting goes through bounded `snprintf`.
- **Fast restart.** The listener sets `SO_REUSEADDR`, so the server can be
  restarted immediately while old connections are in `TIME_WAIT`.
- **Restricted shutdown.** Only clients connected from a loopback address may
  issue `shutdown`; trailing `\r\n` is stripped so it works from both `nc` and
  `telnet`. On shutdown every client is notified and its socket closed.

## Known limitations

- No line-buffering: messages can be split or coalesced across `recv()` calls (see above).
- Blocking sends: a slow client can still stall everyone for up to the 2-second
  send timeout. The proper fix is non-blocking sockets with per-client output
  buffers.
- No authentication, encryption, or rate limiting; trusted-network use only.
- `select()` caps total simultaneous connections at `FD_SETSIZE` (1024 on Linux).
- IPv4 only (`hints.ai_family = AF_INET`).
