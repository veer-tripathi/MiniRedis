#include "commands.h"
#include "heap.h"
#include "h_map.h"
#include "z_set.h"
#include "../utils/common.h"
#include "../timers/timers.h"
#include "../protocol/serializer.h"
#include "../utils/buffer.h"

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Global data store
// ---------------------------------------------------------------------------

static struct {
    Hmap                  db;
    std::vector<HeapItem> heap;   // min-heap of TTL expiry timestamps
} g_data;

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

enum EntryType {
    T_INIT = 0,
    T_STR  = 1,
    T_ZSET = 2,
    T_LIST = 3,
};

struct Entry {
    Hnode       node;
    std::string key;
    uint32_t    type     = T_INIT;

    // T_STR
    std::string str;

    // T_ZSET
    ZSet zset;

    // T_LIST
    // Front = index 0 = left side (lpush/lpop end).
    // Back  = last index    = right side (rpush/rpop end).
    std::deque<std::string> list;

    // Index into g_data.heap[].
    // (size_t)-1 means no TTL is set on this key.
    size_t heap_idx = (size_t)-1;
};

// ---------------------------------------------------------------------------
// entry_set_ttl
// ---------------------------------------------------------------------------
// The only function that touches g_data.heap.
//
//  ttl_ms < 0  →  remove TTL (make key permanent)
//  ttl_ms >= 0 →  set TTL to expire ttl_ms milliseconds from now
//
// HeapItem::ref points at &ent->heap_idx so the heap can update the
// Entry's index whenever it moves the item around internally.

static void entry_set_ttl(Entry *ent, int64_t ttl_ms) {
    if (ttl_ms < 0) {
        if (ent->heap_idx != (size_t)-1) {
            heap_delete(g_data.heap, ent->heap_idx);
            ent->heap_idx = (size_t)-1;
        }
    } else {
        uint64_t expire_at = get_monotonic_msec() + (uint64_t)ttl_ms;
        HeapItem item = { expire_at, &ent->heap_idx };
        heap_upsert(g_data.heap, ent->heap_idx, item);
    }
}

// ---------------------------------------------------------------------------
// entry_new / entry_del
// ---------------------------------------------------------------------------

static Entry *entry_new(uint32_t type) {
    Entry *ent = new Entry();
    ent->type  = type;
    return ent;
}

static void entry_del(Entry *ent) {
    entry_set_ttl(ent, -1);   // remove from heap before freeing
    if (ent->type == T_ZSET) zset_clear(&ent->zset);
    delete ent;
}

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------

struct LookupKey {
    Hnode       node;
    std::string key;
};

static bool entry_eq(Hnode *node, Hnode *key) {
    Entry     *le = container_of(node, Entry,     node);
    LookupKey *re = container_of(key,  LookupKey, node);
    return le->key == re->key;
}

static LookupKey make_lookup(std::string &k) {
    LookupKey lk;
    lk.key        = k;
    lk.node.hcode = str_hash((const uint8_t *)lk.key.data(), lk.key.size());
    return lk;
}

// ---------------------------------------------------------------------------
// entry_get_or_expire  —  lazy expiration
// ---------------------------------------------------------------------------

static Entry *entry_get_or_expire(std::string &k) {
    LookupKey lk   = make_lookup(k);
    Hnode    *node = hm_lookup(&g_data.db, &lk.node, entry_eq);
    if (!node) return nullptr;

    Entry *ent = container_of(node, Entry, node);

    if (ent->heap_idx != (size_t)-1) {
        uint64_t expire_at = g_data.heap[ent->heap_idx].val;
        if (get_monotonic_msec() >= expire_at) {
            hm_delete(&g_data.db, &lk.node, entry_eq);
            entry_del(ent);
            return nullptr;
        }
    }
    return ent;
}

// ---------------------------------------------------------------------------
// is_expired  —  used by do_keys without deleting
// ---------------------------------------------------------------------------

static bool is_expired(const Entry *ent) {
    if (ent->heap_idx == (size_t)-1) return false;
    return get_monotonic_msec() >= g_data.heap[ent->heap_idx].val;
}

// ---------------------------------------------------------------------------
// String commands
// ---------------------------------------------------------------------------

static void do_get(std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != T_STR)
        return out_err(out, ERR_BAD_TYPE, "not a string value");
    return out_str(out, ent->str.data(), ent->str.size());
}

static void do_set(std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (ent) {
        if (ent->type != T_STR)
            return out_err(out, ERR_BAD_TYPE, "a non-string value exists");
        ent->str.swap(cmd[2]);
        entry_set_ttl(ent, -1);   // set clears any existing TTL
    } else {
        LookupKey key = make_lookup(cmd[1]);
        ent = entry_new(T_STR);
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->str.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }
    return out_nil(out);
}

static void do_del(std::vector<std::string> &cmd, Buffer *out) {
    LookupKey key  = make_lookup(cmd[1]);
    Hnode    *node = hm_delete(&g_data.db, &key.node, entry_eq);
    if (node) entry_del(container_of(node, Entry, node));
    return out_int(out, node ? 1 : 0);
}

static void do_keys(std::vector<std::string> & /*cmd*/, Buffer *out) {
    // Pass 1: count only live (non-expired) keys
    uint32_t count = 0;
    hm_for_each(&g_data.db, [&](Hnode *node) {
        Entry *ent = container_of(node, Entry, node);
        if (!is_expired(ent)) count++;
    });

    // Pass 2: emit the same filtered set
    // out_arr must be written before the items because the wire format
    // requires the count header first — so we need two passes.
    out_arr(out, count);
    hm_for_each(&g_data.db, [&](Hnode *node) {
        Entry *ent = container_of(node, Entry, node);
        if (is_expired(ent)) return;
        out_str(out, ent->key.data(), ent->key.size());
    });
}

// ---------------------------------------------------------------------------
// List helpers
// ---------------------------------------------------------------------------

// Get the list entry for a key, or create it if it doesn't exist.
// Returns nullptr on type error.
static Entry *get_or_create_list(std::string &key_str, Buffer *out) {
    Entry *ent = entry_get_or_expire(key_str);
    if (ent) {
        if (ent->type != T_LIST) {
            out_err(out, ERR_BAD_TYPE, "not a list value");
            return nullptr;
        }
        return ent;
    }
    LookupKey key = make_lookup(key_str);
    ent = entry_new(T_LIST);
    ent->key.swap(key.key);
    ent->node.hcode = key.node.hcode;
    hm_insert(&g_data.db, &ent->node);
    return ent;
}

// ---------------------------------------------------------------------------
// List commands
// ---------------------------------------------------------------------------

// lpush <key> <val>  — insert at front, return new length
static void do_lpush(std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = get_or_create_list(cmd[1], out);
    if (!ent) return;
    ent->list.push_front(cmd[2]);
    return out_int(out, (int64_t)ent->list.size());
}

// rpush <key> <val>  — insert at back, return new length
static void do_rpush(std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = get_or_create_list(cmd[1], out);
    if (!ent) return;
    ent->list.push_back(cmd[2]);
    return out_int(out, (int64_t)ent->list.size());
}

// lpop <key>  — remove and return front element, nil if empty/missing
static void do_lpop(std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != T_LIST)
        return out_err(out, ERR_BAD_TYPE, "not a list value");
    if (ent->list.empty()) return out_nil(out);

    std::string val = std::move(ent->list.front());
    ent->list.pop_front();

    // Delete the key when the list becomes empty — matches Redis behaviour
    if (ent->list.empty()) {
        LookupKey lk = make_lookup(cmd[1]);
        hm_delete(&g_data.db, &lk.node, entry_eq);
        entry_del(ent);
    }

    return out_str(out, val.data(), val.size());
}

// rpop <key>  — remove and return back element, nil if empty/missing
static void do_rpop(std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != T_LIST)
        return out_err(out, ERR_BAD_TYPE, "not a list value");
    if (ent->list.empty()) return out_nil(out);

    std::string val = std::move(ent->list.back());
    ent->list.pop_back();

    if (ent->list.empty()) {
        LookupKey lk = make_lookup(cmd[1]);
        hm_delete(&g_data.db, &lk.node, entry_eq);
        entry_del(ent);
    }

    return out_str(out, val.data(), val.size());
}

// llen <key>  — return number of elements, 0 if missing
static void do_llen(std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != T_LIST)
        return out_err(out, ERR_BAD_TYPE, "not a list value");
    return out_int(out, (int64_t)ent->list.size());
}

// lrange <key> <start> <stop>
// Returns elements from index start to stop inclusive.
// Negative indices count from the end: -1 = last, -2 = second to last, etc.
// Out-of-range indices are clamped. Returns empty array if start > stop.
static void do_lrange(std::vector<std::string> &cmd, Buffer *out) {
    int64_t start = 0;
    int64_t stop  = 0;
    try {
        start = std::stoll(cmd[2]);
        stop  = std::stoll(cmd[3]);
    } catch (...) {
        return out_err(out, ERR_BAD_ARG, "expect integer");
    }

    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_arr(out, 0);
    if (ent->type != T_LIST)
        return out_err(out, ERR_BAD_TYPE, "not a list value");

    int64_t len = (int64_t)ent->list.size();

    // Resolve negative indices
    if (start < 0) start += len;
    if (stop  < 0) stop  += len;

    // Clamp to valid range
    if (start < 0) start = 0;
    if (stop >= len) stop = len - 1;

    // Empty result cases
    if (start > stop || len == 0) return out_arr(out, 0);

    int64_t count = stop - start + 1;
    out_arr(out, (uint32_t)count);
    for (int64_t i = start; i <= stop; i++) {
        out_str(out, ent->list[i].data(), ent->list[i].size());
    }
}

// ---------------------------------------------------------------------------
// Sorted-set helpers
// ---------------------------------------------------------------------------

static const ZSet k_empty_zset;

static ZSet *expect_zset(std::string &s) {
    Entry *ent = entry_get_or_expire(s);
    if (!ent) return (ZSet *)&k_empty_zset;
    return ent->type == T_ZSET ? &ent->zset : nullptr;
}

// ---------------------------------------------------------------------------
// Sorted-set commands
// ---------------------------------------------------------------------------

static void do_zadd(std::vector<std::string> &cmd, Buffer *out) {
    double score = std::stod(cmd[2]);

    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) {
        LookupKey key = make_lookup(cmd[1]);
        ent = entry_new(T_ZSET);
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        hm_insert(&g_data.db, &ent->node);
    } else {
        if (ent->type != T_ZSET)
            return out_err(out, ERR_BAD_TYPE, "expect zset");
    }

    const std::string &name = cmd[3];
    bool added = zset_insert(&ent->zset, name.data(), name.size(), score);
    return out_int(out, (int64_t)added);
}

static void do_zrem(std::vector<std::string> &cmd, Buffer *out) {
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) return out_err(out, ERR_BAD_TYPE, "expect zset");
    const std::string &name = cmd[2];
    ZNode *znode = zset_lookup(zset, name.data(), name.size());
    if (znode) zset_delete(zset, znode);
    return out_int(out, znode ? 1 : 0);
}

static void do_zscore(std::vector<std::string> &cmd, Buffer *out) {
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) return out_err(out, ERR_BAD_TYPE, "expect zset");
    const std::string &name = cmd[2];
    ZNode *node = zset_lookup(zset, name.data(), name.size());
    return node ? out_dbl(out, node->score) : out_nil(out);
}

static void do_zquery(std::vector<std::string> &cmd, Buffer *out) {
    double             score  = std::stod(cmd[2]);
    const std::string &name   = cmd[3];
    int64_t            offset = std::stoi(cmd[4]);
    int64_t            limit  = std::stoi(cmd[5]);

    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) return out_err(out, ERR_BAD_TYPE, "expect zset");
    if (limit <= 0) return out_arr(out, 0);

    ZNode *znode = zset_seek(zset, score, name.data(), name.size());
    znode = znode_offset(znode, offset);

    buf_append_u8(out, TAG_ARR);
    size_t size_pos = out->size();
    buf_append_u32(out, 0);

    int64_t n = 0;
    while (znode && n < 2 * limit) {
        out_str(out, znode->name, znode->len);
        out_dbl(out, znode->score);
        znode = znode_offset(znode, +1);
        n += 2;
    }

    uint32_t len = (uint32_t)n;
    memcpy(out->data_begin + size_pos, &len, 4);
}

// ---------------------------------------------------------------------------
// TTL commands
// ---------------------------------------------------------------------------

static void do_expire(std::vector<std::string> &cmd, Buffer *out) {
    int64_t ttl_ms = 0;
    try {
        ttl_ms = std::stoll(cmd[2]);
    } catch (...) {
        return out_err(out, ERR_BAD_ARG, "expect int64");
    }

    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_int(out, 0);

    entry_set_ttl(ent, ttl_ms);
    return out_int(out, 1);
}

static void do_ttl(std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_int(out, -2);
    if (ent->heap_idx == (size_t)-1) return out_int(out, -1);

    uint64_t expire_at = g_data.heap[ent->heap_idx].val;
    uint64_t now       = get_monotonic_msec();
    int64_t  left      = (int64_t)(expire_at - now);
    return out_int(out, left > 0 ? left : 0);
}

static void do_persist(std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->heap_idx == (size_t)-1) return out_int(out, 0);
    entry_set_ttl(ent, -1);
    return out_int(out, 1);
}

// ---------------------------------------------------------------------------
// Active expiration — called by event_loop every tick
// ---------------------------------------------------------------------------

static const size_t k_max_works = 2000;

void expire_keys() {
    uint64_t now_ms = get_monotonic_msec();
    size_t   nworks = 0;

    while (!g_data.heap.empty()
           && g_data.heap[0].val < now_ms
           && nworks++ < k_max_works)
    {
        Entry *ent = container_of(g_data.heap[0].ref, Entry, heap_idx);
        LookupKey lk = make_lookup(ent->key);
        hm_delete(&g_data.db, &lk.node, entry_eq);
        entry_del(ent);
    }
}

uint64_t next_ttl_ms() {
    if (g_data.heap.empty()) return (uint64_t)-1;
    return g_data.heap[0].val;
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

void do_request(std::vector<std::string> &cmd, Buffer *out) {
    if      (cmd.size() == 2 && cmd[0] == "get")     return do_get    (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "set")     return do_set    (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "del")     return do_del    (cmd, out);
    else if (cmd.size() == 1 && cmd[0] == "keys")    return do_keys   (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "lpush")   return do_lpush  (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "rpush")   return do_rpush  (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "lpop")    return do_lpop   (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "rpop")    return do_rpop   (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "llen")    return do_llen   (cmd, out);
    else if (cmd.size() == 4 && cmd[0] == "lrange")  return do_lrange (cmd, out);
    else if (cmd.size() == 4 && cmd[0] == "zadd")    return do_zadd   (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "zrem")    return do_zrem   (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "zscore")  return do_zscore (cmd, out);
    else if (cmd.size() == 6 && cmd[0] == "zquery")  return do_zquery (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "expire")  return do_expire (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "ttl")     return do_ttl    (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "persist") return do_persist(cmd, out);
    else    return out_err(out, ERR_UNKNOWN, "unknown command");
}