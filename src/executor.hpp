#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class Executor {
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;

    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }

public:
    explicit Executor(int workers) {
        try {
            for (int i = 0; i < workers; ++i) threads_.emplace_back([this] { run(); });
        } catch (...) {
            drain();
            throw;
        }
    }

    ~Executor() { drain(); }

    void drain() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        available_.notify_all();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
    }

    bool submit(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || queue_.size() >= 1024) return false;
            queue_.push_back(std::move(task));
        }
        available_.notify_one();
        return true;
    }
};
