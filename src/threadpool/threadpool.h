#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

struct ThreadPool {
    std::vector<std::thread>          workers;
    std::queue<std::function<void()>> queue;
    std::mutex                        mu;
    std::condition_variable           cv;
    bool                              stop = false;

    ~ThreadPool() {
        { std::lock_guard<std::mutex> lock(mu); stop = true; }
        cv.notify_all();
        for (std::thread &t : workers) t.join();
    }
};

std::shared_ptr<ThreadPool> tp_create(size_t num_threads);
void tp_submit(std::weak_ptr<ThreadPool> tp, std::function<void()> task);