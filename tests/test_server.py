#!/usr/bin/env python3
"""Integration tests for chat_server and chat_client.

Usage: python3 tests/test_server.py ./build/chat_server [./build/chat_client]

Starts real server processes on free ports and talks to them over TCP.
Linux/WSL only (uses `hostname -I` and rlimits).
"""
import os
import resource
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

SERVER = os.path.abspath(sys.argv[1])
CLIENT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
FAST = ["-r", "100000", "-b", "100000"]  # effectively no rate limit
LOGS = []          # stderr files of every server started, checked at the end
RESULTS = []


# --------------------------------------------------------------------------- helpers

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def raise_fd_limit(n):
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = min(n, hard)
    if soft < want:
        resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))


class Conn:
    """A raw protocol connection that remembers everything it has read."""

    def __init__(self, sock):
        self.sock = sock
        self.data = b""
        self.nick = None

    def send(self, line):
        self.sock.sendall(line.encode() + b"\n" if isinstance(line, str) else line)

    def expect(self, needle, timeout=3.0):
        """Wait until `needle` has been received; return True if it was."""
        needle = needle.encode() if isinstance(needle, str) else needle
        deadline = time.time() + timeout
        self.sock.settimeout(min(0.05, timeout))
        while needle not in self.data and time.time() < deadline:
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.data += chunk
            except socket.timeout:
                pass
            except OSError:
                break
        return needle in self.data

    def drain(self, t=0.1):
        self.expect(b"\x00never\x00", timeout=t)

    def lines(self):
        return self.data.decode(errors="replace").splitlines()

    def close(self):
        self.sock.close()


class Server:
    def __init__(self, *args, fd_limit=None, env=None):
        self.port = free_port()
        self.log = tempfile.NamedTemporaryFile(prefix="chat_server_", suffix=".log", delete=False)
        LOGS.append(self.log.name)

        def limit():
            if fd_limit:
                resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))

        full_env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", **(env or {}))
        self.proc = subprocess.Popen([SERVER, "-p", str(self.port), *args],
                                     stdout=subprocess.DEVNULL, stderr=self.log,
                                     preexec_fn=limit, env=full_env)
        deadline = time.time() + 5
        while time.time() < deadline:
            if "listening on port" in self.stderr():
                return
            time.sleep(0.02)
        raise RuntimeError("server did not start:\n" + self.stderr())

    def stderr(self):
        with open(self.log.name, errors="replace") as f:
            return f.read()

    def alive(self):
        return self.proc.poll() is None

    def sock(self, host="127.0.0.1", rcvbuf=None):
        s = socket.socket()
        if rcvbuf:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        s.settimeout(5)
        s.connect((host, self.port))
        return s

    def join(self, nick=None, host="127.0.0.1"):
        """Connect, wait for WELCOME and optionally set a nickname."""
        c = Conn(self.sock(host))
        assert c.expect("Type /help"), c.data
        c.nick = next(l.split()[1] for l in c.lines() if l.startswith("WELCOME "))
        if nick:
            c.send(f"/nick {nick}")
            assert c.expect(f"OK NICK {nick}"), c.data
            c.nick = nick
        return c

    def wait(self, timeout=5):
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            return None

    def stop(self):
        if self.alive():
            self.proc.kill()
            self.proc.wait()


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok)))
    print(("PASS  " if ok else "FAIL  ") + name + (f"  ({detail!r})"[:300] if detail and not ok else ""))


# --------------------------------------------------------------------------- tests

def test_cli():
    r = subprocess.run([SERVER, "-p", "notaport"], capture_output=True, text=True)
    check("invalid -p is rejected", r.returncode != 0 and "invalid port" in r.stderr)
    r = subprocess.run([SERVER, "-r", "0"], capture_output=True, text=True)
    check("invalid -r is rejected", r.returncode != 0)
    r = subprocess.run([SERVER, "-h"], capture_output=True, text=True)
    check("-h prints usage", r.returncode == 0 and "Usage:" in r.stdout)


def test_messaging_and_framing():
    srv = Server(*FAST)
    a = srv.join()
    check("welcome assigns a guest nick in the lobby",
          a.nick.startswith("guest") and f"WELCOME {a.nick} lobby" in a.lines())
    b = srv.join()
    check("others see JOIN", a.expect(f"JOIN lobby {b.nick}"))

    a.send("hi")
    check("MSG reaches the room", b.expect(f"MSG lobby {a.nick} hi\n"))
    check("sender gets its own MSG as confirmation", a.expect(f"MSG lobby {a.nick} hi\n"))

    a.send(b"hel")
    time.sleep(0.2)
    a.send(b"lo wor")
    time.sleep(0.2)
    a.send(b"ld\n")
    check("line split across recv() calls is reassembled",
          b.expect(" hello world\n") and f"MSG lobby {a.nick} hel" not in b.lines())

    a.send(b"one\ntwo\r\nthree\n")
    b.expect(" three")
    check("coalesced lines are split (CRLF accepted)",
          all(f"MSG lobby {a.nick} {w}" in b.lines() for w in ("one", "two", "three")))

    a.send("//slash text")
    check("// escapes a leading slash", b.expect(f"MSG lobby {a.nick} /slash text"))

    a.send("look \x1b[31mred\x07")
    check("control characters are neutralised", b.expect(f"MSG lobby {a.nick} look ?[31mred?"))

    a.send("x" * 4000)
    check("over-long text: ERR 413, connection kept",
          a.expect("ERR 413 TOO_LONG Message text") and srv.alive())
    a.send("still connected")
    check("...and the client can keep chatting", b.expect(" still connected"))

    c = srv.join()
    c.send(b"y" * 5000)
    check("over-long line: ERR 413 and disconnect",
          c.expect("ERR 413 TOO_LONG Line longer") and b.expect(f"QUIT {c.nick} protocol error"))

    r = srv.join()
    r.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    r.close()
    for _ in range(5):
        a.send("after reset")
        time.sleep(0.05)
    check("peer reset: server survives (no SIGPIPE)", srv.alive())
    srv.stop()


def test_commands():
    srv = Server(*FAST)
    a = srv.join("alice")
    b = srv.join("bob")
    c = srv.join()

    a.expect(" bob\n")
    check("/nick is announced to the room",
          any(l.startswith("NICK guest") and l.endswith(" bob") for l in a.lines()), a.lines())
    a.send("/nick BOB")
    check("/nick taken (case-insensitive): ERR 409", a.expect("ERR 409 NICK_IN_USE"))
    a.send("/nick 9lives")
    check("/nick invalid: ERR 422", a.expect("ERR 422 INVALID_NAME"))
    a.send("/nick")
    check("/nick without argument: ERR 400 with usage", a.expect("ERR 400 BAD_REQUEST Usage: /nick <name>"))
    b.send("/nick Bob")
    check("/nick case change of own nick is allowed", b.expect("OK NICK Bob"))

    a.send("/msg bob psst, secret")
    check("/msg delivers PM to target only",
          b.expect("PM alice psst, secret") and a.expect("OK MSG Bob"))
    c.drain()
    check("/msg is not seen by others", "psst" not in c.data.decode())
    a.send("/msg nobody hi")
    check("/msg unknown nick: ERR 404", a.expect("ERR 404 NO_SUCH_NICK"))

    a.send("/list")
    a.expect("OK LIST lobby 3")
    users = {l.split()[2] for l in a.lines() if l.startswith("USER lobby ")}
    check("/list returns USER lines and a count", users == {"alice", "Bob", c.nick}, users)

    a.send("/join dev")
    check("/join replies OK JOIN", a.expect("OK JOIN dev"))
    check("old room sees PART", b.expect("PART lobby alice"))
    a.send("in dev only")
    a.expect("MSG dev alice in dev only")
    b.drain(0.3)
    check("messages stay inside their room", "in dev only" not in b.data.decode())
    b.send("/join dev")
    check("new room sees JOIN", a.expect("JOIN dev Bob"))
    a.send("/join bad room")
    check("/join with two words: ERR 400", a.expect("ERR 400 BAD_REQUEST Usage: /join <room>"))

    c.send("/rooms")
    c.expect("OK ROOMS 2")
    check("/rooms lists rooms with counts",
          "ROOM dev 2" in c.lines() and "ROOM lobby 1" in c.lines(), c.lines()[-4:])

    c.send("/help")
    check("/help lists commands", c.expect("OK HELP") and "/msg <nick> <text>" in c.data.decode())
    c.send("/frobnicate")
    check("unknown command: ERR 421", c.expect("ERR 421 UNKNOWN_COMMAND Unknown command /frobnicate"))

    b.send("/quit see you")
    check("/quit replies OK QUIT", b.expect("OK QUIT"))
    check("/quit reason is announced to the room", a.expect("QUIT Bob see you"))
    srv.stop()


def test_rate_limit():
    srv = Server("-r", "2", "-b", "3")
    a, b = srv.join(), srv.join()
    for i in range(10):
        a.send(f"flood {i}")
    a.expect("RATE_LIMITED")
    time.sleep(0.3)
    a.drain()
    delivered = sum(1 for l in a.lines() if l.startswith("MSG ") and " flood " in l)
    errors = sum(1 for l in a.lines() if l.startswith("ERR 429 RATE_LIMITED"))
    check("token bucket lets the burst through", delivered == 3, delivered)
    check("one ERR 429 per burst, not one per line", errors == 1, errors)
    time.sleep(1.1)  # ~2 tokens refilled
    a.send("after pause")
    check("tokens refill over time", b.expect(" after pause"))
    srv.stop()


def test_slow_client():
    srv = Server("-o", "65536", *FAST)
    a, b = srv.join(), srv.join()
    slow = Conn(srv.sock(rcvbuf=4096))      # never reads
    a.expect("JOIN lobby guest3")
    b.expect("JOIN lobby guest3")

    payload = ("Y" * 3000).encode() + b"\n"
    deadline = time.time() + 30
    while not b.expect("QUIT guest3 too slow", timeout=0.001) and time.time() < deadline:
        a.send(payload)
        a.data = a.data[-1000:]
        a.drain(0.001)
        b.data = b.data[-1000:]
    check("client that stops reading is dropped (backpressure)", "QUIT guest3 too slow" in b.data.decode())
    a.send("still here")
    check("fast clients unaffected by the slow one", b.expect(" still here"))
    check("drop is logged", "dropping slow client" in srv.stderr())
    slow.close()
    srv.stop()


def test_shutdown_rules():
    ip = subprocess.check_output(["hostname", "-I"]).split()[0].decode()

    srv = Server(*FAST)
    a = srv.join()
    remote = srv.join(host=ip)
    remote.send("/shutdown")
    check("/shutdown from network without password: ERR 403",
          remote.expect("ERR 403 FORBIDDEN") and srv.alive())
    remote.send("/shutdown guess")
    check("password ignored when none is configured",
          remote.expect("ERR 403 FORBIDDEN", 1) and srv.alive())
    a.send("/shutdown\r")
    check("/shutdown from localhost: OK SHUTDOWN", a.expect("OK SHUTDOWN"))
    check("everyone gets INFO before close", remote.expect("INFO Server is shutting down"))
    check("server exits with status 0", srv.wait() == 0)

    srv = Server(*FAST, env={"CHAT_ADMIN_PASSWORD": "s3cret"})
    remote = srv.join(host=ip)
    remote.send("/shutdown wrong")
    check("wrong admin password: ERR 403", remote.expect("ERR 403 FORBIDDEN") and srv.alive())
    remote.send("/shutdown s3cret")
    check("admin password works from the network",
          remote.expect("OK SHUTDOWN") and srv.wait() == 0)
    check("password shutdown is logged", "with admin password" in srv.stderr())


def test_signals():
    for sig in (signal.SIGTERM, signal.SIGINT):
        srv = Server()
        a = srv.join()
        srv.proc.send_signal(sig)
        check(f"{sig.name}: clients get INFO before close", a.expect("INFO Server is shutting down"))
        check(f"{sig.name}: graceful exit with status 0", srv.wait() == 0)
        check(f"{sig.name}: logged", "received signal" in srv.stderr())


def test_restart():
    srv = Server()
    port = srv.port
    a = srv.join()
    a.send("/shutdown")
    srv.wait()
    p = subprocess.Popen([SERVER, "-p", str(port)], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    check("immediate restart on the same port (SO_REUSEADDR)", p.poll() is None)
    p.kill()
    p.wait()


def test_limits():
    srv = Server("-m", "3")
    keep = [srv.join() for _ in range(3)]
    extra = Conn(srv.sock())
    check("-m limit: ERR 503 SERVER_FULL", extra.expect("ERR 503 SERVER_FULL"))
    for c in keep:
        c.close()
    srv.stop()

    srv = Server(*FAST, fd_limit=16)
    socks = []
    for _ in range(30):
        try:
            socks.append(srv.sock())
        except OSError:
            pass
    time.sleep(0.5)
    for s in socks:
        s.close()
    time.sleep(0.5)
    ok = srv.alive()
    if ok:
        a, b = srv.join(), srv.join()
        a.send("ok")
        ok = b.expect(" ok\n")
    check("fd exhaustion (EMFILE): server survives and keeps serving", ok)
    check("fd exhaustion is logged", "out of file descriptors" in srv.stderr())
    srv.stop()


def test_many_clients():
    n = 1100  # more than select()'s FD_SETSIZE (1024)
    raise_fd_limit(n + 200)
    srv = Server("-m", str(n + 10), *FAST, fd_limit=n + 100)
    conns = [Conn(srv.sock()) for _ in range(n)]
    first, last = conns[0], conns[-1]
    last.expect("Type /help", timeout=60)
    last_nick = next(l.split()[1] for l in last.lines() if l.startswith("WELCOME "))
    first.expect(f"JOIN lobby {last_nick}", timeout=60)
    first.send("hello everyone")
    check(f"{n} simultaneous clients (beyond FD_SETSIZE)", last.expect(" hello everyone", timeout=30))
    for c in conns:
        c.close()
    srv.stop()


def test_console_client():
    if CLIENT is None:
        print("SKIP  console client tests (no client binary given)")
        return
    srv = Server(*FAST)
    watcher = srv.join("watcher")
    script = "hello from the client\n/nick carol\n/list\n/msg watcher psst\n/rooms\n/nope\n"
    r = subprocess.run([CLIENT, "127.0.0.1", str(srv.port)], input=script,
                       capture_output=True, text=True, timeout=10)
    out = r.stdout
    check("client: pretty-prints welcome", "* Connected as guest" in out, out)
    check("client: chat line is formatted", "] guest" in out and ": hello from the client" in out, out)
    check("client: /nick confirmation", "* You are now known as carol" in out, out)
    check("client: /list collected into one line",
          "* Users in #lobby: " in out and "carol" in out and "watcher" in out, out)
    check("client: /msg confirmation", "* Private message delivered to watcher" in out, out)
    check("client: /rooms output", "* #lobby (2 users)" in out, out)
    check("client: errors are shown", "! Unknown command /nope, try /help (421 UNKNOWN_COMMAND)" in out, out)
    check("client: end of input sends /quit and exits 0",
          r.returncode == 0 and watcher.expect("QUIT carol quit"), (r.returncode, out[-200:]))
    check("server saw the client's message", watcher.expect("hello from the client"))
    check("PM from client arrives", watcher.expect("PM carol psst"))

    r = subprocess.run([CLIENT, "-r", "127.0.0.1", str(srv.port)], input="/list\n",
                       capture_output=True, text=True, timeout=10)
    check("client -r prints raw protocol", "OK LIST lobby" in r.stdout, r.stdout)
    r = subprocess.run([CLIENT, "127.0.0.1", str(free_port())], input="",
                       capture_output=True, text=True, timeout=10)
    check("client: connection refused is reported", r.returncode != 0 and "cannot connect" in r.stderr)
    srv.stop()


def main():
    raise_fd_limit(4096)
    for t in (test_cli, test_messaging_and_framing, test_commands, test_rate_limit,
              test_slow_client, test_shutdown_rules, test_signals, test_restart,
              test_limits, test_many_clients, test_console_client):
        try:
            t()
        except Exception as e:  # keep going so one failure doesn't hide others
            check(f"{t.__name__} raised {type(e).__name__}", False, str(e))

    reports = []
    for path in LOGS:
        with open(path, errors="replace") as f:
            text = f.read()
        if "Sanitizer" in text or "runtime error" in text:
            reports.append(path)
    check("no sanitizer reports in any server run", not reports, ", ".join(reports))

    passed = sum(ok for _, ok in RESULTS)
    print(f"\n{passed}/{len(RESULTS)} passed")
    sys.exit(0 if passed == len(RESULTS) else 1)


if __name__ == "__main__":
    main()
