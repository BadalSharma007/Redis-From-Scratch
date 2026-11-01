// learning/ch13/heap.h
//
// A min-heap stored in a plain array, used to find the EARLIEST expiry
// time among all keys that have a TTL.
//
// Every HeapItem remembers where its owner keeps "my position in the
// heap" (the `ref` pointer). Whenever the heap moves an item, it writes
// the item's new position through `ref`. That is how a key (Entry) can
// always find its own timer in O(1): it just reads its own heap_idx.
//
//    Entry                        heap array
//   +-----------+                +-----+-----+-----+
//   | heap_idx=2|<---------------| ref | ref | ref |   (HeapItems)
//   | ...       |  item.ref      +-----+-----+-----+
//   +-----------+  points here     0     1     2
//        |                                    ^
//        +------------------------------------+
//             "my timer is at index 2"

#pragma once

#include <stddef.h>
#include <stdint.h>

struct HeapItem {
    uint64_t val = 0;        // what the heap sorts by: for TTLs, the expiry time
                             // (monotonic microseconds)
    size_t *ref = nullptr;   // points to the owner's heap-position field; the heap
                             // code keeps that field equal to this item's index
};

// Call this after you changed a[pos].val, or after you put a NEW item at
// a[pos]. It moves the item up or down until the heap rule holds again
// (every parent <= its children). `len` is the number of items in the array.
void heap_update(HeapItem *a, size_t pos, size_t len);
