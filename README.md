# Chat Server (select()-based)

A single-threaded, multi-client TCP chat server written in C. Clients connect
over TCP, and any line one client sends is broadcast to every other connected
client. Typing `shutdown` stops the server.

## Build

```sh
gcc -Wall -Wextra -std=c11 -o chat_server CHAT_server.c
```

or with CMake:

```sh
cmake -B build
cmake --build build
```

## Run

```sh
./chat_server
```

The server listens on port `65001` on all interfaces. Connect with `nc localhost 65001`
from as many terminals as you like.

## Design decisions

**`select()` over threads.** This server multiplexes all client sockets on a
single thread with `select()`, rather than spawning a thread per connection.
For a chat server, most sockets are idle most of the time — `select()` avoids
the cost and complexity of thread creation/teardown and needs no locking
around shared state (the connection table, the fd set), since only one thread
ever touches them. The tradeoff is that a slow client or an accidental
blocking call anywhere in the loop stalls every other client; a
thread-per-connection design trades that away for higher per-connection
overhead and mutex-protected shared state instead.

**Fixed-size client table indexed by fd, sized to `FD_SETSIZE`.** Each
client's display name is stored in `connection[fd][...]`, indexed directly by
file descriptor rather than by connection slot. This is only safe because
`select()` itself cannot watch file descriptors `>= FD_SETSIZE` (1024 on
Linux), so sizing the table to `FD_SETSIZE` covers every fd `select()` could
ever report — a smaller fixed array (e.g. sized to `listen()`'s backlog) is
not safe here, since fd numbers are assigned by the OS and are not bounded by
the listen backlog. A production version would replace `select()` with
`poll()`/`epoll()` (no `FD_SETSIZE` ceiling) and the array with a dynamically
grown table or hash map.

**Text protocol, no framing.** Each `recv()` call is treated as one message.
This is simple but not correct in general — TCP is a byte stream, not a
message stream, so a single client message can arrive split across multiple
`recv()` calls, or multiple messages can arrive coalesced into one `recv()`.
There is no line-buffering layer here to reassemble partial reads (see the
`Chat_server` project for a version that adds one). For interactive line-by-line
chat over a local network this rarely surfaces, but it is a known correctness
gap, not an oversight.

## Known limitations

- No line-buffering: messages can be split or coalesced across `recv()` calls (see above).
- No authentication, encryption, or rate limiting — trusted-network use only.
- `select()` caps total simultaneous connections at `FD_SETSIZE` (1024 on Linux).
- Single-threaded: one slow/blocked client can delay delivery to others.
- IPv4 only (`hints.ai_family = AF_INET`).