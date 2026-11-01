# learning/ch12/test_idle.py      (NOT from the book: tests the idle timeout)
#
# One timeline, several connections, each checked against WHEN the
# server should close it (the server's idle timeout is 5 seconds):
#
#   A  connects at t=0, stays silent             -> closed at ~5 s
#   B  connects at t=1, stays silent             -> closed at ~6 s
#   C  connects at t=2, stays silent             -> closed at ~7 s
#        (A, B, C prove the order: the oldest goes first)
#   D  connects at t=0, sends a command at t=2,4,6,8
#        -> each command RENEWS its timer, so it survives far past 5 s,
#           and is closed 5 s after its LAST command (~13 s)
#   E  connects at t=9.5 and runs a command
#        -> proves the server still works normally after closing others
#
# How to use:
#   terminal 1:  ./kv_server
#   terminal 2:  python3 test_idle.py        (takes about 14 seconds)

import socket
import struct
import time

HOST, PORT = '127.0.0.1', 5559
IDLE_TIMEOUT = 5.0     # must match k_idle_timeout_ms in kv_server.cpp
TOLERANCE = 0.4        # allowed error, in seconds


def connect():
    s = socket.create_connection((HOST, PORT))
    return s


def send_cmd(sock, *args):
    """Send one command using the book's request format."""
    body = struct.pack('<I', len(args))
    for a in args:
        b = a.encode()
        body += struct.pack('<I', len(b)) + b
    sock.sendall(struct.pack('<I', len(body)) + body)


def read_reply(sock):
    """Read one length-prefixed reply and return its bytes."""
    header = b''
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        assert chunk, 'server closed the connection unexpectedly'
        header += chunk
    n = struct.unpack('<I', header)[0]
    body = b''
    while len(body) < n:
        chunk = sock.recv(n - len(body))
        assert chunk, 'server closed the connection unexpectedly'
        body += chunk
    return body


def is_closed(sock):
    """Non-blocking check: has the server closed this connection?"""
    sock.setblocking(False)
    try:
        data = sock.recv(1)
        return data == b''          # empty read = the server closed it
    except BlockingIOError:
        return False                # nothing to read: still open
    except ConnectionResetError:
        return True
    finally:
        sock.setblocking(True)      # put it back: other code does blocking reads on it


class Probe:
    def __init__(self, name, start, last_activity=None):
        self.name = name
        self.sock = None
        self.start = start                  # when it connects
        self.last_activity = last_activity  # time of its last request, if any
        self.closed_at = None


t0 = time.monotonic()
now = lambda: time.monotonic() - t0

A = Probe('A', 0.0)
B = Probe('B', 1.0)
C = Probe('C', 2.0)
D = Probe('D', 0.0)
E_start = 9.5
probes = [A, B, C, D]
d_sends = [2.0, 4.0, 6.0, 8.0]
d_next = 0
e_done = False

print('running the timeline (about 14 s)...')
while now() < 14.5:
    t = now()

    # connect each probe at its scheduled time
    for p in probes:
        if p.sock is None and t >= p.start:
            p.sock = connect()
            p.last_activity = t

    # D sends a request at its scheduled times (this renews its timer)
    if d_next < len(d_sends) and t >= d_sends[d_next] and D.closed_at is None:
        send_cmd(D.sock, 'get', 'nokey')
        read_reply(D.sock)
        D.last_activity = now()
        print(f'  t={now():5.2f}s  D sent a command (timer renewed)')
        d_next += 1

    # E: a brand new connection late in the run must work normally
    if not e_done and t >= E_start:
        e = connect()
        send_cmd(e, 'zadd', 'idle_test_z', '1', 'x')
        reply = read_reply(e)
        assert reply[0] == 3, f'unexpected reply: {reply!r}'   # SER_INT
        send_cmd(e, 'del', 'idle_test_z')
        read_reply(e)
        e.close()
        print(f'  t={now():5.2f}s  E connected and ran commands fine')
        e_done = True

    # note the moment each probe is first seen closed
    for p in probes:
        if p.sock is not None and p.closed_at is None and is_closed(p.sock):
            p.closed_at = now()
            print(f'  t={p.closed_at:5.2f}s  {p.name} was closed by the server')

    time.sleep(0.02)

assert e_done, 'E never ran'

# --- check every probe was closed at the right moment ---
failures = 0
for p in probes:
    assert p.closed_at is not None, f'{p.name} was never closed'
    expected = p.last_activity + IDLE_TIMEOUT
    err = p.closed_at - expected
    ok = abs(err) <= TOLERANCE
    print(f'{p.name}: closed at {p.closed_at:5.2f}s, expected ~{expected:5.2f}s '
          f'(off by {err:+.2f}s) {"OK" if ok else "FAIL"}')
    if not ok:
        failures += 1

# --- ordering: oldest connection must be closed first ---
assert A.closed_at < B.closed_at < C.closed_at, 'connections were not closed oldest-first'
print('closing order A, B, C: OK')

# D must have outlived the plain 5 s timeout thanks to its renewals
assert D.closed_at > IDLE_TIMEOUT + 5, 'D was closed too early: timer renewal is broken'
print('D outlived the 5 s timeout thanks to renewals: OK')

assert failures == 0, f'{failures} timing check(s) failed'
print('ALL TESTS PASSED')
