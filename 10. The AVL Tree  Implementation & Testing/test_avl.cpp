// learning/ch10/test_avl.cpp
//
// Tests for the AVL tree. An AVL tree is much easier to get subtly
// wrong than a hashtable, so the book invests heavily in testing.
//
// HOW THE TESTS WORK
// We keep a second, trusted copy of the data in a std::multiset (a
// sorted container from the standard library). After every change we
// check that our tree and the multiset agree, AND that all the AVL
// rules still hold (avl_verify). If either differs, an assert fires.
//
// IMPORTANT: asserts must be active, so do NOT compile with -DNDEBUG.
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o test_avl test_avl.cpp avl.cpp
// Run:   ./test_avl

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <cmath>
#include <set>

#include "avl.h"

using namespace std;

// Same macro as chapter 8: from a pointer to the embedded AVLNode,
// get back to the pointer of the struct that contains it.
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

// ---------------------------------------------------------------
// Test data types
// ---------------------------------------------------------------

// What we store: an AVLNode (the tree link) plus one number.
struct Data {
    AVLNode node;
    uint32_t val = 0;
};

// A tiny wrapper holding just the root pointer of a tree.
struct Container {
    AVLNode *root = nullptr;
};

// ---------------------------------------------------------------
// Insert and delete (these show HOW to use the AVL functions)
// ---------------------------------------------------------------

// Insert = ordinary binary search tree insert, then avl_fix.
static void add(Container &c, uint32_t val) {
    Data *data = new Data();
    avl_init(&data->node);
    data->val = val;

    if (!c.root) {                       // empty tree: new node is the root
        c.root = &data->node;
        return;
    }

    AVLNode *cur = c.root;
    while (true) {
        // go left if val is smaller, otherwise right.
        // `from` points at the child SLOT we would descend into.
        AVLNode **from = (val < container_of(cur, Data, node)->val)
                             ? &cur->left : &cur->right;
        if (!*from) {                    // empty slot found: attach here
            *from = &data->node;
            data->node.parent = cur;
            c.root = avl_fix(&data->node);   // repair balance up to the root
            break;
        }
        cur = *from;                     // otherwise keep descending
    }
}

// Delete = find the node, avl_del it, free it.
static bool del(Container &c, uint32_t val) {
    AVLNode *cur = c.root;
    while (cur) {                        // ordinary BST search
        uint32_t node_val = container_of(cur, Data, node)->val;
        if (val == node_val) {
            break;
        }
        cur = val < node_val ? cur->left : cur->right;
    }
    if (!cur) {
        return false;                    // value not in the tree
    }

    c.root = avl_del(cur);               // unlink (does not free)
    delete container_of(cur, Data, node);// we own the memory
    return true;
}

// ---------------------------------------------------------------
// Verification helpers
// ---------------------------------------------------------------

// Recursively check EVERY rule of the tree, for every node.
static void avl_verify(AVLNode *parent, AVLNode *node) {
    if (!node) {
        return;
    }

    // 1. the parent pointer must point back to the real parent
    assert(node->parent == parent);

    // 2. check both children first
    avl_verify(node, node->left);
    avl_verify(node, node->right);

    // 3. cnt must equal 1 + sizes of both subtrees
    assert(node->cnt == 1 + avl_cnt(node->left) + avl_cnt(node->right));

    // 4. the AVL rule: left/right heights differ by at most 1
    uint32_t l = avl_depth(node->left);
    uint32_t r = avl_depth(node->right);
    assert(l == r || l + 1 == r || l == r + 1);

    // 5. depth must equal 1 + the taller child
    assert(node->depth == 1 + max(l, r));

    // 6. ordering: left value <= this value <= right value
    uint32_t val = container_of(node, Data, node)->val;
    if (node->left) {
        assert(node->left->parent == node);
        assert(container_of(node->left, Data, node)->val <= val);
    }
    if (node->right) {
        assert(node->right->parent == node);
        assert(container_of(node->right, Data, node)->val >= val);
    }
}

// In-order traversal (left, node, right) collects values in sorted order.
static void extract(AVLNode *node, multiset<uint32_t> &extracted) {
    if (!node) {
        return;
    }
    extract(node->left, extracted);
    extracted.insert(container_of(node, Data, node)->val);
    extract(node->right, extracted);
}

// Full check: structure is valid AND contents equal the reference.
static void container_verify(Container &c, const multiset<uint32_t> &ref) {
    avl_verify(nullptr, c.root);
    assert(avl_cnt(c.root) == ref.size());
    multiset<uint32_t> extracted;
    extract(c.root, extracted);
    assert(extracted == ref);
}

// Delete every node so the test does not leak memory.
static void dispose(Container &c) {
    while (c.root) {
        AVLNode *node = c.root;
        c.root = avl_del(c.root);
        delete container_of(node, Data, node);
    }
}

// ---------------------------------------------------------------
// "Targeted" tests: for a tree of size sz, insert or delete at EVERY
// possible position. This exercises every rotation shape.
// ---------------------------------------------------------------

static void test_insert(uint32_t sz) {
    for (uint32_t val = 0; val < sz; ++val) {
        Container c;
        multiset<uint32_t> ref;
        for (uint32_t i = 0; i < sz; ++i) {   // build a tree WITHOUT `val`
            if (i == val) {
                continue;
            }
            add(c, i);
            ref.insert(i);
        }
        container_verify(c, ref);

        add(c, val);                           // now insert it
        ref.insert(val);
        container_verify(c, ref);
        dispose(c);
    }
}

static void test_remove(uint32_t sz) {
    for (uint32_t val = 0; val < sz; ++val) {
        Container c;
        multiset<uint32_t> ref;
        for (uint32_t i = 0; i < sz; ++i) {   // build a full tree
            add(c, i);
            ref.insert(i);
        }
        container_verify(c, ref);

        assert(del(c, val));                   // remove one value
        ref.erase(val);
        container_verify(c, ref);
        dispose(c);
    }
}

// ---------------------------------------------------------------
// EXTRA test (NOT in the book): a big random workload, plus a check
// that the tree really stays short. An AVL tree of n nodes can never
// be taller than about 1.44 * log2(n + 2).
// ---------------------------------------------------------------
static void test_large_random() {
    Container c;
    multiset<uint32_t> ref;
    for (int i = 0; i < 200000; i++) {
        uint32_t val = (uint32_t)rand() % 5000;
        if (rand() % 3 != 0) {                 // 2/3 inserts, 1/3 deletes
            add(c, val);
            ref.insert(val);
        } else {
            auto it = ref.find(val);
            if (it == ref.end()) {
                assert(!del(c, val));
            } else {
                assert(del(c, val));
                ref.erase(it);
            }
        }
        if (i % 20000 == 0) {
            container_verify(c, ref);          // full check now and then (it is O(n))
        }
    }
    container_verify(c, ref);
    double bound = 1.44 * log2((double)ref.size() + 2);
    printf("  large random: %zu nodes, tree height %u (AVL limit ~%.1f)\n",
           ref.size(), avl_depth(c.root), bound);
    assert(avl_depth(c.root) <= bound + 1);
    dispose(c);
}

// A plain unbalanced BST would turn sorted input into a chain of n
// levels. The AVL tree must keep it near log2(n).
static void test_sorted_input_stays_short() {
    Container c;
    const uint32_t n = 100000;
    for (uint32_t i = 0; i < n; i++) {
        add(c, i);                             // worst case for a plain BST
    }
    avl_verify(nullptr, c.root);
    printf("  sorted insert of %u values: height %u (a plain BST would be %u)\n",
           n, avl_depth(c.root), n);
    assert(avl_depth(c.root) <= 1.44 * log2((double)n + 2) + 1);
    dispose(c);
}

int main() {
    Container c;

    // --- the book's tests ---

    // some quick tests
    container_verify(c, {});
    add(c, 123);
    container_verify(c, {123});
    assert(!del(c, 124));                      // deleting a missing value fails
    assert(del(c, 123));
    container_verify(c, {});
    printf("quick tests passed\n");

    // sequential insertion
    multiset<uint32_t> ref;
    for (uint32_t i = 0; i < 1000; i += 3) {
        add(c, i);
        ref.insert(i);
        container_verify(c, ref);
    }
    printf("sequential insertion passed\n");

    // random insertion
    for (uint32_t i = 0; i < 100; i++) {
        uint32_t val = (uint32_t)rand() % 1000;
        add(c, val);
        ref.insert(val);
        container_verify(c, ref);
    }
    printf("random insertion passed\n");

    // random deletion
    for (uint32_t i = 0; i < 200; i++) {
        uint32_t val = (uint32_t)rand() % 1000;
        auto it = ref.find(val);
        if (it == ref.end()) {
            assert(!del(c, val));
        } else {
            assert(del(c, val));
            ref.erase(it);
        }
        container_verify(c, ref);
    }
    printf("random deletion passed\n");
    dispose(c);

    // insertion/deletion at various positions, for tree sizes 0..199
    for (uint32_t i = 0; i < 200; ++i) {
        test_insert(i);
        test_remove(i);
    }
    printf("insert/remove at every position (sizes 0-199) passed\n");

    // --- extra tests (not in the book) ---
    printf("extra tests:\n");
    test_large_random();
    test_sorted_input_stays_short();

    printf("ALL TESTS PASSED\n");
    return 0;
}
