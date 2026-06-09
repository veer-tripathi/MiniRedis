// Exhaustively tests avl_offset() for trees of size 1..499.
// Build:  g++ -std=c++17 -o test_avl test_offset.cpp ../storage/avl.cpp
// Run:    ./test_avl

#include "../storage/avl.h"
#include "../utils/common.h"

#include <cassert>
#include <cstdint>

// A simple ordered container backed by an AVL tree.
struct Data {
    AVLNode node;
    uint32_t val = 0;
};

struct Container {
    AVLNode *root = nullptr;
};

static void add(Container &c, uint32_t val) {
    Data *data = new Data();
    avl_init(&data->node);
    data->val = val;

    if (!c.root) {
        c.root = &data->node;
        return;
    }

    AVLNode *cur = c.root;
    while (true) {
        AVLNode **from = (val < container_of(cur, Data, node)->val)
                         ? &cur->left : &cur->right;
        if (!*from) {
            *from             = &data->node;
            data->node.parent = cur;
            c.root            = avl_fix(&data->node);
            break;
        }
        cur = *from;
    }
}

static void dispose(AVLNode *node) {
    if (!node) return;
    dispose(node->left);
    dispose(node->right);
    delete container_of(node, Data, node);
}

static void test_case(uint32_t sz) {
    Container c;
    for (uint32_t i = 0; i < sz; ++i) add(c, i);

    // Find the in-order minimum.
    AVLNode *min = c.root;
    while (min->left) min = min->left;

    for (uint32_t i = 0; i < sz; ++i) {
        AVLNode *node = avl_offset(min, (int64_t)i);
        assert(container_of(node, Data, node)->val == i);

        // Verify all pairwise offsets.
        for (uint32_t j = 0; j < sz; ++j) {
            int64_t  off = (int64_t)j - (int64_t)i;
            AVLNode *n2  = avl_offset(node, off);
            assert(container_of(n2, Data, node)->val == j);
        }

        // Out-of-bounds checks.
        assert(!avl_offset(node, -(int64_t)i - 1));
        assert(!avl_offset(node, (int64_t)(sz - i)));
    }

    dispose(c.root);
}

int main() {
    for (uint32_t i = 1; i < 500; ++i) test_case(i);
    return 0;
}
