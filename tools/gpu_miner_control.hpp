#pragma once

#include <atomic>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace geocoin::tools::gpu_miner {

class StdinControl {
    bool enabled_;
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::string line_;
public:
    explicit StdinControl(bool enabled) : enabled_(enabled) {}
    void request_stop() { stopped_.store(true, std::memory_order_relaxed); }
    void consume(const std::string& bytes)
    {
        for (char c : bytes) {
            if (c == '\n') {
                if (line_ == "stop" || line_ == "cancel") request_stop();
                line_.clear();
            } else if (c != '\r') {
                if (line_.size() >= 1024) { request_stop(); return; }
                line_ += c;
            }
        }
    }
    bool cancelled()
    {
        if (!enabled_ || stopped_.load(std::memory_order_relaxed)) return stopped_.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mutex_);
        char bytes[256];
#ifdef _WIN32
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (!input || input == INVALID_HANDLE_VALUE) { request_stop(); return true; }
        const DWORD type = GetFileType(input);
        DWORD available = 0;
        if (type == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) { request_stop(); return true; }
            if (!available) return false;
        } else if (type != FILE_TYPE_DISK) {
            // GUI control uses a pipe. Never block a worker on console line input.
            return false;
        }
        DWORD count = 0;
        const DWORD wanted = type == FILE_TYPE_PIPE ? (available < sizeof(bytes) ? available : sizeof(bytes)) : sizeof(bytes);
        if (!ReadFile(input, bytes, wanted, &count, nullptr) || count == 0) request_stop();
        else consume(std::string(bytes, count));
#else
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (::poll(&input, 1, 0) > 0) {
            const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count <= 0) request_stop();
            else consume(std::string(bytes, static_cast<size_t>(count)));
        }
#endif
        return stopped_.load(std::memory_order_relaxed);
    }
};

} // namespace geocoin::tools::gpu_miner
