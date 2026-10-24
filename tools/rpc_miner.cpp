#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/types.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/secret_file.hpp"
#define IOKE_SHA256_NO_OPENSSL_COMPAT_NAMES
#include "geocoin/crypto/ioke_sha256.h"
#include "nlohmann_json.hpp"

#include "http_post.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr int kTemplateRefreshSeconds = 10;

struct Options {
    std::string url = "http://127.0.0.1:19445/";
    std::string rpc_user = "smoke";
    std::string rpc_pass;
    std::string rpc_pass_file;
    std::string address;
    int blocks = 1;
    int threads = 0;
    int timeout_sec = 1800;
    std::optional<uint32_t> timestamp;
    uint32_t timestamp_step = 0;
};

struct MiningResult {
    bool found = false;
    uint64_t nonce = 0;
    geocoin::Hash256 hash{};
    uint64_t hashes = 0;
    std::string error;
};

void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " --url <rpc-url> --rpc-user <user> (--rpc-pass <pass>|--rpc-pass-file <path>) [options]\n"
        << "Use RPC credentials configured with geocoin-node --rpc-miner-user/--rpc-miner-pass-file.\n"
        << "Options:\n"
        << "  --address <address>   Mining reward address for getblocktemplate\n"
        << "  --blocks <n>          Number of blocks to mine (default: 1)\n"
        << "  --threads <n>         Mining threads (default: hardware concurrency)\n"
        << "  --timeout <seconds>   Per-block timeout (default: 1800)\n"
        << "  --timestamp <unix>    Timestamp for the first block template\n"
        << "  --timestamp-step <s>  Increment timestamp after each accepted block\n";
}

std::string read_first_line(const std::string& path) {
    return geocoin::utils::read_secret_file(path, "RPC password");
}

Options parse_options(int argc, char** argv) {
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
        } else if (arg == "--blocks") {
            opts.blocks = std::stoi(need_value("--blocks"));
        } else if (arg == "--threads") {
            opts.threads = std::stoi(need_value("--threads"));
        } else if (arg == "--timeout") {
            opts.timeout_sec = std::stoi(need_value("--timeout"));
        } else if (arg == "--timestamp") {
            const uint64_t value = std::stoull(need_value("--timestamp"));
            if (value > std::numeric_limits<uint32_t>::max()) {
                throw std::runtime_error("--timestamp exceeds uint32 range");
            }
            opts.timestamp = static_cast<uint32_t>(value);
        } else if (arg == "--timestamp-step") {
            const uint64_t value = std::stoull(need_value("--timestamp-step"));
            if (value > std::numeric_limits<uint32_t>::max()) {
                throw std::runtime_error("--timestamp-step exceeds uint32 range");
            }
            opts.timestamp_step = static_cast<uint32_t>(value);
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    if (!opts.rpc_pass_file.empty()) {
        opts.rpc_pass = read_first_line(opts.rpc_pass_file);
    }
    if (opts.rpc_pass.empty()) {
        throw std::runtime_error("RPC password is required");
    }
    if (opts.blocks < 1) {
        throw std::runtime_error("--blocks must be >= 1");
    }
    if (opts.timeout_sec < 1) {
        throw std::runtime_error("--timeout must be >= 1");
    }
    if (opts.timestamp_step != 0 && !opts.timestamp.has_value()) {
        throw std::runtime_error("--timestamp-step requires --timestamp");
    }
    if (opts.threads < 1) {
        opts.threads = static_cast<int>(std::thread::hardware_concurrency());
        if (opts.threads < 1) {
            opts.threads = 1;
        }
    }
    return opts;
}

json rpc_call(const Options& opts, const std::string& method, const json& params = json::array()) {
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

geocoin::bytes parse_hex_bytes_or_throw(const std::string& hex, const std::string& label) {
    geocoin::bytes out;
    if (!geocoin::utils::hex_to_bytes_checked(hex, out)) {
        throw std::runtime_error("invalid hex in " + label);
    }
    return out;
}

bool hash_meets_target(const geocoin::Hash256& hash, const geocoin::uint256& target) {
    for (int i = 3; i >= 0; --i) {
        const int hash_offset = (3 - i) * 8;
        uint64_t hash_part = 0;
        for (int j = 0; j < 8; ++j) {
            hash_part = (hash_part << 8) | static_cast<uint64_t>(hash[hash_offset + j]);
        }
        if (hash_part < target.data[i]) return true;
        if (hash_part > target.data[i]) return false;
    }
    return true;
}

geocoin::uint256 bits_to_target(uint32_t bits) {
    const uint8_t exponent = static_cast<uint8_t>(bits >> 24);
    uint32_t mantissa = bits & 0x007fffff;
    const bool negative = (bits & 0x00800000) != 0;

    geocoin::uint256 target;
    target.data[0] = 0;
    target.data[1] = 0;
    target.data[2] = 0;
    target.data[3] = 0;

    if (exponent <= 3) {
        mantissa >>= 8 * (3 - exponent);
        target.data[0] = mantissa;
    } else {
        const int shift = 8 * (exponent - 3);
        const int word_index = shift / 64;
        const int bit_shift = shift % 64;

        if (word_index < 4) {
            target.data[word_index] |= static_cast<uint64_t>(mantissa) << bit_shift;
        }
        if (bit_shift > 64 - 24 && word_index + 1 < 4) {
            target.data[word_index + 1] |= static_cast<uint64_t>(mantissa) >> (64 - bit_shift);
        }
    }

    if (negative) {
        target.data[0] = target.data[1] = target.data[2] = target.data[3] = 0;
    }
    return target;
}

int remaining_seconds_until(const std::chrono::steady_clock::time_point& deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>((remaining_ms + 999) / 1000);
}

// 124-byte PoW header: the first 64 bytes never change inside one template,
// and the 8-byte nonce sits in the second compression block. Precompute that
// midstate and the fixed SHA-256 padding so each nonce is three compressions
// instead of two full digest calls.
struct HeaderPow {
    static constexpr size_t kHeaderSize = 124;
    static constexpr size_t kNonceOffset = 116;

    uint32_t midstate[8]{};
    uint8_t chunk[64]{};
    uint8_t length_block[64]{};
    alignas(16) uint8_t length_rounds[512]{};
    bool have_sha_ni = false;

    static HeaderPow prepare(const uint8_t* header) {
        HeaderPow pow;
        ioke_sha256_init_state(pow.midstate);
        ioke_sha256_compress(pow.midstate, header, 1);

        std::memcpy(pow.chunk, header + 64, 60);
        pow.chunk[60] = 0x80;

        // 124 bytes * 8 = 992 bits, stored big-endian in the final block.
        pow.length_block[62] = 0x03;
        pow.length_block[63] = 0xe0;
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
        pow.have_sha_ni = ioke_sha256_backend_available(IOKE_SHA256_BACKEND_NATIVE) != 0;
#endif
        if (pow.have_sha_ni) {
            ioke_sha256_expand_rounds_x86_sha_ni(pow.length_block, pow.length_rounds);
        }
        return pow;
    }

    void hash(uint64_t nonce, uint8_t out[32]) const {
        uint8_t block[64];
        std::memcpy(block, chunk, sizeof(block));
        for (int i = 0; i < 8; ++i) {
            block[52 + i] = static_cast<uint8_t>(nonce >> (8 * i));
        }

        uint32_t state[8];
        std::memcpy(state, midstate, sizeof(state));
        ioke_sha256_compress(state, block, 1);
        ioke_sha256_compress(state, length_block, 1);

        uint8_t first[64]{};
        ioke_sha256_write_hash(first, state);
        first[32] = 0x80;
        first[62] = 0x01;  // 32 bytes * 8 = 256 bits

        ioke_sha256_init_state(state);
        ioke_sha256_compress(state, first, 1);
        ioke_sha256_write_hash(out, state);
    }

    void hash_pair(uint64_t nonce_a, uint64_t nonce_b, uint8_t out_a[32], uint8_t out_b[32]) const {
        if (!have_sha_ni) {
            hash(nonce_a, out_a);
            hash(nonce_b, out_b);
            return;
        }

        uint8_t block_a[64];
        uint8_t block_b[64];
        std::memcpy(block_a, chunk, sizeof(block_a));
        std::memcpy(block_b, chunk, sizeof(block_b));
        for (int i = 0; i < 8; ++i) {
            block_a[52 + i] = static_cast<uint8_t>(nonce_a >> (8 * i));
            block_b[52 + i] = static_cast<uint8_t>(nonce_b >> (8 * i));
        }

        uint32_t state_a[8];
        uint32_t state_b[8];
        std::memcpy(state_a, midstate, sizeof(state_a));
        std::memcpy(state_b, midstate, sizeof(state_b));
        ioke_sha256_compress2_x86_sha_ni(state_a, block_a, state_b, block_b, 1);
        ioke_sha256_compress2_scheduled_x86_sha_ni(state_a, state_b, length_rounds);

        uint8_t second_a[64]{};
        uint8_t second_b[64]{};
        ioke_sha256_write_hash(second_a, state_a);
        ioke_sha256_write_hash(second_b, state_b);
        second_a[32] = second_b[32] = 0x80;
        second_a[62] = second_b[62] = 0x01;

        ioke_sha256_init_state(state_a);
        ioke_sha256_init_state(state_b);
        ioke_sha256_compress2_x86_sha_ni(state_a, second_a, state_b, second_b, 1);
        ioke_sha256_write_hash(out_a, state_a);
        ioke_sha256_write_hash(out_b, state_b);
    }

    void hash4(uint64_t n0, uint64_t n1, uint64_t n2, uint64_t n3, uint8_t out[4][32]) const {
        const uint64_t nonces[4] = {n0, n1, n2, n3};
        if (!have_sha_ni) {
            for (int lane = 0; lane < 4; ++lane) {
                hash(nonces[lane], out[lane]);
            }
            return;
        }

        uint8_t block[4][128];
        uint32_t state[4][8];
        for (int lane = 0; lane < 4; ++lane) {
            std::memcpy(block[lane], chunk, 64);
            std::memcpy(block[lane] + 64, length_block, 64);
            for (int i = 0; i < 8; ++i) {
                block[lane][52 + i] = static_cast<uint8_t>(nonces[lane] >> (8 * i));
            }
            std::memcpy(state[lane], midstate, sizeof(state[lane]));
        }
        ioke_sha256_compress4_x86_sha_ni(
            state[0], block[0], state[1], block[1], state[2], block[2], state[3], block[3], 2);

        uint8_t second[4][64]{};
        for (int lane = 0; lane < 4; ++lane) {
            ioke_sha256_write_hash(second[lane], state[lane]);
            second[lane][32] = 0x80;
            second[lane][62] = 0x01;
            ioke_sha256_init_state(state[lane]);
        }
        ioke_sha256_compress4_x86_sha_ni(
            state[0], second[0], state[1], second[1], state[2], second[2], state[3], second[3], 1);
        for (int lane = 0; lane < 4; ++lane) {
            ioke_sha256_write_hash(out[lane], state[lane]);
        }
    }
};

void check_header_pow_matches_double_sha256(const uint8_t* header) {
    const HeaderPow pow = HeaderPow::prepare(header);
    geocoin::bytes slow_header(header, header + HeaderPow::kHeaderSize);
    for (const uint64_t nonce : {0ULL, 1ULL, 0x0102030405060708ULL, 0xffffffffffffffffULL}) {
        uint8_t fast[32];
        pow.hash(nonce, fast);
        for (int i = 0; i < 8; ++i) {
            slow_header[HeaderPow::kNonceOffset + static_cast<size_t>(i)] =
                static_cast<uint8_t>(nonce >> (8 * i));
        }
        const geocoin::Hash256 slow = geocoin::crypto::double_sha256(slow_header);
        if (std::memcmp(fast, slow.data(), 32) != 0) {
            throw std::runtime_error("fast header PoW does not match double SHA-256");
        }
    }
    uint8_t pair_a[32];
    uint8_t pair_b[32];
    pow.hash_pair(1, 2, pair_a, pair_b);
    uint8_t single[32];
    pow.hash(1, single);
    if (std::memcmp(pair_a, single, 32) != 0) {
        throw std::runtime_error("paired header PoW does not match the single fast path");
    }
    pow.hash(2, single);
    if (std::memcmp(pair_b, single, 32) != 0) {
        throw std::runtime_error("paired header PoW does not match the single fast path");
    }
    uint8_t quad[4][32];
    pow.hash4(3, 4, 5, 6, quad);
    for (int lane = 0; lane < 4; ++lane) {
        pow.hash(static_cast<uint64_t>(3 + lane), single);
        if (std::memcmp(quad[lane], single, 32) != 0) {
            throw std::runtime_error("quad header PoW does not match the single fast path");
        }
    }
}

bool is_stale_submit_reject(const json& reject) {
    const std::string value = reject.is_string() ? reject.get<std::string>() : reject.dump();
    return value.find("bad-prevblk") != std::string::npos ||
           value.find("stale") != std::string::npos;
}

MiningResult mine_block_bytes(
    const geocoin::bytes& block_bytes,
    uint32_t bits,
    int threads,
    int timeout_sec
) {
    static constexpr size_t kHeaderSize = 124;
    static constexpr size_t kNonceOffset = 116;
    if (block_bytes.size() < kHeaderSize) {
        throw std::runtime_error("block template is shorter than block header");
    }

    check_header_pow_matches_double_sha256(block_bytes.data());
    const HeaderPow header_pow = HeaderPow::prepare(block_bytes.data());
    const geocoin::uint256 target = bits_to_target(bits);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);

    std::atomic<bool> found{false};
    std::atomic<uint64_t> total_hashes{0};
    MiningResult result;
    std::mutex result_mutex;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads));

    for (int tid = 0; tid < threads; ++tid) {
        workers.emplace_back([&, tid]() {
            uint64_t nonce = static_cast<uint64_t>(tid);
            uint64_t local_hashes = 0;
            const uint64_t stride = static_cast<uint64_t>(threads);
            constexpr uint64_t kCheckEvery = 4096;

            auto consider = [&](uint64_t candidate, const uint8_t digest[32]) {
                ++local_hashes;
                geocoin::Hash256 hash{};
                std::memcpy(hash.data(), digest, 32);
                if (!hash_meets_target(hash, target)) {
                    return false;
                }
                bool expected = false;
                if (found.compare_exchange_strong(expected, true)) {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    result.found = true;
                    result.nonce = candidate;
                    result.hash = hash;
                }
                return true;
            };

            while (!found.load(std::memory_order_relaxed)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                for (uint64_t n = 0; n < kCheckEvery; n += 2) {
                    uint8_t digest_a[32];
                    uint8_t digest_b[32];
                    const uint64_t nonce_b = nonce + stride;
                    header_pow.hash_pair(nonce, nonce_b, digest_a, digest_b);
                    if (consider(nonce, digest_a) || consider(nonce_b, digest_b)) {
                        break;
                    }
                    nonce += stride * 2;
                }
            }
            total_hashes.fetch_add(local_hashes, std::memory_order_relaxed);
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    result.hashes = total_hashes.load(std::memory_order_relaxed);
    if (!found.load(std::memory_order_relaxed)) {
        result.error = "mining timeout after " + std::to_string(timeout_sec) +
                       "s, hashes=" + std::to_string(result.hashes);
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options opts = parse_options(argc, argv);

        json mined = json::array();
        for (int i = 0; i < opts.blocks; ++i) {
            const auto block_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opts.timeout_sec);
            const auto block_start = std::chrono::steady_clock::now();
            uint64_t block_hashes = 0;
            bool accepted = false;

            while (!accepted) {
                const int remaining_sec = remaining_seconds_until(block_deadline);
                if (remaining_sec <= 0) {
                    throw std::runtime_error("mining timeout after " + std::to_string(opts.timeout_sec) +
                                             "s, hashes=" + std::to_string(block_hashes));
                }

                json template_params = json::array();
                json template_request = json::object();
                if (!opts.address.empty()) {
                    template_request["address"] = opts.address;
                }
                if (opts.timestamp.has_value()) {
                    const uint64_t offset = static_cast<uint64_t>(i) * opts.timestamp_step;
                    const uint64_t block_timestamp = static_cast<uint64_t>(*opts.timestamp) + offset;
                    if (block_timestamp > std::numeric_limits<uint32_t>::max()) {
                        throw std::runtime_error("requested mining timestamp exceeds uint32 range");
                    }
                    template_request["timestamp"] = block_timestamp;
                }
                if (!template_request.empty()) {
                    template_params.push_back(std::move(template_request));
                }
                const json tpl = rpc_call(opts, "getblocktemplate", template_params);
                const uint64_t height = tpl.value("height", uint64_t{0});
                const uint32_t bits = tpl.at("bits").get<uint32_t>();
                geocoin::bytes block_bytes = parse_hex_bytes_or_throw(
                    tpl.at("blockhex").get<std::string>(),
                    "getblocktemplate.blockhex"
                );

                MiningResult mining = mine_block_bytes(
                    block_bytes,
                    bits,
                    opts.threads,
                    std::min(remaining_sec, kTemplateRefreshSeconds)
                );
                block_hashes += mining.hashes;
                if (!mining.found) {
                    if (mining.error.find("timeout") != std::string::npos) {
                        continue;
                    }
                    throw std::runtime_error(mining.error.empty() ? "mining failed" : mining.error);
                }

                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - block_start).count();

                geocoin::utils::write_u64_le(block_bytes, 116, mining.nonce);
                const json submit_result = rpc_call(opts, "submitblock", json::array({geocoin::utils::bytes_to_hex(block_bytes)}));
                if (!submit_result.is_null()) {
                    if (is_stale_submit_reject(submit_result)) {
                        continue;
                    }
                    throw std::runtime_error("submitblock rejected block: " + submit_result.dump());
                }

                mined.push_back(json{
                    {"height", height},
                    {"nonce", mining.nonce},
                    {"hash", geocoin::utils::hash256_to_hex(mining.hash)},
                    {"hashes", block_hashes},
                    {"elapsed_ms", elapsed_ms},
                });
                accepted = true;
            }
        }

        std::cout << json{{"result", "ok"}, {"blocks", mined}}.dump(2) << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "geocoin-rpc-miner error: " << e.what() << "\n";
        return 1;
    }
}
