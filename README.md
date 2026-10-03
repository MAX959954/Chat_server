# chat_server

A single-threaded, event-driven, multi-client TCP chat server in C11.

- Linux **epoll in edge-triggered mode**, with a portable `poll()` backend
  selectable at build time
- **non-blocking sockets** with a per-client output queue and **backpressure**:
  a client that stops reading is disconnected instead of stalling everyone
- **line framing** over the TCP byte stream (split and coalesced reads handled)
- **graceful shutdown** on `SIGINT`/`SIGTERM` via the self-pipe trick
- survives fd exhaustion (`EMFILE`), peer resets (`SIGPIPE`) and malformed input
- unit tests, integration tests, ASan/UBSan build, clean under `-Wall -Wextra -Wpedantic`

## Platform

POSIX only (Linux, WSL, macOS). On Windows, build and run it inside WSL or
Docker; CMake stops with a clear error on non-POSIX platforms.

## Build and run

```sh
cmake -B build
cmake --build build
./build/chat_server -p 65001
```

```
Usage: chat_server [-p port] [-m max_clients] [-o max_queue_bytes] [-h]
  -p port             TCP port to listen on (default 65001)
  -m max_clients      maximum simultaneous clients (default 1000)
  -o max_queue_bytes  per-client output queue limit before a slow
                      client is dropped (default 262144)
```

Connect with `nc localhost 65001` or `telnet localhost 65001` from several terminals.
Chat messages are echoed to stdout as a transcript; server events are logged to stderr.

Build options:

| Option | Values | Default |
|---|---|---|
| `CHAT_POLLER` | `auto`, `epoll`, `poll` | `auto` (epoll on Linux, poll elsewhere) |
| `CHAT_SANITIZE` | `ON`/`OFF` | `OFF` (AddressSanitizer + UndefinedBehaviorSanitizer) |

## Tests

```sh
cmake -B build -DCHAT_SANITIZE=ON
cmake --build build
cd build && ctest --output-on-failure
```

- `unit_buffer`, `unit_protocol`: C unit tests for the output queue and the line framer
  (split lines, coalesced lines, CRLF, empty lines, maximum length, buffer reuse).
- `integration` (Python 3, Linux/WSL): starts real server processes and checks
  broadcasting, framing, the line-length limit, peer resets, slow-client backpressure,
  `/quit`, `/shutdown` rules, `SIGINT`/`SIGTERM`, fast restart, `-m`, fd exhaustion,
  1100 simultaneous clients (beyond `select()`'s 1024 limit), and that no
  sanitizer report appears in any server run.

## Protocol

Plain text, one message per line, terminated by `\n` (`\r\n` is accepted).
Lines longer than 4095 bytes are a protocol violation and disconnect the client.

| Client sends | Effect |
|---|---|
| any text | broadcast to every client (sender included) as `ip:port> text` |
| `/quit` | server replies `SERVER> Bye` and closes the connection |
| `/shutdown` | stops the server; only accepted from a loopback address |
| other `/command` | `SERVER> Unknown command: ...` |

Server notices start with `SERVER> ` (welcome, joins, disconnects, errors, shutdown).

## Architecture

```
src/
  main.c          command-line parsing
  server.c/.h     event loop, dispatch, broadcast, shutdown
  client.c/.h     per-connection state: line reader + output queue
  protocol.c/.h   line framing and command parsing (no I/O, unit-tested)
  buffer.c/.h     growable byte FIFO (unit-tested)
  net.c/.h        socket helpers: listener, accept4, peer names
  poller.h        readiness API
  poller_epoll.c  edge-triggered epoll backend
  poller_poll.c   level-triggered poll() backend
  log.c/.h        timestamped logging
tests/
  test_buffer.c, test_protocol.c, test_server.py
```

One iteration of the event loop:

```
poller_wait()
  listener ready      -> accept until EAGAIN, register clients
  signal pipe ready   -> stop the loop
  client writable     -> flush its output queue
  client readable     -> recv until EAGAIN, feed the line reader,
                         dispatch every complete line
reap_dead()           -> close clients that failed during this iteration
```

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

**Per-client output queue and backpressure.** Broadcasting appends the message
to each client's queue and tries to send it immediately. If the kernel buffer
is full, the rest waits for write readiness. A client whose queue grows past
`max_queue_bytes` is too slow to keep up and is disconnected. This bounds
memory per client and guarantees one stuck reader cannot stall the others.

**Line framing.** TCP is a byte stream: one `recv()` can return half a line or
several lines. Each client has a fixed 4 KB line reader; complete lines are
dispatched, partial ones wait for more bytes, and a full buffer without a
newline is rejected as a protocol violation, so input memory per client is bounded.

**Deferred disconnects.** A failing client is only marked dead and is closed at
the end of the loop iteration. Closing immediately would let `accept()` reuse
the fd number while events for the old connection are still in the current
batch. Announcing a disconnect can reveal further dead clients, so reaping
repeats until nothing changes.

**Signals through a self-pipe.** The `SIGINT`/`SIGTERM` handler only writes a
byte to a non-blocking pipe watched by the event loop, so all real work runs
outside signal context. On shutdown the server notifies every client, gives
queued output up to one second to drain, then closes everything.
`SIGPIPE` is ignored and sends use `MSG_NOSIGNAL`, so a vanished peer yields
`EPIPE` instead of killing the process.

**fd exhaustion.** When `accept()` fails with `EMFILE`, the pending connection
would keep the listener readable forever. The server keeps one spare fd
reserved: it closes it, accepts and immediately closes the connection, then
reserves it again.

**Dynamic client table.** Clients live in an intrusive list (for broadcast)
and a growable fd-indexed array (for O(1) lookup on events), so there is no
`FD_SETSIZE` ceiling; `-m` sets the policy limit.

## Known limitations

- No authentication, nicknames, rooms or encryption; trusted-network use only.
- Fairness: a client that sends continuously is read until `EAGAIN` before
  others are served. A per-iteration read budget would bound this.
- Broadcast copies each message into every client's queue (O(clients) memory per
  message); a shared, reference-counted message would avoid the copies.
- The stdout transcript is written with blocking stdio; redirect it to a file
  or `/dev/null` rather than a slow pipe.
- IPv4 only.
