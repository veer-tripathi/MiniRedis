#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Intrusive hash-table node.  Embed in your payload struct.
// ---------------------------------------------------------------------------
struct Hnode {
    Hnode  *next  = nullptr;
    uint64_t hcode = 0;        // full hash of the key
};

// ---------------------------------------------------------------------------
// Single open-addressing bucket array.
// ---------------------------------------------------------------------------
struct HTab {
    Hnode **tab  = nullptr;    // array of bucket heads (chaining)
    size_t  mask = 0;          // capacity - 1  (capacity is always a power of 2)
    size_t  size = 0;          // number of entries currently stored
};

// ---------------------------------------------------------------------------
// Top-level hashmap: two HTabs for progressive (incremental) rehashing.
// ---------------------------------------------------------------------------
struct Hmap {
    HTab   newer;
    HTab   older;
    size_t migrate_pos = 0;    // next bucket in `older` to migrate
};

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

// Look up a node whose key matches `key` according to `eq`.
// Returns the matching Hnode* or nullptr.
Hnode *hm_lookup(Hmap *hmap, Hnode *key, bool (*eq)(Hnode *, Hnode *));

// Insert `node` (already initialised with hcode) into the map.
void hm_insert(Hmap *hmap, Hnode *node);

// Remove and return the node matching `key`, or nullptr if not found.
Hnode *hm_delete(Hmap *hmap, Hnode *key, bool (*eq)(Hnode *, Hnode *));

// Release all memory used by the map (does NOT free the payload structs).
void hm_clear(Hmap *hmap);

// Total number of entries across both tables.
size_t hm_size(Hmap *hmap);

// ---------------------------------------------------------------------------
// Iteration (template so the lambda can be inlined by the compiler)
// ---------------------------------------------------------------------------
template <typename Func>
void hm_for_each(Hmap *hmap, Func fn) {
    auto visit = [&](HTab &ht, size_t start) {
        if (!ht.tab) return;
        size_t cap = ht.mask + 1;
        for (size_t i = start; i < cap; ++i) {
            for (Hnode *n = ht.tab[i]; n; n = n->next) fn(n);
        }
    };
    visit(hmap->newer, 0);
    visit(hmap->older, hmap->migrate_pos);  // skip already-migrated buckets
}
