// learning/ch10/avl.h
//
// A self-balancing binary search tree (AVL tree), written as an
// "intrusive" structure like the hashtable in chapter 8: the tree does
// NOT own your data. You embed an AVLNode inside your own struct:
//
//     struct Data {
//         AVLNode node;      // <-- the tree links live in here
//         uint32_t val;      //     your own data
//     };
//
// WHAT IS AN AVL TREE?
// A normal binary search tree (BST) keeps smaller values on the left
// and bigger values on the right. If you insert 1,2,3,4,5 in order, a
// plain BST becomes a long chain (like a linked list) and searching
// takes O(n). An AVL tree fixes this with one rule:
//
//     For every node, the heights of its left and right subtrees
//     differ by AT MOST 1.
//
// When an insert or delete breaks the rule, the tree repairs itself
// with "rotations". That keeps the height about log2(n), so search,
// insert and delete are all O(log n) no matter what order you insert.
//
// WHY DOES REDIS NEED THIS? Sorted sets (chapter 11) must keep items
// ordered by score AND answer "what is the item ranked #10,000,000?"
// quickly. The `cnt` field below is what makes rank queries fast.

#pragma once

#include <stddef.h>
#include <stdint.h>

struct AVLNode {
    uint32_t depth = 0;      // height of the subtree rooted at this node (a leaf has depth 1)
    uint32_t cnt = 0;        // number of nodes in the subtree rooted here (this node + all below)
    AVLNode *left = nullptr;
    AVLNode *right = nullptr;
    AVLNode *parent = nullptr;   // pointing UP lets us walk from a changed node to the root
};

// Make a fresh single node: depth 1, cnt 1, no links.
void avl_init(AVLNode *node);

// Null-safe helpers: an empty subtree (nullptr) has depth 0 and cnt 0.
uint32_t avl_depth(AVLNode *node);
uint32_t avl_cnt(AVLNode *node);

// Call this AFTER you have attached a new leaf (insertion). It walks
// from `node` up to the root, repairing balance. Returns the (possibly
// new) root of the whole tree.
AVLNode *avl_fix(AVLNode *node);

// Remove `node` from its tree and return the (possibly new) root.
// It does NOT free the node; the caller owns the memory.
AVLNode *avl_del(AVLNode *node);
