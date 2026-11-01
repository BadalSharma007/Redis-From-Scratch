// learning/ch14/test_async_free.cpp      (NOT from the book)
//
// Tests exactly what the chapter adds to the server, but WITHOUT the
// network: build big sorted sets on the main thread, then hand them to
// the thread pool to be freed (zset_dispose + delete), the way
// entry_del_async does.
//
// Why a separate program? This one EXITS normally, so a memory checker
// (AddressSanitizer's leak detector) can verify at exit that every
// node was freed exactly once. It can also run under ThreadSanitizer,
// which detects unsynchronized access between the threads.
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -pthread -o test_async_free test_async_free.cpp zset.cpp avl.cpp hashtable.cpp thread_pool.cpp
// Run:   ./test_async_free
// Extra: build with -fsanitize=address (leaks) or -fsanitize=thread (races).

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "common.h"
#include "thread_pool.h"
#include "zset.h"

using namespace std;

static atomic<int> g_freed{0};

// What the server's Entry looks like for this purpose: it owns a ZSet.
struct Holder {
    ZSet *zset = nullptr;
};

// The job a worker runs: the same steps as entry_destroy for a zset.
static void free_holder(void *arg) {
    Holder *h = (Holder *)arg;
    zset_dispose(h->zset);     // frees every node and the hashtable's bucket arrays
    delete h->zset;
    delete h;
    g_freed.fetch_add(1);
}

static Holder *make_zset(int members, int salt) {
    Holder *h = new Holder();
    h->zset = new ZSet();
    for (int i = 0; i < members; i++) {
        string name = "member" + to_string(salt) + "_" + to_string(i);
        bool added = zset_add(h->zset, name.data(), name.size(), (double)(i % 977));
        assert(added);
        (void)added;
    }
    return h;
}

int main() {
    static ThreadPool tp;
    thread_pool_init(&tp, 4);

    // 1. one big set freed by a worker, while the main thread keeps
    //    using a DIFFERENT set at the same time
    Holder *keep = make_zset(2000, 99);              // stays alive on the main thread
    Holder *big = make_zset(50000, 1);
    thread_pool_queue(&tp, &free_holder, big);       // ownership moves to the pool:
                                                     // the main thread never touches `big` again
    for (int i = 0; i < 2000; i++) {                 // meanwhile, keep using `keep`
        string name = "member99_" + to_string(i);
        assert(zset_lookup(keep->zset, name.data(), name.size()));
    }
    ZNode *first = zset_query(keep->zset, -1, "", 0, 0);
    assert(first != nullptr);

    // 2. more jobs than workers: 12 sets of varying sizes queued at once
    for (int k = 0; k < 12; k++) {
        thread_pool_queue(&tp, &free_holder, make_zset(10001 + k * 1500, 100 + k));
    }

    // wait for all 13 frees to complete
    auto start = chrono::steady_clock::now();
    while (g_freed.load() < 13) {
        usleep(1000);
        assert(chrono::steady_clock::now() - start < chrono::seconds(30));
    }
    assert(g_freed.load() == 13);
    printf("13 sorted sets freed by worker threads (1 big + 12 queued at once): OK\n");

    // 3. the set the main thread kept is untouched and still correct
    for (int i = 0; i < 2000; i++) {
        string name = "member99_" + to_string(i);
        ZNode *n = zset_lookup(keep->zset, name.data(), name.size());
        assert(n && n->score == (double)(i % 977));
    }
    printf("the set kept on the main thread is intact: OK\n");

    // free the kept one ourselves (main thread) so nothing is left over
    zset_dispose(keep->zset);
    delete keep->zset;
    delete keep;

    printf("ALL TESTS PASSED\n");
    return 0;
}
