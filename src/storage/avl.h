#pragma once

#include <cstdint>

// Intrusive AVL tree node.  Embed this inside your payload struct and
// use container_of() to recover the payload from a node pointer.
struct AVLNode {
    AVLNode *parent = nullptr;
    AVLNode *left   = nullptr;
    AVLNode *right  = nullptr;
    uint32_t height = 0;   // height of this subtree
    uint32_t cnt    = 0;   // size of this subtree (for rank / offset queries)
};

// Initialise a freshly allocated node before inserting it into a tree.
inline void avl_init(AVLNode *node) {
    node->height = 1;
    node->cnt    = 1;
}

// Safe accessors that treat nullptr as height-0 / size-0.
inline uint32_t avl_height(const AVLNode *node) { return node ? node->height : 0; }
inline uint32_t avl_cnt   (const AVLNode *node) { return node ? node->cnt    : 0; }

// Re-balance from `node` up to the root after an insert or delete.
// Returns the new root of the whole tree.
AVLNode *avl_fix(AVLNode *node);

// Remove `node` from the tree.
// Returns the new root of the whole tree.
AVLNode *avl_del(AVLNode *node);

// Walk `offset` positions forward (positive) or backward (negative) from
// `node` in in-order traversal.  Returns nullptr if the offset goes out of
// bounds.
AVLNode *avl_offset(AVLNode *node, int64_t offset);
