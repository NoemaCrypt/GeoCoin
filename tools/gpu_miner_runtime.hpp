#pragma once

#include "gpu_miner_common.hpp"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace geocoin::tools::gpu_miner {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

inline std::string device_identity(const DeviceInfo& d)
{
    const std::string key = !d.uuid.empty() ? "uuid:" + d.uuid
        : !d.pci_bus_id.empty() ? "pci:" + d.pci_bus_id : "id:" + std::to_string(d.id);
    return std::string(backend_name(d.backend)) + ":" + key;
}

inline Json device_json(const DeviceInfo& d)
{
    return {{"backend", backend_name(d.backend)}, {"id", d.id}, {"name", d.name},
            {"vendor", d.vendor}, {"detail", d.detail}, {"available", d.available},
            {"identity", device_identity(d)}, {"pci_bus_id", d.pci_bus_id}, {"uuid", d.uuid}};
}

// Backend progress is job-cumulative. Merge only its new delta, including the final snapshot.
class SessionStats {
    struct Counter { DeviceInfo device; uint64_t hashes = 0; uint64_t job_hashes = 0; };
    Clock::time_point start_;
    std::vector<Counter> counters_;
public:
    explicit SessionStats(Clock::time_point start = Clock::now()) : start_(start) {}
    void reset(Clock::time_point start = Clock::now()) { start_ = start; counters_.clear(); }
    void begin_job() { for (auto& c : counters_) c.job_hashes = 0; }
    void observe(const std::vector<DeviceInfo>& devices, const std::vector<uint64_t>& hashes)
    {
        if (devices.size() != hashes.size()) throw std::runtime_error("invalid GPU device counters");
        for (size_t i = 0; i < devices.size(); ++i) {
            const auto key = device_identity(devices[i]);
            auto it = std::find_if(counters_.begin(), counters_.end(), [&](const Counter& c) {
                return device_identity(c.device) == key;
            });
            if (it == counters_.end()) {
                counters_.push_back({devices[i], 0, 0});
                it = counters_.end() - 1;
            }
            if (hashes[i] < it->job_hashes) throw std::runtime_error("GPU job counters decreased");
            it->hashes += hashes[i] - it->job_hashes;
            it->job_hashes = hashes[i];
            it->device = devices[i];
        }
    }
    uint64_t hashes() const { uint64_t n = 0; for (const auto& c : counters_) n += c.hashes; return n; }
    Json snapshot(Clock::time_point now = Clock::now()) const
    {
        const auto ms = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count());
        auto rate = [ms](uint64_t n) { return ms > 0 ? n * 1000.0 / ms : 0.0; };
        Json devices = Json::array();
        for (const auto& c : counters_) {
            auto d = device_json(c.device);
            d["hashes"] = c.hashes;
            d["hashrate"] = rate(c.hashes);
            devices.push_back(std::move(d));
        }
        return {{"devices", devices}, {"hashes", hashes()}, {"hashrate", rate(hashes())}, {"elapsed_ms", ms}};
    }
};

enum class State { Starting, Running, Retrying, Stopping, Stopped, Error };
inline const char* state_name(State state)
{
    switch (state) {
    case State::Starting: return "Starting";
    case State::Running: return "Running";
    case State::Retrying: return "Retrying";
    case State::Stopping: return "Stopping";
    case State::Stopped: return "Stopped";
    default: return "Error";
    }
}
inline Json state_event(State state, const std::string& backend, const std::string& message = {})
{
    return {{"event", "state"}, {"name", state_name(state)}, {"backend", backend}, {"message", message}};
}

enum class Failure { Transient, Auth, Device, Corrupt, Rpc, Configuration };
inline const char* failure_name(Failure f)
{
    switch (f) {
    case Failure::Transient: return "transient";
    case Failure::Auth: return "auth";
    case Failure::Device: return "device";
    case Failure::Corrupt: return "corrupt";
    case Failure::Rpc: return "rpc";
    default: return "configuration";
    }
}
struct FailureError : std::runtime_error {
    Failure category;
    FailureError(Failure f, const std::string& message) : std::runtime_error(message), category(f) {}
};
struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("cancelled") {} };

inline Failure http_failure(int status)
{
    if (status == 401 || status == 403) return Failure::Auth;
    if (status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504)
        return Failure::Transient;
    return Failure::Rpc;
}

inline Json decode_rpc_result(int status, const std::string& body, const std::string& method)
{
    if ((status < 200 || status >= 300) && status != 500)
        throw FailureError(http_failure(status), "RPC HTTP error: " + std::to_string(status));
    try {
        const Json response = Json::parse(body);
        if (!response.is_object()) throw FailureError(Failure::Corrupt, "invalid RPC response object");
        if (response.contains("error") && !response["error"].is_null()) {
            if (!response["error"].is_object()) throw FailureError(Failure::Corrupt, "invalid RPC error");
            const int code = response["error"].value("code", 0);
            const auto category = code == -28 || code == -9 || code == -10 ? Failure::Transient
                : code == -2 || code == -13 || code == -14 ? Failure::Auth : Failure::Rpc;
            throw FailureError(category, method + " failed: " + response["error"].dump());
        }
        if (status == 500) throw FailureError(Failure::Transient, "RPC HTTP error: 500");
        if (!response.contains("result")) throw FailureError(Failure::Corrupt, "RPC response missing result");
        return response.at("result");
    } catch (const Json::exception&) {
        throw FailureError(Failure::Corrupt, "invalid RPC JSON response");
    }
}

class RetryPolicy {
    unsigned attempt_ = 0;
public:
    unsigned attempt() const { return attempt_; }
    int next_delay_ms() {
        const unsigned n = std::min(attempt_, 5u);
        if (attempt_ != (std::numeric_limits<unsigned>::max)()) ++attempt_;
        return std::min(30, 1 << n) * 1000;
    }
    void reset() { attempt_ = 0; }
};

template<class Cancel, class Now, class Sleep>
bool wait_cancelable(int delay_ms, Cancel cancel, Now now, Sleep sleep)
{
    const auto deadline = now() + std::chrono::milliseconds(delay_ms);
    while (!cancel()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now());
        if (remaining.count() <= 0) return true;
        sleep(std::min(remaining, std::chrono::milliseconds(50)));
    }
    return false;
}

template<class Operation, class Cancel, class Retrying, class Wait>
auto retry_rpc(Operation operation, bool continuous, Cancel cancel, Retrying retrying, Wait wait)
    -> decltype(operation())
{
    RetryPolicy retry;
    for (;;) {
        if (cancel()) throw Cancelled();
        try { return operation(); }
        catch (const FailureError& error) {
            if (!continuous || error.category != Failure::Transient) throw;
            const int delay = retry.next_delay_ms();
            retrying(error, retry.attempt(), delay);
            if (!wait(delay)) throw Cancelled();
        }
    }
}

} // namespace geocoin::tools::gpu_miner
