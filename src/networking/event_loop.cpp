#include "socket_io.h"
#include "../utils/logging.h"
#include "../utils/common.h"
#include "../timers/timers.h"
#include "../storage/commands.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const uint64_t k_idle_timeout_ms = 5000;   // close after 5s idle

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static struct {
    std::vector<Conn *> fd2conn;   // fd → Conn*
    DList               idle_list; // sentinel head of idle-timeout list
} g_data;

// ---------------------------------------------------------------------------
// conn_destroy
// ---------------------------------------------------------------------------
// All connection cleanup goes through here — whether graceful close,
// error, or idle timeout. Never inline close/delete anywhere else.

static void conn_destroy(Conn *conn) {
    close(conn->fd);
    g_data.fd2conn[conn->fd] = nullptr;
    dlist_detach(&conn->idle_node);
    delete conn;
}

// ---------------------------------------------------------------------------
// next_timer_ms
// ---------------------------------------------------------------------------
// Returns milliseconds until the nearest timer fires — used as poll() timeout.
// Checks two timer sources:
//   1. idle_list front  — nearest idle connection expiry
//   2. next_ttl_ms()    — nearest key TTL expiry (from the heap in commands.cpp)
// Returns -1 if no timers exist (poll blocks forever).
// Returns  0 if a timer already fired (poll returns immediately).

static int32_t next_timer_ms() {
    uint64_t now_ms  = get_monotonic_msec();
    uint64_t next_ms = (uint64_t)-1;   // UINT64_MAX = no timer yet

    // Check idle connection list
    if (!dlist_empty(&g_data.idle_list)) {
        Conn    *conn = container_of(g_data.idle_list.next, Conn, idle_node);
        uint64_t exp  = conn->last_active_ms + k_idle_timeout_ms;
        if (exp < next_ms) next_ms = exp;
    }

    // Check TTL heap — next_ttl_ms() returns heap[0].val or UINT64_MAX
    uint64_t ttl = next_ttl_ms();
    if (ttl < next_ms) next_ms = ttl;

    if (next_ms == (uint64_t)-1) return -1;   // no timers
    if (next_ms <= now_ms)       return  0;   // already overdue
    return (int32_t)(next_ms - now_ms);
}

// ---------------------------------------------------------------------------
// process_timers
// ---------------------------------------------------------------------------
// Called every event loop iteration after poll() returns.
// Handles both idle connection timeouts and TTL key expiration.

static void process_timers() {
    uint64_t now_ms = get_monotonic_msec();

    // 1. Close idle connections
    // List is oldest-first, stop at first connection still within timeout.
    while (!dlist_empty(&g_data.idle_list)) {
        Conn    *conn   = container_of(g_data.idle_list.next, Conn, idle_node);
        uint64_t expire = conn->last_active_ms + k_idle_timeout_ms;
        if (expire > now_ms) break;
        fprintf(stderr, "closing idle connection fd=%d\n", conn->fd);
        conn_destroy(conn);
    }

    // 2. Delete expired keys from the TTL heap
    // expire_keys() deletes up to 2000 keys per call.
    // If it hits the limit, next_timer_ms() returns 0 so poll()
    // doesn't block and we come back here immediately.
    expire_keys();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    // Create and bind listening socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (const sockaddr *)&addr, sizeof(addr))) die("bind()");
    fd_set_nb(fd);
    if (listen(fd, SOMAXCONN)) die("listen()");

    // Initialise the idle list sentinel (points to itself = empty)
    dlist_init(&g_data.idle_list);

    std::vector<struct pollfd> poll_args;

    while (true) {
        poll_args.clear();

        // Slot 0: listening socket
        poll_args.push_back({fd, POLLIN, 0});

        // One slot per active connection
        for (Conn *conn : g_data.fd2conn) {
            if (!conn) continue;
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            if (conn->want_read)  pfd.events |= POLLIN;
            if (conn->want_write) pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        // Block until IO is ready or the nearest timer fires
        int32_t timeout_ms = next_timer_ms();
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);
        if (rv < 0 && errno == EINTR) continue;
        if (rv < 0) die("poll()");

        // New connection on the listening socket
        if (poll_args[0].revents) {
            if (Conn *conn = handle_accept(fd, &g_data.idle_list)) {
                if (g_data.fd2conn.size() <= (size_t)conn->fd)
                    g_data.fd2conn.resize(conn->fd + 1);
                assert(!g_data.fd2conn[conn->fd]);
                g_data.fd2conn[conn->fd] = conn;
            }
        }

        // IO on existing connections
        for (size_t i = 1; i < poll_args.size(); ++i) {
            uint32_t ready = (uint32_t)poll_args[i].revents;
            if (!ready) continue;

            Conn *conn = g_data.fd2conn[poll_args[i].fd];

            // Refresh idle timer: update timestamp and move to back of list
            conn->last_active_ms = get_monotonic_msec();
            dlist_detach(&conn->idle_node);
            dlist_insert_before(&g_data.idle_list, &conn->idle_node);

            if (ready & POLLIN)  handle_read (conn);
            if (ready & POLLOUT) handle_write(conn);

            if ((ready & POLLERR) || conn->want_close) {
                conn_destroy(conn);
            }
        }

        // Run timers after every poll wakeup
        process_timers();
    }

    return 0;
}