#include "threadpool.h"

// ---------------------------------------------------------------------------
// Worker loop
// ---------------------------------------------------------------------------
// Each worker thread runs this loop forever until stop is set.
//
// The flow:
//   1. Lock the mutex and wait on the condition variable.
//      wait() atomically releases the lock and sleeps until notified.
//   2. On wake-up: if stop is set and queue is empty, exit.
//      If stop is set but queue is non-empty, drain remaining tasks first
//      so submitted work is never silently dropped on shutdown.
//   3. Pop one task, release the lock, run the task.
//      Lock is released BEFORE running so other workers can pick up tasks
//      concurrently — we never hold the lock during task execution.
// ---------------------------------------------------------------------------

static void worker_loop(ThreadPool *tp) {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(tp->mu);

            // Sleep until there is work to do or we are told to stop
            tp->cv.wait(lock, [tp] {
                return !tp->queue.empty() || tp->stop;
            });

            // Drain remaining tasks even if stop is set
            if (tp->queue.empty()) return;   // stop=true and nothing left

            task = std::move(tp->queue.front());
            tp->queue.pop();
        }   // lock released here

        task();   // run outside the lock so other workers can proceed
    }
}

// ---------------------------------------------------------------------------
// tp_create
// ---------------------------------------------------------------------------

ThreadPool *tp_create(size_t num_threads) {
    ThreadPool *tp = new ThreadPool();
    tp->workers.reserve(num_threads);
    for (size_t i = 0; i < num_threads; i++)
        tp->workers.emplace_back(worker_loop, tp);
    return tp;
}

// ---------------------------------------------------------------------------
// tp_submit
// ---------------------------------------------------------------------------

void tp_submit(ThreadPool *tp, std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(tp->mu);
        tp->queue.push(std::move(task));
    }
    tp->cv.notify_one();   // wake exactly one sleeping worker
}

// ---------------------------------------------------------------------------
// tp_destroy
// ---------------------------------------------------------------------------

void tp_destroy(ThreadPool *tp) {
    {
        std::lock_guard<std::mutex> lock(tp->mu);
        tp->stop = true;
    }
    tp->cv.notify_all();   // wake every worker so they can see stop=true

    for (std::thread &t : tp->workers)
        t.join();

    delete tp;
}