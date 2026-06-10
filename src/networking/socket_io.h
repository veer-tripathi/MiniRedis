#pragma once

#include "connection.h"
#include "../threadpool/threadpool.h"

// Set fd to non-blocking mode. Calls die() on failure.
void fd_set_nb(int fd);

// Must be called once at startup before any connections are accepted.
// Gives socket_io the threadpool to pass into do_request.
void socket_io_set_threadpool(ThreadPool *tp);

// Accept a new connection from the listening socket fd.
// idle_list is the global sentinel from event_loop.cpp — the new Conn
// is appended to it to start its idle timer.
// Returns a heap-allocated Conn with want_read=true, or nullptr on error.
Conn *handle_accept(int fd, DList *idle_list);

// Called when the connection socket is readable.
void handle_read(Conn *conn);

// Called when the connection socket is writable.
void handle_write(Conn *conn);