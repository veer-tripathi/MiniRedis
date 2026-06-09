#pragma once

#include <cstdint>
#include <ctime>

// ---------------------------------------------------------------------------
// Monotonic clock
// ---------------------------------------------------------------------------
// Returns current time in milliseconds on the monotonic clock.
// Monotonic time only moves forward and cannot be adjusted by the OS,
// making it safe for timers. Never use CLOCK_REALTIME for timers —
// it can jump backward due to NTP or DST changes.
// ---------------------------------------------------------------------------
static inline uint64_t get_monotonic_msec() {
    struct timespec tv = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return (uint64_t)tv.tv_sec * 1000
         + (uint64_t)tv.tv_nsec / 1'000'000;
}

// ---------------------------------------------------------------------------
// Intrusive doubly-linked list
// ---------------------------------------------------------------------------
// Embed DList inside any struct to make it a list node.
// The list is circular with a dummy sentinel head.
//
//  Empty:          sentinel <-> sentinel
//  With A, B, C:   sentinel <-> A <-> B <-> C <-> sentinel
//
// The sentinel is never a real element — it anchors the circle so
// insert and remove never need to special-case an empty list.
// ---------------------------------------------------------------------------
struct DList {
    DList *prev = nullptr;
    DList *next = nullptr;
};

// Make a node point to itself — an empty circular list.
inline void dlist_init(DList *node) {
    node->prev = node->next = node;
}

// True when the list has no real elements (only the sentinel).
inline bool dlist_empty(const DList *node) {
    return node->next == node;
}

// Unlink a node from wherever it currently sits. O(1).
inline void dlist_detach(DList *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

// Insert rookie immediately before target. O(1).
// Inserting before the sentinel == appending to the back.
inline void dlist_insert_before(DList *target, DList *rookie) {
    DList *prev  = target->prev;
    prev->next   = rookie;
    rookie->prev = prev;
    rookie->next = target;
    target->prev = rookie;
}