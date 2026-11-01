# Redis Clone Validation Results

**Date:** 2026-11-01  
**Project:** Chapter 3-14 consolidated Redis clone  
**Platform:** macOS  
**Compiler:** Apple Clang 21  
**Python:** 3.11.9  
**Server:** `127.0.0.1:5559`

## Overall result

**PASS for the implemented Chapter 3-14 scope.**

The server compiled successfully and passed functional, timer, concurrency,
sorted-set, asynchronous cleanup, capacity, and sanitizer checks.

## Build and test results

| Area | Command/check | Result |
|---|---|---|
| Build | `make all test_thread_pool test_async_free` | PASS |
| Command behavior | `python3 test_cmds.py` | PASS: 35 cases |
| Thread pool | `./test_thread_pool` | PASS: 6 checks, 265,000 jobs |
| Async memory cleanup | `./test_async_free` | PASS: 13 sorted sets |
| Async deletion | `python3 test_async_delete.py` | PASS: 5 checks |
| TTL/heap behavior | `python3 test_ttl.py` | PASS: 8 scenarios |
| Idle timers | `python3 test_idle.py` | PASS |
| Idle stress | `python3 test_idle_stress.py` | PASS: 200 connections |
| AddressSanitizer | `test_async_free` with ASan | PASS |

### Thread-pool evidence

- 100,000 jobs executed exactly once
- 8 parallel jobs completed in approximately 210 ms
- 5,000 single-worker jobs preserved FIFO order
- 160,000 multi-producer jobs completed correctly
- Idle workers used approximately 0 ms CPU during the idle check
- Worker wake-up latency was below 0.1 ms

### TTL and timer evidence

- Basic `EXPIRE` and `TTL`: PASS
- Silent-key expiration by the event loop: PASS
- TTL replacement and removal: PASS
- Deletion cancelled stale timers: PASS
- Sorted-set expiration and cleanup: PASS
- 300 randomized timers: 54,600 checks passed
- 6,000 simultaneous expirations: PASS
- Worst response latency during the 6,000-key burst: 1.2 ms
- Randomized heap model validation: 100,799 operations passed

### Idle-connection evidence

- Oldest connections closed in the correct order
- Timer renewal kept active connections alive
- Idle timeout accuracy was within approximately 0.01 seconds
- 200-connection stress test passed:
  - 100 silent connections closed
  - 30 active connections remained alive
  - Remaining active connections later closed correctly
  - New requests worked afterward

## Performance measurements

Measurements were taken locally on macOS using persistent custom-protocol
clients. They are indicative results, not universal hardware-independent
guarantees.

| Workload | Result |
|---|---:|
| Sequential 10,000 SET/GET operations | 28,065.9 ops/sec |
| Sequential latency p50 | 0.057 ms |
| Sequential latency p95 | 0.155 ms |
| Sequential latency p99 | 0.251 ms |
| Sequential maximum latency | 0.643 ms |
| Pipelined 10,000 SET operations | 326,325.9 ops/sec |
| 10 concurrent clients, 20,000 operations | 46,497.1 ops/sec |
| Concurrent client errors | 0 |
| 20,000 sorted-set inserts | 28,328.1 ZADD ops/sec |
| Sorted-set query latency | 0.054 ms |
| 20,000-member sorted-set deletion | 0.028 ms response time |
| 200 ms TTL expiration check | Expired correctly |

## Capacity checks

- 50,000 keys inserted successfully in approximately 0.224 seconds.
- 50,000 keys deleted successfully in approximately 0.188 seconds.
- Sampled keys were read back successfully.
- Server RSS during the capacity workload was approximately 6 MB.
- The project was not pushed to millions of keys because the current protocol
  and `KEYS` response buffer are intentionally limited to the learned scope.

## Boundary and robustness findings

### Oversized request behavior

An individual request larger than the server's 4 KiB message limit causes the
server to close that client connection. The server process remains alive and
continues serving other clients.

This is safe fail-closed behavior, but the client receives a connection reset
instead of a typed error response. It should be documented as a current
protocol limitation for the portfolio project.

### Response-size limitation

The response buffer is also limited to approximately 4 KiB. A large `KEYS`
response returns the project's `response is too big` error instead of sending
an unbounded response.

### Platform-specific test

The async-delete test's Linux `/proc` worker-CPU attribution was skipped on
macOS. The functional asynchronous deletion checks passed.

### Sanitizer note

AddressSanitizer passed for the asynchronous cleanup test. macOS does not
support the requested LeakSanitizer option in this environment, so the first
run with `ASAN_OPTIONS=detect_leaks=1` aborted due to platform support. The
same ASan test passed with leak detection disabled.

## Environment limitations

- Docker: unavailable; not required by this project.
- Node.js: available, but browser testing is not applicable because this is a
  custom TCP server with no browser UI.
- Redis benchmark tools were not used because this project implements a custom
  binary protocol rather than Redis RESP.

## Scope validated

- TCP networking
- Non-blocking sockets
- Poll-based event loop
- Request parsing and pipelining
- Binary serialization/deserialization
- Hash-table storage
- String commands: `SET`, `GET`, `DEL`, `KEYS`
- AVL tree and sorted sets
- `ZADD`, `ZREM`, `ZSCORE`, `ZQUERY`
- Idle connection timers
- Heap-based key TTL
- `EXPIRE` and `TTL`
- Thread pool and asynchronous large-key cleanup

Persistence, replication, authentication, eviction policies, and additional
Redis data types were not tested because they are outside the implemented
Chapter 3-14 scope.
