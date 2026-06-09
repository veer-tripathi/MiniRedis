#pragma once

// ---------------------------------------------------------------------------
// Thread pool module — placeholder for Day N
// ---------------------------------------------------------------------------
//
// Planned interface:
//
//   struct ThreadPool;
//
//   ThreadPool *tp_create(size_t num_threads);
//   void        tp_submit(ThreadPool *tp, std::function<void()> task);
//   void        tp_destroy(ThreadPool *tp);   // waits for in-flight tasks
