#include "walletsyncworker.h"

#include "geocoin/state/state.hpp"
#include "geocoin/wallet/wallet.hpp"

#include <QDebug>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <utility>

WalletSyncWorker::WalletSyncWorker(
    std::shared_ptr<geocoin::wallet::Wallet> wallet,
    std::shared_ptr<geocoin::state::ChainState> chainState,
    MempoolSnapshotProvider mempoolSnapshotProvider
)
    : wallet_(std::move(wallet))
    , chainState_(std::move(chainState))
    , mempoolSnapshotProvider_(std::move(mempoolSnapshotProvider))
    , worker_(&WalletSyncWorker::run, this)
{
}

WalletSyncWorker::~WalletSyncWorker()
{
    stopAndDrain();
}

bool WalletSyncWorker::enqueue(
    const geocoin::core::Transaction& tx,
    geocoin::Height height,
    uint64_t timestamp
) {
    geocoin::services::ChainMutationEvent event;
    event.kind = geocoin::services::ChainMutationEventKind::TRANSACTION_ACCEPTED;
    event.transaction = tx;
    event.observed_timestamp = timestamp;
    try {
        event.approximate_bytes = tx.serialize().size();
    } catch (...) {
        return false;
    }
    if (height > 0) {
        event.kind = geocoin::services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED;
        geocoin::services::ChainBlockChange change;
        change.height = height;
        change.block.transactions = {tx};
        change.block.header.timestamp = timestamp;
        change.body_available = true;
        event.transition.connected.push_back(std::move(change));
    }
    return enqueue(event);
}

bool WalletSyncWorker::enqueue(const geocoin::services::ChainMutationEvent& event)
{
    size_t serializedSize = 0;
    bool sizeValid = true;
    try {
        serializedSize = event.approximate_bytes;
        if (serializedSize == 0 &&
            event.kind == geocoin::services::ChainMutationEventKind::TRANSACTION_ACCEPTED) {
            serializedSize = event.transaction.serialize().size();
        }
    } catch (...) {
        sizeValid = false;
    }
    if (event.kind == geocoin::services::ChainMutationEventKind::RESCAN_REQUIRED) {
        serializedSize = 1;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) {
        return false;
    }

    if (event.kind == geocoin::services::ChainMutationEventKind::RESCAN_REQUIRED) {
        scheduleRecoveryLocked(event);
        lastObservedSequence_ = std::max(
            lastObservedSequence_,
            event.gap.last_missing_sequence
        );
        workAvailable_.notify_one();
        return true;
    }

    bool unknownGap = false;
    if (event.sequence != 0) {
        if (event.sequence <= lastObservedSequence_) {
            return true;
        }
        unknownGap = lastObservedSequence_ != 0 &&
            event.sequence != lastObservedSequence_ + 1;
        lastObservedSequence_ = event.sequence;
    }

    const bool hasCapacity = sizeValid && serializedSize > 0 &&
        serializedSize <= kMaxPendingBytes &&
        pending_.size() < kMaxPendingTransactions &&
        pendingBytes_ <= kMaxPendingBytes - serializedSize;
    if (unknownGap || recovery_.pending || !hasCapacity) {
        scheduleRecoveryLocked(event, unknownGap);
        workAvailable_.notify_one();
        return false;
    }

    try {
        pending_.push_back({event, serializedSize});
    } catch (...) {
        scheduleRecoveryLocked(event);
        workAvailable_.notify_one();
        return false;
    }
    pendingBytes_ += serializedSize;
    workAvailable_.notify_one();
    return true;
}

void WalletSyncWorker::includeInRecoveryLocked(
    const geocoin::services::ChainMutationEvent& event,
    bool unknownGap
) {
    uint64_t firstSequence = event.sequence;
    uint64_t lastSequence = event.sequence;
    bool chainRescan = unknownGap;
    geocoin::Height fromHeight = 0;

    if (event.kind == geocoin::services::ChainMutationEventKind::RESCAN_REQUIRED) {
        firstSequence = event.gap.first_missing_sequence;
        lastSequence = event.gap.last_missing_sequence;
        chainRescan = chainRescan || event.gap.chain_transition_lost;
        fromHeight = event.gap.rescan_from_height;
    } else if (event.kind == geocoin::services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED) {
        chainRescan = true;
        fromHeight = event.transition.fork_height <
                (std::numeric_limits<geocoin::Height>::max)()
            ? event.transition.fork_height + 1
            : event.transition.fork_height;
    }

    if (!recovery_.pending) {
        recovery_.pending = true;
        recovery_.firstMissingSequence = firstSequence;
        recovery_.lastMissingSequence = lastSequence;
        recovery_.chainRescan = chainRescan;
        recovery_.fromHeight = fromHeight;
        recovery_.attempts = 0;
        recovery_.notBefore = std::chrono::steady_clock::now();
        return;
    }

    if (firstSequence != 0 &&
        (recovery_.firstMissingSequence == 0 ||
         firstSequence < recovery_.firstMissingSequence)) {
        recovery_.firstMissingSequence = firstSequence;
    }
    recovery_.lastMissingSequence = std::max(
        recovery_.lastMissingSequence,
        lastSequence
    );
    if (chainRescan) {
        if (!recovery_.chainRescan) {
            recovery_.fromHeight = fromHeight;
        } else {
            recovery_.fromHeight = std::min(recovery_.fromHeight, fromHeight);
        }
        recovery_.chainRescan = true;
    }
    // A newly observed gap should be handled immediately even if an older
    // transient read failure was waiting for its retry deadline.
    recovery_.attempts = 0;
    recovery_.notBefore = std::chrono::steady_clock::now();
}

void WalletSyncWorker::scheduleRecoveryLocked(
    const geocoin::services::ChainMutationEvent& event,
    bool unknownGap
) {
    for (const auto& pending : pending_) {
        includeInRecoveryLocked(pending.event);
    }
    pending_.clear();
    pendingBytes_ = 0;
    includeInRecoveryLocked(event, unknownGap);
}

void WalletSyncWorker::stopAndDrain()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
    }
    workAvailable_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void WalletSyncWorker::run()
{
    for (;;) {
        PendingEvent pending;
        RecoveryRequest recovery;
        bool recover = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                workAvailable_.wait(lock, [this]() {
                    return !accepting_ || !pending_.empty() || recovery_.pending;
                });
                if (!accepting_ && pending_.empty() && !recovery_.pending) {
                    return;
                }
                if (recovery_.pending) {
                    const auto now = std::chrono::steady_clock::now();
                    if (accepting_ && recovery_.notBefore > now) {
                        const auto deadline = recovery_.notBefore;
                        workAvailable_.wait_until(lock, deadline, [this, deadline]() {
                            return !accepting_ || !recovery_.pending ||
                                recovery_.notBefore < deadline;
                        });
                        continue;
                    }
                    recovery = recovery_;
                    recovery_ = {};
                    recover = true;
                    break;
                }
                if (!pending_.empty()) {
                    pending = std::move(pending_.front());
                    pending_.pop_front();
                    pendingBytes_ -= pending.serializedSize;
                    break;
                }
            }
        }

        if (recover) {
            bool chainComplete = true;
            QString retryReason;
            if (recovery.chainRescan) {
                qWarning() << "Wallet event stream gap; reconciling active chain from height"
                           << static_cast<qulonglong>(recovery.fromHeight)
                           << "for sequences"
                           << static_cast<qulonglong>(recovery.firstMissingSequence)
                           << ".."
                           << static_cast<qulonglong>(recovery.lastMissingSequence);
                try {
                    const auto scan = wallet_->reconcile_blockchain(chainState_, recovery.fromHeight);
                    chainComplete = scan.complete() ||
                        scan.status == geocoin::wallet::WalletScanStatus::NO_ADDRESSES;
                    if (!chainComplete) {
                        retryReason = QString::fromStdString(scan.error);
                        recovery.fromHeight = std::min(recovery.fromHeight, scan.next_height);
                    }
                } catch (const std::exception& exception) {
                    qWarning() << "Wallet reconciliation failed:" << exception.what();
                    chainComplete = false;
                    retryReason = QString::fromUtf8(exception.what());
                } catch (...) {
                    qWarning() << "Wallet reconciliation failed with an unknown exception";
                    chainComplete = false;
                    retryReason = QStringLiteral("unknown exception");
                }
            }
            if (!chainComplete) {
                ++recovery.attempts;
                const unsigned shift = std::min(recovery.attempts - 1, 4u);
                const auto delay = std::chrono::seconds(1u << shift);
                recovery.pending = true;
                recovery.notBefore = std::chrono::steady_clock::now() + delay;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (accepting_) {
                        if (!recovery_.pending) {
                            recovery_ = recovery;
                        } else {
                            if (recovery.firstMissingSequence != 0 &&
                                (recovery_.firstMissingSequence == 0 ||
                                 recovery.firstMissingSequence < recovery_.firstMissingSequence)) {
                                recovery_.firstMissingSequence = recovery.firstMissingSequence;
                            }
                            recovery_.lastMissingSequence = std::max(
                                recovery_.lastMissingSequence,
                                recovery.lastMissingSequence
                            );
                            if (recovery.chainRescan) {
                                recovery_.fromHeight = recovery_.chainRescan
                                    ? std::min(recovery_.fromHeight, recovery.fromHeight)
                                    : recovery.fromHeight;
                                recovery_.chainRescan = true;
                            }
                            recovery_.attempts = std::max(recovery_.attempts, recovery.attempts);
                            recovery_.notBefore = std::min(recovery_.notBefore, recovery.notBefore);
                        }
                    }
                }
                qWarning() << "Wallet reconciliation remains incomplete; retrying in"
                           << delay.count() << "seconds:" << retryReason;
                workAvailable_.notify_one();
                continue;
            }
            if (mempoolSnapshotProvider_) {
                try {
                    for (const auto& tx : mempoolSnapshotProvider_()) {
                        wallet_->sync_transaction(tx, 0, tx.timestamp, chainState_);
                    }
                } catch (const std::exception& exception) {
                    qWarning() << "Wallet mempool reconciliation failed:" << exception.what();
                } catch (...) {
                    qWarning() << "Wallet mempool reconciliation failed with an unknown exception";
                }
            }
            continue;
        }

        try {
            if (pending.event.kind ==
                geocoin::services::ChainMutationEventKind::TRANSACTION_ACCEPTED) {
                wallet_->sync_transaction(
                    pending.event.transaction,
                    0,
                    pending.event.observed_timestamp,
                    chainState_
                );
                continue;
            }

            const auto& transition = pending.event.transition;
            const bool incompleteBodies = std::any_of(
                transition.disconnected.begin(),
                transition.disconnected.end(),
                [](const auto& change) { return !change.body_available; }
            ) || std::any_of(
                transition.connected.begin(),
                transition.connected.end(),
                [](const auto& change) { return !change.body_available; }
            );
            if (incompleteBodies) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (accepting_) {
                    scheduleRecoveryLocked(pending.event);
                    workAvailable_.notify_one();
                }
                continue;
            }
            for (const auto& change : transition.disconnected) {
                wallet_->sync_disconnected_block(
                    change.block,
                    change.height,
                    transition.new_height,
                    transition.new_tip_hash
                );
            }
            for (const auto& change : transition.connected) {
                for (const auto& tx : change.block.transactions) {
                    wallet_->sync_transaction(
                        tx,
                        change.height,
                        change.block.header.timestamp,
                        chainState_
                    );
                }
            }
        } catch (const std::exception& exception) {
            qWarning() << "Wallet transaction sync failed:" << exception.what();
            std::lock_guard<std::mutex> lock(mutex_);
            if (accepting_) {
                scheduleRecoveryLocked(pending.event);
                workAvailable_.notify_one();
            }
        } catch (...) {
            qWarning() << "Wallet transaction sync failed with an unknown exception";
            std::lock_guard<std::mutex> lock(mutex_);
            if (accepting_) {
                scheduleRecoveryLocked(pending.event);
                workAvailable_.notify_one();
            }
        }
    }
}
