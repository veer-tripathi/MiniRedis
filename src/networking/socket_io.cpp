#include "socket_io.h"
#include "../utils/logging.h"
#include "../protocol/protocol.h"
#include "../protocol/serializer.h"
#include "../storage/commands.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

static const size_t k_max_msg = 32 << 20;

// ---------------------------------------------------------------------------
// fd_set_nb
// ---------------------------------------------------------------------------

void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) die("fcntl F_GETFL error");
    errno = 0;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (errno) die("fcntl F_SETFL error");
}

// ---------------------------------------------------------------------------
// handle_accept
// ---------------------------------------------------------------------------
// idle_list is passed in from event_loop.cpp (where g_data lives).
// We append the new Conn to the back of the idle list to start its timer.

Conn *handle_accept(int fd, DList *idle_list) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0) {
        msg_errno("accept() error");
        return nullptr;
    }

    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
        ntohs(client_addr.sin_port));

    fd_set_nb(connfd);

    Conn *conn      = new Conn();
    conn->fd        = connfd;
    conn->want_read = true;
    conn->incoming  = buf_init();
    conn->outgoing  = buf_init();

    // Start the idle timer — record when this connection was accepted
    // and insert it at the back of the idle list.
    conn->last_active_ms = get_monotonic_msec();
    dlist_insert_before(idle_list, &conn->idle_node);

    return conn;
}

// ---------------------------------------------------------------------------
// try_one_request
// ---------------------------------------------------------------------------

static bool try_one_request(Conn *conn) {
    if (conn->incoming.size() < 4) return false;

    uint32_t body_len = 0;
    memcpy(&body_len, conn->incoming.data(), 4);

    if (body_len > k_max_msg) {
        msg("request too large — closing connection");
        conn->want_close = true;
        return false;
    }

    if (4 + body_len > conn->incoming.size()) return false;

    const uint8_t *body = conn->incoming.data() + 4;
    std::vector<std::string> cmd;
    if (parse_req(body, body_len, cmd) < 0) {
        msg("malformed request — closing connection");
        conn->want_close = true;
        return false;
    }

    size_t header_pos = conn->outgoing.size();
    buf_append_u32(&conn->outgoing, 0);
    do_request(cmd, &conn->outgoing);
    uint32_t resp_len = (uint32_t)(conn->outgoing.size() - header_pos - 4);
    memcpy(conn->outgoing.data() + header_pos, &resp_len, 4);

    buf_consume(&conn->incoming, 4 + body_len);
    return true;
}

// ---------------------------------------------------------------------------
// handle_read
// ---------------------------------------------------------------------------

void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));

    if (rv < 0 && errno == EAGAIN) return;

    if (rv < 0) {
        msg_errno("read() error");
        conn->want_close = true;
        return;
    }

    if (rv == 0) {
        if (conn->incoming.size() > 0) msg("unexpected EOF mid-message");
        else                           msg("client disconnected");
        conn->want_close = true;
        return;
    }

    buf_append(&conn->incoming, buf, (size_t)rv);
    while (try_one_request(conn)) {}

    if (!conn->want_close && conn->outgoing.size() > 0) {
        conn->want_read  = false;
        conn->want_write = true;
        handle_write(conn);
    }
}

// ---------------------------------------------------------------------------
// handle_write
// ---------------------------------------------------------------------------

void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);

    ssize_t rv = write(conn->fd,
                       conn->outgoing.data(),
                       conn->outgoing.size());

    if (rv < 0 && errno == EAGAIN) return;

    if (rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;
        return;
    }

    buf_consume(&conn->outgoing, (size_t)rv);

    if (conn->outgoing.size() == 0) {
        conn->want_read  = true;
        conn->want_write = false;
    }
}