// learning/ch08/test_hashtable.cpp
//
// Stress-tests hashtable.cpp on its own (no networking): inserts many
// keys so the table resizes several times, then checks that every key
// is still findable -- including while a resize is half finished.
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o test_hashtable test_hashtable.cpp hashtable.cpp
// Run:   ./test_hashtable

#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "hashtable.h"

using namespace std;

struct Item {
    HNode node;
    uint64_t id;
};

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

static bool item_eq(HNode *a, HNode *b) {
    return container_of(a, Item, node)->id == container_of(b, Item, node)->id;
}

// A simple spreading hash so ids land in different buckets.
static uint64_t hash_id(uint64_t id) {
    return id * 11400714819323198485ULL;
}

static bool exists(HMap *m, uint64_t id) {
    Item probe;
    probe.id = id;
    probe.node.hcode = hash_id(id);
    return hm_lookup(m, &probe.node, &item_eq) != nullptr;
}

int main() {
    HMap map;
    const uint64_t N = 200000;
    vector<Item *> items;

    // 1. insert N items, checking along the way that earlier ones survive
    //    (this includes moments when a resize is only partly done)
    for (uint64_t i = 0; i < N; i++) {
        Item *it = new Item();
        it->id = i;
        it->node.hcode = hash_id(i);
        hm_insert(&map, &it->node);
        items.push_back(it);

        if (i % 5000 == 0) {
            for (uint64_t j = 0; j <= i; j += 997) {
                assert(exists(&map, j));
            }
        }
    }
    printf("inserted %llu items, all spot checks passed\n", (unsigned long long)N);
    printf("table buckets now: %zu (was 4 at start)\n", map.ht1.mask + 1);

    // 2. every single key must be findable
    for (uint64_t i = 0; i < N; i++) {
        assert(exists(&map, i));
    }
    printf("all %llu keys found\n", (unsigned long long)N);

    // 3. a key that was never inserted must NOT be found
    assert(!exists(&map, N + 12345));
    printf("missing key correctly not found\n");

    // 4. delete the even ids, check evens gone and odds still there
    for (uint64_t i = 0; i < N; i += 2) {
        Item probe;
        probe.id = i;
        probe.node.hcode = hash_id(i);
        HNode *n = hm_pop(&map, &probe.node, &item_eq);
        assert(n != nullptr);
        delete container_of(n, Item, node);
    }
    for (uint64_t i = 0; i < N; i++) {
        assert(exists(&map, i) == (i % 2 == 1));
    }
    printf("deleted evens: evens gone, odds intact\n");

    size_t total = map.ht1.size + map.ht2.size;
    assert(total == N / 2);
    assert(hm_size(&map) == N / 2);   // chapter 9: hm_size must agree
    printf("final element count = %zu (expected %llu)\n", total, (unsigned long long)(N / 2));
    printf("ALL TESTS PASSED\n");
    return 0;
}
