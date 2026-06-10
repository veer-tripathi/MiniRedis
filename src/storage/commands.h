#pragma once

#include "../utils/buffer.h"
#include <cstdint>
#include <string>
#include <vector>

// Dispatch a parsed command and write the response into out.
void do_request(std::vector<std::string> &cmd, Buffer *out);
#pragma once

#include "../utils/buffer.h"
#include "../networking/connection.h"
#include <cstdint>
#include <string>
#include <vector>

// Dispatch a parsed command and write the response into out.
// conn is passed so that SUBSCRIBE/UNSUBSCRIBE can mutate connection state
// and PUBLISH can write directly into subscriber outgoing buffers.
void do_request(std::vector<std::string> &cmd, Buffer *out, Conn *conn);

// Iterate every live (non-expired) key in the database and call fn(key,
// type, str, list, zset, expire_at_ms) where:
//   key          — the key string
//   type         — 1=string, 2=zset, 3=list
//   str          — value if type==1, else ""
//   list         — elements if type==3, else empty
//   zset_root    — AVLNode* root of the ZSet AVL tree if type==2, else nullptr
//   expire_at_ms — absolute monotonic-ms expiry, or 0 if no TTL
// Used by aof_compact() to snapshot live state without coupling
// persistence.cpp to the internal Entry struct.
#include <functional>
#include <string>
#include <deque>
#include "avl.h"
void db_for_each_entry(
    std::function<void(
        const std::string &key,
        uint32_t           type,
        const std::string &str,
        const std::deque<std::string> &list,
        AVLNode           *zset_root,
        uint64_t           expire_at_ms
    )> fn
);

// Called by process_timers() every event loop tick.
void expire_keys();

// Returns the monotonic-ms timestamp of the nearest TTL expiry.
// Returns UINT64_MAX if no keys have a TTL set.
uint64_t next_ttl_ms();

// Called by conn_destroy() in event_loop.cpp when a connection closes.
// Removes the connection from all channels it was subscribed to.
void pubsub_unsubscribe_all(Conn *conn);
// Called by process_timers() every event loop tick.
// Pops up to 2000 expired keys from the TTL heap and deletes them.
void expire_keys();

// Returns the monotonic-ms timestamp of the nearest TTL expiry.
// Returns UINT64_MAX if no keys have a TTL set.
// Used by next_timer_ms() in event_loop.cpp.
uint64_t next_ttl_ms();