#include "h_map.h"

#include <cassert>
#include <cstdlib>    // calloc, free

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void h_init(HTab *htab, size_t n) {
    assert(n > 0 && (n & (n - 1)) == 0);   // must be a power of 2
    htab->tab  = (Hnode **)calloc(n, sizeof(Hnode *));
    htab->mask = n - 1;
    htab->size = 0;
}

static void h_insert(HTab *htab, Hnode *node) {
    size_t  pos  = node->hcode & htab->mask;
    Hnode  *head = htab->tab[pos];
    node->next   = head;
    htab->tab[pos] = node;
    htab->size++;
}

// Returns a pointer-to-pointer so the caller can detach the node without a
// second traversal.  Returns nullptr when the key is not in the table.
static Hnode **h_lookup(HTab *htab, Hnode *key, bool (*eq)(Hnode *, Hnode *)) {
    if (!htab->tab) return nullptr;
    size_t   pos  = key->hcode & htab->mask;
    Hnode  **from = &htab->tab[pos];
    for (Hnode *cur; (cur = *from) != nullptr; from = &cur->next) {
        if (cur->hcode == key->hcode && eq(cur, key)) return from;
    }
    return nullptr;
}

static Hnode *h_detach(HTab *htab, Hnode **from) {
    Hnode *node = *from;
    *from = node->next;
    htab->size--;
    return node;
}

// ---------------------------------------------------------------------------
// Progressive rehashing
// ---------------------------------------------------------------------------

static const size_t k_rehashing_work = 128;   // buckets migrated per call

static void hm_help_rehashing(Hmap *hmap) {
    size_t nwork = 0;
    while (nwork < k_rehashing_work && hmap->older.size != 0) {
        Hnode **from = &hmap->older.tab[hmap->migrate_pos];
        if (!*from) {
            hmap->migrate_pos++;
            continue;
        }
        h_insert(&hmap->newer, h_detach(&hmap->older, from));
        nwork++;
    }
    if (hmap->older.size == 0 && hmap->older.tab) {
        free(hmap->older.tab);
        hmap->older = HTab{};
    }
}

static void hm_trigger_rehashing(Hmap *hmap) {
    assert(!hmap->older.tab);
    hmap->older = hmap->newer;
    h_init(&hmap->newer, (hmap->newer.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static const size_t k_max_load_factor = 8;

Hnode *hm_lookup(Hmap *hmap, Hnode *key, bool (*eq)(Hnode *, Hnode *)) {
    hm_help_rehashing(hmap);
    Hnode **from = h_lookup(&hmap->newer, key, eq);
    if (!from) from = h_lookup(&hmap->older, key, eq);
    return from ? *from : nullptr;
}

void hm_insert(Hmap *hmap, Hnode *node) {
    if (!hmap->newer.tab) h_init(&hmap->newer, 4);
    h_insert(&hmap->newer, node);
    if (!hmap->older.tab) {
        size_t threshold = (hmap->newer.mask + 1) * k_max_load_factor;
        if (hmap->newer.size >= threshold) hm_trigger_rehashing(hmap);
    }
    hm_help_rehashing(hmap);
}

Hnode *hm_delete(Hmap *hmap, Hnode *key, bool (*eq)(Hnode *, Hnode *)) {
    hm_help_rehashing(hmap);
    if (Hnode **from = h_lookup(&hmap->newer, key, eq)) return h_detach(&hmap->newer, from);
    if (Hnode **from = h_lookup(&hmap->older, key, eq)) return h_detach(&hmap->older, from);
    return nullptr;
}

void hm_clear(Hmap *hmap) {
    free(hmap->newer.tab);
    free(hmap->older.tab);
    *hmap = Hmap{};
}

size_t hm_size(Hmap *hmap) {
    return hmap->newer.size + hmap->older.size;
}
