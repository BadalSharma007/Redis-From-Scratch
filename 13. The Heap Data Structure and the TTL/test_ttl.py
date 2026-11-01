# learning/ch13/test_ttl.py      (NOT from the book: tests key expiration)
#
# Talks to a RUNNING ./kv_server directly (no ./kv_client needed) and
# checks, with real timing:
#   1. basic expire / ttl replies
#   2. a key expires BY ITSELF while the server is otherwise silent
#      (proves poll() is woken up by the heap's earliest timer)
#   3. a negative ttl removes the timer; setting a new ttl replaces the old
#   4. `del` cancels the timer (no stale timer deletes a re-created key)
#   5. a sorted set with a TTL is freed when it expires
#   6. 300 keys with random TTLs expire in the right order, at the right time
#   7. 6000 keys expiring at the same instant (the 2000-per-round limit)
#      do not freeze the server
#   8. a randomized run checked against a model (catches wrong heap indexes)
#
# How to use:
#   terminal 1:  ./kv_server
#   terminal 2:  python3 test_ttl.py        (takes about 13 seconds)

import random
import socket
import struct
import time

HOST, PORT = '127.0.0.1', 5559
MARGIN = 0.08          # seconds of allowed timing error around an expiry

T0 = time.monotonic()
def now():
    return time.monotonic() - T0


# ------------------------------------------------------------------
# a tiny client: sends commands, decodes the typed replies
# ------------------------------------------------------------------

def decode(buf, pos=0):
    """Decode one typed value. Returns (python value, next position)."""
    t = buf[pos]
    if t == 0:                                   # SER_NIL
        return None, pos + 1
    if t == 1:                                   # SER_ERR
        code, ln = struct.unpack_from('<iI', buf, pos + 1)
        return ('err', code, buf[pos + 9:pos + 9 + ln].decode()), pos + 9 + ln
    if t == 2:                                   # SER_STR
        (ln,) = struct.unpack_from('<I', buf, pos + 1)
        return buf[pos + 5:pos + 5 + ln].decode(), pos + 5 + ln
    if t == 3:                                   # SER_INT
        (v,) = struct.unpack_from('<q', buf, pos + 1)
        return v, pos + 9
    if t == 4:                                   # SER_DBL
        (v,) = struct.unpack_from('<d', buf, pos + 1)
        return v, pos + 9
    if t == 5:                                   # SER_ARR
        (n,) = struct.unpack_from('<I', buf, pos + 1)
        pos += 5
        items = []
        for _ in range(n):
            item, pos = decode(buf, pos)
            items.append(item)
        return items, pos
    raise ValueError(f'unknown reply type {t}')


class Client:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT))

    def _read(self, n):
        data = b''
        while len(data) < n:
            chunk = self.s.recv(n - len(data))
            assert chunk, 'server closed the connection'
            data += chunk
        return data

    def cmd(self, *args):
        body = struct.pack('<I', len(args))
        for a in args:
            b = str(a).encode()
            body += struct.pack('<I', len(b)) + b
        self.s.sendall(struct.pack('<I', len(body)) + body)
        (n,) = struct.unpack('<I', self._read(4))
        reply = self._read(n)
        value, end = decode(reply)
        assert end == len(reply), 'reply has extra bytes'
        return value


def cleanup(c, keys):
    for k in keys:
        c.cmd('del', k)


# ------------------------------------------------------------------
# the tests
# ------------------------------------------------------------------

def test_basic():
    c = Client()
    cleanup(c, ['b1'])
    assert c.cmd('expire', 'nokey', 1000) == 0
    assert c.cmd('ttl', 'nokey') == -2
    assert c.cmd('set', 'b1', 'v') is None
    assert c.cmd('ttl', 'b1') == -1                      # no TTL yet
    assert c.cmd('expire', 'b1', 1000) == 1
    assert 900 <= c.cmd('ttl', 'b1') <= 1000
    assert c.cmd('expire', 'b1', 3000) == 1              # change it
    assert 2900 <= c.cmd('ttl', 'b1') <= 3000
    assert c.cmd('expire', 'b1', -1) == 1                # remove it
    assert c.cmd('ttl', 'b1') == -1
    assert c.cmd('expire', 'b1', 'abc') == ('err', 4, 'expect int64')
    cleanup(c, ['b1'])
    print('1. basic expire/ttl replies: OK')


def test_expires_by_itself():
    c = Client()
    cleanup(c, ['q'])
    c.cmd('set', 'q', 'v')
    c.cmd('expire', 'q', 400)
    t = now()
    time.sleep(0.25)
    assert c.cmd('get', 'q') == 'v', 'key expired too early'
    # Now send NOTHING until just after the expiry (400 ms). Only the heap's
    # timer can wake the server. If poll() ignored the heap it would sleep
    # on, and our next `get` would still find the key.
    while now() - t < 0.55:
        time.sleep(0.01)
    assert c.cmd('get', 'q') is None, 'the timer did not fire on its own'
    print('2. a key expires by itself while the server is silent: OK')


def test_persist_and_update():
    c = Client()
    cleanup(c, ['p1', 'p2'])
    # a negative ttl removes the timer: the key must survive past it
    c.cmd('set', 'p1', 'v')
    c.cmd('expire', 'p1', 300)
    c.cmd('expire', 'p1', -1)
    # a new ttl REPLACES the old one: 200 ms -> 900 ms
    c.cmd('set', 'p2', 'v')
    c.cmd('expire', 'p2', 200)
    c.cmd('expire', 'p2', 900)
    t = now()
    time.sleep(0.5)
    assert c.cmd('get', 'p1') == 'v' and c.cmd('ttl', 'p1') == -1, 'ttl was not removed'
    assert c.cmd('get', 'p2') == 'v', 'the OLD 200 ms timer fired after being replaced'
    while now() - t < 1.15:
        time.sleep(0.01)
    assert c.cmd('get', 'p2') is None, 'the new 900 ms timer did not fire'
    assert c.cmd('get', 'p1') == 'v'
    cleanup(c, ['p1'])
    print('3. removing and replacing a ttl: OK')


def test_del_cancels_timer():
    c = Client()
    cleanup(c, ['d', 'd2'])
    c.cmd('set', 'd', 'v')
    c.cmd('expire', 'd', 300)
    assert c.cmd('del', 'd') == 1
    c.cmd('set', 'd', 'v2')                  # the same name again, no ttl
    t = now()
    time.sleep(0.6)
    # a stale timer left behind by `del` would delete the NEW key (or crash)
    assert c.cmd('get', 'd') == 'v2', 'a stale timer deleted the re-created key'
    assert c.cmd('ttl', 'd') == -1
    assert c.cmd('set', 'd2', 'x') is None   # server still healthy
    cleanup(c, ['d', 'd2'])
    print('4. del cancels the key\'s timer (no stale timers): OK')


def test_zset_ttl():
    c = Client()
    cleanup(c, ['z'])
    assert c.cmd('zadd', 'z', 1, 'a') == 1
    assert c.cmd('zadd', 'z', 2, 'b') == 1
    assert c.cmd('expire', 'z', 300) == 1
    assert c.cmd('zscore', 'z', 'a') == 1.0
    time.sleep(0.5)
    assert c.cmd('zscore', 'z', 'a') is None
    assert c.cmd('zquery', 'z', 0, '', 0, 10) == []
    keys = c.cmd('keys')
    assert isinstance(keys, list) and 'z' not in keys
    print('5. an expiring sorted set is freed: OK')


def test_many_timers_in_order():
    random.seed(7)
    c = Client()
    names = [f'm{i}' for i in range(300)]
    cleanup(c, names)
    expire_at = {}
    for k in names:
        c.cmd('set', k, 'v')
    for k in names:
        ttl_ms = random.randint(100, 1600)
        t = now()
        assert c.cmd('expire', k, ttl_ms) == 1
        expire_at[k] = t + ttl_ms / 1000.0

    gone = {k: False for k in names}
    samples = 0
    end = max(expire_at.values()) + 0.3
    while now() < end:
        for k in names:
            t = now()
            exists = c.cmd('get', k) is not None
            samples += 1
            if not exists:
                gone[k] = True
            assert not (exists and gone[k]), f'{k} came back after expiring'
            if abs(t - expire_at[k]) > MARGIN:           # outside the grey zone
                assert exists == (t < expire_at[k]), (
                    f'{k}: at t={t:.3f} expected exists={t < expire_at[k]} '
                    f'(expires at {expire_at[k]:.3f})')
    assert all(gone.values()), 'some keys never expired'
    print(f'6. 300 random timers fired in order and on time ({samples} checks): OK')


def test_burst():
    N = 6000
    c = Client()
    for i in range(N):
        c.cmd('set', f'burst{i}', 'x')
    target = now() + 1.6                      # ALL keys expire at this moment
    for i in range(N):
        ms = int((target - now()) * 1000)
        assert ms > 300, 'setup too slow'
        c.cmd('expire', f'burst{i}', ms)

    probe = Client()                          # a second client, measuring latency
    worst = 0.0
    while now() < target + 0.6:
        t = time.monotonic()
        probe.cmd('get', 'nokey')
        worst = max(worst, time.monotonic() - t)
        time.sleep(0.002)

    keys = c.cmd('keys')
    assert isinstance(keys, list), f'unexpected reply to keys: {keys!r}'
    assert not any(k.startswith('burst') for k in keys), 'some burst keys survived'
    assert worst < 0.2, f'the server stalled for {worst:.3f} s during the mass expiry'
    print(f'7. {N} keys expiring at once: all gone, worst reply latency '
          f'{worst * 1000:.1f} ms: OK')


def test_random_model():
    random.seed(3)
    c = Client()
    keys = [f'r{i}' for i in range(40)]
    cleanup(c, keys)
    exists = {k: False for k in keys}
    expire_at = {k: None for k in keys}
    end = now() + 3.0
    ops = 0
    while now() < end:
        k = random.choice(keys)
        t = now()
        # bring the model up to date for this key
        if exists[k] and expire_at[k] is not None:
            if expire_at[k] < t - MARGIN:
                exists[k], expire_at[k] = False, None      # it has expired
            elif expire_at[k] < t + MARGIN:
                continue                                    # too close to call: skip
        r = random.random()
        if r < 0.30:                                        # set
            assert c.cmd('set', k, 'v') is None
            if not exists[k]:
                exists[k], expire_at[k] = True, None
            # (an existing key keeps its TTL, as in the book)
        elif r < 0.45:                                      # del
            assert c.cmd('del', k) == (1 if exists[k] else 0)
            exists[k], expire_at[k] = False, None
        elif r < 0.70:                                      # new ttl
            ms = random.randint(100, 900)
            assert c.cmd('expire', k, ms) == (1 if exists[k] else 0)
            if exists[k]:
                expire_at[k] = t + ms / 1000.0
        elif r < 0.80:                                      # remove ttl
            assert c.cmd('expire', k, -1) == (1 if exists[k] else 0)
            if exists[k]:
                expire_at[k] = None
        else:                                               # check everything about k
            v = c.cmd('get', k)
            assert (v is not None) == exists[k], f'{k}: wrong existence'
            tt = c.cmd('ttl', k)
            if not exists[k]:
                assert tt == -2
            elif expire_at[k] is None:
                assert tt == -1, f'{k}: should have no ttl, got {tt}'
            else:
                rem_ms = (expire_at[k] - now()) * 1000
                assert abs(tt - rem_ms) < 80, (
                    f'{k}: ttl {tt} ms but the model says {rem_ms:.0f} ms '
                    '(the heap points at the wrong key?)')
        ops += 1
    cleanup(c, keys)
    print(f'8. randomized run against a model ({ops} operations): OK')


if __name__ == '__main__':
    print('running TTL tests (about 13 seconds)...')
    test_basic()
    test_expires_by_itself()
    test_persist_and_update()
    test_del_cancels_timer()
    test_zset_ttl()
    test_many_timers_in_order()
    test_burst()
    test_random_model()
    print('ALL TESTS PASSED')
