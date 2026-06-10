#include "socket_io.h"
#include "../utils/logging.h"
#include "../utils/common.h"
#include "../timers/timers.h"
#include "../storage/commands.h"
#include "../persistence/persistence.h"
#include "../threadpool/threadpool.h"

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

static const uint64_t k_idle_timeout_ms = 5000;

static struct {
    std::vector<Conn *> fd2conn;
    DList               idle_list;
} g_data;

static void conn_destroy(Conn *conn) {
    // Clean up pub/sub state before freeing — removes this Conn* from
    // every channel it was subscribed to, preventing dangling pointers.
    pubsub_unsubscribe_all(conn);

    close(conn->fd);
    g_data.fd2conn[conn->fd] = nullptr;
    dlist_detach(&conn->idle_node);
    delete conn;
}

static int32_t next_timer_ms() {
    uint64_t now_ms  = get_monotonic_msec();
    uint64_t next_ms = (uint64_t)-1;

    if (!dlist_empty(&g_data.idle_list)) {
        Conn    *conn = container_of(g_data.idle_list.next, Conn, idle_node);
        uint64_t exp  = conn->last_active_ms + k_idle_timeout_ms;
        if (exp < next_ms) next_ms = exp;
    }

    uint64_t ttl = next_ttl_ms();
    if (ttl < next_ms) next_ms = ttl;

    if (next_ms == (uint64_t)-1) return -1;
    if (next_ms <= now_ms)       return  0;
    return (int32_t)(next_ms - now_ms);
}

static void process_timers() {
    uint64_t now_ms = get_monotonic_msec();

    while (!dlist_empty(&g_data.idle_list)) {
        Conn    *conn   = container_of(g_data.idle_list.next, Conn, idle_node);

        // Subscribers are exempt from the idle timeout.
        // They may sit silently for a long time waiting for published messages.
        // Once they unsubscribe they re-enter the normal timeout path.
        if (conn->is_subscriber) break;

        uint64_t expire = conn->last_active_ms + k_idle_timeout_ms;
        if (expire > now_ms) break;
        fprintf(stderr, "closing idle connection fd=%d\n", conn->fd);
        conn_destroy(conn);
    }

    expire_keys();
}

int main() {
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

    dlist_init(&g_data.idle_list);

    // Create the threadpool — 4 workers for background tasks (BGREWRITEAOF etc.)
    ThreadPool *tp = tp_create(4);
    socket_io_set_threadpool(tp);

    // Replay existing AOF and open file for appending
    aof_init("appendonly.aof");

    std::vector<struct pollfd> poll_args;

    while (true) {
        poll_args.clear();
        poll_args.push_back({fd, POLLIN, 0});

        for (Conn *conn : g_data.fd2conn) {
            if (!conn) continue;
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            if (conn->want_read)  pfd.events |= POLLIN;
            if (conn->want_write) pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        int32_t timeout_ms = next_timer_ms();
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);
        if (rv < 0 && errno == EINTR) continue;
        if (rv < 0) die("poll()");

        if (poll_args[0].revents) {
            if (Conn *conn = handle_accept(fd, &g_data.idle_list)) {
                if (g_data.fd2conn.size() <= (size_t)conn->fd)
                    g_data.fd2conn.resize(conn->fd + 1);
                assert(!g_data.fd2conn[conn->fd]);
                g_data.fd2conn[conn->fd] = conn;
            }
        }

        for (size_t i = 1; i < poll_args.size(); ++i) {
            uint32_t ready = (uint32_t)poll_args[i].revents;
            if (!ready) continue;

            Conn *conn = g_data.fd2conn[poll_args[i].fd];

            // Refresh idle timer only for non-subscribers.
            // Subscribers' last_active_ms doesn't matter since they're
            // exempt from the timeout, but we still move them to keep
            // the list consistent if they later unsubscribe.
            conn->last_active_ms = get_monotonic_msec();
            dlist_detach(&conn->idle_node);
            dlist_insert_before(&g_data.idle_list, &conn->idle_node);

            if (ready & POLLIN)  handle_read (conn);
            if (ready & POLLOUT) handle_write(conn);

            if ((ready & POLLERR) || conn->want_close)
                conn_destroy(conn);
        }

        process_timers();
    }

    return 0;
}