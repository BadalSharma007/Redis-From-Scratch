// learning/ch13/test_heap.cpp
//
// Tests the heap on its own (no networking). It acts out exactly what
// the server does with TTLs: add a timer, change a timer's time, remove
// a timer. After every step it checks:
//   1. the heap rule: every parent <= its children
//   2. the back-pointers: each item's `ref` holds its true index, and
//      each owner's heap_idx points at an item that points back at it
//   3. the contents match a trusted std::multiset
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o test_heap test_heap.cpp heap.cpp
// Run:   ./test_heap    (do not compile with -DNDEBUG: asserts must be active)

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <vector>

#include "heap.h"

using namespace std;

// Stands in for the server's Entry: it only remembers its own timer position.
struct Owner {
    size_t heap_idx = (size_t)-1;   // -1 = "no timer"
};

static vector<HeapItem> heap;       // like g_data.heap in the server
static vector<Owner *> owners;      // every owner that currently has a timer

// ---- the same three operations the server performs ----

static void add_timer(Owner *o, uint64_t val) {
    HeapItem item;
    item.ref = &o->heap_idx;
    heap.push_back(item);
    size_t pos = heap.size() - 1;
    heap[pos].val = val;
    heap_update(heap.data(), pos, heap.size());
}

static void change_timer(Owner *o, uint64_t val) {
    size_t pos = o->heap_idx;
    heap[pos].val = val;
    heap_update(heap.data(), pos, heap.size());
}

// Remove by replacing with the LAST item, then fixing that one's position.
static void remove_timer(Owner *o) {
    size_t pos = o->heap_idx;
    heap[pos] = heap.back();
    heap.pop_back();
    if (pos < heap.size()) {
        heap_update(heap.data(), pos, heap.size());
    }
    o->heap_idx = (size_t)-1;
}

// ---- the checks ----

static void verify(const multiset<uint64_t> &ref) {
    assert(heap.size() == ref.size());
    for (size_t i = 0; i < heap.size(); i++) {
        // 1. heap rule
        if (i > 0) {
            assert(heap[(i + 1) / 2 - 1].val <= heap[i].val);
        }
        // 2a. each item's ref holds its real index
        assert(*heap[i].ref == i);
    }
    // 2b. each owner points at an item that points back at it
    for (Owner *o : owners) {
        assert(o->heap_idx < heap.size());
        assert(heap[o->heap_idx].ref == &o->heap_idx);
    }
    // 3. same contents as the reference, and the root is the minimum
    multiset<uint64_t> vals;
    for (const HeapItem &h : heap) {
        vals.insert(h.val);
    }
    assert(vals == ref);
    if (!heap.empty()) {
        assert(heap[0].val == *ref.begin());
    }
}

// An owner is removed from the `owners` list when its timer is removed.
static void forget(Owner *o) {
    for (size_t i = 0; i < owners.size(); i++) {
        if (owners[i] == o) {
            owners[i] = owners.back();
            owners.pop_back();
            return;
        }
    }
    assert(0);
}

int main() {
    // ---- test 1: random add / change / remove ----
    multiset<uint64_t> ref;
    vector<Owner *> all;                    // every Owner ever created (to free at the end)
    for (int step = 0; step < 100000; step++) {
        int op = rand() % 10;
        if (op < 4 || owners.empty()) {
            // add a timer; use small values so equal values (ties) happen a lot
            Owner *o = new Owner();
            all.push_back(o);
            uint64_t v = (uint64_t)(rand() % 500);
            add_timer(o, v);
            owners.push_back(o);
            ref.insert(v);
        } else if (op < 7) {
            // change a random timer to a new value (smaller OR bigger)
            Owner *o = owners[rand() % owners.size()];
            uint64_t old = heap[o->heap_idx].val;
            uint64_t v = (uint64_t)(rand() % 500);
            change_timer(o, v);
            ref.erase(ref.find(old));
            ref.insert(v);
        } else {
            // remove a random timer
            Owner *o = owners[rand() % owners.size()];
            uint64_t old = heap[o->heap_idx].val;
            remove_timer(o);
            forget(o);
            ref.erase(ref.find(old));
            assert(o->heap_idx == (size_t)-1);
        }
        if (step < 3000 || step % 100 == 0) {
            verify(ref);
        }
    }
    verify(ref);
    printf("random add/change/remove: %zu timers left, all checks OK\n", heap.size());

    // ---- test 2: "heap sort": removing the root repeatedly must give
    //      the values in non-decreasing order (the way timers fire) ----
    uint64_t last = 0;
    size_t n = heap.size();
    while (!heap.empty()) {
        Owner *o = nullptr;
        for (Owner *x : owners) {            // find the owner of the root item
            if (x->heap_idx == 0) {
                o = x;
                break;
            }
        }
        assert(o);
        uint64_t v = heap[0].val;
        assert(v >= last);                   // never goes backwards
        last = v;
        remove_timer(o);
        forget(o);
        ref.erase(ref.find(v));
        verify(ref);
    }
    printf("drained %zu timers in sorted order: OK\n", n);

    for (Owner *o : all) {
        delete o;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
