#!/usr/bin/env python3
"""Integration tests for chat_server.

Usage: python3 tests/test_server.py ./build/chat_server

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

BIN = os.path.abspath(sys.argv[1])
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


class Server:
    def __init__(self, *args, fd_limit=None):
        self.port = free_port()
        self.log = tempfile.NamedTemporaryFile(prefix="chat_server_", suffix=".log", delete=False)
        LOGS.append(self.log.name)

        def limit():
            if fd_limit:
                resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))

        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1")
        self.proc = subprocess.Popen([BIN, "-p", str(self.port), *args],
                                     stdout=subprocess.DEVNULL, stderr=self.log,
                                     preexec_fn=limit, env=env)
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

    def connect(self, host="127.0.0.1", rcvbuf=None):
        s = socket.socket()
        if rcvbuf:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        s.settimeout(5)
        s.connect((host, self.port))
        return s

    def wait(self, timeout=5):
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            return None

    def stop(self):
        if self.alive():
            self.proc.kill()
            self.proc.wait()


def recv_until(sock, needle, timeout=3.0):
    """Read until `needle` appears; return everything read."""
    sock.settimeout(0.1)
    data = b""
    deadline = time.time() + timeout
    while needle not in data and time.time() < deadline:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
        except socket.timeout:
            pass
        except OSError:
            break
    return data


def drain(sock, t=0.2):
    sock.settimeout(t)
    data = b""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
    except (socket.timeout, OSError):
        pass
    return data


def name_of(sock):
    host, port = sock.getsockname()
    return f"{host}:{port}".encode()


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok)))
    print(("PASS  " if ok else "FAIL  ") + name + (f"  ({detail})" if detail and not ok else ""))


def two_clients(srv):
    a, b = srv.connect(), srv.connect()
    recv_until(a, name_of(b) + b" has joined")
    recv_until(b, b"Commands:")
    return a, b


# --------------------------------------------------------------------------- tests

def test_messaging():
    srv = Server()
    a, b = two_clients(srv)

    a.sendall(b"hi\n")
    check("broadcast reaches other client", name_of(a) + b"> hi\n" in recv_until(b, b"> hi"))
    check("sender gets its own echo", b"> hi\n" in recv_until(a, b"> hi"))

    # framing: one line split across several TCP segments
    a.sendall(b"hel")
    time.sleep(0.2)
    a.sendall(b"lo wor")
    time.sleep(0.2)
    a.sendall(b"ld\n")
    got = recv_until(b, b"hello world")
    check("line split across recv() calls is reassembled",
          b"> hello world\n" in got and b"> hel\n" not in got, got)

    # framing: several lines in one segment
    a.sendall(b"one\ntwo\r\nthree\n")
    got = recv_until(b, b"> three")
    check("coalesced lines are split (CRLF accepted)",
          all(name_of(a) + b"> " + w + b"\n" in got for w in (b"one", b"two", b"three")), got)

    a.sendall(b"/nope\n")
    check("unknown command is reported", b"Unknown command: /nope" in recv_until(a, b"Unknown"))

    # oversized line without newline -> protocol violation
    c = srv.connect()
    recv_until(c, b"Commands:")
    c.sendall(b"x" * 5000)
    check("too-long line is rejected", b"Line too long" in recv_until(c, b"Line too long"))
    check("too-long line disconnects the client",
          name_of(c) + b" disconnected" in recv_until(b, name_of(c) + b" disconnected"))

    # peer resets the connection while others keep talking
    r = srv.connect()
    recv_until(r, b"Commands:")
    r.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    r.close()
    for _ in range(5):
        a.sendall(b"after reset\n")
        time.sleep(0.05)
    check("peer reset: server survives (no SIGPIPE)", srv.alive())

    a.sendall(b"/quit\n")
    check("/quit says goodbye", b"SERVER> Bye" in recv_until(a, b"Bye"))
    check("/quit is announced to others",
          name_of(a) + b" disconnected" in recv_until(b, name_of(a) + b" disconnected"))
    srv.stop()


def test_slow_client():
    srv = Server("-o", "65536")
    a, b = two_clients(srv)
    slow = srv.connect(rcvbuf=4096)       # never reads
    slow_name = name_of(slow)
    recv_until(a, slow_name + b" has joined")
    recv_until(b, slow_name + b" has joined")

    payload = b"Y" * 3000 + b"\n"
    seen = b""
    deadline = time.time() + 30
    t0 = time.time()
    while slow_name + b" disconnected" not in seen and time.time() < deadline:
        a.sendall(payload)
        drain(a, 0.001)
        seen = seen[-200:] + drain(b, 0.001)
    dropped = slow_name + b" disconnected" in seen
    check("client that stops reading is dropped (backpressure)", dropped,
          f"after {time.time() - t0:.1f}s")

    a.sendall(b"still here\n")
    check("fast clients unaffected by the slow one", b"still here" in recv_until(b, b"still here"))
    check("drop reason is logged", "dropping slow client" in srv.stderr())
    slow.close()
    srv.stop()


def test_shutdown_rules():
    srv = Server()
    a, b = two_clients(srv)
    ip = subprocess.check_output(["hostname", "-I"]).split()[0].decode()
    remote = srv.connect(ip)
    recv_until(remote, b"Commands:")
    remote.sendall(b"/shutdown\n")
    check("/shutdown from non-loopback address is refused",
          b"only allowed from localhost" in recv_until(remote, b"localhost\n") and srv.alive())

    a.sendall(b"/shutdown\r\n")
    check("clients are told about shutdown", b"shutting down" in recv_until(b, b"shutting down"))
    check("/shutdown from localhost exits with status 0", srv.wait() == 0)


def test_signals():
    for sig in (signal.SIGTERM, signal.SIGINT):
        srv = Server()
        a, b = two_clients(srv)
        srv.proc.send_signal(sig)
        got = recv_until(a, b"shutting down")
        check(f"{sig.name}: clients get goodbye message", b"shutting down" in got)
        check(f"{sig.name}: graceful exit with status 0", srv.wait() == 0)
        check(f"{sig.name}: shutdown is logged", "received signal" in srv.stderr())


def test_restart():
    srv = Server()
    port = srv.port
    a, _ = two_clients(srv)
    a.sendall(b"/shutdown\n")
    srv.wait()
    # rebind the same port right away while connections sit in TIME_WAIT
    p = subprocess.Popen([BIN, "-p", str(port)], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    check("immediate restart on the same port (SO_REUSEADDR)", p.poll() is None)
    p.kill()
    p.wait()


def test_limits():
    srv = Server("-m", "3")
    keep = [srv.connect() for _ in range(3)]
    extra = srv.connect()
    check("-m limit: extra client gets 'Server is full'",
          b"Server is full" in recv_until(extra, b"Server is full"))
    for s in keep:
        s.close()
    srv.stop()

    srv = Server(fd_limit=16)
    socks = []
    for _ in range(30):
        try:
            socks.append(srv.connect())
        except OSError:
            pass
    time.sleep(0.5)
    for s in socks:
        s.close()
    time.sleep(0.5)
    ok = srv.alive()
    if ok:
        a, b = two_clients(srv)
        a.sendall(b"ok\n")
        ok = b"> ok" in recv_until(b, b"> ok")
    check("fd exhaustion (EMFILE): server survives and keeps serving", ok)
    check("fd exhaustion is logged", "out of file descriptors" in srv.stderr())
    srv.stop()


def test_many_clients():
    n = 1100  # more than select()'s FD_SETSIZE (1024)
    raise_fd_limit(n + 200)
    srv = Server("-m", str(n + 10), fd_limit=n + 100)
    socks = [srv.connect() for _ in range(n)]
    first, last = socks[0], socks[-1]
    recv_until(first, name_of(last) + b" has joined", timeout=60)
    first.sendall(b"hello everyone\n")
    got = recv_until(last, b"hello everyone", timeout=30)
    check(f"{n} simultaneous clients (beyond FD_SETSIZE)", b"hello everyone" in got)
    for s in socks:
        s.close()
    srv.stop()


def test_cli():
    r = subprocess.run([BIN, "-p", "notaport"], capture_output=True, text=True)
    check("invalid -p is rejected", r.returncode != 0 and "invalid port" in r.stderr)
    r = subprocess.run([BIN, "-h"], capture_output=True, text=True)
    check("-h prints usage", r.returncode == 0 and "Usage:" in r.stdout)


def main():
    raise_fd_limit(4096)
    for t in (test_cli, test_messaging, test_slow_client, test_shutdown_rules,
              test_signals, test_restart, test_limits, test_many_clients):
        t()

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
