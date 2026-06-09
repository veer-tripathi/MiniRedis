#include "avl.h"
#include <cassert>
#include <cstdint>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static uint32_t u32_max(uint32_t l, uint32_t r) {
    // BUG FIX: original code returned `l < r ? l : r` (the minimum).
    return l > r ? l : r;
}

// Recompute height and subtree count from children.
static void update(AVLNode *node) {
    node->height = 1 + u32_max(avl_height(node->left), avl_height(node->right));
    node->cnt    = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}

static int balance_factor(const AVLNode *n) {
    return (int)avl_height(n->left) - (int)avl_height(n->right);
}

// ---------------------------------------------------------------------------
// Rotations
// Naming convention: the direction names describe which way the heavy child
// moves (LR = left-right case; RR = right-right / left rotation, etc.).
// ---------------------------------------------------------------------------

// Left rotation: z's right-heavy subtree is rotated left so x becomes root.
//
//     z                x
//    / |              / |
//   A   x    =>     z   C
//      / |         / |
//     B   C       A   B
static AVLNode *rotate_left(AVLNode *z) {
    AVLNode *x = z->right;
    AVLNode *B = x->left;
    AVLNode *p = z->parent;

    // Relink
    x->left  = z;
    z->right = B;

    if (B) B->parent = z;

    x->parent = p;
    z->parent = x;

    if (p) {
        if (p->left == z) p->left  = x;
        else              p->right = x;
    }

    update(z);
    update(x);
    return x;
}

// Right rotation: z's left-heavy subtree is rotated right so x becomes root.
//
//       z              x
//      / |            / |
//     x   C   =>    A   z
//    / |                / |
//   A   B              B   C
static AVLNode *rotate_right(AVLNode *z) {
    AVLNode *x = z->left;
    AVLNode *B = x->right;
    AVLNode *p = z->parent;

    // Relink
    x->right = z;
    z->left  = B;

    if (B) B->parent = z;

    x->parent = p;
    z->parent = x;

    if (p) {
        if (p->left == z) p->left  = x;
        else              p->right = x;
    }

    update(z);
    update(x);
    return x;
}

// Fix imbalance at `z` (balance factor out of [-1, 1]).
// Returns the new local root after at most two rotations.
static AVLNode *rebalance(AVLNode *z) {
    int bf = balance_factor(z);

    if (bf > 1) {
        // Left-heavy
        if (balance_factor(z->left) < 0) {
            rotate_left(z->left);   // Left-Right case: double rotation
        }
        return rotate_right(z);
    }

    if (bf < -1) {
        // Right-heavy
        if (balance_factor(z->right) > 0) {
            rotate_right(z->right); // Right-Left case: double rotation
        }
        return rotate_left(z);
    }

    return z;   // already balanced
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AVLNode *avl_fix(AVLNode *node) {
    AVLNode *root = node;
    while (node) {
        update(node);
        AVLNode *new_sub = rebalance(node);
        if (!new_sub->parent) {
            root = new_sub;
        }
        node = new_sub->parent;
    }
    return root;
}

// Delete when `node` has at most one child (the easy case).
static AVLNode *avl_del_easy(AVLNode *node) {
    AVLNode *child  = node->left ? node->left : node->right;
    AVLNode *parent = node->parent;

    if (child) child->parent = parent;
    if (!parent) return child;   // deleted the root

    if (parent->left == node) parent->left  = child;
    else                      parent->right = child;

    return avl_fix(parent);
}

AVLNode *avl_del(AVLNode *node) {
    // If the node has fewer than two children, delegate to the easy case.
    if (!node->left || !node->right) {
        return avl_del_easy(node);
    }

    // Two children: swap with the in-order successor (leftmost in right subtree),
    // then delete the successor (which has at most one child).
    AVLNode *successor = node->right;
    while (successor->left) successor = successor->left;

    avl_del_easy(successor); // rebalances up; we rebuild root below

    // Transplant successor into node's position.
    successor->left   = node->left;
    successor->right  = node->right;
    successor->parent = node->parent;

    if (successor->left)  successor->left->parent  = successor;
    if (successor->right) successor->right->parent = successor;

    if (!successor->parent) {
        (void)successor; // becomes root, returned via avl_fix below
    } else {
        if (successor->parent->left  == node) successor->parent->left  = successor;
        else                                  successor->parent->right = successor;
    }

    return avl_fix(successor);
}

AVLNode *avl_offset(AVLNode *node, int64_t offset) {
    // `pos` tracks the in-order rank of `node` relative to the starting node.
    int64_t pos = 0;

    while (offset != pos) {
        if (pos < offset && (offset - pos) <= (int64_t)avl_cnt(node->right)) {
            // Target is in the right subtree.
            node = node->right;
            pos += 1 + (int64_t)avl_cnt(node->left);
        } else if (pos > offset && (pos - offset) <= (int64_t)avl_cnt(node->left)) {
            // Target is in the left subtree.
            node = node->left;
            pos -= 1 + (int64_t)avl_cnt(node->right);
        } else {
            // Must go up.
            AVLNode *parent = node->parent;
            if (!parent) return nullptr;   // offset out of bounds
            if (parent->right == node) pos -= (int64_t)avl_cnt(node->left)  + 1;
            else                       pos += (int64_t)avl_cnt(node->right) + 1;
            node = parent;
        }
    }
    return node;
}