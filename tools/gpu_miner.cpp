#include "gpu_miner_common.hpp"

#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/types.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/secret_file.hpp"
#include "nlohmann_json.hpp"

#include "gpu_miner_rpc.hpp"
#include "gpu_miner_control.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace gm = geocoin::tools::gpu_miner;

namespace {

constexpr uint64_t kDefaultStatsLaunchAttempts = 134217728ull;
constexpr int kTemplateRefreshSeconds = 10;

struct Options {
    std::string url = "http://127.0.0.1:19445/";
    std::string rpc_user = "smoke";
    std::string rpc_pass;
    std::string rpc_pass_file;
    std::string address;
    gm::Backend backend = gm::Backend::Auto;
    std::vector<int> devices;
    uint32_t intensity = 1073741824;
    int blocks = 1;
    int timeout_sec = 1800;
    uint64_t max_launch_attempts = 0;
    bool stats_json = false;
    bool list_devices = false;
    bool benchmark = false;
    bool continuous = false;
    bool control_stdin = false;
};

void print_usage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " --url <rpc-url> --rpc-user <user> (--rpc-pass <pass>|--rpc-pass-file <path>) [options]\n"
        << "       " << argv0 << " --benchmark [--backend auto|cuda|hip|opencl] [--devices ids] [--timeout seconds]\n"
        << "Use RPC credentials configured with geocoin-node --rpc-miner-user/--rpc-miner-pass-file.\n"
        << "Options:\n"
        << "  --address <address>       Mining reward address for getblocktemplate\n"
        << "  --backend auto|cuda|hip|opencl GPU backend (default: auto)\n"
        << "  --devices <ids>           Comma-separated backend-local GPU ids\n"
        << "  --intensity <n>           Kernel attempts per launch per GPU (default: 1073741824)\n"
        << "  --max-launch-attempts <n> Cap attempts per single GPU launch; 0 keeps intensity as-is\n"
        << "  --blocks <n>              Number of accepted blocks to mine (default: 1)\n"
        << "  --timeout <seconds>       Per-block timeout (default: 1800)\n"
        << "  --benchmark               Run backend throughput benchmark without RPC\n"
        << "  --continuous              Mine until stopped; retry transient RPC failures\n"
        << "  --control-stdin           Stop on stdin stop/cancel line or EOF\n"
        << "  --stats-json              Print JSONL stats/events for GUI integration\n"
        << "  --list-devices            List CUDA/HIP/OpenCL devices and exit\n";
}

std::string read_first_line(const std::string& path)
{
    return geocoin::utils::read_secret_file(path, "RPC password");
}

std::vector<int> parse_devices(const std::string& value)
{
    std::vector<int> out;
    std::stringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        out.push_back(std::stoi(token));
    }
    return out;
}

Options parse_options(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + flag);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--url") {
            opts.url = need_value("--url");
        } else if (arg == "--rpc-user") {
            opts.rpc_user = need_value("--rpc-user");
        } else if (arg == "--rpc-pass") {
            opts.rpc_pass = need_value("--rpc-pass");
        } else if (arg == "--rpc-pass-file") {
            opts.rpc_pass_file = need_value("--rpc-pass-file");
        } else if (arg == "--address") {
            opts.address = need_value("--address");
        } else if (arg == "--backend") {
            opts.backend = gm::parse_backend(need_value("--backend"));
        } else if (arg == "--devices") {
            opts.devices = parse_devices(need_value("--devices"));
        } else if (arg == "--intensity") {
            opts.intensity = static_cast<uint32_t>(std::stoul(need_value("--intensity")));
        } else if (arg == "--max-launch-attempts") {
            opts.max_launch_attempts = static_cast<uint64_t>(std::stoull(need_value("--max-launch-attempts")));
        } else if (arg == "--blocks") {
            opts.blocks = std::stoi(need_value("--blocks"));
        } else if (arg == "--timeout") {
            opts.timeout_sec = std::stoi(need_value("--timeout"));
        } else if (arg == "--benchmark") {
            opts.benchmark = true;
        } else if (arg == "--continuous") {
            opts.continuous = true;
        } else if (arg == "--control-stdin") {
            opts.control_stdin = true;
        } else if (arg == "--stats-json") {
            opts.stats_json = true;
        } else if (arg == "--list-devices") {
            opts.list_devices = true;
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    if (!opts.rpc_pass_file.empty()) {
        opts.rpc_pass = read_first_line(opts.rpc_pass_file);
    }
    if (!opts.list_devices && !opts.benchmark && opts.rpc_pass.empty()) {
        throw std::runtime_error("RPC password is required");
    }
    if (opts.blocks < 1) {
        throw std::runtime_error("--blocks must be >= 1");
    }
    if (opts.timeout_sec < 1) {
        throw std::runtime_error("--timeout must be >= 1");
    }
    if (opts.continuous && (opts.benchmark || opts.list_devices)) {
        throw std::runtime_error("--continuous cannot be combined with --benchmark or --list-devices");
    }
    if (opts.intensity < 1024) {
        opts.intensity = 1024;
    }
    if (opts.max_launch_attempts > 0 && opts.max_launch_attempts < 1024) {
        opts.max_launch_attempts = 1024;
    }
    return opts;
}

std::vector<geocoin::byte> make_benchmark_header()
{
    std::vector<geocoin::byte> header(124);
    for (size_t i = 0; i < header.size(); ++i) {
        header[i] = static_cast<geocoin::byte>((i * 131u + 17u) & 0xffu);
    }
    return header;
}

json rpc_call(const Options& opts, const std::string& method, const json& params,
              const std::function<bool()>& cancel)
{
    json payload = {
        {"jsonrpc", "2.0"},
        {"id", method},
        {"method", method},
        {"params", params},
    };
    const auto http = gm::cancellable_http_post(opts.url, opts.rpc_user, opts.rpc_pass, payload.dump(), cancel);
    return gm::decode_rpc_result(http.status, http.body, method);
}

geocoin::bytes parse_hex_bytes_or_throw(const std::string& hex, const std::string& label)
{
    geocoin::bytes out;
    if (!geocoin::utils::hex_to_bytes_checked(hex, out)) {
        throw gm::FailureError(gm::Failure::Corrupt, "invalid hex in " + label);
    }
    return out;
}

gm::Backend resolve_backend(gm::Backend requested)
{
    if (requested == gm::Backend::Auto) {
        if (!gm::list_cuda_devices().empty()) {
            return gm::Backend::Cuda;
        }
        if (!gm::list_hip_devices().empty()) {
            return gm::Backend::Hip;
        }
        if (!gm::list_opencl_devices().empty()) {
            return gm::Backend::OpenCl;
        }
        throw gm::FailureError(gm::Failure::Device, "no CUDA, HIP, or OpenCL GPU devices available");
    }
    return requested;
}

void validate_devices(gm::Backend backend, const std::vector<int>& selected)
{
    const auto devices = backend == gm::Backend::Cuda ? gm::list_cuda_devices()
        : backend == gm::Backend::Hip ? gm::list_hip_devices() : gm::list_opencl_devices();
    if (devices.empty()) throw gm::FailureError(gm::Failure::Device, "no devices available for selected GPU backend");
    std::vector<int> seen;
    for (int id : selected) {
        if (std::find(seen.begin(), seen.end(), id) != seen.end() ||
            std::none_of(devices.begin(), devices.end(), [id](const gm::DeviceInfo& d) { return d.id == id && d.available; }))
            throw gm::FailureError(gm::Failure::Device, "invalid or duplicate GPU device id: " + std::to_string(id));
        seen.push_back(id);
    }
}

std::vector<gm::DeviceInfo> list_all_devices()
{
    std::vector<gm::DeviceInfo> out = gm::list_cuda_devices();
    auto hip = gm::list_hip_devices();
    out.insert(out.end(), hip.begin(), hip.end());
    auto opencl = gm::list_opencl_devices();
    out.insert(out.end(), opencl.begin(), opencl.end());
    return out;
}

void emit_json(bool enabled, const json& event)
{
    if (enabled) {
        std::cout << event.dump() << "\n";
        std::cout.flush();
    }
}

void print_devices(bool json_output)
{
    const auto devices = list_all_devices();
    if (json_output) {
        json out = json::array();
        for (const auto& d : devices) {
            out.push_back(gm::device_json(d));
        }
        std::cout << json{{"event", "devices"}, {"devices", out}}.dump() << "\n";
        return;
    }

    if (devices.empty()) {
        std::cout << "No CUDA/HIP/OpenCL GPU devices found.\n";
        return;
    }
    for (const auto& d : devices) {
        std::cout << gm::backend_name(d.backend) << ":" << d.id << " "
                  << d.vendor << " " << d.name << " " << d.detail << "\n";
    }
}

gm::MiningResult mine_with_backend(const gm::MiningJob& job, gm::Backend backend, const std::vector<int>& devices)
{
    if (backend == gm::Backend::Cuda) {
        return gm::mine_cuda(job, devices);
    }
    if (backend == gm::Backend::Hip) {
        return gm::mine_hip(job, devices);
    }
    if (backend == gm::Backend::OpenCl) {
        return gm::mine_opencl(job, devices);
    }
    throw std::runtime_error("backend auto was not resolved before mining");
}

bool is_stale_submit_reject(const json& reject)
{
    const std::string value = reject.is_string() ? reject.get<std::string>() : reject.dump();
    return value.find("bad-prevblk") != std::string::npos ||
           value.find("stale") != std::string::npos;
}

int remaining_seconds_until(const std::chrono::steady_clock::time_point& deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>((remaining_ms + 999) / 1000);
}

} // namespace

namespace geocoin::tools::gpu_miner {

const char* backend_name(Backend backend)
{
    switch (backend) {
    case Backend::Cuda:
        return "cuda";
    case Backend::Hip:
        return "hip";
    case Backend::OpenCl:
        return "opencl";
    case Backend::Auto:
    default:
        return "auto";
    }
}

Backend parse_backend(const std::string& value)
{
    if (value == "auto") return Backend::Auto;
    if (value == "cuda") return Backend::Cuda;
    if (value == "hip") return Backend::Hip;
    if (value == "opencl") return Backend::OpenCl;
    throw std::runtime_error("unsupported backend: " + value);
}

} // namespace geocoin::tools::gpu_miner

namespace {

using RpcOperation = std::function<json(const std::string&, const json&)>;
using MineOperation = std::function<gm::MiningResult(const gm::MiningJob&)>;

int run_session(const Options& opts, gm::Backend backend, gm::StdinControl& control,
                const RpcOperation& rpc, const MineOperation& mine)
{
    const std::string backend_label = gm::backend_name(backend);
    auto cancel = [&] { return control.cancelled(); };
    gm::State current_state = gm::State::Starting;
    auto state = [&](gm::State value) {
        if (current_state != value) emit_json(opts.stats_json, gm::state_event(value, backend_label));
        current_state = value;
    };
    auto call = [&](const std::string& method, const json& params) {
        auto result = gm::retry_rpc([&] { return rpc(method, params); }, opts.continuous, cancel,
            [&](const gm::FailureError& error, unsigned attempt, int delay) {
                auto event = gm::state_event(gm::State::Retrying, backend_label, error.what());
                event["retry_attempt"] = attempt;
                event["retry_delay_ms"] = delay;
                emit_json(opts.stats_json, event);
                current_state = gm::State::Retrying;
            }, [&](int delay) {
                return gm::wait_cancelable(delay, cancel, [] { return gm::Clock::now(); },
                    [](std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); });
            });
        if (cancel()) throw gm::Cancelled();
        state(gm::State::Running);
        return result;
    };
    gm::SessionStats stats;
    int accepted = 0;
    int rejected = 0;
    auto status = [&] {
        auto event = stats.snapshot();
        event["event"] = "status";
        event["backend"] = backend_label;
        event["accepted"] = accepted;
        event["rejected"] = rejected;
        emit_json(opts.stats_json, event);
    };
    auto prepare_job = [&](gm::MiningJob& job) {
        stats.begin_job();
        job.intensity = opts.intensity;
        job.max_launch_attempts = opts.max_launch_attempts;
        if (opts.stats_json && !job.max_launch_attempts) job.max_launch_attempts = kDefaultStatsLaunchAttempts;
        if (opts.control_stdin) job.cancel_requested = cancel;
        if (opts.stats_json) {
            job.progress_callback = [&](const std::string&, uint64_t,
                const std::vector<gm::DeviceInfo>& devices, const std::vector<uint64_t>& hashes) {
                stats.observe(devices, hashes);
                status();
            };
        }
    };
    if (opts.benchmark) {
        gm::MiningJob job;
        job.block_bytes = make_benchmark_header();
        job.bits = 0x01000000u;
        job.timeout_sec = opts.timeout_sec;
        prepare_job(job);
        emit_json(opts.stats_json, {{"event", "benchmark_start"}, {"backend", backend_label},
            {"devices", opts.devices}, {"intensity", opts.intensity}, {"timeout", opts.timeout_sec}});
        state(gm::State::Running);
        const auto mining = mine(job);
        stats.observe(mining.devices, mining.device_hashes);
        auto event = stats.snapshot();
        event["event"] = "benchmark_result";
        event["backend"] = mining.backend;
        event["error"] = mining.error;
        emit_json(opts.stats_json, event);
        if (!mining.error.empty() && !mining.timed_out)
            throw gm::FailureError(gm::Failure::Device, mining.error);
        if (mining.cancelled || cancel()) throw gm::Cancelled();
        if (!opts.stats_json) std::cout << "benchmark backend=" << mining.backend << " hashes=" << stats.hashes()
            << " elapsed_ms=" << event["elapsed_ms"] << " hashrate=" << event["hashrate"] << " H/s\n";
        state(gm::State::Stopped);
        return 0;
    }

    emit_json(opts.stats_json, {{"event", "start"}, {"backend", backend_label},
        {"devices", opts.devices}, {"intensity", opts.intensity}});
    json mined = json::array();
    while (opts.continuous || accepted < opts.blocks) {
        const auto block_start = gm::Clock::now();
        const auto block_deadline = block_start + std::chrono::seconds(opts.timeout_sec);
        uint64_t block_hashes = 0;
        bool accepted_this_block = false;
        while (!accepted_this_block) {
            if (cancel()) throw gm::Cancelled();
            const int remaining_sec = opts.continuous ? kTemplateRefreshSeconds : remaining_seconds_until(block_deadline);
            if (remaining_sec <= 0) throw gm::FailureError(gm::Failure::Device,
                "GPU mining timeout after " + std::to_string(opts.timeout_sec) + "s, hashes=" + std::to_string(block_hashes));
            json params = json::array();
            if (!opts.address.empty()) params.push_back(json{{"address", opts.address}});
            const json tpl = call("getblocktemplate", params);
            gm::MiningJob job;
            uint64_t height = 0;
            try {
                if (!tpl.is_object() || !tpl.at("bits").is_number_unsigned() ||
                    tpl.at("bits").get<uint64_t>() > UINT32_MAX)
                    throw gm::FailureError(gm::Failure::Corrupt, "invalid getblocktemplate bits");
                height = tpl.value("height", uint64_t{0});
                job.bits = tpl.at("bits").get<uint32_t>();
                job.block_bytes = parse_hex_bytes_or_throw(tpl.at("blockhex").get<std::string>(), "getblocktemplate.blockhex");
                if (job.block_bytes.size() < 124) throw gm::FailureError(gm::Failure::Corrupt, "short getblocktemplate header");
                emit_json(opts.stats_json, {{"event", "template"}, {"backend", backend_label},
                    {"height", height}, {"previousblockhash", tpl.value("previousblockhash", "")}});
            } catch (const json::exception&) {
                throw gm::FailureError(gm::Failure::Corrupt, "invalid getblocktemplate response");
            }
            job.timeout_sec = std::min(remaining_sec, kTemplateRefreshSeconds);
            prepare_job(job);
            if (cancel()) throw gm::Cancelled();
            const auto mining = mine(job);
            stats.observe(mining.devices, mining.device_hashes);
            block_hashes += mining.hashes;
            status();
            if (!mining.error.empty() && !mining.timed_out)
                throw gm::FailureError(gm::Failure::Device, mining.error);
            if (mining.cancelled || cancel()) throw gm::Cancelled();
            if (!mining.found) {
                if (mining.timed_out) continue;
                throw gm::FailureError(gm::Failure::Device, "GPU mining failed");
            }
            geocoin::utils::write_u64_le(job.block_bytes, 116, mining.nonce);
            emit_json(opts.stats_json, {{"event", "block_found"}, {"backend", mining.backend},
                {"device", mining.device_id}, {"nonce", mining.nonce},
                {"hash", geocoin::utils::hash256_to_hex(mining.hash)}, {"hashrate", stats.snapshot()["hashrate"]}});
            const json submitted = call("submitblock", json::array({geocoin::utils::bytes_to_hex(job.block_bytes)}));
            if (!submitted.is_null() && !submitted.is_string())
                throw gm::FailureError(gm::Failure::Corrupt, "invalid submitblock response");
            if (!submitted.is_null()) {
                ++rejected;
                emit_json(opts.stats_json, {{"event", "block_rejected"}, {"backend", mining.backend},
                    {"reason", submitted.dump()}, {"accepted", accepted}, {"rejected", rejected}});
                if (is_stale_submit_reject(submitted)) continue;
                throw gm::FailureError(gm::Failure::Rpc, "submitblock rejected block: " + submitted.dump());
            }
            ++accepted;
            emit_json(opts.stats_json, {{"event", "block_accepted"}, {"backend", mining.backend}, {"height", height},
                {"hash", geocoin::utils::hash256_to_hex(mining.hash)}, {"accepted", accepted}, {"rejected", rejected}});
            if (!opts.continuous) mined.push_back({{"height", height}, {"backend", mining.backend},
                {"device", mining.device_id}, {"nonce", mining.nonce}, {"hash", geocoin::utils::hash256_to_hex(mining.hash)},
                {"hashes", block_hashes}, {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(gm::Clock::now() - block_start).count()}});
            accepted_this_block = true;
        }
    }
    if (!opts.stats_json) std::cout << json{{"result", "ok"}, {"blocks", mined}}.dump(2) << "\n";
    state(gm::State::Stopped);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    bool json_output = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--stats-json") json_output = true;
    std::string backend_label = "auto";
    try {
        const Options opts = parse_options(argc, argv);

        if (opts.list_devices) {
            print_devices(opts.stats_json);
            return 0;
        }

        backend_label = gm::backend_name(opts.backend);
        emit_json(opts.stats_json, gm::state_event(gm::State::Starting, backend_label));
        gm::StdinControl control(opts.control_stdin);
        if (control.cancelled()) throw gm::Cancelled();
        const gm::Backend backend = resolve_backend(opts.backend);
        backend_label = gm::backend_name(backend);
        validate_devices(backend, opts.devices);
        return run_session(opts, backend, control,
            [&](const std::string& method, const json& params) {
                return rpc_call(opts, method, params, [&] { return control.cancelled(); });
            }, [&](const gm::MiningJob& job) { return mine_with_backend(job, backend, opts.devices); });
    } catch (const gm::Cancelled&) {
        emit_json(json_output, gm::state_event(gm::State::Stopping, backend_label));
        emit_json(json_output, gm::state_event(gm::State::Stopped, backend_label));
        return 0;
    } catch (const std::exception& e) {
        json event = {{"event", "error"}, {"message", e.what()}};
        const auto* failure = dynamic_cast<const gm::FailureError*>(&e);
        const std::string category = gm::failure_name(failure ? failure->category : gm::Failure::Configuration);
        event["category"] = category;
        auto state = gm::state_event(gm::State::Error, backend_label, e.what());
        state["category"] = category;
        emit_json(json_output, state);
        std::cerr << "geocoin-gpu-miner error: " << e.what() << "\n";
        std::cout << event.dump() << "\n";
        return 1;
    }
}
