#pragma once

#include "../utils/buffer.h"
#include "../networking/connection.h"
#include "../threadpool/threadpool.h"
#include "avl.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Main command dispatcher
// ---------------------------------------------------------------------------
// Parses cmd, executes it against g_data, writes the response into out.
// conn   — needed for SUBSCRIBE / UNSUBSCRIBE / PUBLISH
// tp     — threadpool used by BGREWRITEAOF to run compaction off-thread
void do_request(const std::vector<std::string> &cmd,
                Buffer *out,
                Conn *conn,
                std::weak_ptr<ThreadPool> tp);

// ---------------------------------------------------------------------------
// DB snapshot iterator (used by aof_compact)
// ---------------------------------------------------------------------------
// Walks every live (non-expired) key and calls fn with enough info to
// reconstruct it. Skips keys whose TTL has already elapsed.
void db_for_each_entry(
    std::function<void(
        const std::string             &key,
        uint32_t                       type,
        const std::string             &str,
        const std::deque<std::string> &list,
        AVLNode                       *zset_root,
        uint64_t                       expire_at_ms
    )> fn
);

// ---------------------------------------------------------------------------
// TTL / timer helpers  (called from event_loop.cpp)
// ---------------------------------------------------------------------------

// Pop up to 2000 expired keys from the TTL heap and delete them.
// Called once per event loop tick.
void expire_keys();

// Return the monotonic-ms deadline of the soonest-expiring key.
// Returns UINT64_MAX when no keys have a TTL.
uint64_t next_ttl_ms();

// ---------------------------------------------------------------------------
// Pub/Sub cleanup  (called from event_loop.cpp on connection close)
// ---------------------------------------------------------------------------
void pubsub_unsubscribe_all(Conn *conn);