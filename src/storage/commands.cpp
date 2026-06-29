#include "commands.h"
#include "heap.h"
#include "h_map.h"
#include "z_set.h"
#include "../utils/common.h"
#include "../timers/timers.h"
#include "../protocol/serializer.h"
#include "../utils/buffer.h"
#include "../persistence/persistence.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Global data store
// ---------------------------------------------------------------------------

static struct {
    Hmap                  db;
    std::vector<HeapItem> heap;
} g_data;

// ---------------------------------------------------------------------------
// Pub/Sub channel registry
// ---------------------------------------------------------------------------

static std::unordered_map<std::string, std::vector<Conn *>> g_channels;

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
    uint32_t    type = T_INIT;

    std::string             str;
    ZSet                    zset;
    std::deque<std::string> list;

    size_t heap_idx = (size_t)-1;
};

// ---------------------------------------------------------------------------
// TTL heap management
// ---------------------------------------------------------------------------

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
    entry_set_ttl(ent, -1);
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

static LookupKey make_lookup(const std::string &k) {
    LookupKey lk;
    lk.key        = k;
    lk.node.hcode = str_hash((const uint8_t *)lk.key.data(), lk.key.size());
    return lk;
}

// ---------------------------------------------------------------------------
// Lazy expiration
// ---------------------------------------------------------------------------

static Entry *entry_get_or_expire(const std::string &k) {
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

static bool is_expired(const Entry *ent) {
    if (ent->heap_idx == (size_t)-1) return false;
    return get_monotonic_msec() >= g_data.heap[ent->heap_idx].val;
}

// ---------------------------------------------------------------------------
// Pub/Sub helpers
// ---------------------------------------------------------------------------

static void write_pubsub_frame(Buffer *out,
                                const std::string &kind,
                                const std::string &channel,
                                int64_t int_payload,
                                const std::string *str_payload)
{
    out_arr(out, 3);
    out_str(out, kind.data(), kind.size());
    out_str(out, channel.data(), channel.size());
    if (str_payload)
        out_str(out, str_payload->data(), str_payload->size());
    else
        out_int(out, int_payload);
}

static void push_to_subscriber(Conn *sub,
                                const std::string &channel,
                                const std::string &message)
{
    size_t header_pos = sub->outgoing.size();
    buf_append_u32(&sub->outgoing, 0);

    write_pubsub_frame(&sub->outgoing, "message", channel, 0, &message);

    uint32_t resp_len = (uint32_t)(sub->outgoing.size() - header_pos - 4);
    memcpy(sub->outgoing.data() + header_pos, &resp_len, 4);

    sub->want_write = true;
    sub->want_read  = false;
}

// ---------------------------------------------------------------------------
// String commands
// ---------------------------------------------------------------------------

static void do_get(const std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != T_STR)
        return out_err(out, ERR_BAD_TYPE, "not a string value");
    return out_str(out, ent->str.data(), ent->str.size());
}

static void do_set(const std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (ent) {
        if (ent->type != T_STR)
            return out_err(out, ERR_BAD_TYPE, "a non-string value exists");
        aof_append(cmd);   // persist — cmd[2] is const so copy assign
        ent->str = cmd[2];
        entry_set_ttl(ent, -1);
    } else {
        aof_append(cmd);
        LookupKey key = make_lookup(cmd[1]);
        ent = entry_new(T_STR);
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->str = cmd[2];
        hm_insert(&g_data.db, &ent->node);
    }
    return out_nil(out);
}

static void do_del(const std::vector<std::string> &cmd, Buffer *out) {
    LookupKey key  = make_lookup(cmd[1]);
    Hnode    *node = hm_delete(&g_data.db, &key.node, entry_eq);
    if (node) {
        entry_del(container_of(node, Entry, node));
        aof_append(cmd);
    }
    return out_int(out, node ? 1 : 0);
}

static void do_keys(const std::vector<std::string> & /*cmd*/, Buffer *out) {
    uint32_t count = 0;
    hm_for_each(&g_data.db, [&](Hnode *node) {
        Entry *ent = container_of(node, Entry, node);
        if (!is_expired(ent)) count++;
    });

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

static Entry *get_or_create_list(const std::string &key_str, Buffer *out) {
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

static void do_lpush(const std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = get_or_create_list(cmd[1], out);
    if (!ent) return;
    ent->list.push_front(cmd[2]);
    aof_append(cmd);
    return out_int(out, (int64_t)ent->list.size());
}

static void do_rpush(const std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = get_or_create_list(cmd[1], out);
    if (!ent) return;
    ent->list.push_back(cmd[2]);
    aof_append(cmd);
    return out_int(out, (int64_t)ent->list.size());
}

static void do_lpop(const std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != T_LIST)
        return out_err(out, ERR_BAD_TYPE, "not a list value");
    if (ent->list.empty()) return out_nil(out);

    std::string val = std::move(ent->list.front());
    ent->list.pop_front();

    if (ent->list.empty()) {
        LookupKey lk = make_lookup(cmd[1]);
        hm_delete(&g_data.db, &lk.node, entry_eq);
        entry_del(ent);
    }
    aof_append(cmd);
    return out_str(out, val.data(), val.size());
}

static void do_rpop(const std::vector<std::string> &cmd, Buffer *out) {
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
    aof_append(cmd);
    return out_str(out, val.data(), val.size());
}

static void do_llen(const std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != T_LIST)
        return out_err(out, ERR_BAD_TYPE, "not a list value");
    return out_int(out, (int64_t)ent->list.size());
}

static void do_lrange(const std::vector<std::string> &cmd, Buffer *out) {
    int64_t start = 0, stop = 0;
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
    if (start < 0) start += len;
    if (stop  < 0) stop  += len;
    if (start < 0) start = 0;
    if (stop >= len) stop = len - 1;
    if (start > stop || len == 0) return out_arr(out, 0);

    int64_t count = stop - start + 1;
    out_arr(out, (uint32_t)count);
    for (int64_t i = start; i <= stop; i++)
        out_str(out, ent->list[i].data(), ent->list[i].size());
}

// ---------------------------------------------------------------------------
// Sorted-set helpers
// ---------------------------------------------------------------------------

static const ZSet k_empty_zset;

static ZSet *expect_zset(const std::string &s) {
    Entry *ent = entry_get_or_expire(s);
    if (!ent) return (ZSet *)&k_empty_zset;
    return ent->type == T_ZSET ? &ent->zset : nullptr;
}

// ---------------------------------------------------------------------------
// Sorted-set commands
// ---------------------------------------------------------------------------

static void do_zadd(const std::vector<std::string> &cmd, Buffer *out) {
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
    bool added = zset_insert(&ent->zset, name, score);
    aof_append(cmd);
    return out_int(out, (int64_t)added);
}

static void do_zrem(const std::vector<std::string> &cmd, Buffer *out) {
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) return out_err(out, ERR_BAD_TYPE, "expect zset");
    const std::string &name = cmd[2];
    ZNode *znode = zset_lookup(zset, name);
    if (znode) {
        zset_delete(zset, znode);
        aof_append(cmd);
    }
    return out_int(out, znode ? 1 : 0);
}

static void do_zscore(const std::vector<std::string> &cmd, Buffer *out) {
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) return out_err(out, ERR_BAD_TYPE, "expect zset");
    const std::string &name = cmd[2];
    ZNode *node = zset_lookup(zset, name);
    return node ? out_dbl(out, node->score) : out_nil(out);
}

static void do_zquery(const std::vector<std::string> &cmd, Buffer *out) {
    double             score  = std::stod(cmd[2]);
    const std::string &name   = cmd[3];
    int64_t            offset = std::stoi(cmd[4]);
    int64_t            limit  = std::stoi(cmd[5]);

    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) return out_err(out, ERR_BAD_TYPE, "expect zset");
    if (limit <= 0) return out_arr(out, 0);

    ZNode *znode = zset_seek(zset, score, name);
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

static void do_expire(const std::vector<std::string> &cmd, Buffer *out) {
    int64_t ttl_ms = 0;
    try {
        ttl_ms = std::stoll(cmd[2]);
    } catch (...) {
        return out_err(out, ERR_BAD_ARG, "expect int64");
    }
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_int(out, 0);
    entry_set_ttl(ent, ttl_ms);
    aof_append(cmd);
    return out_int(out, 1);
}

static void do_ttl(const std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_int(out, -2);
    if (ent->heap_idx == (size_t)-1) return out_int(out, -1);
    uint64_t expire_at = g_data.heap[ent->heap_idx].val;
    uint64_t now       = get_monotonic_msec();
    int64_t  left      = (int64_t)(expire_at - now);
    return out_int(out, left > 0 ? left : 0);
}

static void do_persist(const std::vector<std::string> &cmd, Buffer *out) {
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->heap_idx == (size_t)-1) return out_int(out, 0);
    entry_set_ttl(ent, -1);
    aof_append(cmd);
    return out_int(out, 1);
}

static void do_expireat(const std::vector<std::string> &cmd, Buffer *out) {
    uint64_t abs_ms = 0;
    try {
        abs_ms = (uint64_t)std::stoull(cmd[2]);
    } catch (...) {
        return out_err(out, ERR_BAD_ARG, "expect uint64");
    }
    Entry *ent = entry_get_or_expire(cmd[1]);
    if (!ent) return out_int(out, 0);
    uint64_t now = get_monotonic_msec();
    int64_t  rel = (abs_ms > now) ? (int64_t)(abs_ms - now) : 1;
    entry_set_ttl(ent, rel);
    return out_int(out, 1);
}

// ---------------------------------------------------------------------------
// BGREWRITEAOF
// ---------------------------------------------------------------------------

static void do_bgrewriteaof(const std::vector<std::string> &cmd, Buffer *out,
                             std::weak_ptr<ThreadPool> tp) {
    (void)cmd;
    if (auto locked = tp.lock()) {
        tp_submit(tp, []() {
            aof_compact("appendonly.aof");
        });
        return out_str(out, "Background append only file rewriting started", 45);
    } else {
        bool ok = aof_compact("appendonly.aof");
        if (ok)
            return out_str(out, "Background append only file rewriting started", 45);
        else
            return out_err(out, ERR_UNKNOWN, "AOF compact failed");
    }
}

// ---------------------------------------------------------------------------
// Pub/Sub commands
// ---------------------------------------------------------------------------

static void do_subscribe(const std::vector<std::string> &cmd, Buffer *out, Conn *conn) {
    const std::string &channel = cmd[1];

    auto &subs = conn->subscriptions;
    if (std::find(subs.begin(), subs.end(), channel) == subs.end()) {
        subs.push_back(channel);
        g_channels[channel].push_back(conn);
    }

    conn->is_subscriber = true;
    write_pubsub_frame(out, "subscribe", channel, (int64_t)subs.size(), nullptr);
}

static void do_unsubscribe(const std::vector<std::string> &cmd, Buffer *out, Conn *conn) {
    const std::string &channel = cmd[1];

    auto &subs = conn->subscriptions;
    auto  it   = std::find(subs.begin(), subs.end(), channel);
    if (it != subs.end()) subs.erase(it);

    auto ch_it = g_channels.find(channel);
    if (ch_it != g_channels.end()) {
        auto &vec = ch_it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), conn), vec.end());
        if (vec.empty()) g_channels.erase(ch_it);
    }

    if (subs.empty()) conn->is_subscriber = false;
    write_pubsub_frame(out, "unsubscribe", channel, (int64_t)subs.size(), nullptr);
}

static void do_publish(const std::vector<std::string> &cmd, Buffer *out) {
    const std::string &channel = cmd[1];
    const std::string &message = cmd[2];

    auto it = g_channels.find(channel);
    if (it == g_channels.end()) return out_int(out, 0);

    int64_t count = 0;
    for (Conn *sub : it->second) {
        push_to_subscriber(sub, channel, message);
        count++;
    }
    return out_int(out, count);
}

void pubsub_unsubscribe_all(Conn *conn) {
    for (const std::string &channel : conn->subscriptions) {
        auto it = g_channels.find(channel);
        if (it == g_channels.end()) continue;
        auto &vec = it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), conn), vec.end());
        if (vec.empty()) g_channels.erase(it);
    }
    conn->subscriptions.clear();
    conn->is_subscriber = false;
}

// ---------------------------------------------------------------------------
// Active expiration
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

void do_request(const std::vector<std::string> &cmd, Buffer *out,
                Conn *conn, std::weak_ptr<ThreadPool> tp) {
    if (conn && conn->is_subscriber) {
        if (cmd.size() == 2 && cmd[0] == "subscribe")
            return do_subscribe  (cmd, out, conn);
        if (cmd.size() == 2 && cmd[0] == "unsubscribe")
            return do_unsubscribe(cmd, out, conn);
        return out_err(out, ERR_BAD_TYPE,
                       "only SUBSCRIBE/UNSUBSCRIBE allowed in subscriber mode");
    }

    if      (cmd.size() == 2 && cmd[0] == "get")          return do_get        (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "set")          return do_set        (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "del")          return do_del        (cmd, out);
    else if (cmd.size() == 1 && cmd[0] == "keys")         return do_keys       (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "lpush")        return do_lpush      (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "rpush")        return do_rpush      (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "lpop")         return do_lpop       (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "rpop")         return do_rpop       (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "llen")         return do_llen       (cmd, out);
    else if (cmd.size() == 4 && cmd[0] == "lrange")       return do_lrange     (cmd, out);
    else if (cmd.size() == 4 && cmd[0] == "zadd")         return do_zadd       (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "zrem")         return do_zrem       (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "zscore")       return do_zscore     (cmd, out);
    else if (cmd.size() == 6 && cmd[0] == "zquery")       return do_zquery     (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "expire")       return do_expire     (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "ttl")          return do_ttl        (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "persist")      return do_persist    (cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "subscribe")    return do_subscribe  (cmd, out, conn);
    else if (cmd.size() == 2 && cmd[0] == "unsubscribe")  return do_unsubscribe(cmd, out, conn);
    else if (cmd.size() == 3 && cmd[0] == "publish")      return do_publish    (cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "expireat")     return do_expireat   (cmd, out);
    else if (cmd.size() == 1 && cmd[0] == "bgrewriteaof") return do_bgrewriteaof(cmd, out, tp);
    else return out_err(out, ERR_UNKNOWN, "unknown command");
}

// ---------------------------------------------------------------------------
// db_for_each_entry
// ---------------------------------------------------------------------------

void db_for_each_entry(
    std::function<void(
        const std::string &key,
        uint32_t           type,
        const std::string &str,
        const std::deque<std::string> &list,
        AVLNode           *zset_root,
        uint64_t           expire_at_ms
    )> fn
) {
    hm_for_each(&g_data.db, [&](Hnode *node) {
        Entry *ent = container_of(node, Entry, node);

        if (ent->heap_idx != (size_t)-1) {
            if (get_monotonic_msec() >= g_data.heap[ent->heap_idx].val)
                return;
        }

        uint64_t expire_at = (ent->heap_idx != (size_t)-1)
            ? g_data.heap[ent->heap_idx].val
            : 0;

        fn(ent->key, ent->type, ent->str, ent->list,
           ent->type == T_ZSET ? ent->zset.root : nullptr,
           expire_at);
    });
}