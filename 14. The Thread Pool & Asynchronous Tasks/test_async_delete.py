# learning/ch14/test_async_delete.py      (NOT from the book)
#
# Tests deleting HUGE sorted sets, which the server now hands to worker threads.
# It talks to a RUNNING ./kv_server and checks:
#   1. a small sorted set is deleted normally (the "free it now" path)
#   2. a big one (over 10,000 members, the "worker thread" path) is deleted
#      instantly from the client's point of view, and the SAME key name can be
#      re-created straight away while the old one is still being freed
#   3. a big sorted set whose TTL expires is also freed in the background
#   4. more big deletions than there are workers (6 jobs, 4 threads) all complete
#   5. WHICH THREAD did the freeing: the worker threads, not the main thread
#      (Linux only; it reads per-thread CPU time from /proc and is skipped elsewhere)
#
# How to use:
#   terminal 1:  ./kv_server
#   terminal 2:  python3 test_async_delete.py            (about 20-30 s)
#   options:     --pid N        server process id (default: found with pgrep)
#                --members N    size of the big set in check 5 (default 100000)

import argparse
import os
import socket
import struct
import subprocess
import sys
import time

HOST, PORT = '127.0.0.1', 5559
BIG = 10001            # just over the server's k_large_container_size (10000)

_i = struct.Struct('<I').pack


# ------------------------------------------------------------------
# a small client
# ------------------------------------------------------------------

def decode(buf, pos=0):
    t = buf[pos]
    if t == 0:
        return None, pos + 1
    if t == 1:
        code, ln = struct.unpack_from('<iI', buf, pos + 1)
        return ('err', code, buf[pos + 9:pos + 9 + ln].decode()), pos + 9 + ln
    if t == 2:
        (ln,) = struct.unpack_from('<I', buf, pos + 1)
        return buf[pos + 5:pos + 5 + ln].decode(), pos + 5 + ln
    if t == 3:
        (v,) = struct.unpack_from('<q', buf, pos + 1)
        return v, pos + 9
    if t == 4:
        (v,) = struct.unpack_from('<d', buf, pos + 1)
        return v, pos + 9
    if t == 5:
        (n,) = struct.unpack_from('<I', buf, pos + 1)
        pos += 5
        items = []
        for _ in range(n):
            item, pos = decode(buf, pos)
            items.append(item)
        return items, pos
    raise ValueError(f'unknown reply type {t}')


def encode(*args):
    body = _i(len(args))
    for a in args:
        b = str(a).encode()
        body += _i(len(b)) + b
    return _i(len(body)) + body


class Client:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT))

    def _read(self, n):
        data = bytearray()
        while len(data) < n:
            chunk = self.s.recv(min(65536, n - len(data)))
            assert chunk, 'server closed the connection'
            data += chunk
        return bytes(data)

    def cmd(self, *args):
        self.s.sendall(encode(*args))
        (n,) = struct.unpack('<I', self._read(4))
        reply = self._read(n)
        value, end = decode(reply)
        assert end == len(reply)
        return value

    def populate(self, key, n, first=0, batch=500):
        """Add n members to a sorted set, pipelined: send a batch of zadds,
        then read the batch's replies (each is 4 length bytes + 9 int bytes)."""
        kb = key.encode()
        for start in range(first, first + n, batch):
            cnt = min(batch, first + n - start)
            reqs = []
            for i in range(start, start + cnt):
                sc, nm = b'%d' % i, b'm%d' % i
                body = (b'\x04\x00\x00\x00\x04\x00\x00\x00zadd' + _i(len(kb)) + kb +
                        _i(len(sc)) + sc + _i(len(nm)) + nm)
                reqs.append(_i(len(body)) + body)
            self.s.sendall(b''.join(reqs))
            self._read(13 * cnt)


# ------------------------------------------------------------------
# the tests
# ------------------------------------------------------------------

def test_small_delete():
    c = Client()
    c.cmd('del', 'ad_small')
    c.populate('ad_small', 100)
    assert c.cmd('zscore', 'ad_small', 'm50') == 50.0
    assert c.cmd('del', 'ad_small') == 1
    assert c.cmd('zscore', 'ad_small', 'm50') is None
    assert c.cmd('del', 'ad_small') == 0                 # already gone
    print('1. small sorted set (freed immediately): OK')


def test_big_delete_and_recreate():
    c = Client()
    c.cmd('del', 'ad_big')
    for cycle in range(3):
        c.populate('ad_big', BIG)
        assert c.cmd('zscore', 'ad_big', f'm{BIG - 1}') == float(BIG - 1)
        t = time.monotonic()
        assert c.cmd('del', 'ad_big') == 1
        reply_ms = (time.monotonic() - t) * 1000
        # from the client's view it is gone at once...
        assert c.cmd('zscore', 'ad_big', 'm5') is None
        keys = c.cmd('keys')
        assert isinstance(keys, list) and 'ad_big' not in keys
        # ...and the SAME name can be re-created straight away, while a worker
        # may still be freeing the old set. The two must not interfere.
        assert c.cmd('zadd', 'ad_big', 7, 'fresh') == 1
        assert c.cmd('zadd', 'ad_big', 8, 'fresher') == 1
        assert c.cmd('zquery', 'ad_big', 0, '', 0, 10) == ['fresh', 7.0, 'fresher', 8.0]
        assert c.cmd('del', 'ad_big') == 1               # (small now: the immediate path)
        print(f'   cycle {cycle + 1}: del of a {BIG}-member set replied in {reply_ms:.2f} ms')
    print('2. big sorted set: deleted at once, same key re-created safely: OK')


def test_big_expiry():
    c = Client()
    c.cmd('del', 'ad_exp')
    c.populate('ad_exp', BIG + 2000)
    assert c.cmd('expire', 'ad_exp', 300) == 1
    assert c.cmd('zscore', 'ad_exp', 'm5') == 5.0
    time.sleep(0.8)
    assert c.cmd('zscore', 'ad_exp', 'm5') is None
    assert c.cmd('ttl', 'ad_exp') == -2
    print('3. a big sorted set expiring by TTL is freed too: OK')


def test_more_jobs_than_workers():
    c = Client()
    names = [f'ad_m{i}' for i in range(6)]               # 6 jobs, only 4 workers
    for k in names:
        c.cmd('del', k)
        c.populate(k, BIG)
    for k in names:                                      # queue all 6 back to back
        assert c.cmd('del', k) == 1
    for k in names:
        assert c.cmd('zscore', k, 'm5') is None
    assert c.cmd('set', 'ad_after', 'ok') is None        # the server is still healthy
    assert c.cmd('get', 'ad_after') == 'ok'
    c.cmd('del', 'ad_after')
    time.sleep(0.5)
    keys = c.cmd('keys')
    assert isinstance(keys, list) and not any(k.startswith('ad_') for k in keys)
    print('4. 6 big deletions queued on 4 workers: all done, server healthy: OK')


# --- 5. which thread did the freeing? (Linux only) ---

def find_pid(arg_pid):
    if arg_pid:
        return arg_pid
    try:
        out = subprocess.check_output(['pgrep', '-x', 'kv_server']).split()
        return int(out[0]) if out else None
    except Exception:
        return None


def thread_cpu_ns(pid):
    """CPU time used by each thread of `pid`, in nanoseconds."""
    result = {}
    base = f'/proc/{pid}/task'
    for tid in os.listdir(base):
        with open(f'{base}/{tid}/schedstat') as f:
            result[int(tid)] = int(f.read().split()[0])
    return result


def test_which_thread_freed(arg_pid, members):
    pid = find_pid(arg_pid)
    if pid is None or not os.path.exists(f'/proc/{pid}/task/{pid}/schedstat'):
        print('5. which thread freed it: SKIPPED (needs Linux /proc; '
              'pass --pid N if the server is not called kv_server)')
        return
    c = Client()
    c.cmd('del', 'ad_cpu')
    c.populate('ad_cpu', members)
    time.sleep(0.5)                                      # let everything settle
    before = thread_cpu_ns(pid)
    t = time.monotonic()
    assert c.cmd('del', 'ad_cpu') == 1
    reply_ms = (time.monotonic() - t) * 1000
    time.sleep(0.7)                                      # give the workers time to finish
    after = thread_cpu_ns(pid)

    main_ns = after[pid] - before[pid]                   # the main thread has tid == pid
    workers_ns = sum(after[t_] - before.get(t_, 0) for t_ in after if t_ != pid)
    print(f'   deleting {members} members: `del` replied in {reply_ms:.2f} ms; '
          f'CPU used: main thread {main_ns / 1e6:.2f} ms, worker threads {workers_ns / 1e6:.2f} ms')
    # The freeing must have happened on the workers, not on the main thread.
    assert workers_ns > 1_000_000, 'the workers did almost no work: was it freed on the main thread?'
    assert workers_ns > 3 * main_ns, 'the MAIN thread did most of the freeing (not asynchronous!)'
    print('5. the worker threads did the freeing, not the main thread: OK')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int, default=None)
    ap.add_argument('--members', type=int, default=100000)
    args = ap.parse_args()

    print('running async-delete tests...')
    test_small_delete()
    test_big_delete_and_recreate()
    test_big_expiry()
    test_more_jobs_than_workers()
    test_which_thread_freed(args.pid, args.members)
    print('ALL TESTS PASSED')
