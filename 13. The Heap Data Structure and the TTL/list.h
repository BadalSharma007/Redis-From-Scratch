// learning/ch12/list.h
//
// A circular doubly linked list, used to keep connections ordered by
// "last activity" so the one that will time out first is always at the
// front.
//
// It is INTRUSIVE (like the hashtable and AVL tree): you embed a DList
// inside your own struct and the list links through it:
//
//     struct Conn {
//         ...
//         DList idle_list;     // this connection's place in the list
//     };
//
// HOW THE LIST IS SHAPED
// There is one extra "dummy head" node that holds no data. The nodes
// form a ring:
//
//        +--------------------------------------------+
//        v                                            |
//     [head] <-> [node A] <-> [node B] <-> [node C] <-+
//     (dummy)     (front)                     (back)
//
//   head->next  is the FRONT  (oldest / will expire first)
//   head->prev  is the BACK   (newest)
//
// An empty list is just the head pointing at itself. Because the ring
// has no nullptr ends, inserting and removing never need special cases
// for "first node" or "last node".

#pragma once

struct DList {
    DList *prev = nullptr;
    DList *next = nullptr;
};

// Make `node` an empty list (a ring containing only itself).
// Call this once on the dummy head before using the list.
inline void dlist_init(DList *node) {
    node->prev = node->next = node;
}

// A list is empty when the dummy head points at itself.
inline bool dlist_empty(DList *node) {
    return node->next == node;
}

// Unlink `node` from whatever list it is in: its two neighbours now
// point at each other, skipping it.
//
//   before:  prev <-> node <-> next
//   after:   prev <-> next
//
// (node's own prev/next still hold their old values afterwards; that is
// fine because the caller either frees the node or re-inserts it.)
inline void dlist_detach(DList *node) {
    DList *prev = node->prev;
    DList *next = node->next;
    prev->next = next;
    next->prev = prev;
}

// Insert `rookie` right BEFORE `target`.
//
//   before:  prev <-> target
//   after:   prev <-> rookie <-> target
//
// Calling it with target = the dummy head inserts at the BACK of the
// list, because "before the head" wraps around to the end of the ring.
inline void dlist_insert_before(DList *target, DList *rookie) {
    DList *prev = target->prev;
    prev->next = rookie;
    rookie->prev = prev;
    rookie->next = target;
    target->prev = rookie;
}
