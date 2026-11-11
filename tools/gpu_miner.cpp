#include "gpu_miner_common.hpp"

#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/types.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/secret_file.hpp"
#include "nlohmann_json.hpp"

#include "http_post.hpp"

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

json rpc_call(const Options& opts, const std::string& method, const json& params = json::array())
{
    json payload = {
        {"jsonrpc", "2.0"},
        {"id", method},
        {"method", method},
        {"params", params},
    };
    const auto http = geocoin::tools::http_post(
        opts.url, opts.rpc_user, opts.rpc_pass, "application/json", payload.dump(), 30);
    if (http.status < 200 || http.status >= 300) {
        throw std::runtime_error("RPC HTTP error: " + std::to_string(http.status));
    }

    json response = json::parse(http.body);
    if (response.contains("error")) {
        throw std::runtime_error(method + " failed: " + response["error"].dump());
    }
    return response["result"];
}

geocoin::bytes parse_hex_bytes_or_throw(const std::string& hex, const std::string& label)
{
    geocoin::bytes out;
    if (!geocoin::utils::hex_to_bytes_checked(hex, out)) {
        throw std::runtime_error("invalid hex in " + label);
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
        throw std::runtime_error("no CUDA, HIP, or OpenCL GPU devices available");
    }
    return requested;
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
            out.push_back({
                {"backend", gm::backend_name(d.backend)},
                {"id", d.id},
                {"name", d.name},
                {"vendor", d.vendor},
                {"detail", d.detail},
                {"available", d.available},
            });
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

json device_stats_json(const std::vector<gm::DeviceInfo>& mining_devices,
                       const std::vector<uint64_t>& mining_hashes,
                       int64_t elapsed_ms)
{
    json devices = json::array();
    const size_t count = std::min(mining_devices.size(), mining_hashes.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& device = mining_devices[i];
        const uint64_t hashes = mining_hashes[i];
        const double hashrate = elapsed_ms > 0
            ? (static_cast<double>(hashes) * 1000.0 / static_cast<double>(elapsed_ms))
            : 0.0;
        devices.push_back({
            {"backend", gm::backend_name(device.backend)},
            {"id", device.id},
            {"name", device.name},
            {"vendor", device.vendor},
            {"hashes", hashes},
            {"hashrate", hashrate},
        });
    }
    return devices;
}

json device_stats_json(const gm::MiningResult& mining, int64_t elapsed_ms)
{
    return device_stats_json(mining.devices, mining.device_hashes, elapsed_ms);
}

bool is_timeout_error(const std::string& error)
{
    return error.find("timeout") != std::string::npos;
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

int main(int argc, char** argv)
{
    try {
        const Options opts = parse_options(argc, argv);

        if (opts.list_devices) {
            print_devices(opts.stats_json);
            return 0;
        }

        const gm::Backend backend = resolve_backend(opts.backend);

        if (opts.benchmark) {
            gm::MiningJob job;
            job.block_bytes = make_benchmark_header();
            job.bits = 0x01000000u; // Zero target: benchmark runs until timeout without accepting a block.
            job.intensity = opts.intensity;
            job.timeout_sec = opts.timeout_sec;
            job.max_launch_attempts = opts.max_launch_attempts;
            if (opts.stats_json && job.max_launch_attempts == 0) {
                job.max_launch_attempts = kDefaultStatsLaunchAttempts;
            }

            emit_json(opts.stats_json, {
                {"event", "benchmark_start"},
                {"backend", gm::backend_name(backend)},
                {"devices", opts.devices},
                {"intensity", opts.intensity},
                {"timeout", opts.timeout_sec},
            });

            const auto start = std::chrono::steady_clock::now();
            gm::MiningResult mining = mine_with_backend(job, backend, opts.devices);
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            const double hashrate = elapsed_ms > 0
                ? (static_cast<double>(mining.hashes) * 1000.0 / static_cast<double>(elapsed_ms))
                : 0.0;

            emit_json(opts.stats_json, {
                {"event", "benchmark_result"},
                {"backend", mining.backend},
                {"devices", device_stats_json(mining, elapsed_ms)},
                {"hashrate", hashrate},
                {"hashes", mining.hashes},
                {"elapsed_ms", elapsed_ms},
                {"error", mining.error},
            });
            if (!opts.stats_json) {
                std::cout << "benchmark backend=" << mining.backend
                          << " hashes=" << mining.hashes
                          << " elapsed_ms=" << elapsed_ms
                          << " hashrate=" << static_cast<uint64_t>(hashrate)
                          << " H/s\n";
            }
            if (!mining.error.empty() && mining.error.find("timeout") == std::string::npos) {
                throw std::runtime_error(mining.error);
            }
            return 0;
        }

        emit_json(opts.stats_json, {
            {"event", "start"},
            {"backend", gm::backend_name(backend)},
            {"devices", opts.devices},
            {"intensity", opts.intensity},
        });

        json mined = json::array();
        int accepted = 0;
        int rejected = 0;
        for (int i = 0; i < opts.blocks; ++i) {
            const auto block_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opts.timeout_sec);
            const auto block_start = std::chrono::steady_clock::now();
            uint64_t block_hashes = 0;
            bool accepted_this_block = false;

            while (!accepted_this_block) {
                const int remaining_sec = remaining_seconds_until(block_deadline);
                if (remaining_sec <= 0) {
                    throw std::runtime_error("GPU mining timeout after " + std::to_string(opts.timeout_sec) +
                                             "s, hashes=" + std::to_string(block_hashes));
                }

                json template_params = json::array();
                if (!opts.address.empty()) {
                    template_params.push_back(json{{"address", opts.address}});
                }
                const json tpl = rpc_call(opts, "getblocktemplate", template_params);
                emit_json(opts.stats_json, {
                    {"event", "template"},
                    {"backend", gm::backend_name(backend)},
                    {"height", tpl.value("height", 0)},
                    {"previousblockhash", tpl.value("previousblockhash", "")},
                });

                gm::MiningJob job;
                const uint64_t job_height = tpl.value("height", uint64_t{0});
                job.bits = tpl.at("bits").get<uint32_t>();
                job.block_bytes = parse_hex_bytes_or_throw(
                    tpl.at("blockhex").get<std::string>(),
                    "getblocktemplate.blockhex"
                );
                job.intensity = opts.intensity;
                job.timeout_sec = std::min(remaining_sec, kTemplateRefreshSeconds);
                job.max_launch_attempts = opts.max_launch_attempts;
                if (opts.stats_json && job.max_launch_attempts == 0) {
                    job.max_launch_attempts = kDefaultStatsLaunchAttempts;
                }

                const auto attempt_start = std::chrono::steady_clock::now();
                if (opts.stats_json) {
                    job.progress_callback = [&](const std::string& progress_backend,
                                                uint64_t progress_hashes,
                                                const std::vector<gm::DeviceInfo>& progress_devices,
                                                const std::vector<uint64_t>& progress_device_hashes) {
                        const auto progress_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - block_start).count();
                        const uint64_t total_progress_hashes = block_hashes + progress_hashes;
                        const double progress_hashrate = progress_elapsed_ms > 0
                            ? (static_cast<double>(total_progress_hashes) * 1000.0 / static_cast<double>(progress_elapsed_ms))
                            : 0.0;
                        emit_json(true, {
                            {"event", "status"},
                            {"backend", progress_backend},
                            {"devices", device_stats_json(progress_devices, progress_device_hashes, progress_elapsed_ms)},
                            {"hashrate", progress_hashrate},
                            {"hashes", total_progress_hashes},
                            {"accepted", accepted},
                            {"rejected", rejected},
                            {"elapsed_ms", progress_elapsed_ms},
                        });
                    };
                }
                gm::MiningResult mining = mine_with_backend(job, backend, opts.devices);
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - block_start).count();
                block_hashes += mining.hashes;

                const double hashrate = elapsed_ms > 0
                    ? (static_cast<double>(block_hashes) * 1000.0 / static_cast<double>(elapsed_ms))
                    : 0.0;

                emit_json(opts.stats_json, {
                    {"event", "status"},
                    {"backend", mining.backend},
                    {"devices", device_stats_json(mining, std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - attempt_start).count())},
                    {"hashrate", hashrate},
                    {"hashes", block_hashes},
                    {"accepted", accepted},
                    {"rejected", rejected},
                    {"elapsed_ms", elapsed_ms},
                });

                if (!mining.found) {
                    if (is_timeout_error(mining.error)) {
                        continue;
                    }
                    throw std::runtime_error(mining.error.empty() ? "GPU mining failed" : mining.error);
                }

                geocoin::utils::write_u64_le(job.block_bytes, 116, mining.nonce);
                emit_json(opts.stats_json, {
                    {"event", "block_found"},
                    {"backend", mining.backend},
                    {"device", mining.device_id},
                    {"nonce", mining.nonce},
                    {"hash", geocoin::utils::hash256_to_hex(mining.hash)},
                    {"hashrate", hashrate},
                });

                const json submit_result = rpc_call(opts, "submitblock", json::array({geocoin::utils::bytes_to_hex(job.block_bytes)}));
                if (!submit_result.is_null()) {
                    ++rejected;
                    emit_json(opts.stats_json, {
                        {"event", "block_rejected"},
                        {"backend", mining.backend},
                        {"reason", submit_result.dump()},
                        {"accepted", accepted},
                        {"rejected", rejected},
                    });
                    if (is_stale_submit_reject(submit_result)) {
                        continue;
                    }
                    throw std::runtime_error("submitblock rejected block: " + submit_result.dump());
                }

                ++accepted;
                emit_json(opts.stats_json, {
                    {"event", "block_accepted"},
                    {"backend", mining.backend},
                    {"height", job_height},
                    {"hash", geocoin::utils::hash256_to_hex(mining.hash)},
                    {"accepted", accepted},
                    {"rejected", rejected},
                });
                mined.push_back({
                    {"height", job_height},
                    {"backend", mining.backend},
                    {"device", mining.device_id},
                    {"nonce", mining.nonce},
                    {"hash", geocoin::utils::hash256_to_hex(mining.hash)},
                    {"hashes", block_hashes},
                    {"elapsed_ms", elapsed_ms},
                });
                accepted_this_block = true;
            }
        }

        if (!opts.stats_json) {
            std::cout << json{{"result", "ok"}, {"blocks", mined}}.dump(2) << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        json event = {{"event", "error"}, {"message", e.what()}};
        std::cerr << "geocoin-gpu-miner error: " << e.what() << "\n";
        std::cout << event.dump() << "\n";
        return 1;
    }
}
