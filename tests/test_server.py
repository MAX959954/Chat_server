# Integration tests for chat_server. Usage: python3 tests/test_server.py ./build/chat_server
import os, socket, subprocess, sys, time, struct

BIN = sys.argv[1]
PORT = 65101

def start(port=PORT, ulimit=None):
    cmd = f"ulimit -n {ulimit}; exec {BIN} {port}" if ulimit else f"exec {BIN} {port}"
    p = subprocess.Popen(["bash", "-c", cmd], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            time.sleep(0.05)
            return p
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")

def conn(host="127.0.0.1", port=PORT):
    s = socket.create_connection((host, port), timeout=3)
    time.sleep(0.05)
    return s

def drain(s, t=0.3):
    s.settimeout(t); data = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            data += d
    except socket.timeout:
        pass
    return data

def alive(p): return p.poll() is None

results = []
def check(name, ok):
    results.append((name, ok)); print(("PASS " if ok else "FAIL ") + name)

p = start()
a, b = conn(), conn()
drain(a); drain(b)
a.sendall(b"hi\n")
check("broadcast reaches other client", b"127.0.0.1> hi" in drain(b))

# 1. oversized message (old code wrote buffer[BUFSIZ])
a.sendall(b"X" * 20000)
time.sleep(0.3); drain(b)
check("oversized message: server alive", alive(p))

# 2. SIGPIPE: peer resets, then we broadcast to it
c = conn(); drain(c)
c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)); c.close()
for _ in range(3):
    a.sendall(b"after reset\n"); time.sleep(0.1)
check("send to reset peer: server alive (no SIGPIPE)", alive(p))

# 3. slow client that never reads gets dropped, others keep working
slow = conn()
slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
t0 = time.time()
for _ in range(400):
    a.sendall(b"Y" * 3000 + b"\n"); time.sleep(0.005)
    drain(a, 0.001); drain(b, 0.001)
time.sleep(3)
drain(a); drain(b)
a.sendall(b"still here\n")
check("non-reading client dropped, chat continues", b"still here" in drain(b, 1.0))
slow.close()

# 4. shutdown from non-loopback address is refused
ip = subprocess.check_output(["hostname", "-I"]).split()[0].decode()
r = conn(ip); drain(r)
r.sendall(b"shutdown\n")
check("remote shutdown denied", b"only allowed from localhost" in drain(r) and alive(p))
r.close()

# 5. telnet-style CRLF shutdown from localhost
a.sendall(b"shutdown\r\n")
try: p.wait(3)
except subprocess.TimeoutExpired: pass
check("CRLF shutdown from localhost exits cleanly", p.returncode == 0)
check("clients told about shutdown", b"Shutdown issued" in drain(b))
err = p.stderr.read().decode()
check("no sanitizer reports", "ERROR: AddressSanitizer" not in err and "runtime error" not in err)

# 6. immediate restart on same port (SO_REUSEADDR)
p = start()
check("immediate restart binds port", alive(p))
p.kill(); p.wait()

# 7. EMFILE: low fd limit, flood connections, server keeps serving
p = start(ulimit=16)
socks = []
for _ in range(30):
    try: socks.append(socket.create_connection(("127.0.0.1", PORT), timeout=1))
    except OSError: pass
time.sleep(0.5)
for s in socks: s.close()
time.sleep(0.5)
x, y = conn(), conn(); drain(x); drain(y)
x.sendall(b"ok\n")
check("EMFILE: survives fd exhaustion, still serves", alive(p) and b"ok" in drain(y))
p.kill(); p.wait()

# 8. fd >= FD_SETSIZE rejected instead of UB
p = start(ulimit=1400)
socks = []
for _ in range(1100):
    socks.append(socket.create_connection(("127.0.0.1", PORT), timeout=2))
time.sleep(1)
last = drain(socks[-1], 0.5)
check("fd >= FD_SETSIZE gets 'Server is full'", b"Server is full" in last and alive(p))
for s in socks: s.close()
p.kill(); p.wait()

print(f"\n{sum(ok for _, ok in results)}/{len(results)} passed")
