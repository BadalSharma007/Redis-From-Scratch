// learning/ch14/test_thread_pool.cpp      (NOT from the book)
//
// Tests the thread pool on its own (no networking). It checks:
//   1. every job runs EXACTLY ONCE (none lost, none duplicated)
//   2. jobs really run IN PARALLEL on several threads
//   3. with ONE worker, jobs run in the order they were queued
//   4. several threads can queue jobs at the same time (multi-producer)
//   5. idle workers SLEEP (they use almost no CPU while waiting)
//   6. a sleeping worker wakes up quickly when a job arrives
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -pthread -o test_thread_pool test_thread_pool.cpp thread_pool.cpp
// Run:   ./test_thread_pool    (do not compile with -DNDEBUG: asserts must be active)

#include <assert.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "thread_pool.h"

using namespace std;
using namespace std::chrono;

static double now_sec() {
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Poll until pred() is true, or give up after timeout_s seconds.
static bool wait_until(const function<bool()> &pred, double timeout_s) {
    double end = now_sec() + timeout_s;
    while (now_sec() < end) {
        if (pred()) {
            return true;
        }
        usleep(1000);
    }
    return pred();
}

// CPU time (user + system) this whole process has used so far, in seconds.
static double cpu_sec() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
           ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

// ---------------- jobs used by the tests ----------------

static const int kMax = 200000;
static atomic<int> g_ran[kMax];        // g_ran[id] = how many times job `id` ran
static atomic<int> g_total{0};

static void job_count(void *arg) {
    int id = (int)(intptr_t)arg;
    g_ran[id].fetch_add(1);
    g_total.fetch_add(1);
}

static mutex g_mu;
static set<pthread_t> g_thread_ids;
static atomic<int> g_sleepers_done{0};

static void job_sleep_100ms(void *) {
    {
        lock_guard<mutex> lk(g_mu);
        g_thread_ids.insert(pthread_self());
    }
    usleep(100 * 1000);
    g_sleepers_done.fetch_add(1);
}

static vector<int> g_order;            // written only by the single worker of test 3
static atomic<int> g_order_done{0};

static void job_record_order(void *arg) {
    g_order.push_back((int)(intptr_t)arg);
    g_order_done.fetch_add(1);
}

static atomic<double> g_woke_at{0};

static void job_stamp(void *) {
    g_woke_at.store(now_sec());
}

// ---------------- the tests ----------------

static void test_exactly_once() {
    static ThreadPool tp;
    thread_pool_init(&tp, 4);
    const int N = 100000;
    for (int i = 0; i < N; i++) {
        thread_pool_queue(&tp, &job_count, (void *)(intptr_t)i);
    }
    assert(wait_until([] { return g_total.load() == N; }, 20));
    usleep(100 * 1000);                       // give any DUPLICATE runs time to show up
    assert(g_total.load() == N);
    for (int i = 0; i < N; i++) {
        assert(g_ran[i].load() == 1);
    }
    printf("1. %d jobs, each ran exactly once: OK\n", N);
}

static void test_parallel() {
    static ThreadPool tp;
    thread_pool_init(&tp, 4);
    double t0 = now_sec();
    for (int i = 0; i < 8; i++) {
        thread_pool_queue(&tp, &job_sleep_100ms, nullptr);
    }
    assert(wait_until([] { return g_sleepers_done.load() == 8; }, 10));
    double elapsed = now_sec() - t0;
    // 8 jobs of 100 ms on 4 workers = 2 rounds = ~200 ms.
    // One worker only would take 800 ms; 8 workers would take 100 ms.
    assert(elapsed >= 0.19 && elapsed <= 0.45);
    assert(g_thread_ids.size() >= 2);
    printf("2. 8 x 100 ms jobs on 4 workers took %.0f ms using %zu threads: OK\n",
           elapsed * 1000, g_thread_ids.size());
}

static void test_fifo_single_worker() {
    static ThreadPool tp;
    thread_pool_init(&tp, 1);
    const int N = 5000;
    for (int i = 0; i < N; i++) {
        thread_pool_queue(&tp, &job_record_order, (void *)(intptr_t)i);
    }
    assert(wait_until([] { return g_order_done.load() == N; }, 10));
    assert((int)g_order.size() == N);
    for (int i = 0; i < N; i++) {
        assert(g_order[i] == i);              // same order they were queued
    }
    printf("3. one worker ran %d jobs in the order queued: OK\n", N);
}

static void test_multi_producer() {
    static ThreadPool tp;
    thread_pool_init(&tp, 4);
    g_total.store(0);
    for (int i = 0; i < kMax; i++) {
        g_ran[i].store(0);
    }
    const int PRODUCERS = 4, EACH = 40000;    // 160000 jobs in total
    vector<thread> producers;
    for (int p = 0; p < PRODUCERS; p++) {
        producers.emplace_back([p] {
            for (int i = 0; i < EACH; i++) {
                thread_pool_queue(&tp, &job_count, (void *)(intptr_t)(p * EACH + i));
            }
        });
    }
    for (auto &t : producers) {
        t.join();
    }
    const int N = PRODUCERS * EACH;
    assert(wait_until([N] { return g_total.load() == N; }, 20));
    usleep(100 * 1000);
    assert(g_total.load() == N);
    for (int i = 0; i < N; i++) {
        assert(g_ran[i].load() == 1);
    }
    printf("4. %d producer threads queued %d jobs: each ran exactly once: OK\n", PRODUCERS, N);
}

static void test_idle_workers_sleep() {
    static ThreadPool tp;
    thread_pool_init(&tp, 4);
    usleep(50 * 1000);                        // let the workers reach their wait
    double cpu0 = cpu_sec();
    usleep(400 * 1000);                       // 400 ms of doing NOTHING
    double used = cpu_sec() - cpu0;
    // If idle workers looped instead of sleeping, 4 threads would burn
    // about 1.6 s of CPU in 400 ms. Sleeping workers use almost none.
    assert(used < 0.08);
    printf("5. 4 idle workers used %.0f ms of CPU in 400 ms (they sleep): OK\n", used * 1000);

    // 6. a job queued now must wake a sleeping worker quickly
    double t0 = now_sec();
    thread_pool_queue(&tp, &job_stamp, nullptr);
    assert(wait_until([] { return g_woke_at.load() > 0; }, 2));
    double latency = g_woke_at.load() - t0;
    assert(latency < 0.05);
    printf("6. a sleeping worker woke up and ran the job in %.2f ms: OK\n", latency * 1000);
}

int main() {
    test_exactly_once();
    test_parallel();
    test_fifo_single_worker();
    test_multi_producer();
    test_idle_workers_sleep();
    printf("ALL TESTS PASSED\n");
    return 0;
}
