#pragma once

#include "avl.h"
#include "h_map.h"

#include <string_view>
#include <cstddef>

// ---------------------------------------------------------------------------
// ZNode: a single element in a sorted set.
//
// It lives in two indices simultaneously:
//   • an AVL tree ordered by (score, name) for range queries
//   • a hashmap keyed by name for O(1) point lookups
//
// The name is stored as a flexible array member at the end of the struct.
// ---------------------------------------------------------------------------
struct ZNode {
    AVLNode tree;       // embedded AVL node (intrusive)
    Hnode   hmap;       // embedded hashmap node (intrusive)
    double  score = 0;
    size_t  len   = 0;  // length of name[]
    char    name[0];    // flexible array member — allocate with malloc
};

// ---------------------------------------------------------------------------
// ZSet: the sorted-set container.
// ---------------------------------------------------------------------------
struct ZSet {
    AVLNode *root = nullptr;   // root of the AVL tree
    Hmap     hmap;             // hashmap index for point lookups
};

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

// Insert or update (name, score).  Returns true if a new node was created.
// BUG FIX: parameter order in original definition was (name, score, len)
// but declaration and call sites used (name, len, score).  Canonical order
// is now (name, len, score) everywhere.
bool zset_insert(ZSet *zset, std::string_view name, double score);

// Look up a node by name.  Returns nullptr if not found.
ZNode *zset_lookup(ZSet *zset, std::string_view name);

// Remove `node` from the set (must belong to `zset`).
void zset_delete(ZSet *zset, ZNode *node);

// Find the first node >= (score, name) in sorted order.
// Returns nullptr when the set is empty or all nodes are strictly less.
ZNode *zset_seek(ZSet *zset, double score, std::string_view name);

// Walk `offset` positions forward/backward from `znode` in sorted order.
// Returns nullptr when the offset goes out of range.
ZNode *znode_offset(ZNode *znode, int64_t offset);

// Release all memory owned by `zset` (does not free the ZSet itself).
void zset_clear(ZSet *zset);