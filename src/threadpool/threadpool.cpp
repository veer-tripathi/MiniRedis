#include "threadpool.h"

static void worker_loop(std::weak_ptr<ThreadPool> wp) {
    while (true) {
        std::function<void()> task;
        {
            auto tp = wp.lock();
            if (!tp) return;

            std::unique_lock<std::mutex> lock(tp->mu);
            tp->cv.wait(lock, [&tp] { return !tp->queue.empty() || tp->stop; });
            if (tp->queue.empty()) return;
            task = std::move(tp->queue.front());
            tp->queue.pop();
        }
        task();
    }
}

std::shared_ptr<ThreadPool> tp_create(size_t num_threads) {
    auto tp = std::make_shared<ThreadPool>();
    tp->workers.reserve(num_threads);
    for (size_t i = 0; i < num_threads; i++)
        tp->workers.emplace_back(worker_loop, std::weak_ptr<ThreadPool>(tp));
    return tp;
}

void tp_submit(std::weak_ptr<ThreadPool> tp, std::function<void()> task) {
    if (auto locked = tp.lock()) {
        std::lock_guard<std::mutex> lock(locked->mu);
        locked->queue.push(std::move(task));
        locked->cv.notify_one();
    }
}