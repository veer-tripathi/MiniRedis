#include "z_set.h"
#include "../utils/common.h"

#include <algorithm>   // std::min
#include <cassert>
#include <cstdlib>     // malloc, free
#include <cstring>     // memcpy, memcmp

// ---------------------------------------------------------------------------
// ZNode allocation
// ---------------------------------------------------------------------------

static ZNode *znode_new(const char *name, size_t len, double score) {
    // Allocate struct + name bytes as a flat byte array, then placement-new
    // the ZNode into it. This avoids the flexible-array-member + malloc
    // pattern that causes heap corruption when adjacent buffer allocations
    // overwrite the struct fields.
    uint8_t *mem  = new uint8_t[sizeof(ZNode) + len];
    ZNode   *node = new (mem) ZNode();   // placement new — zero-inits all fields
    avl_init(&node->tree);
    node->hmap.next  = nullptr;
    node->hmap.hcode = str_hash((const uint8_t *)name, len);
    node->score = score;
    node->len   = len;
    memcpy(node->name, name, len);
    return node;
}

static void znode_del(ZNode *node) {
    node->~ZNode();                      // explicit destructor
    delete[] reinterpret_cast<uint8_t *>(node);
}

// ---------------------------------------------------------------------------
// Comparator: lexicographic (score, name) ordering
// ---------------------------------------------------------------------------

static bool zless(AVLNode *lhs, double score, const char *name, size_t len) {
    ZNode *zl = container_of(lhs, ZNode, tree);
    if (zl->score != score) return zl->score < score;
    size_t m = std::min(zl->len, len);
    int cmp  = memcmp(zl->name, name, m);
    if (cmp != 0) return cmp < 0;
    return zl->len < len;
}

static bool zless(AVLNode *lhs, AVLNode *rhs) {
    ZNode *zr = container_of(rhs, ZNode, tree);
    return zless(lhs, zr->score, zr->name, zr->len);
}

// ---------------------------------------------------------------------------
// AVL tree helpers
// ---------------------------------------------------------------------------

static void tree_insert(ZSet *zset, ZNode *node) {
    AVLNode  *parent = nullptr;
    AVLNode **from   = &zset->root;
    while (*from) {
        parent = *from;
        from   = zless(&node->tree, parent) ? &parent->left : &parent->right;
    }
    *from             = &node->tree;
    node->tree.parent = parent;
    zset->root        = avl_fix(&node->tree);
}

static void zset_update(ZSet *zset, ZNode *node, double score) {
    if (node->score == score) return;
    zset->root = avl_del(&node->tree);
    avl_init(&node->tree);
    node->score = score;
    tree_insert(zset, node);
}

// ---------------------------------------------------------------------------
// Hashmap comparator
// ---------------------------------------------------------------------------

struct HKey {
    Hnode       node;
    const char *name = nullptr;
    size_t      len  = 0;
};

static bool hcmp(Hnode *node, Hnode *key) {
    ZNode *znode = container_of(node, ZNode, hmap);
    HKey  *hkey  = container_of(key,  HKey,  node);
    if (znode->len != hkey->len) return false;
    return memcmp(znode->name, hkey->name, znode->len) == 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool zset_insert(ZSet *zset, std::string_view name, double score) {
    ZNode *node = zset_lookup(zset, name);
    if (node) {
        zset_update(zset, node, score);
        return false;
    }
    node = znode_new(name.data(), name.size(), score);
    hm_insert(&zset->hmap, &node->hmap);
    tree_insert(zset, node);
    return true;
}

ZNode *zset_lookup(ZSet *zset, std::string_view name) {
    if (!zset->root) return nullptr;
    HKey key;
    key.len        = name.size();
    key.name       = name.data();
    key.node.hcode = str_hash((const uint8_t *)name.data(), name.size());
    Hnode *found   = hm_lookup(&zset->hmap, &key.node, hcmp);
    return found ? container_of(found, ZNode, hmap) : nullptr;
}

void zset_delete(ZSet *zset, ZNode *node) {
    HKey key;
    key.node.hcode = node->hmap.hcode;
    key.len        = node->len;
    key.name       = node->name;
    Hnode *found   = hm_delete(&zset->hmap, &key.node, hcmp);
    assert(found);
    zset->root = avl_del(&node->tree);
    znode_del(node);
}

ZNode *zset_seek(ZSet *zset, double score, std::string_view name) {
    AVLNode *found = nullptr;
    for (AVLNode *node = zset->root; node; ) {
        if (zless(node, score, name.data(), name.size())) {
            node = node->right;
        } else {
            found = node;
            node  = node->left;
        }
    }
    return found ? container_of(found, ZNode, tree) : nullptr;
}

ZNode *znode_offset(ZNode *znode, int64_t offset) {
    AVLNode *tnode = znode ? avl_offset(&znode->tree, offset) : nullptr;
    return tnode ? container_of(tnode, ZNode, tree) : nullptr;
}

static void tree_dispose(AVLNode *root) {
    if (!root) return;
    tree_dispose(root->left);
    tree_dispose(root->right);
    znode_del(container_of(root, ZNode, tree));
}

void zset_clear(ZSet *zset) {
    hm_clear(&zset->hmap);
    tree_dispose(zset->root);
    zset->root = nullptr;
}