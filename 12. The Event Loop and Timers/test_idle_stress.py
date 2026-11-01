# learning/ch12/test_idle_stress.py      (NOT from the book: optional extra test)
#
# 200 connections at once: 70 leave by themselves (so connections are
# removed from the MIDDLE of the timer list), 30 stay active (their
# timers keep getting renewed), 100 stay silent. Checks that exactly
# the silent ones are timed out at ~5 s, the active ones ~5 s after
# their last command, and the server keeps working.
#
# How to use:
#   terminal 1:  ./kv_server
#   terminal 2:  python3 test_idle_stress.py        (takes about 11 seconds)

import random, socket, struct, time
HOST, PORT = '127.0.0.1', 5559

def cmd(sock, *args):
    body = struct.pack('<I', len(args))
    for a in args:
        b = a.encode(); body += struct.pack('<I', len(b)) + b
    sock.sendall(struct.pack('<I', len(body)) + body)
    h = b''
    while len(h) < 4: h += sock.recv(4 - len(h))
    n = struct.unpack('<I', h)[0]; r = b''
    while len(r) < n: r += sock.recv(n - len(r))
    return r

def closed(s):
    s.setblocking(False)
    try: return s.recv(1) == b''
    except BlockingIOError: return False
    except ConnectionResetError: return True
    finally: s.setblocking(True)

random.seed(1)
socks = [socket.create_connection((HOST, PORT)) for _ in range(200)]
print('opened 200 connections')

# 1. 70 clients leave on their own (the list loses nodes from the MIDDLE)
leavers = random.sample(range(200), 70)
for i in leavers:
    socks[i].close()
stay = [s for i, s in enumerate(socks) if i not in set(leavers)]   # 130 remain
print(f'{len(leavers)} clients left by themselves, {len(stay)} remain')

# 2. 30 of the remaining ones stay active (renewing) for 3 s, every 1 s
active = stay[:30]
quiet = stay[30:]
t0 = time.monotonic()
for rnd in range(3):
    time.sleep(1.0)
    for s in active:
        r = cmd(s, 'get', 'nokey')
        assert r[0] == 0
print('30 clients stayed active for 3 s; 100 were silent')

# 3. at t~5 s the 100 silent ones must be gone, the 30 active ones still alive
time.sleep(max(0, 5.6 - (time.monotonic() - t0)))
assert all(closed(s) for s in quiet), 'a silent connection was not timed out'
assert not any(closed(s) for s in active), 'an ACTIVE connection was wrongly closed'
print('t~5.6 s: all 100 silent connections closed, all 30 active ones alive: OK')

# 4. the active ones go quiet; ~5 s after their last command they must go too
time.sleep(5.0)
assert all(closed(s) for s in active), 'an idle connection was not timed out'
print('~5 s later: the remaining 30 also closed: OK')

# 5. the server still serves a new client
c = socket.create_connection((HOST, PORT))
assert cmd(c, 'zadd', 'stress_z', '1', 'a')[0] == 3
cmd(c, 'del', 'stress_z'); c.close()
print('server still works afterwards: OK')
print('STRESS TEST PASSED')
