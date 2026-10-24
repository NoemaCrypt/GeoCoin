/**
 * @file mine_genesis.cpp
 * @brief Утилита для майнинга Genesis блока
 *
 * Майнит Genesis блок один раз и выводит его параметры для hardcode в genesis.cpp
 */

#include "geocoin/core/genesis.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/optimized_miner.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace geocoin;
using namespace geocoin::core;
using namespace geocoin::consensus;
using namespace geocoin::crypto;

namespace {

void configure_mining_worker(int thread_id) {
#ifdef _WIN32
    const WORD group_count = GetActiveProcessorGroupCount();
    if (group_count > 1) {
        std::vector<WORD> groups;
        groups.reserve(group_count);
        for (WORD group = 0; group < group_count; ++group) {
            if (GetActiveProcessorCount(group) > 0) {
                groups.push_back(group);
            }
        }

        if (!groups.empty()) {
            const size_t group_index = static_cast<size_t>(thread_id) % groups.size();
            const WORD group = groups[group_index];
            const DWORD processors = GetActiveProcessorCount(group);
            if (processors > 0) {
                const DWORD cpu_index = static_cast<DWORD>(
                    (static_cast<size_t>(thread_id) / groups.size()) % processors
                );
                if (cpu_index < sizeof(KAFFINITY) * 8) {
                    GROUP_AFFINITY affinity{};
                    affinity.Group = group;
                    affinity.Mask = static_cast<KAFFINITY>(1) << cpu_index;
                    (void)SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr);
                }
            }
        }
    }
#else
    (void)thread_id;
#endif
}

}

void print_hash(const Hash256& hash) {
    for (const auto& byte : hash) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    std::cout << std::dec;
}

int main(int argc, char* argv[]) {
    // Количество потоков (по умолчанию = количество ядер)
    int num_threads = std::thread::hardware_concurrency();
    uint32_t override_bits = 0;
    uint64_t start_nonce = 0;
    uint64_t max_iterations_per_thread = 0;
    std::string sha256_backend = "auto";
    GenesisNetwork genesis_network = GenesisNetwork::MAINNET;

    if (argc > 1) {
        num_threads = std::atoi(argv[1]);
        if (num_threads <= 0) {
            num_threads = 1;
        }
    }

    if (argc > 3) {
        try {
            start_nonce = static_cast<uint64_t>(std::stoull(argv[3], nullptr, 10));
        } catch (...) {
            start_nonce = 0;
        }
    }

    if (argc > 2) {
        // argv[2] can be like "1d00ffff" or "0x1d00ffff"
        std::string s = argv[2];
        if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
            s = s.substr(2);
        }
        try {
            override_bits = static_cast<uint32_t>(std::stoul(s, nullptr, 16));
        } catch (...) {
            override_bits = 0;
        }
    }

    if (argc > 4) {
        sha256_backend = argv[4];
    } else if (const char* env = std::getenv("GEOCOIN_MINING_SHA256")) {
        sha256_backend = env;
    }

    if (argc > 5) {
        const std::string network_arg = argv[5];
        if (network_arg == "degree" || network_arg == "mainnet") {
            genesis_network = GenesisNetwork::MAINNET;
        } else if (network_arg == "degree-test" || network_arg == "testnet") {
            genesis_network = GenesisNetwork::TESTNET;
        } else if (network_arg == "degree-load" || network_arg == "loadnet") {
            genesis_network = GenesisNetwork::LOADNET;
        } else {
            std::cerr << "Invalid genesis network: " << network_arg
                      << " (expected degree|degree-test|degree-load)\n";
            return 1;
        }
    }

    if (argc > 6) {
        try {
            max_iterations_per_thread = static_cast<uint64_t>(std::stoull(argv[6], nullptr, 10));
        } catch (...) {
            max_iterations_per_thread = 0;
        }
    }

    if (!set_mining_sha256_backend(sha256_backend)) {
        std::cerr << "Invalid mining SHA256 backend: " << sha256_backend
                  << " (expected auto|native|legacy|openssl)\n";
        return 1;
    }

    if (argc > 2 && override_bits != 0) {
        const uint32_t exponent = (override_bits >> 24) & 0xFF;
        if (exponent < 3) {
            std::cerr << "Invalid bits compact format: exponent < 3 (bits=0x" << std::hex << override_bits << std::dec << ")\n"
                      << "Hint: bits should usually be 8 hex digits, e.g. 207fffff (very easy), 1e0fffff (easy), 1d00ffff (Bitcoin difficulty=1).\n";
            return 1;
        }
    }

    std::cout << "=== GeoCoin Genesis Block Miner ===" << std::endl;
    std::cout << "Threads: " << num_threads << std::endl;
    std::cout << "Nonce stride: " << num_threads << " (no overlap between threads)" << std::endl;
    std::cout << "SHA256 backend: " << get_mining_sha256_backend() << std::endl;
    if (start_nonce != 0) {
        std::cout << "Start nonce base: " << start_nonce << std::endl;
    }
    if (max_iterations_per_thread != 0) {
        std::cout << "Maximum attempts per thread: " << max_iterations_per_thread << std::endl;
    }
    std::cout << std::endl;

    // Создаем Genesis блок
    Block genesis = create_genesis_block(genesis_network);

    if (override_bits != 0) {
        genesis.header.bits = override_bits;
    }

    std::cout << "Genesis block created:" << std::endl;
    std::cout << "  Network: " << genesis_parameters(genesis_network).network_id << std::endl;
    std::cout << "  Timestamp: " << genesis.header.timestamp << std::endl;
    std::cout << "  Bits: 0x" << std::hex << genesis.header.bits << std::dec << std::endl;
    std::cout << "  Message: " << genesis_message(genesis_network) << std::endl;

    {
        const auto target = ProofOfWork::bits_to_target(genesis.header.bits);
        std::cout << "  Target: 0x";
        std::cout << std::hex << std::setfill('0');
        for (int i = 3; i >= 0; --i) {
            std::cout << std::setw(16) << target.data[i];
        }
        std::cout << std::dec << std::endl;
    }
    std::cout << std::endl;

    std::cout << "Mining Genesis block with " << num_threads << " threads..." << std::endl;
    std::cout << "This may take a while..." << std::endl;
    std::cout << std::endl;

    auto start_time = std::chrono::steady_clock::now();
    std::atomic<bool> found(false);
    std::atomic<bool> mining(true);
    std::atomic<uint64_t> hash_counter(0);
    std::vector<std::thread> threads;

    Block found_block;

    // Поток прогресса
    std::atomic<bool> progress_running(true);
    std::thread progress([&]() {
        uint64_t last_hashes = 0;
        auto last_time = std::chrono::steady_clock::now();

        while (progress_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));

            uint64_t total = hash_counter.load(std::memory_order_relaxed);
            auto now = std::chrono::steady_clock::now();
            auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
            uint64_t dh = total - last_hashes;

            double hps = 0.0;
            if (dt_ms > 0) {
                hps = (1000.0 * static_cast<double>(dh)) / static_cast<double>(dt_ms);
            }

            auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

            std::cout << "[mine_genesis] elapsed=" << elapsed_s << "s"
                      << " total_hashes=" << total
                      << " rate=" << std::fixed << std::setprecision(2) << (hps / 1e6) << " MH/s"
                      << std::endl;

            last_hashes = total;
            last_time = now;
        }
    });

    // Запускаем потоки майнинга
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&, i]() {
            configure_mining_worker(i);

            // Каждый поток получает свою подпоследовательность nonce:
            // thread i: start_nonce + i, start_nonce + i + num_threads, ...
            // Так диапазоны потоков никогда не пересекаются.
            Block local_block = genesis;
            local_block.header.nonce = start_nonce + static_cast<uint64_t>(i);

            OptimizedMiner miner;

            // Майним пока не найдем или не остановят
            bool result = miner.mine_block_optimized(
                local_block,
                local_block.header.bits,
                0U,  // prev_timestamp (genesis)
                Hash256{},  // prev_hash (genesis)
                0, // genesis height
                max_iterations_per_thread,  // 0 = бесконечно
                &mining,
                i,
                &hash_counter,
                static_cast<uint64_t>(num_threads)
            );

            if (result && !found.exchange(true)) {
                // Первый поток который нашел
                found_block = local_block;
                mining = false;  // Останавливаем остальные
            }
        });
    }

    // Ждем завершения всех потоков
    for (auto& thread : threads) {
        thread.join();
    }

    progress_running.store(false, std::memory_order_relaxed);
    if (progress.joinable()) {
        progress.join();
    }

    if (found) {
        // Safety net: verify the found header using the canonical PoW verifier.
        // This prevents hardcoding a bogus genesis due to any miner-side bug.
        if (!ProofOfWork::verify_proof_of_work(
                found_block.header,
                found_block.header.bits,
                0U,
                Hash256{},
                0, Params(genesis_network == GenesisNetwork::MAINNET ? Network::MAIN :
                    genesis_network == GenesisNetwork::TESTNET ? Network::TESTNET : Network::LOADNET))) {
            std::cerr << "ERROR: miner reported a solution, but verify_proof_of_work() failed.\n";
            std::cerr << "This indicates a bug in mining code or header serialization.\n";
            return 1;
        }

        Hash256 hash = found_block.get_hash();
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

        std::cout << std::endl;
        std::cout << "✓ Genesis block mined!" << std::endl;
        std::cout << std::endl;
        std::cout << "=== Genesis Block Parameters ===" << std::endl;
        std::cout << "Nonce: " << found_block.header.nonce << std::endl;
        std::cout << "Hash: ";
        print_hash(hash);
        std::cout << std::endl;
        std::cout << "Time taken: " << duration.count() << " seconds" << std::endl;
        std::cout << std::endl;

        std::cout << "=== Add to genesis.cpp ===" << std::endl;
        std::cout << "genesis.header.nonce = " << found_block.header.nonce << "ULL;" << std::endl;
        std::cout << std::endl;
        std::cout << "Hash256 get_genesis_hash(" << genesis_parameters(genesis_network).network_id
                  << ") {" << std::endl;
        std::cout << "    return Hash256{";
        for (size_t i = 0; i < hash.size(); i++) {
            if (i > 0) std::cout << ", ";
            if (i % 8 == 0) std::cout << std::endl << "        ";
            std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        std::cout << std::dec << std::endl;
        std::cout << "    };" << std::endl;
        std::cout << "}" << std::endl;
    } else {
        std::cout << "Mining failed!" << std::endl;
        return 1;
    }

    return 0;
}
