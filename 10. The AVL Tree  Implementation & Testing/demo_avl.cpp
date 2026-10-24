// learning/ch10/demo_avl.cpp   (NOT from the book: a learning aid)
//
// Inserts numbers one at a time and prints the tree after each insert
// so you can SEE the rotations. The tree is printed sideways: the
// ROOT is on the left, the right subtree is drawn above its parent,
// the left subtree below it. Tilt your head left to read it as a tree.
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o demo_avl demo_avl.cpp avl.cpp
// Run:   ./demo_avl

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "avl.h"

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

struct Data {
    AVLNode node;
    uint32_t val = 0;
};

static AVLNode *root = nullptr;

static void insert(uint32_t val) {
    Data *data = new Data();
    avl_init(&data->node);
    data->val = val;

    if (!root) {
        root = &data->node;
        return;
    }
    AVLNode *cur = root;
    while (true) {
        AVLNode **from = (val < container_of(cur, Data, node)->val) ? &cur->left : &cur->right;
        if (!*from) {
            *from = &data->node;
            data->node.parent = cur;
            root = avl_fix(&data->node);
            return;
        }
        cur = *from;
    }
}

// Print right subtree first (so it appears on top), then this node,
// then the left subtree. `level` controls the indentation.
static void print_tree(AVLNode *node, int level) {
    if (!node) {
        return;
    }
    print_tree(node->right, level + 1);
    for (int i = 0; i < level; i++) {
        printf("      ");
    }
    printf("%u\n", container_of(node, Data, node)->val);
    print_tree(node->left, level + 1);
}

int main() {
    // Sorted input is the WORST case for a plain binary tree (it would
    // become a straight chain). Watch the AVL tree stay balanced.
    for (uint32_t v = 1; v <= 7; v++) {
        insert(v);
        printf("--- after inserting %u (height %u) ---\n", v, avl_depth(root));
        print_tree(root, 0);
        printf("\n");
    }
    return 0;
}
