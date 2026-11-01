// learning/ch13/heap.cpp
//
// Read in this order:
//   1. the index math     (heap_parent, heap_left, heap_right)
//   2. moving UP          (heap_up)
//   3. moving DOWN        (heap_down)
//   4. choosing which     (heap_update)

#include "heap.h"

/*
 * ---------------------------------------------------------------
 * 1. The tree layout is fixed, so parent and children are just
 *    arithmetic on the array index:
 *
 *            tree of indexes:        0
 *                                  /   \
 *                                 1     2
 *                                / \   / \
 *                               3   4 5   6
 * ---------------------------------------------------------------
 */

static size_t heap_parent(size_t i) {
    return (i + 1) / 2 - 1;
}

static size_t heap_left(size_t i) {
    return i * 2 + 1;
}

static size_t heap_right(size_t i) {
    return i * 2 + 2;
}

// ---------------------------------------------------------------
// 2. Moving an item UP: used when it became SMALLER than its parent.
//    Keep swapping with the parent while the parent is bigger.
//
//    Instead of doing real swaps, we hold the moving item in `t` and
//    just copy each bigger parent DOWN one level (half the writes of a
//    swap), then drop `t` into the final gap.
// ---------------------------------------------------------------
static void heap_up(HeapItem *a, size_t pos) {
    HeapItem t = a[pos];                       // the item that is moving
    while (pos > 0 && a[heap_parent(pos)].val > t.val) {
        a[pos] = a[heap_parent(pos)];          // the bigger parent moves DOWN into the gap
        *a[pos].ref = pos;                     // tell that parent's owner its new position
        pos = heap_parent(pos);                // the gap is now where the parent was
    }
    a[pos] = t;                                // put the moving item into the final gap
    *a[pos].ref = pos;                         // and tell ITS owner where it ended up
}

// ---------------------------------------------------------------
// 3. Moving an item DOWN: used when it became BIGGER than a child.
//    Swap with the SMALLEST child (it must be the smallest one, or
//    the other child would now be smaller than its new parent).
// ---------------------------------------------------------------
static void heap_down(HeapItem *a, size_t pos, size_t len) {
    HeapItem t = a[pos];                       // the item that is moving
    while (true) {
        size_t l = heap_left(pos);
        size_t r = heap_right(pos);

        // find the smallest among: the moving item, its left kid, its right kid
        size_t min_pos = (size_t)-1;           // -1 = "the moving item itself is smallest"
        uint64_t min_val = t.val;
        if (l < len && a[l].val < min_val) {
            min_pos = l;
            min_val = a[l].val;
        }
        if (r < len && a[r].val < min_val) {
            min_pos = r;
        }
        if (min_pos == (size_t)-1) {
            break;                             // no kid is smaller: the item is in place
        }

        a[pos] = a[min_pos];                   // the smaller kid moves UP into the gap
        *a[pos].ref = pos;
        pos = min_pos;                         // the gap is now where that kid was
    }
    a[pos] = t;
    *a[pos].ref = pos;
}

// ---------------------------------------------------------------
// 4. After a value changed (or an item was inserted/replaced at `pos`),
//    decide which direction to fix. This ONE function serves updating,
//    inserting AND deleting.
// ---------------------------------------------------------------
void heap_update(HeapItem *a, size_t pos, size_t len) {
    if (pos > 0 && a[heap_parent(pos)].val > a[pos].val) {
        heap_up(a, pos);        // smaller than its parent: go up
    } else {
        heap_down(a, pos, len); // otherwise it may be too big for its kids: go down
    }
}
