# Redis Clone in C++: Chapters 3–14

An educational Redis-inspired in-memory server built from scratch in C++17.
This repository combines the work from Chapters 3 through 14 into one runnable
portfolio project. It demonstrates networking, protocol design, event loops,
data structures, timers, TTL expiration, and asynchronous cleanup.

> This is a learning project, not a production Redis replacement. It uses the
> custom binary protocol taught in the chapters, not Redis RESP.

## What each chapter teaches and builds

| Chapter | Main topic | What is built |
|---|---|---|
| 3 | Hello server and client | A TCP server accepts a connection and a client connects to it. |
| 4 | Protocol parsing | Requests are encoded, decoded, validated, and parsed into command arguments. |
| 5 | Event loop and non-blocking I/O | Non-blocking sockets prevent one slow client from stopping the server. |
| 6 | Event-loop implementation | A `poll`-based loop monitors the listening socket and client sockets. |
| 7 | Basic key-value server | `SET`, `GET`, `DEL`, and `KEYS` operate on in-memory data. |
| 8 | Hash tables | Keys are stored and found efficiently using an intrusive hash table. |
| 9 | Data serialization | Typed replies such as strings, integers, doubles, errors, and arrays are serialized and decoded. |
| 10 | AVL tree implementation | A self-balancing binary-search tree is implemented and tested. |
| 11 | Sorted sets | AVL trees and hash tables combine to provide `ZADD`, `ZREM`, `ZSCORE`, and `ZQUERY`. |
| 12 | Event-loop timers | Idle connections are tracked and closed after five seconds without activity. |
| 13 | Heap and TTL | A min-heap manages key expiration through `EXPIRE` and `TTL`. |
| 14 | Thread pool and async tasks | Large sorted sets are freed by worker threads so the event loop stays responsive. |

## Features included

- TCP client/server communication
- Length-prefixed custom request and response protocol
- Request parsing and pipelining
- Non-blocking sockets and a `poll` event loop
- Hash-table key/value storage
- String commands: `SET`, `GET`, `DEL`, and `KEYS`
- Binary serialization and deserialization
- AVL tree implementation
- Sorted-set commands: `ZADD`, `ZREM`, `ZSCORE`, and `ZQUERY`
- Idle connection timers
- Key expiration with `EXPIRE` and `TTL`
- Min-heap timer management
- Thread pool with mutexes and condition variables
- Asynchronous freeing of large sorted sets

The project intentionally does not add persistence, replication, authentication,
eviction policies, lists, hashes, sets, or other features outside the completed
Chapter 3–14 scope.

## Build

Requirements: a C++17 compiler, POSIX sockets, pthreads, and Python 3 for the
command-level tests.

```bash
make
```

## Run

Terminal 1:

```bash
./kv_server
```

Terminal 2:

```bash
./kv_client set name redis
./kv_client get name
./kv_client zadd leaderboard 10 alice
./kv_client zquery leaderboard 0 "" 0 10
```

The server listens on `127.0.0.1:5559`.

## Tests

Run the standalone thread-pool tests:

```bash
make unit-tests
```

With `./kv_server` running in another terminal, run the command tests:

```bash
python3 test_cmds.py
```

The asynchronous deletion test is a longer integration test:

```bash
python3 test_async_delete.py
```

The complete validation evidence is documented in
[result.md](./result.md).

## Validation results

These measurements were taken locally on macOS using the project's custom
binary protocol. They show how the server behaved in the recorded tests; they
are not universal guarantees for every computer.

| Measurement | Result | Simple explanation |
|---|---:|---|
| Sequential throughput | **28,065 operations/sec** | One client sent one command and waited for its response before sending the next. |
| Pipelined throughput | **326,325 operations/sec** | The client sent many commands together and then read all responses, reducing waiting between commands. |
| Concurrent throughput | **46,497 operations/sec** | Ten clients operated at the same time, with zero client errors in the test. |
| Sequential p50 latency | **0.057 ms** | Half of the measured sequential operations completed within this time. |
| Sequential p95 latency | **0.155 ms** | 95% of the measured sequential operations completed within this time. |
| Sequential p99 latency | **0.251 ms** | 99 out of every 100 measured operations completed within this time. |
| Sequential maximum latency | **0.643 ms** | This was the slowest measured operation in that test run. |
| Key insertion capacity | **50,000 keys** | 50,000 small keys were inserted and read back successfully. |
| Key cleanup | **50,000 keys in 0.188 sec** | The same 50,000 keys were deleted successfully. |
| Sorted-set insertion | **28,328 ZADD/sec** | 20,000 members were inserted into one sorted set. |
| Sorted-set query | **0.054 ms** | A sorted-set range query returned the requested members quickly. |
| Large sorted-set deletion | **0.028 ms response** | Deleting a 20,000-member set returned quickly because large cleanup runs asynchronously. |
| TTL burst | **6,000 keys** | 6,000 keys expired at approximately the same time and were removed correctly. |
| TTL burst worst response | **1.2 ms** | The slowest measured response during that expiration burst took 1.2 ms. |
| Randomized TTL model test | **100,799 operations** | Random expiration operations were compared with an expected model and passed. |
| Idle connection stress | **200 connections** | Silent and active connections were timed out or retained correctly. |

### What the performance words mean

- **Operation:** One command such as `SET`, `GET`, `ZADD`, or `DEL`.
- **Operations/sec:** How many commands the server completed in one second.
- **Sequential:** One client waits after every command.
- **Pipelined:** Many commands are sent before waiting for responses.
- **Concurrent:** Multiple clients send commands at the same time.
- **Latency:** Time from sending a command until receiving its response.
- **p50:** The middle result; 50% of requests were faster.
- **p95:** 95% of requests were faster.
- **p99:** 99% of requests were faster; useful for spotting slower requests.
- **TTL:** Time to live; how long a key remains before expiration.

## description

Built a Redis-inspired in-memory key-value server in C++17 from scratch,
implementing non-blocking networking, a poll-based event loop, custom binary
protocol serialization, intrusive hash tables, AVL-tree sorted sets, heap-based
TTL expiration, idle connection timers, and a pthread-based asynchronous
cleanup pool. Validated the implementation with functional, concurrency, timer,
capacity, latency, throughput, and sanitizer tests.
