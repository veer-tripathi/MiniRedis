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