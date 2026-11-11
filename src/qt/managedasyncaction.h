// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <utility>

/**
 * One joinable background action owned by a GUI object.
 *
 * Lifecycle methods are called from the owning GUI thread. The worker only
 * publishes completion through atomics; it never touches QObject state.
 */
class ManagedAsyncAction
{
public:
    ManagedAsyncAction() = default;
    ~ManagedAsyncAction() { wait(); }

    ManagedAsyncAction(const ManagedAsyncAction&) = delete;
    ManagedAsyncAction& operator=(const ManagedAsyncAction&) = delete;
    ManagedAsyncAction(ManagedAsyncAction&&) = delete;
    ManagedAsyncAction& operator=(ManagedAsyncAction&&) = delete;

    bool start(std::function<void()> action)
    {
        if (!action || !reapIfFinished()) {
            return false;
        }

        completed_.store(false, std::memory_order_relaxed);
        failed_.store(false, std::memory_order_relaxed);
        try {
            worker_ = std::thread([this, action = std::move(action)]() mutable {
                try {
                    action();
                } catch (...) {
                    failed_.store(true, std::memory_order_relaxed);
                }
                completed_.store(true, std::memory_order_release);
            });
        } catch (...) {
            failed_.store(true, std::memory_order_relaxed);
            completed_.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    bool reapIfFinished()
    {
        if (!worker_.joinable()) {
            return true;
        }
        if (!completed_.load(std::memory_order_acquire)) {
            return false;
        }
        worker_.join();
        return true;
    }

    void wait()
    {
        if (worker_.joinable()) {
            worker_.join();
        }
        completed_.store(true, std::memory_order_release);
    }

    bool isRunning() const
    {
        return worker_.joinable() && !completed_.load(std::memory_order_acquire);
    }

    bool failed() const
    {
        return failed_.load(std::memory_order_acquire);
    }

private:
    std::thread worker_;
    std::atomic<bool> completed_{true};
    std::atomic<bool> failed_{false};
};
