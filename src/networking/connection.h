#pragma once

#include "../utils/buffer.h"
#include "../timers/timers.h"
#include <cstdint>
#include <string>
#include <vector>

// Per-connection state held by the event loop.
struct Conn {
    int  fd          = -1;

    bool want_read   = false;
    bool want_write  = false;
    bool want_close  = false;

    Buffer incoming;
    Buffer outgoing;

    // -------------------------------------------------------------------------
    // Idle-connection timer
    // -------------------------------------------------------------------------
    uint64_t last_active_ms = 0;
    DList    idle_node;

    // -------------------------------------------------------------------------
    // Pub/Sub state
    // -------------------------------------------------------------------------
    // Names of all channels this connection is currently subscribed to.
    // Empty = normal request/reply mode.
    // Non-empty = subscriber mode: no commands accepted except UNSUBSCRIBE.
    std::vector<std::string> subscriptions;

    // True when this connection is in subscriber mode.
    // Subscribers are exempt from the idle timeout — they may sit
    // silently for a long time waiting for published messages.
    bool is_subscriber = false;
};