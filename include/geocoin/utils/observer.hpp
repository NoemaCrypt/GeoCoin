#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace geocoin {

template<typename... Args>
class ObserverList {
public:
    class Connection {
    public:
        Connection() = default;
        void disconnect() {
            if (list_ != nullptr) {
                list_->disconnect(id_);
                list_ = nullptr;
            }
        }

    private:
        friend class ObserverList;
        ObserverList* list_ = nullptr;
        uint64_t id_ = 0;
    };

    template<typename Callback>
    Connection connect(Callback&& callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t id = next_id_++;
        slots_.emplace_back(id, std::function<void(Args...)>(std::forward<Callback>(callback)));
        Connection connection;
        connection.list_ = this;
        connection.id_ = id;
        return connection;
    }

    void operator()(Args... args) const {
        std::vector<std::function<void(Args...)>> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot.reserve(slots_.size());
            for (const auto& slot : slots_) {
                snapshot.push_back(slot.second);
            }
        }
        for (const auto& callback : snapshot) {
            callback(args...);
        }
    }

private:
    void disconnect(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = slots_.begin(); it != slots_.end(); ++it) {
            if (it->first == id) {
                slots_.erase(it);
                return;
            }
        }
    }

    mutable std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::vector<std::pair<uint64_t, std::function<void(Args...)>>> slots_;
};

class ScopedObserver {
public:
    ScopedObserver() = default;
    ScopedObserver(const ScopedObserver&) = delete;
    ScopedObserver& operator=(const ScopedObserver&) = delete;
    ScopedObserver(ScopedObserver&& other) noexcept : disconnect_(std::move(other.disconnect_)) {
        other.disconnect_ = nullptr;
    }
    ScopedObserver& operator=(ScopedObserver&& other) noexcept {
        if (this != &other) {
            reset();
            disconnect_ = std::move(other.disconnect_);
            other.disconnect_ = nullptr;
        }
        return *this;
    }
    ~ScopedObserver() { reset(); }

    template<typename Connection>
    ScopedObserver& operator=(Connection connection) {
        reset();
        disconnect_ = [connection = std::move(connection)]() mutable { connection.disconnect(); };
        return *this;
    }

    void disconnect() { reset(); }

private:
    void reset() {
        if (disconnect_) {
            auto disconnect = std::move(disconnect_);
            disconnect_ = nullptr;
            disconnect();
        }
    }

    std::function<void()> disconnect_;
};

} // namespace geocoin
