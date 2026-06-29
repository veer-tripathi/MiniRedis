#pragma once

#include <memory>
#include "connection.h"
#include "../threadpool/threadpool.h"

void fd_set_nb(int fd);

// Takes shared_ptr — event_loop passes ownership reference at startup.
// Stored internally as weak_ptr since socket_io does not own the pool.
void socket_io_set_threadpool(std::shared_ptr<ThreadPool> tp);

Conn *handle_accept(int fd, DList *idle_list);
void handle_read(Conn *conn);
void handle_write(Conn *conn);