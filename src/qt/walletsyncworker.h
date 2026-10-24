// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/services/chain_mutation.hpp"
#include "geocoin/types.hpp"

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace geocoin::state {
class ChainState;
}

namespace geocoin::wallet {
class Wallet;
}

/** Bounded background queue that applies node transaction events to a wallet. */
class WalletSyncWorker
{
public:
    using MempoolSnapshotProvider =
        std::function<std::vector<geocoin::core::Transaction>()>;

    WalletSyncWorker(
        std::shared_ptr<geocoin::wallet::Wallet> wallet,
        std::shared_ptr<geocoin::state::ChainState> chainState,
        MempoolSnapshotProvider mempoolSnapshotProvider = {}
    );
    ~WalletSyncWorker();

    WalletSyncWorker(const WalletSyncWorker&) = delete;
    WalletSyncWorker& operator=(const WalletSyncWorker&) = delete;

    bool enqueue(
        const geocoin::core::Transaction& tx,
        geocoin::Height height,
        uint64_t timestamp
    );
    bool enqueue(const geocoin::services::ChainMutationEvent& event);
    void stopAndDrain();

private:
    struct PendingEvent {
        geocoin::services::ChainMutationEvent event;
        size_t serializedSize;
    };

    struct RecoveryRequest {
        bool pending{false};
        bool chainRescan{false};
        geocoin::Height fromHeight{0};
        uint64_t firstMissingSequence{0};
        uint64_t lastMissingSequence{0};
        unsigned attempts{0};
        std::chrono::steady_clock::time_point notBefore{
            std::chrono::steady_clock::time_point::min()
        };
    };

    static constexpr size_t kMaxPendingTransactions = 1024;
    static constexpr size_t kMaxPendingBytes = 32 * 1024 * 1024;

    void includeInRecoveryLocked(
        const geocoin::services::ChainMutationEvent& event,
        bool unknownGap = false
    );
    void scheduleRecoveryLocked(
        const geocoin::services::ChainMutationEvent& event,
        bool unknownGap = false
    );
    void run();

    std::shared_ptr<geocoin::wallet::Wallet> wallet_;
    std::shared_ptr<geocoin::state::ChainState> chainState_;
    MempoolSnapshotProvider mempoolSnapshotProvider_;
    std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::deque<PendingEvent> pending_;
    size_t pendingBytes_{0};
    RecoveryRequest recovery_{};
    uint64_t lastObservedSequence_{0};
    bool accepting_{true};
    std::thread worker_;
};
