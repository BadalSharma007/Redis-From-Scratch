// learning/ch08/hashtable.h
//
// A chaining hashtable with progressive resizing (the same idea real
// Redis uses so that growing a huge table never freezes the server).
//
// THE BIG IDEA -- an "intrusive" data structure:
// The table does NOT store your data. It only stores tiny HNode links,
// and YOU embed an HNode inside your own struct:
//
//     struct Entry {
//         HNode node;        // <-- the hashtable links live in here
//         string key;
//         string val;
//     };
//
// So one allocation holds both your data and its hashtable link. No
// extra malloc per node, no void*, no templates. To get from an HNode*
// back to your Entry*, the server uses the container_of macro.

#pragma once

#include <stddef.h>
#include <stdint.h>

// One link in a bucket's chain. Embed this inside your own struct.
struct HNode {
    HNode *next = nullptr;   // next node in the same bucket (collision chain)
    uint64_t hcode = 0;      // the key's hash value, saved so we never recompute it
};

// A simple fixed-size hashtable: an array of bucket heads.
struct HTab {
    HNode **tab = nullptr;   // array of pointers; tab[i] = first node of bucket i
    size_t mask = 0;         // (array size - 1). Size is always a power of 2.
    size_t size = 0;         // how many nodes are stored
};

// The real hashtable users touch. It owns TWO fixed tables so it can
// grow gradually:
//   ht1 = the table currently in use (newest, biggest)
//   ht2 = the OLD smaller table, being drained into ht1 (empty when idle)
struct HMap {
    HTab ht1;
    HTab ht2;
    size_t resizing_pos = 0; // where we are while draining ht2
};

// cmp(a, b) must return true if the two nodes hold the same key.
HNode *hm_lookup(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *));
void   hm_insert(HMap *hmap, HNode *node);
HNode *hm_pop(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *));

// NEW in chapter 9: total number of nodes stored (both tables together).
size_t hm_size(HMap *hmap);
