#include "geocoin/consensus/optimized_miner.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/consensus/chainparams.hpp"
#define IOKE_SHA256_NO_OPENSSL_COMPAT_NAMES
#include "geocoin/crypto/ioke_sha256.h"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logging.hpp"
#include "geocoin/utils/logger.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>

using geocoin::utils::LogPrintf;

namespace geocoin::consensus {

namespace {
static std::atomic<int> g_mining_sha256_requested_backend{
    static_cast<int>(MiningSha256Backend::Auto)
};
static std::atomic<int> g_mining_sha256_effective_backend{
    static_cast<int>(MiningSha256Backend::Auto)
};
static std::mutex g_mining_sha256_backend_mutex;
static std::atomic<uint64_t> g_mining_sha256_benchmark_sink{0};

inline bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}

std::string normalize_backend_name(std::string backend) {
    std::transform(backend.begin(), backend.end(), backend.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return backend;
}

inline Hash256 openssl_double_sha256(const bytes& data) {
    Hash256 first{};
    Hash256 second{};
    SHA256(data.data(), data.size(), first.data());
    SHA256(first.data(), first.size(), second.data());
    return second;
}

inline void ioke_sha256_stack(const uint8_t* data, size_t len, uint8_t out[32]) {
    constexpr size_t kMaxMiningInput = 256;
    const size_t blocks = (len + 9 + 63) / 64;
    const size_t padded_len = blocks * 64;
    if (padded_len > kMaxMiningInput) {
        // This fast path is for fixed-size PoW headers. Fall back to the
        // generic implementation if somebody reuses it with larger data.
        IOKE_SHA256(data, len, out);
        return;
    }

    std::array<uint8_t, kMaxMiningInput> buf{};
    std::memcpy(buf.data(), data, len);
    buf[len] = 0x80U;
    const uint64_t bit_len = static_cast<uint64_t>(len) << 3;
    for (int i = 0; i < 8; ++i) {
        buf[padded_len - 8 + i] = static_cast<uint8_t>(bit_len >> (56 - 8 * i));
    }

    uint32_t state[8]{};
    ioke_sha256_init_state(state);
    ioke_sha256_compress(state, buf.data(), blocks);
    ioke_sha256_write_hash(out, state);
}

inline Hash256 tonc_double_sha256(const bytes& data) {
    Hash256 first{};
    Hash256 second{};
    ioke_sha256_stack(data.data(), data.size(), first.data());
    ioke_sha256_stack(first.data(), first.size(), second.data());
    return second;
}

const char* backend_label(MiningSha256Backend backend) {
    switch (backend) {
        case MiningSha256Backend::Native:
            return "native";
        case MiningSha256Backend::Legacy:
            return "legacy";
        case MiningSha256Backend::OpenSSL:
            return "openssl";
        case MiningSha256Backend::Auto:
        default:
            return "auto";
    }
}

void apply_effective_backend(MiningSha256Backend backend) {
    if (backend == MiningSha256Backend::Native) {
        ioke_sha256_set_backend(IOKE_SHA256_BACKEND_NATIVE);
    } else if (backend == MiningSha256Backend::Legacy) {
        ioke_sha256_set_backend(IOKE_SHA256_BACKEND_LEGACY);
    } else if (backend == MiningSha256Backend::Auto) {
        ioke_sha256_set_backend(IOKE_SHA256_BACKEND_AUTO);
    }
    g_mining_sha256_effective_backend.store(static_cast<int>(backend), std::memory_order_relaxed);
}

uint64_t benchmark_backend(MiningSha256Backend backend, std::chrono::milliseconds min_duration) {
    if (backend == MiningSha256Backend::Native &&
        !ioke_sha256_backend_available(IOKE_SHA256_BACKEND_NATIVE)) {
        return 0;
    }

    if (backend == MiningSha256Backend::Native) {
        ioke_sha256_set_backend(IOKE_SHA256_BACKEND_NATIVE);
    } else if (backend == MiningSha256Backend::Legacy) {
        ioke_sha256_set_backend(IOKE_SHA256_BACKEND_LEGACY);
    }

    bytes input(92);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<uint8_t>((i * 131U + 17U) & 0xffU);
    }

    constexpr uint64_t kBatch = 1024;
    Hash256 hash{};
    uint64_t iterations = 0;
    uint64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + min_duration;
    do {
        for (uint64_t i = 0; i < kBatch; ++i) {
            const uint64_t nonce = iterations + i;
            for (int b = 0; b < 8; ++b) {
                input[80 + b] = static_cast<uint8_t>(nonce >> (8 * b));
            }
            if (backend == MiningSha256Backend::OpenSSL) {
                hash = openssl_double_sha256(input);
            } else {
                hash = tonc_double_sha256(input);
            }
            sink += hash[0];
        }
        iterations += kBatch;
    } while (std::chrono::steady_clock::now() < deadline);

    g_mining_sha256_benchmark_sink.fetch_add(sink, std::memory_order_relaxed);

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (elapsed_ns <= 0) {
        return 0;
    }
    const long double hashes_per_second =
        (static_cast<long double>(iterations) * 1000000000.0L) /
        static_cast<long double>(elapsed_ns);
    if (hashes_per_second >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(hashes_per_second);
}

MiningSha256Backend resolve_auto_backend() {
    // Keep this short: it runs only when auto mode is selected, before mining
    // threads start. It compares implementation overhead on this CPU without
    // changing PoW bytes or consensus rules.
    constexpr auto kDuration = std::chrono::milliseconds(35);
    const uint64_t native_rate = benchmark_backend(MiningSha256Backend::Native, kDuration);
    const uint64_t openssl_rate = benchmark_backend(MiningSha256Backend::OpenSSL, kDuration);
    const uint64_t legacy_rate = benchmark_backend(MiningSha256Backend::Legacy, kDuration);

    MiningSha256Backend selected = MiningSha256Backend::Legacy;
    uint64_t selected_rate = legacy_rate;
    if (openssl_rate > selected_rate) {
        selected = MiningSha256Backend::OpenSSL;
        selected_rate = openssl_rate;
    }
    if (native_rate > selected_rate) {
        selected = MiningSha256Backend::Native;
        selected_rate = native_rate;
    }

    apply_effective_backend(selected);
    LogPrintf(
        "Mining SHA256 auto-bench: native=%.2f MH/s, openssl=%.2f MH/s, legacy=%.2f MH/s, selected=%s\n",
        native_rate / 1000000.0,
        openssl_rate / 1000000.0,
        legacy_rate / 1000000.0,
        backend_label(selected)
    );
    (void)selected_rate;
    return selected;
}

inline Hash256 mining_double_sha256(const bytes& data) {
    const auto backend = static_cast<MiningSha256Backend>(
        g_mining_sha256_effective_backend.load(std::memory_order_relaxed)
    );
    if (backend == MiningSha256Backend::OpenSSL) {
        return openssl_double_sha256(data);
    }
    return tonc_double_sha256(data);
}
}

bool set_mining_sha256_backend(const std::string& backend_name) {
    const std::string backend = normalize_backend_name(backend_name);
    std::lock_guard<std::mutex> lock(g_mining_sha256_backend_mutex);
    if (backend.empty() || backend == "auto" || backend == "tonc" || backend == "ioke") {
        g_mining_sha256_requested_backend.store(static_cast<int>(MiningSha256Backend::Auto), std::memory_order_relaxed);
        resolve_auto_backend();
        return true;
    }
    if (backend == "native" || backend == "sha-ni" || backend == "shani" ||
        backend == "tonc-native" || backend == "ioke-native") {
        g_mining_sha256_requested_backend.store(static_cast<int>(MiningSha256Backend::Native), std::memory_order_relaxed);
        apply_effective_backend(MiningSha256Backend::Native);
        return true;
    }
    if (backend == "legacy" || backend == "portable" ||
        backend == "tonc-legacy" || backend == "ioke-legacy") {
        g_mining_sha256_requested_backend.store(static_cast<int>(MiningSha256Backend::Legacy), std::memory_order_relaxed);
        apply_effective_backend(MiningSha256Backend::Legacy);
        return true;
    }
    if (backend == "openssl") {
        g_mining_sha256_requested_backend.store(static_cast<int>(MiningSha256Backend::OpenSSL), std::memory_order_relaxed);
        apply_effective_backend(MiningSha256Backend::OpenSSL);
        return true;
    }
    return false;
}

std::string get_mining_sha256_backend() {
    const auto requested = static_cast<MiningSha256Backend>(
        g_mining_sha256_requested_backend.load(std::memory_order_relaxed)
    );
    const auto effective = static_cast<MiningSha256Backend>(
        g_mining_sha256_effective_backend.load(std::memory_order_relaxed)
    );
    if (requested == MiningSha256Backend::Auto) {
        return std::string("auto/") + backend_label(effective);
    }
    if (effective == MiningSha256Backend::OpenSSL) {
        return "openssl";
    }
    if (effective == MiningSha256Backend::Native) {
        return std::string("native/") + ioke_sha256_backend_name();
    }
    if (effective == MiningSha256Backend::Legacy) {
        return "legacy";
    }
    return "auto";
}

// Глобальные переменные для hashrate (как в Bitcoin miner.cpp:520-543)
static std::atomic<int64_t> nHPSTimerStart(0);
static std::atomic<int64_t> nHashCounter(0);
static std::atomic<int64_t> nLogTime(0);
double dHashesPerSec = 0.0;

OptimizedMiner::OptimizedMiner()
    : hashrate_(0.0),
      stats_initialized_(false),
      total_iterations_(0),
      current_height_(0),
      height_initialized_(false) {
}

bool OptimizedMiner::quick_check_target(const Hash256& hash, const uint256& target) {
    // Сравниваем первые 8 байт хеша с target.data[3]
    uint64_t hash_high = 0;
    for (int i = 0; i < 8; i++) {
        hash_high = (hash_high << 8) | hash[i];
    }

    // Если hash_high < target.data[3] - точно проходит
    if (hash_high < target.data[3]) return true;
    // Если hash_high > target.data[3] - точно не проходит
    if (hash_high > target.data[3]) return false;
    // Если равны - нужна полная проверка остальных байтов
    return true;
}

bool fast_compare_hash_target(const uint8_t hash[32], const uint256& target) {
    // SHA-256 возвращает 32 байта в big-endian (hash[0] = старший байт числа)
    // uint256.data[] хранит число как 4 x uint64_t, где data[3] = старшие 64 бита
    //
    // hash[0..7] = старшие 64 бита → сравниваем с target.data[3]
    // hash[8..15] = следующие 64 бита → сравниваем с target.data[2]
    // hash[16..23] = следующие 64 бита → сравниваем с target.data[1]
    // hash[24..31] = младшие 64 бита → сравниваем с target.data[0]

    for (int i = 3; i >= 0; i--) {
        // Берём 8 байт из hash и интерпретируем как big-endian uint64
        int hash_offset = (3 - i) * 8;  // data[3] -> hash[0..7], data[0] -> hash[24..31]
        uint64_t hash_part = 0;

        // Читаем 8 байт как big-endian число (старший байт имеет больший вес)
        for (int j = 0; j < 8; j++) {
            hash_part = (hash_part << 8) | static_cast<uint64_t>(hash[hash_offset + j]);
        }

        if (hash_part < target.data[i]) return true;
        if (hash_part > target.data[i]) return false;
    }
    return true;  // Равны - проходит (hash == target допустимо)
}

void inline_sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    if (!state || !block) {
        return;
    }
    ioke_sha256_compress(state, block, 1);
}

bool OptimizedMiner::mine_block_optimized(
    core::Block& block,
    uint32_t target_bits,
    uint32_t prev_timestamp,
    const Hash256& prev_hash,
    Height height,
    uint64_t max_iterations,
    std::atomic<bool>* stop_flag,
    int thread_id,
    std::atomic<uint64_t>* hash_counter,
    uint64_t nonce_step
) {
    if (nonce_step == 0) {
        nonce_step = 1;
    }

    auto target = ProofOfWork::bits_to_target(target_bits);

    // НЕ сбрасываем nonce! Каждый поток продолжает с текущего значения
    block.header.bits = target_bits;

    const auto tag = geo::compute_block_geo_tag(prev_timestamp, prev_hash, height, Params().geopow_activation_height());
    block.header.geo_tag = tag.geo_tag;

    // Вывод target один раз
    static bool target_printed = false;
    if (!target_printed && thread_id == 0) {
        target_printed = true;
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("miner", "Mining target: 0x" << std::hex << target_bits << std::dec);
            {
                std::ostringstream ss;
                ss << "Target value: ";
                ss << std::hex;
                for (int i = 3; i >= 0; --i) {
                    ss << std::setw(16) << std::setfill('0') << target.data[i];
                }
                GEO_LOG_DEBUG("miner", ss.str());
            }
            GEO_LOG_DEBUG("miner", "Starting mining loop...");
        }
    }

    uint64_t iterations = 0;

    // ОПТИМИЗАЦИЯ: Сериализуем заголовок один раз
    bytes header_bytes = block.header.serialize();

    // ВАЖНО: оффсеты должны строго соответствовать BlockHeader::serialize().
    // Структура header124:
    // version(4) + timestamp(4) + prev_hash(32) + merkle_root(32) + bits(4) + geo_tag(8)
    // + witness_commitment(32) + nonce(8)
    // nonce offset = 4 + 4 + 32 + 32 + 4 + 8 + 32 = 116
    constexpr size_t NONCE_OFFSET = 116;

    uint64_t local_hashes = 0;
    auto flush_hashes = [&]() {
        if (local_hashes == 0) {
            return;
        }
        if (hash_counter) {
            hash_counter->fetch_add(local_hashes, std::memory_order_relaxed);
        }
        nHashCounter.fetch_add(static_cast<int64_t>(local_hashes), std::memory_order_relaxed);
        local_hashes = 0;
    };

    while (max_iterations == 0 || iterations < max_iterations) {
        // Проверка флага остановки каждые 4096 итераций
        if ((iterations & 0xFFF) == 0 && stop_flag && !stop_flag->load()) {
            return false;
        }

        // ОПТИМИЗАЦИЯ: Обновляем только nonce в буфере (8 байт LE)
        uint64_t nonce = block.header.nonce;
        for (int i = 0; i < 8; i++) {
            header_bytes[NONCE_OFFSET + i] = static_cast<byte>((nonce >> (i * 8)) & 0xFF);
        }

        // Вычисляем GeoPoW hash: double_sha256(header92)
        auto hash = mining_double_sha256(header_bytes);

        // Быстрая проверка старших байтов
        if (quick_check_target(hash, target)) {
            // КРИТИЧНО: Выводим ВСЕ хеши которые прошли quick_check
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG(
                    "miner",
                    "[FOUND] Hash passed quick_check: "
                        << std::hex
                        << (int)hash[0] << (int)hash[1] << (int)hash[2] << (int)hash[3]
                        << (int)hash[4] << (int)hash[5] << (int)hash[6] << (int)hash[7]
                        << "... nonce=" << std::dec << block.header.nonce
                );
            }

            // Полная проверка
            bool full_check = fast_compare_hash_target(hash.data(), target);

            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("miner", "[FOUND] Full check result: " << (full_check ? "PASS" : "FAIL"));
            }

            if (full_check) {
                // БЛОК НАЙДЕН!
                const std::string hash_hex = utils::hash256_to_hex(hash);

                if (SyncDebugEnabled()) {
                    GEO_LOG_DEBUG("miner", "GeoPoW OptimizedMiner: proof-of-work found");
                    GEO_LOG_DEBUG("miner", "  hash: " << hash_hex);
                    GEO_LOG_DEBUG("miner", "  nonce: " << block.header.nonce);
                }

                flush_hashes();
                return true;
            }
        }

        // ОТЛАДКА: Только хеши с 4 нулями (редко)
        if (SyncDebugEnabled() && hash[0] == 0 && hash[1] == 0 && hash[2] == 0 && hash[3] == 0) {
            GEO_LOG_DEBUG(
                "miner",
                "Found hash with 4 zeros: "
                    << std::hex
                    << (int)hash[0] << (int)hash[1] << (int)hash[2] << (int)hash[3]
                    << (int)hash[4] << (int)hash[5] << (int)hash[6] << (int)hash[7]
                    << "... nonce=" << std::dec << block.header.nonce
            );
            GEO_LOG_DEBUG("miner", "This should pass target check");
        }

        const uint64_t prev_nonce_value = block.header.nonce;
        block.header.nonce += nonce_step;
        iterations++;
        local_hashes++;

        // Flush counters in large chunks. Per-hash atomics destroy scaling on
        // 64/128/192-thread CPUs because every worker fights for one cache line.
        if ((iterations & 0xFFFF) == 0) {
            flush_hashes();

            int64_t nNow = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();

            // Инициализируем таймер если нужно
            int64_t expected = 0;
            nHPSTimerStart.compare_exchange_strong(expected, nNow, std::memory_order_relaxed);

            int64_t nStart = nHPSTimerStart.load(std::memory_order_relaxed);

            // Обновляем hashrate каждые 4 секунды
            if (nNow - nStart > 4000) {
                int64_t counter = nHashCounter.exchange(0, std::memory_order_relaxed);
                if (counter > 0) {
                    dHashesPerSec = 1000.0 * counter / (nNow - nStart);
                    hashrate_ = dHashesPerSec;
                    nHPSTimerStart.store(nNow, std::memory_order_relaxed);

                    int64_t nTimeNow = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();

                    int64_t lastLog = nLogTime.load(std::memory_order_relaxed);

                    // Выводим каждые 5 секунд
                    if (nTimeNow - lastLog >= 5) {
                        if (nLogTime.compare_exchange_strong(lastLog, nTimeNow, std::memory_order_relaxed)) {
                            if (SyncDebugEnabled()) {
                                if (dHashesPerSec >= 1000000.0) {
                                    GEO_LOG_DEBUG("miner", "hashmeter " << std::fixed << std::setprecision(2) << (dHashesPerSec / 1000000.0) << " Mhash/s");
                                } else if (dHashesPerSec >= 1000.0) {
                                    GEO_LOG_DEBUG("miner", "hashmeter " << std::fixed << std::setprecision(2) << (dHashesPerSec / 1000.0) << " khash/s");
                                } else {
                                    GEO_LOG_DEBUG("miner", "hashmeter " << std::fixed << std::setprecision(0) << dHashesPerSec << " hash/s");
                                }
                            }
                        }
                    }
                }
            }
            if (hashrate_ <= 0.0 && dHashesPerSec > 0.0) {
                hashrate_ = dHashesPerSec;
            }
        }

        // Если nonce переполнился, выходим (caller должен пересоздать шаблон блока)
        if (block.header.nonce < prev_nonce_value) {
            flush_hashes();
            return false;
        }
    }

    flush_hashes();
    return false;
}

bytes OptimizedMiner::compute_midstate(const core::BlockHeader& header) {
    return bytes();
}

Hash256 OptimizedMiner::optimized_sha256(const bytes& data, const bytes& midstate) {
    return crypto::sha256(data);
}

} // namespace geocoin::consensus
