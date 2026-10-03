# chat_server

A single-threaded, event-driven, multi-client TCP chat server in C11, with its
own console client.

- **Chat:** nicknames, rooms, private messages, user and room lists, over a
  documented line-based protocol with error codes ([PROTOCOL.md](PROTOCOL.md))
- **I/O:** Linux epoll in edge-triggered mode, with a portable `poll()` backend
  selectable at build time; non-blocking sockets throughout
- **Robustness:** per-client output queues with backpressure (a client that stops
  reading is disconnected instead of stalling everyone), line framing over the
  TCP byte stream, a token-bucket rate limit per client, survives fd exhaustion
  (`EMFILE`) and peer resets (`SIGPIPE`)
- **Security:** bounded memory per client, control-character sanitizing (no
  terminal escape injection), `/shutdown` only from localhost or with an admin password
- **Operations:** graceful shutdown on `SIGINT`/`SIGTERM` (self-pipe), timestamped logs
- **Quality:** unit and integration tests (72 checks), ASan/UBSan build, clean under
  `-Wall -Wextra -Wpedantic` with gcc and clang

## Platform

POSIX only (Linux, WSL, macOS). On Windows, build and run it inside WSL or
Docker; CMake stops with a clear error on non-POSIX platforms.

## Build and run

```sh
cmake -B build
cmake --build build
./build/chat_server              # terminal 1
./build/chat_client              # terminals 2, 3, ... (or: nc localhost 65001)
```

```
[#lobby] alice: hi everyone
* bob is now known as robert
[private] carol: lunch?
* Users in #lobby: robert, alice, carol
! Nickname alice is already taken (409 NICK_IN_USE)
```

Server options:

```
Usage: chat_server [-p port] [-m max_clients] [-o max_queue_bytes]
                   [-r msgs_per_sec] [-b burst] [-h]
  -p port             TCP port to listen on (default 65001)
  -m max_clients      maximum simultaneous clients (default 1000)
  -o max_queue_bytes  per-client output queue limit before a slow
                      client is dropped (default 262144)
  -r msgs_per_sec     sustained message rate per client (default 5)
  -b burst            messages allowed in a burst (default 10)

Environment:
  CHAT_ADMIN_PASSWORD  enables '/shutdown <password>' from any address
```

The password comes from the environment rather than a flag because command
lines are visible to every user in `ps`.

Client: `chat_client [-r] [host [port]]`. `-r` prints raw protocol lines.
End of input (Ctrl+D) sends `/quit`, so it also works in scripts:
`echo "deploy finished" | chat_client`.

Build options:

| Option | Values | Default |
|---|---|---|
| `CHAT_POLLER` | `auto`, `epoll`, `poll` | `auto` (epoll on Linux, poll elsewhere) |
| `CHAT_SANITIZE` | `ON`/`OFF` | `OFF` (AddressSanitizer + UndefinedBehaviorSanitizer) |

## Commands

| Command | |
|---|---|
| `<text>` | message to everyone in your room |
| `/nick <name>` | change nickname |
| `/join <room>` | switch rooms (created on demand) |
| `/list`, `/rooms` | users in your room, all rooms |
| `/msg <nick> <text>` | private message |
| `/quit [reason]` | leave |
| `/shutdown [password]` | stop the server (localhost or admin password) |
| `/help` | list commands |

The full wire format, error codes and limits are in [PROTOCOL.md](PROTOCOL.md).

## Tests

```sh
cmake -B build -DCHAT_SANITIZE=ON
cmake --build build
cd build && ctest --output-on-failure
```

- `unit_buffer`, `unit_protocol`, `unit_ratelimit`: C unit tests for the output
  queue, line framing, command parsing, name validation, sanitizing and the token bucket.
- `integration` (Python 3, Linux/WSL): starts real server processes and checks
  every command and error code, framing, rate limiting, slow-client backpressure,
  shutdown authorization (localhost, password, wrong password), `SIGINT`/`SIGTERM`,
  fast restart, `-m`, fd exhaustion, 1100 simultaneous clients (beyond `select()`'s
  1024 limit), the console client end to end, and that no sanitizer report
  appears in any server run.

## Architecture

```
src/
  main.c             command-line options
  server.c           event loop, accept, read/write, disconnects, shutdown
  commands.c         chat semantics: one handler per command
  server_internal.h  what server.c and commands.c share
  client.c/.h        per-connection state: nick, room, line reader, output queue
  protocol.c/.h      framing, command parsing, validation, error codes (no I/O)
  ratelimit.c/.h     token bucket (time passed in, unit-testable)
  buffer.c/.h        growable byte FIFO
  net.c/.h           socket helpers: listener, accept4, peer names
  poller.h           readiness API
  poller_epoll.c     edge-triggered epoll backend
  poller_poll.c      level-triggered poll() backend
  log.c/.h           timestamped logging
tools/
  chat_client.c      console client: poll() on stdin + socket
tests/
  test_buffer.c, test_protocol.c, test_ratelimit.c, test_server.py
```

One iteration of the event loop:

```
poller_wait()
  listener ready      -> accept until EAGAIN, register clients
  signal pipe ready   -> stop the loop
  client writable     -> flush its output queue
  client readable     -> recv until EAGAIN, feed the line reader,
                         rate-limit and dispatch every complete line
reap_dead()           -> close clients that failed during this iteration,
                         announce QUIT to their rooms
```

The I/O layer (`server.c`) knows nothing about nicknames or rooms; the chat
layer (`commands.c`) never touches sockets and only queues output. Adding a
command means one parser table entry and one handler.

## Design decisions

**Single thread, readiness-based I/O.** Most chat connections are idle, so one
thread multiplexing every socket is cheaper than a thread per connection and
needs no locking: only one thread ever touches the client table. The price is
that nothing in the loop may block, which is why every socket is non-blocking.

**epoll, edge-triggered.** With edge triggering the kernel reports a socket
only when its state changes, so every handler drains its socket until `EAGAIN`
(reads, writes and `accept()`). The code is written to that stricter contract,
which also makes it correct under the level-triggered `poll()` backend. Write
readiness is requested only while a client has queued output; otherwise an
always-writable socket would wake the loop for nothing.

**Per-client output queue and backpressure.** Sending appends to the client's
queue and tries a non-blocking send immediately. If the kernel buffer is full,
the rest waits for write readiness. A client whose queue grows past
`max_queue_bytes` is too slow to keep up and is disconnected. This bounds
memory per client and guarantees one stuck reader cannot stall the others.

**Line framing.** TCP is a byte stream: one `recv()` can return half a line or
several lines. Each client has a fixed 4 KB line reader; complete lines are
dispatched, partial ones wait for more bytes, and a full buffer without a
newline is a protocol violation, so input memory per client is bounded too.
Server lines obey the same 4 KB limit, which is why `/list` answers with one
`USER` line per member instead of one long line.

**Token bucket rate limiting.** A bucket of `burst` tokens refilled at `rate`
per second allows pasting a few lines while capping sustained floods. Only the
first rejected line of a burst gets an error, so a flooding client cannot make
the server amplify its traffic.

**Deferred disconnects.** A failing client is only marked dead (with a reason)
and is closed at the end of the loop iteration. Closing immediately would let
`accept()` reuse the fd number while events for the old connection are still in
the current batch. Announcing a disconnect can reveal further dead clients, so
reaping repeats until nothing changes.

**Signals through a self-pipe.** The `SIGINT`/`SIGTERM` handler only writes a
byte to a non-blocking pipe watched by the event loop, so all real work runs
outside signal context. On shutdown the server notifies every client, gives
queued output up to one second to drain, then closes everything. `SIGPIPE` is
ignored and sends use `MSG_NOSIGNAL`, so a vanished peer yields `EPIPE` instead
of killing the process.

**fd exhaustion.** When `accept()` fails with `EMFILE`, the pending connection
would keep the listener readable forever. The server keeps one spare fd
reserved: it closes it, accepts and immediately closes the connection, then
reserves it again.

**Dynamic client table.** Clients live in an intrusive list (for broadcast)
and a growable fd-indexed array (O(1) lookup on events), so there is no
`FD_SETSIZE` ceiling; `-m` sets the policy limit.

## Known limitations

- No encryption or accounts; the admin password travels in plain text.
  Trusted networks only (TLS via OpenSSL would be the next step).
- Nickname lookup and room membership are linear scans over all clients. Fine
  for thousands of users; a hash map and per-room member lists would be the
  next step beyond that.
- Fairness: a client that sends continuously is read until `EAGAIN` before
  others are served (the rate limiter bounds what it can make the server do).
  A per-iteration read budget would bound this too.
- Broadcast copies each message into every recipient's queue; a shared,
  reference-counted message would avoid the copies.
- The stdout transcript is written with blocking stdio; redirect it to a file
  or `/dev/null` rather than a slow pipe.
- IPv4 only on the server side.
