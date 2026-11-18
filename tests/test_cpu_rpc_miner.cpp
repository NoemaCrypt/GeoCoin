#define main geocoin_rpc_miner_entry_for_cpu_test
#include "../tools/rpc_miner.cpp"
#undef main

#if defined(GEOCOIN_MACOS_ARM64)
void check_arm_sha2_nonce_batches() {
    geocoin::bytes header(HeaderPow::kHeaderSize);
    uint64_t sequence = 0x9328de0109ff003bULL;
    for (size_t variant = 0; variant < 8; ++variant) {
        for (size_t i = 0; i < header.size(); ++i) {
            header[i] = static_cast<uint8_t>(i * 173 + variant * 67);
        }
        const auto pow = HeaderPow::prepare(header.data());
        if (!pow.have_arm_sha2) return;
        for (size_t batch = 0; batch < 128; ++batch) {
            uint64_t nonces[4] = {0, UINT64_MAX, 0x0102030405060708ULL, sequence};
            if (batch != 0) {
                for (auto& nonce : nonces) {
                    sequence ^= sequence << 13;
                    sequence ^= sequence >> 7;
                    sequence ^= sequence << 17;
                    nonce = sequence;
                }
            }
            uint8_t four[4][32];
            uint8_t pair[2][32];
            pow.hash4(nonces[0], nonces[1], nonces[2], nonces[3], four);
            pow.hash_pair(nonces[0], nonces[1], pair[0], pair[1]);
            for (size_t lane = 0; lane < 4; ++lane) {
                for (size_t byte = 0; byte < sizeof(nonces[lane]); ++byte) {
                    header[HeaderPow::kNonceOffset + byte] = static_cast<uint8_t>(nonces[lane] >> (byte * 8));
                }
                const auto expected = geocoin::crypto::double_sha256(header);
                if (std::memcmp(four[lane], expected.data(), expected.size()) != 0 ||
                    (lane < 2 && std::memcmp(pair[lane], expected.data(), expected.size()) != 0)) {
                    throw std::runtime_error("ARM SHA2 nonce batch differs from canonical proof-of-work hash");
                }
            }
        }
    }
}
#endif

int main() {
    try {
#if defined(GEOCOIN_MACOS_ARM64)
        check_arm_sha2_nonce_batches();
#endif
        const auto capacity = geocoin::node::cpu_mining_thread_limit();
        geocoin::bytes block(124, 0);
        const auto mined = mine_block_bytes(block, 0x207fffff, 1, 2);
        if (!mined.found || mined.timed_out || !mined.error.empty() ||
            mined.requested_threads != 1 || mined.spawned_threads != 1 || mined.allowed_threads != capacity) {
            throw std::runtime_error("one-worker offline mining failed: " + mined.error);
        }
        const auto automatic = mine_block_bytes(block, 0x207fffff, 0, 2);
        if (!automatic.found || automatic.spawned_threads != capacity || automatic.requested_threads != capacity) {
            throw std::runtime_error("automatic threads did not use OS capacity: " + automatic.error);
        }
        const auto clamped = mine_block_bytes(block, 0x207fffff, capacity + 1024, 2);
        if (!clamped.found || clamped.spawned_threads != capacity || clamped.requested_threads != capacity + 1024) {
            throw std::runtime_error("oversized request was not reported/clamped: " + clamped.error);
        }
        const auto timed = mine_block_bytes(block, 0, 1, 0);
        if (timed.found || !timed.timed_out || timed.error.empty()) {
            throw std::runtime_error("timeout is not distinguished from worker failure");
        }
        std::atomic<bool> keep_running{true};
        MiningResult cancelled;
        std::thread worker([&] { cancelled = mine_block_bytes(block, 0, 2, 30, &keep_running); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto stopped_at = std::chrono::steady_clock::now();
        keep_running = false;
        worker.join();
        if (!cancelled.cancelled || cancelled.timed_out || !cancelled.error.empty() || cancelled.hashes == 0 ||
            std::chrono::steady_clock::now() - stopped_at >= std::chrono::seconds(5)) {
            throw std::runtime_error("real hash loop did not stop cooperatively");
        }
        Options benchmark;
        benchmark.benchmark = true;
        benchmark.threads = 2;
        benchmark.timeout_sec = 1;
        const auto report = run_cpu_benchmark(benchmark);
        if (report.at("hashes").get<uint64_t>() == 0 || report.at("active_threads") != 0 ||
            report.at("spawned_threads").get<size_t>() != (std::min)(size_t{2}, capacity)) {
            throw std::runtime_error("offline benchmark did not exercise production hashing");
        }
        std::cout << "test_cpu_rpc_miner: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_cpu_rpc_miner: " << error.what() << '\n';
        return 1;
    }
}
