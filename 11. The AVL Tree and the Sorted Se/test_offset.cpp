// learning/ch11/test_offset.cpp
//
// The book's test for avl_offset. For trees of many sizes, it checks
// that jumping from ANY node to ANY other node by the right offset
// lands exactly on the right value, and that jumping off either end
// returns nullptr.
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o test_offset test_offset.cpp avl.cpp
// Run:   ./test_offset   (do not compile with -DNDEBUG: asserts must be active)

#include <assert.h>
#include <stdio.h>

#include "avl.h"
#include "common.h"

struct Data {
    AVLNode node;
    uint32_t val = 0;
};

struct Container {
    AVLNode *root = nullptr;
};

// Insert val into the tree (ordinary tree insert + avl_fix, as in chapter 10).
static void add(Container &c, uint32_t val) {
    Data *data = new Data();
    avl_init(&data->node);
    data->val = val;

    if (!c.root) {
        c.root = &data->node;
        return;
    }
    AVLNode *cur = c.root;
    while (true) {
        AVLNode **from = (val < container_of(cur, Data, node)->val) ? &cur->left : &cur->right;
        if (!*from) {
            *from = &data->node;
            data->node.parent = cur;
            c.root = avl_fix(&data->node);
            break;
        }
        cur = *from;
    }
}

// Free the whole tree (post-order: children first).
static void dispose(AVLNode *node) {
    if (!node) {
        return;
    }
    dispose(node->left);
    dispose(node->right);
    delete container_of(node, Data, node);
}

static void test_case(uint32_t sz) {
    Container c;
    for (uint32_t i = 0; i < sz; ++i) {
        add(c, i);                              // tree holds the values 0..sz-1
    }

    AVLNode *min = c.root;                      // the smallest value is the leftmost node
    while (min->left) {
        min = min->left;
    }

    for (uint32_t i = 0; i < sz; ++i) {
        // from the minimum, offset i must land on value i
        AVLNode *node = avl_offset(min, (int64_t)i);
        assert(container_of(node, Data, node)->val == i);

        // from node i, offset (j - i) must land on value j, for every j
        for (uint32_t j = 0; j < sz; ++j) {
            int64_t offset = (int64_t)j - (int64_t)i;
            AVLNode *n2 = avl_offset(node, offset);
            assert(container_of(n2, Data, node)->val == j);
        }

        // one step before the first / one step past the last: nothing there
        assert(!avl_offset(node, -(int64_t)i - 1));
        assert(!avl_offset(node, (int64_t)sz - (int64_t)i));
    }

    dispose(c.root);
}

int main() {
    for (uint32_t i = 1; i < 300; ++i) {
        test_case(i);
    }
    printf("avl_offset: ALL TESTS PASSED\n");
    return 0;
}
