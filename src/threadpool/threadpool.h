#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// ThreadPool
// ---------------------------------------------------------------------------
// Fixed-size pool of worker threads. The main thread submits tasks via
// tp_submit(); workers pick them up and run them independently.
//
// The main thread never blocks waiting for a task to finish — fire and forget.
// Used to offload slow background work (e.g. BGREWRITEAOF) so the event loop
// keeps serving other clients while the heavy operation runs.
//
// Thread safety: tp_submit() is safe to call from the main thread at any time.
// ---------------------------------------------------------------------------

struct ThreadPool {
    std::vector<std::thread>          workers;
    std::queue<std::function<void()>> queue;
    std::mutex                        mu;
    std::condition_variable           cv;
    bool                              stop = false;
};

// Create a pool with num_threads worker threads and start them immediately.
ThreadPool *tp_create(size_t num_threads);

// Push a task onto the queue. Returns immediately — does not wait for the
// task to complete. The task will run on one of the worker threads.
void tp_submit(ThreadPool *tp, std::function<void()> task);

// Signal all workers to stop, wait for in-flight tasks to finish, then
// join all threads. Safe to call only once.
void tp_destroy(ThreadPool *tp);