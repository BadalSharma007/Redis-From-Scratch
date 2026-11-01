// learning/ch11/zset.h
//
// A sorted set: a collection of (name, score) pairs that can be
//   - looked up by NAME           in O(1)      (via the hashtable)
//   - kept ordered by SCORE       in O(log n)  (via the AVL tree)
//   - range-queried by position   in O(log n)  (via avl_offset)
//
// One ZNode lives in BOTH structures at once. That is possible because
// both are "intrusive": each ZNode simply contains an AVLNode and an
// HNode, and the two structures link through their own part.

#pragma once

#include "avl.h"
#include "hashtable.h"

struct ZSet {
    AVLNode *tree = nullptr;   // root of the AVL tree (ordered by score, then name)
    HMap hmap;                 // hashtable (lookup by name)
};

struct ZNode {
    AVLNode tree;      // this node's links inside the AVL tree
    HNode hmap;        // this node's links inside the hashtable
    double score = 0;
    size_t len = 0;    // length of the name
    char name[0];      // the name's bytes live here, right after the struct.
                       // (A zero-length array at the end is a GNU extension.
                       // It is fine for learning, but not standard C++.)
};

// Add a new (name, score), or update the score if the name exists.
// Returns true if a NEW item was added, false if an existing one was updated.
bool zset_add(ZSet *zset, const char *name, size_t len, double score);

// Find an item by name (hashtable lookup). Returns nullptr if absent.
ZNode *zset_lookup(ZSet *zset, const char *name, size_t len);

// Remove an item by name and hand it back (caller frees it with znode_del).
ZNode *zset_pop(ZSet *zset, const char *name, size_t len);

// Find the first item >= (score, name) in sorted order, then move
// `offset` places from it (negative = backwards). nullptr if none.
ZNode *zset_query(ZSet *zset, double score, const char *name, size_t len, int64_t offset);

// Free one node / free the whole set.
void znode_del(ZNode *node);
void zset_dispose(ZSet *zset);
