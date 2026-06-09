#pragma once

#include "../utils/buffer.h"
#include "../timers/timers.h"   // DList + get_monotonic_msec
#include <cstdint>

// Per-connection state held by the event loop.
struct Conn {
    int  fd          = -1;

    // Readiness intentions communicated back to the event loop.
    bool want_read   = false;
    bool want_write  = false;
    bool want_close  = false;

    // Raw bytes received from the client.
    // Parser reads from the front, new data appends to the back.
    Buffer incoming;

    // Serialised responses queued for sending.
    // Responses append to the back, handle_write() drains from the front.
    Buffer outgoing;

    // -------------------------------------------------------------------------
    // Idle-connection timer
    // -------------------------------------------------------------------------
    // last_active_ms is updated every time this connection does any IO.
    // idle_node is the intrusive DList node linking this Conn into the
    // global idle_list in event_loop.cpp.
    //
    // Connections are kept in idle_list sorted by last_active_ms (oldest
    // at the front, newest at the back). Since all timeouts use the same
    // fixed duration, this ordering is maintained simply by moving a conn
    // to the back of the list on every IO event.
    uint64_t last_active_ms = 0;
    DList    idle_node;
};