// learning/ch10/avl.cpp
//
// The AVL tree logic. Read it in this order:
//   1. small helpers          (avl_init, avl_depth, avl_cnt, avl_update)
//   2. rotations              (rot_left, rot_right)
//   3. fixing one bad node    (avl_fix_left, avl_fix_right)
//   4. fixing the whole path  (avl_fix)     <-- the core idea
//   5. removing a node        (avl_del)
//
// Build: compiled together with the test (see test_avl.cpp header).

#include "avl.h"

// ---------------------------------------------------------------
// 1. Small helpers
// ---------------------------------------------------------------

void avl_init(AVLNode *node) {
    node->depth = 1;   // a lone node is a tree of height 1
    node->cnt = 1;     // ...containing 1 node
    node->left = node->right = node->parent = nullptr;
}

uint32_t avl_depth(AVLNode *node) {
    return node ? node->depth : 0;   // empty subtree has height 0
}

uint32_t avl_cnt(AVLNode *node) {
    return node ? node->cnt : 0;     // empty subtree has 0 nodes
}

static uint32_t max(uint32_t lhs, uint32_t rhs) {
    return lhs < rhs ? rhs : lhs;
}

// Recompute this node's depth and cnt from its two children.
// (Children must already be correct; we only look one level down.)
//
//   depth = 1 + the taller child's depth
//   cnt   = 1 (this node) + nodes in the left + nodes in the right
static void avl_update(AVLNode *node) {
    node->depth = 1 + max(avl_depth(node->left), avl_depth(node->right));
    node->cnt = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}

/*
 * ---------------------------------------------------------------
 * 2. Rotations
 * ---------------------------------------------------------------
 *
 * A rotation changes the shape of a small part of the tree WITHOUT
 * breaking the "smaller on the left, bigger on the right" ordering.
 * It lifts one node up and pushes another down, so a too-tall side
 * gets shorter.
 *
 * rot_left(b): b's RIGHT child (d) takes b's place.
 *
 *        b                    d
 *       / \                  / \
 *      a   d       ==>      b   e
 *         / \              / \
 *        c   e            a   c
 *
 * Ordering check: a < b < c < d < e both before and after. The only
 * subtree that changes parent is c: it was d's left child, it becomes
 * b's right child. That's the whole trick.
 *
 * Returns the new top of this little subtree (d). The CALLER must make
 * whatever pointed at b (a parent's child pointer) point at d instead;
 * the rotation itself only fixes the links below and the parent
 * pointers.
 */
static AVLNode *rot_left(AVLNode *node) {
    AVLNode *new_node = node->right;          // d
    if (new_node->left) {
        new_node->left->parent = node;        // c now has b as its parent
    }
    node->right = new_node->left;             // b's right child becomes c
    new_node->left = node;                    // d's left child becomes b
    new_node->parent = node->parent;          // d takes b's old parent
    node->parent = new_node;                  // b's parent is now d
    avl_update(node);                         // b is lower now: update it FIRST
    avl_update(new_node);                     // then d, which sits above b
    return new_node;
}

/*
 * rot_right is the exact mirror image of rot_left (left <-> right).
 *
 *          b                  a
 *         / \                / \
 *        a   e     ==>      c   b
 *       / \                    / \
 *      c   d                  d   e
 */
static AVLNode *rot_right(AVLNode *node) {
    AVLNode *new_node = node->left;           // a
    if (new_node->right) {
        new_node->right->parent = node;       // d now has b as its parent
    }
    node->left = new_node->right;             // b's left child becomes d
    new_node->right = node;                   // a's right child becomes b
    new_node->parent = node->parent;          // a takes b's old parent
    node->parent = new_node;                  // b's parent is now a
    avl_update(node);
    avl_update(new_node);
    return new_node;
}

/*
 * ---------------------------------------------------------------
 * 3. Fixing one node whose height rule is broken
 * ---------------------------------------------------------------
 *
 * "Too deep" means one side is exactly 2 taller than the other.
 *
 * LEFT side too deep. Two shapes are possible:
 *
 *  (a) left-left: the extra height is on the OUTER side
 *          root                   l
 *          /                     / \
 *         l            ==>      x  root
 *        /
 *       x
 *      one rotation (rot_right on root) is enough.
 *
 *  (b) left-right: the extra height is on the INNER side (a "zigzag")
 *          root                  root                 x
 *          /                     /                   / \
 *         l           ==>       x         ==>       l  root
 *          \                   /
 *           x                 l
 *      first rot_left on `l` straightens the zigzag into shape (a),
 *      then rot_right on root finishes the job.
 *
 * The `if` below detects the zigzag: left child's RIGHT subtree is
 * deeper than its LEFT subtree.
 */
static AVLNode *avl_fix_left(AVLNode *root) {
    if (avl_depth(root->left->left) < avl_depth(root->left->right)) {
        root->left = rot_left(root->left);    // straighten the zigzag
    }
    return rot_right(root);
}

// RIGHT side too deep: the mirror image of avl_fix_left.
static AVLNode *avl_fix_right(AVLNode *root) {
    if (avl_depth(root->right->right) < avl_depth(root->right->left)) {
        root->right = rot_right(root->right); // straighten the zigzag
    }
    return rot_left(root);
}

// ---------------------------------------------------------------
// 4. Fixing the whole path: the core of the AVL tree
// ---------------------------------------------------------------
//
// After an insert or delete, only the nodes on the path from the
// changed spot UP to the root can have wrong depth/cnt or a broken
// balance. So: start at the changed node and climb to the root, at
// each step (1) refreshing depth/cnt, (2) rotating if unbalanced.
//
// The same function works for both insertion and deletion, which is
// why AVL trees are short to code.
//
// A rotation changes which node is on top of a subtree, so the parent
// must be re-pointed at the new top. We remember WHICH pointer in the
// parent referred to this node (`from`) BEFORE rotating, because
// afterwards we could no longer tell. `from` is an AVLNode** (a
// pointer to a pointer) for the same reason as in the hashtable
// chapter: it lets us treat "parent's left slot" and "parent's right
// slot" with identical code.
//
// Returns the root of the whole tree (rotations can change the root).
AVLNode *avl_fix(AVLNode *node) {
    while (true) {
        avl_update(node);                          // refresh depth and cnt
        uint32_t l = avl_depth(node->left);
        uint32_t r = avl_depth(node->right);

        AVLNode **from = nullptr;                  // which parent slot holds `node`?
        if (node->parent) {
            from = (node->parent->left == node) ? &node->parent->left
                                                : &node->parent->right;
        }

        if (l == r + 2) {                          // left side too deep
            node = avl_fix_left(node);
        } else if (l + 2 == r) {                   // right side too deep
            node = avl_fix_right(node);
        }
        // (`node` now refers to whatever is on top of this subtree:
        //  the same node if nothing rotated, or the rotated-up node.)

        if (!from) {
            return node;                           // no parent: we reached the root
        }
        *from = node;                              // re-attach the (maybe new) top to its parent
        node = node->parent;                       // climb one level
    }
}

// ---------------------------------------------------------------
// 5. Removing a node
// ---------------------------------------------------------------
//
// Two cases, depending on whether the node has a right subtree.
//
// CASE A: no right child. Splice the node out by connecting its left
//         subtree (maybe empty) directly to its parent. Then repair
//         the path upward from the parent.
//
//            parent                 parent
//              |                      |
//             node        ==>         left
//            /                       (subtree)
//          left
//
// CASE B: it has a right child. We cannot just delete it (two
//         subtrees, one slot). Instead find its in-order SUCCESSOR:
//         the smallest value in the right subtree = go right once, then
//         left as far as possible. That "victim" has no left child, so
//         removing it is CASE A (the recursive call). Then the victim
//         is moved into the deleted node's position, keeping order.
//
// Returns the new root of the whole tree.
AVLNode *avl_del(AVLNode *node) {
    if (node->right == nullptr) {
        // ---- CASE A ----
        AVLNode *parent = node->parent;
        if (node->left) {
            node->left->parent = parent;           // left subtree now hangs from parent
        }
        if (parent) {
            // replace the parent's slot that held `node` with node's left subtree
            (parent->left == node ? parent->left : parent->right) = node->left;
            return avl_fix(parent);                // repair from the parent up
        } else {
            return node->left;                     // node was the root: left subtree is the new root
        }
    } else {
        // ---- CASE B ----
        AVLNode *victim = node->right;
        while (victim->left) {                     // leftmost node of the right subtree
            victim = victim->left;
        }
        AVLNode *root = avl_del(victim);           // detach the victim (this is CASE A)

        // Now put the victim where `node` was: it inherits node's
        // depth, cnt, left, right, and parent in one struct copy.
        *victim = *node;
        if (victim->left) {
            victim->left->parent = victim;         // children must point back at the new parent
        }
        if (victim->right) {
            victim->right->parent = victim;
        }

        AVLNode *parent = node->parent;
        if (parent) {
            (parent->left == node ? parent->left : parent->right) = victim;
            return root;
        } else {
            return victim;                         // node was the root: victim is the new root
        }
    }
}
