// learning/ch11/zset.cpp
//
// Sorted set implementation. Read in this order:
//   1. creating a node             (znode_new)
//   2. ordering rule               (zless)
//   3. inserting into the tree     (tree_add)
//   4. lookup by name              (HKey, hcmp, zset_lookup)
//   5. add / update / remove       (zset_update, zset_add, zset_pop)
//   6. range query                 (zset_query)
//   7. cleanup                     (zset_dispose)
//
// Build: compiled together with avl.cpp and hashtable.cpp.

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "zset.h"

// ---------------------------------------------------------------
// 1. Creating and freeing a node
// ---------------------------------------------------------------

// ONE malloc holds the struct AND the name bytes that follow it:
//
//   [ ZNode fields ... | name bytes (len of them) ]
//                        ^ node->name points here
static ZNode *znode_new(const char *name, size_t len, double score) {
    ZNode *node = (ZNode *)malloc(sizeof(ZNode) + len);
    assert(node);

    avl_init(&node->tree);                               // fresh tree links
    node->hmap.next = nullptr;                           // fresh hashtable links...
    node->hmap.hcode = str_hash((uint8_t *)name, len);   // ...with the name's hash saved
    node->score = score;
    node->len = len;
    memcpy(&node->name[0], name, len);
    return node;
}

void znode_del(ZNode *node) {
    free(node);   // it was malloc'ed, so it is free()'d (not delete)
}

// ---------------------------------------------------------------
// 2. The ordering rule: compare by (score, then name, then length)
// ---------------------------------------------------------------

// Is the node `lhs` LESS than the tuple (score, name)?
static bool zless(AVLNode *lhs, double score, const char *name, size_t len) {
    ZNode *zl = container_of(lhs, ZNode, tree);
    if (zl->score != score) {
        return zl->score < score;                 // different scores: score decides
    }
    // same score: compare the names byte by byte over the shorter length
    size_t common = zl->len < len ? zl->len : len;
    int rv = memcmp(zl->name, name, common);
    if (rv != 0) {
        return rv < 0;
    }
    return zl->len < len;                         // one name is a prefix: shorter is smaller
}

// Same thing, comparing two nodes.
static bool zless(AVLNode *lhs, AVLNode *rhs) {
    ZNode *zr = container_of(rhs, ZNode, tree);
    return zless(lhs, zr->score, zr->name, zr->len);
}

// ---------------------------------------------------------------
// 3. Inserting into the AVL tree
// ---------------------------------------------------------------

// Ordinary binary-tree insert (go left if smaller, else right), then
// avl_fix to repair the balance. Same as the test code in chapter 10.
static void tree_add(ZSet *zset, ZNode *node) {
    if (!zset->tree) {                            // empty tree: this is the root
        zset->tree = &node->tree;
        return;
    }

    AVLNode *cur = zset->tree;
    while (true) {
        AVLNode **from = zless(&node->tree, cur) ? &cur->left : &cur->right;
        if (!*from) {                             // empty slot: attach here
            *from = &node->tree;
            node->tree.parent = cur;
            zset->tree = avl_fix(&node->tree);    // rebalance, get the (new) root
            break;
        }
        cur = *from;
    }
}

// ---------------------------------------------------------------
// 4. Lookup by name (uses the hashtable)
// ---------------------------------------------------------------

// A throwaway "search key" for the hashtable: an HNode plus the name
// we are looking for. Same idea as the stack-allocated Entry probe in
// the server.
struct HKey {
    HNode node;
    const char *name = nullptr;
    size_t len = 0;
};

// The hashtable calls this to ask: "is this stored node the one I'm looking for?"
static bool hcmp(HNode *node, HNode *key) {
    ZNode *znode = container_of(node, ZNode, hmap);
    HKey *hkey = container_of(key, HKey, node);
    if (znode->len != hkey->len) {
        return false;
    }
    return 0 == memcmp(znode->name, hkey->name, znode->len);
}

ZNode *zset_lookup(ZSet *zset, const char *name, size_t len) {
    if (!zset->tree) {
        return nullptr;                           // empty set: nothing to find
    }
    HKey key;
    key.node.hcode = str_hash((uint8_t *)name, len);
    key.name = name;
    key.len = len;
    HNode *found = hm_lookup(&zset->hmap, &key.node, &hcmp);
    return found ? container_of(found, ZNode, hmap) : nullptr;
}

// ---------------------------------------------------------------
// 5. Add / update / remove
// ---------------------------------------------------------------

// Change the score of an item that is already in the set.
// The tree is ordered by score, so the node must move to a new place:
// take it out, change the score, put it back in. The hashtable is NOT
// touched because the name (the hashtable's key) did not change.
static void zset_update(ZSet *zset, ZNode *node, double score) {
    if (node->score == score) {
        return;                                   // nothing to do
    }
    zset->tree = avl_del(&node->tree);            // unlink from the tree
    node->score = score;
    avl_init(&node->tree);                        // reset its tree links
    tree_add(zset, node);                         // re-insert at the right place
}

bool zset_add(ZSet *zset, const char *name, size_t len, double score) {
    ZNode *node = zset_lookup(zset, name, len);
    if (node) {
        zset_update(zset, node, score);           // existing name: just update
        return false;
    } else {
        node = znode_new(name, len, score);       // new name: create it...
        hm_insert(&zset->hmap, &node->hmap);      // ...and put it in BOTH structures
        tree_add(zset, node);
        return true;
    }
}

ZNode *zset_pop(ZSet *zset, const char *name, size_t len) {
    if (!zset->tree) {
        return nullptr;
    }
    HKey key;
    key.node.hcode = str_hash((uint8_t *)name, len);
    key.name = name;
    key.len = len;
    HNode *found = hm_pop(&zset->hmap, &key.node, &hcmp);   // out of the hashtable
    if (!found) {
        return nullptr;
    }
    ZNode *node = container_of(found, ZNode, hmap);
    zset->tree = avl_del(&node->tree);                       // out of the tree
    return node;
}

// ---------------------------------------------------------------
// 6. Range query
// ---------------------------------------------------------------

// Step 1: find the smallest item that is >= (score, name).
//         Walk down the tree; whenever a node is NOT smaller than the
//         target it is a candidate, and we try to find a better
//         (smaller) one on its left.
// Step 2: move `offset` places from it using avl_offset.
ZNode *zset_query(ZSet *zset, double score, const char *name, size_t len, int64_t offset) {
    AVLNode *found = nullptr;
    AVLNode *cur = zset->tree;
    while (cur) {
        if (zless(cur, score, name, len)) {
            cur = cur->right;        // cur < target: the answer is further right
        } else {
            found = cur;             // cur >= target: a candidate...
            cur = cur->left;         // ...but maybe a smaller one exists on the left
        }
    }

    if (found) {
        found = avl_offset(found, offset);
    }
    return found ? container_of(found, ZNode, tree) : nullptr;
}

// ---------------------------------------------------------------
// 7. Cleanup
// ---------------------------------------------------------------

static void tree_dispose(AVLNode *node) {
    if (!node) {
        return;
    }
    tree_dispose(node->left);
    tree_dispose(node->right);
    znode_del(container_of(node, ZNode, tree));
}

void zset_dispose(ZSet *zset) {
    tree_dispose(zset->tree);   // frees every node (each node is in the tree)
    zset->tree = nullptr;
    hm_destroy(&zset->hmap);    // frees the hashtable's own bucket arrays
}
