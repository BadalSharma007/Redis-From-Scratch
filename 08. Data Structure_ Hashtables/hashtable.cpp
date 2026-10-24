// learning/ch08/hashtable.cpp
//
// Build: compiled together with the server (see kv_server.cpp header).

#include <assert.h>
#include <stdlib.h>

#include "hashtable.h"

// ---------------------------------------------------------------
// Part 1: the basic fixed-size table (HTab)
// ---------------------------------------------------------------

// Creates a table with n buckets. n MUST be a power of two (4, 8, 16...).
// Why? Because then "hash % n" can be replaced by the much faster
// "hash & (n-1)". Example, n = 8 -> mask = 7 = 0b111, so
// hash & mask keeps just the lowest 3 bits, giving a number 0..7.
static void h_init(HTab *htab, size_t n) {
    assert(n > 0 && ((n - 1) & n) == 0);   // power-of-two check
    htab->tab = (HNode **)calloc(sizeof(HNode *), n);  // calloc = zeroed = all buckets empty
    htab->mask = n - 1;
    htab->size = 0;
}

// Insert a node at the FRONT of its bucket's chain (O(1)).
//
//   before:  tab[pos] -> A -> B
//   after:   tab[pos] -> NODE -> A -> B
static void h_insert(HTab *htab, HNode *node) {
    size_t pos = node->hcode & htab->mask;   // which bucket?
    HNode *next = htab->tab[pos];            // current first node (maybe null)
    node->next = next;                       // new node points to old first
    htab->tab[pos] = node;                   // bucket now starts at new node
    htab->size++;
}

// Find a node. Walk the bucket's chain comparing keys.
//
// IMPORTANT trick: this returns a pointer to the POINTER that points at
// the found node (a HNode**), not the node itself. Why? Because to
// delete a node from a linked list you must change whatever points to it,
// and that "whatever" is either the bucket slot (tab[pos]) or the
// previous node's `next` field. Both are just an `HNode*` variable
// somewhere in memory, so a pointer-to-that-variable handles both cases
// with identical code -- no special case for "deleting the first node".
//
//   tab[pos] --> A --> B --> C         looking for B
//   from starts as &tab[pos]           (points at the variable holding A)
//   then from = &A->next               (points at the variable holding B)
//   found: *from == B, and `from` is the thing to rewrite to unlink B.
static HNode **h_lookup(HTab *htab, HNode *key, bool (*cmp)(HNode *, HNode *)) {
    if (!htab->tab) {
        return nullptr;   // table not allocated yet
    }
    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];
    while (*from) {
        if (cmp(*from, key)) {
            return from;
        }
        from = &(*from)->next;
    }
    return nullptr;
}

// Unlink the node that `*from` points to and hand it back to the caller
// (the caller decides whether to free it). Works for first-in-bucket
// and middle-of-chain nodes with the same two lines, thanks to HNode**.
static HNode *h_detach(HTab *htab, HNode **from) {
    HNode *node = *from;
    *from = (*from)->next;   // whoever pointed at `node` now skips over it
    htab->size--;
    return node;
}

// ---------------------------------------------------------------
// Part 2: progressive resizing (HMap)
// ---------------------------------------------------------------
//
// A fixed table gets slow when buckets grow long chains, so we must
// move to a bigger table. But moving a million nodes at once would
// freeze the server for a noticeable time. Instead:
//   1. Make a bigger table (new ht1); the old one becomes ht2.
//   2. Every operation moves just a few nodes (128) from ht2 to ht1.
//   3. When ht2 is empty, free it. Done -- no single long pause.
// While this is in progress a key may live in EITHER table, so lookups
// check both.

const size_t k_resizing_work = 128;   // nodes moved per operation
const size_t k_max_load_factor = 8;   // avg chain length that triggers growth

// Move up to k_resizing_work nodes from the old table (ht2) to the new (ht1).
static void hm_help_resizing(HMap *hmap) {
    if (hmap->ht2.tab == nullptr) {
        return;   // not resizing right now
    }
    size_t nwork = 0;
    while (nwork < k_resizing_work && hmap->ht2.size > 0) {
        // resizing_pos remembers which bucket of ht2 we're up to
        HNode **from = &hmap->ht2.tab[hmap->resizing_pos];
        if (!*from) {
            hmap->resizing_pos++;   // empty bucket, skip it (not counted as work)
            continue;
        }
        // take the first node of this bucket out of ht2 and put it in ht1
        h_insert(&hmap->ht1, h_detach(&hmap->ht2, from));
        nwork++;
    }

    if (hmap->ht2.size == 0) {
        // old table fully drained: free it and reset ht2 to "empty"
        free(hmap->ht2.tab);
        hmap->ht2 = HTab{};
    }
}

// Begin a resize: old table becomes ht2, a table twice as big becomes ht1.
static void hm_start_resizing(HMap *hmap) {
    assert(hmap->ht2.tab == nullptr);   // never start while already resizing
    hmap->ht2 = hmap->ht1;                          // demote current table
    h_init(&hmap->ht1, (hmap->ht1.mask + 1) * 2);   // new table, double size
    hmap->resizing_pos = 0;
}

HNode *hm_lookup(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *)) {
    hm_help_resizing(hmap);   // every operation does a little resizing work
    HNode **from = h_lookup(&hmap->ht1, key, cmp);
    if (!from) {
        from = h_lookup(&hmap->ht2, key, cmp);   // maybe not moved yet
    }
    return from ? *from : nullptr;
}

void hm_insert(HMap *hmap, HNode *node) {
    if (!hmap->ht1.tab) {
        h_init(&hmap->ht1, 4);   // first ever insert: start with 4 buckets
    }
    h_insert(&hmap->ht1, node);   // new nodes always go into the newest table

    if (!hmap->ht2.tab) {
        // not already resizing: check whether chains have become too long
        size_t load_factor = hmap->ht1.size / (hmap->ht1.mask + 1);
        if (load_factor >= k_max_load_factor) {
            hm_start_resizing(hmap);
        }
    }
    hm_help_resizing(hmap);
}

// Remove a key and return its node (caller frees it), or null if absent.
HNode *hm_pop(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *)) {
    hm_help_resizing(hmap);
    HNode **from = h_lookup(&hmap->ht1, key, cmp);
    if (from) {
        return h_detach(&hmap->ht1, from);
    }
    from = h_lookup(&hmap->ht2, key, cmp);
    if (from) {
        return h_detach(&hmap->ht2, from);
    }
    return nullptr;
}
