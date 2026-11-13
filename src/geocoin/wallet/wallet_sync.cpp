#include "geocoin/wallet/wallet.hpp"
#include "geocoin/wallet/wallet_internal.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace geocoin::wallet {

namespace {
inline bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}
}

WalletScanResult Wallet::scan_blockchain(std::shared_ptr<state::ChainState> chain_state, Height from_height) {
    std::unique_lock<std::recursive_mutex> scan_lock(scan_mutex_);
    ++scan_jobs_;
    struct ScanJobGuard { std::atomic<uint32_t>& count; ~ScanJobGuard() { --count; } } scan_job{scan_jobs_};
    scan_abort_requested_.store(false);
    WalletScanResult result;
    result.next_height = from_height;
    const auto finish = [&]() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_scan_result_ = result;
        }
        scan_lock.unlock();
        if (!result.complete()) GEO_LOG_WARN("wallet", result.error);
        NotifyScanFinished(this, result);
        return result;
    };
    Height durable_resume = from_height;
    try {
    if (!chain_state) {
        result.status = WalletScanStatus::READ_ERROR;
        result.error = "Wallet history scan needs an available chain state.";
        return finish();
    }

    constexpr Height kScanBatchSize = 1000;
    result.target_height = chain_state->get_height();
    Hash256 target_hash{};
    if (!chain_state->get_active_block_hash(result.target_height, target_hash)) {
        result.status = WalletScanStatus::READ_ERROR;
        result.error = "Cannot read the active chain for wallet history scanning.";
        return finish();
    }

    const auto demote_and_reset = [&](Height reset_from, Height cursor, const Hash256& anchor) {
        std::vector<Hash256> changed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& tx : transactions_) {
                if (tx.block_height != 0 && tx.block_height >= reset_from) {
                    tx.disconnect();
                    changed.push_back(tx.tx_hash);
                }
            }
            last_scanned_height_ = cursor;
            last_scanned_block_hash_ = anchor;
            ++wallet_revision_;
        }
        for (const auto& txid : changed) {
            NotifyTransactionChanged(this, txid);
        }
    };

    if (from_height > result.target_height) {
        bool already_complete = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            already_complete = last_scanned_height_ == result.target_height &&
                last_scanned_block_hash_ == target_hash;
        }
        if (!already_complete) {
            result.status = WalletScanStatus::CHAIN_CHANGED;
            result.next_height = 0;
            result.error = "The wallet scan cursor is not anchored to the current active chain.";
            demote_and_reset(0, 0, {});
        }
        return finish();
    }

    Hash256 prefix_hash{};
    if (from_height > 0 && !chain_state->get_active_block_hash(from_height - 1, prefix_hash)) {
        // Some imported/legacy stores start their active index at height one.
        // Height zero is represented by the all-zero predecessor in that
        // format, so scanning from block one can still verify continuity.
        if (from_height != 1) {
            result.status = WalletScanStatus::READ_ERROR;
            result.error = "Cannot read the active-chain prefix for wallet history scanning.";
            return finish();
        }
        prefix_hash = {};
    }

    std::set<Address> wallet_addresses;
    using Outpoint = std::pair<Hash256, uint32_t>;
    std::map<Outpoint, Amount> owned_outputs;
    std::map<Outpoint, std::vector<Hash256>> watched_inputs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (from_height > 0) {
            const Height expected_cursor = from_height - 1;
            if (last_scanned_height_ < expected_cursor ||
                (last_scanned_height_ == expected_cursor &&
                 last_scanned_block_hash_ != prefix_hash)) {
                result.status = WalletScanStatus::CHAIN_CHANGED;
            }
        }
        for (const auto& [address, key] : keys_) wallet_addresses.insert(address);
        for (const auto& [address, config] : multisig_addresses_) {
            try { wallet_addresses.insert(Address(address)); } catch (...) {}
        }
        // Seed known owned outputs preceding this range, including height-zero
        // genesis/pending records. Only confirmed earlier spends can remove them.
        // Full transactions belonging to other wallets never enter the scan cache.
        for (const auto& wtx : transactions_) {
            for (const auto& input : wtx.tx.inputs)
                watched_inputs[{input.prev_tx_hash, input.vout}].push_back(wtx.tx_hash);
            if (wtx.block_height >= from_height) continue;
            for (uint32_t i = 0; i < wtx.tx.outputs.size(); ++i) {
                const auto& output = wtx.tx.outputs[i];
                try {
                    if (wallet_addresses.count(Address(output.address)))
                        owned_outputs[{wtx.tx_hash, i}] = output.value;
                } catch (...) {}
            }
        }
        for (const auto& wtx : transactions_) {
            if (wtx.block_height == 0 || wtx.block_height >= from_height) continue;
            for (const auto& input : wtx.tx.inputs) owned_outputs.erase({input.prev_tx_hash, input.vout});
        }
        // A new scan can only certify the prefix it actually reads, including
        // after an earlier full scan.
        const Height prefix_height = from_height == 0 ? 0 : from_height - 1;
        if (from_height == 0 || last_scanned_height_ > prefix_height) {
            last_scanned_height_ = prefix_height;
            last_scanned_block_hash_ = from_height == 0 ? Hash256{} : prefix_hash;
        }
    }
    if (result.status == WalletScanStatus::CHAIN_CHANGED) {
        result.next_height = 0;
        result.error = "The saved wallet scan cursor belongs to a different active chain.";
        demote_and_reset(0, 0, {});
        return finish();
    }
    if (wallet_addresses.empty()) {
        result.status = WalletScanStatus::NO_ADDRESSES;
        result.error = "Wallet history scan has no wallet addresses.";
        return finish();
    }
    result.peak_owned_outputs = owned_outputs.size();

    std::vector<WalletTransaction> batch_updates;
    std::map<Hash256, std::vector<std::pair<Hash256, WalletBlockLocation>>> batch_conflicts;
    std::set<Hash256> batch_seen_txids;
    Height batch_start = from_height;
    Hash256 previous_hash = prefix_hash;
    const auto merge_batch = [&](Height scanned_height) {
        Hash256 active_hash{};
        if (!chain_state->get_active_block_hash(result.target_height, active_hash) || active_hash != target_hash)
            return false;
        if (!chain_state->get_active_block_hash(scanned_height, active_hash) || active_hash != previous_hash)
            return false;
        std::set<Hash256> changed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& existing : transactions_) {
                if (existing.block_height >= batch_start &&
                    existing.block_height <= scanned_height &&
                    batch_seen_txids.count(existing.tx_hash) == 0) {
                    if (existing.block_height != 0) {
                        existing.disconnect();
                        changed.insert(existing.tx_hash);
                    }
                }
            }
            for (auto& update : batch_updates) {
                auto it = tx_index_.find(update.tx_hash);
                if (it != tx_index_.end()) {
                    auto& existing = transactions_[it->second];
                    existing.tx = std::move(update.tx);
                    existing.block_height = update.block_height;
                    existing.block_hash = update.block_hash;
                    existing.block_index = update.block_index;
                    existing.block_time = update.block_time;
                    if (existing.received_at == 0) existing.received_at = existing.timestamp;
                    existing.timestamp = update.timestamp;
                    existing.amount = update.amount;
                    existing.is_coinbase = update.is_coinbase;
                    existing.mark_dirty();
                } else {
                    tx_index_[update.tx_hash] = transactions_.size();
                    transactions_.push_back(std::move(update));
                }
                changed.insert(update.tx_hash);
            }
            for (const auto& [txid, conflicts] : batch_conflicts) {
                const auto found = tx_index_.find(txid);
                if (found == tx_index_.end()) continue;
                for (const auto& [other, location] : conflicts)
                    transactions_[found->second].note_conflict(other, location);
            }
            last_scanned_height_ = scanned_height;
            last_scanned_block_hash_ = previous_hash;
            ++wallet_revision_;
        }
        batch_updates.clear();
        batch_conflicts.clear();
        batch_seen_txids.clear();
        durable_resume = scanned_height == (std::numeric_limits<Height>::max)() ? scanned_height : scanned_height + 1;
        batch_start = scanned_height == (std::numeric_limits<Height>::max)()
            ? scanned_height
            : scanned_height + 1;
        for (const auto& txid : changed) {
            NotifyTransactionChanged(this, txid);
        }
        return true;
    };
    const auto chain_changed = [&]() {
        result.status = WalletScanStatus::CHAIN_CHANGED;
        result.next_height = from_height;
        result.error = "The active chain changed during the wallet history scan; scan again from height " +
            std::to_string(from_height) + ".";
        demote_and_reset(
            from_height,
            from_height == 0 ? 0 : from_height - 1,
            from_height == 0 ? Hash256{} : prefix_hash
        );
    };

    Height batch_counter = 0;
    for (Height h = from_height; h <= result.target_height; ++h) {
        if (scan_abort_requested_.load() || scan_shutdown_requested_.load()) {
            result.status = WalletScanStatus::READ_ERROR;
            result.error = "Wallet scan was aborted";
            result.next_height = durable_resume;
            return finish();
        }
        core::Block block;
        if (!chain_state->get_block_by_height(h, block)) {
            if (batch_counter && !merge_batch(h - 1)) {
                chain_changed();
            } else {
                const bool pruned = chain_state->is_block_body_pruned(h);
                result.status = pruned ? WalletScanStatus::HISTORY_PRUNED : WalletScanStatus::READ_ERROR;
                result.next_height = h;
                result.error = "Wallet history is incomplete at block " + std::to_string(h) +
                    (pruned ? ": the block body was pruned. Restore block history before rescanning."
                            : ": the block cannot be read. Retry the scan after storage is available.");
            }
            return finish();
        }
        if (h > 0 && block.header.prev_block_hash != previous_hash) {
            chain_changed();
            return finish();
        }
        previous_hash = block.get_hash();
        uint32_t transaction_index = 0;
        for (const auto& tx : block.transactions) {
            const auto block_index = transaction_index++;
            const auto tx_hash = tx.get_hash();
            for (const auto& input : tx.inputs) {
                const auto watched = watched_inputs.find({input.prev_tx_hash, input.vout});
                if (watched == watched_inputs.end()) continue;
                for (const auto& txid : watched->second) if (txid != tx_hash)
                    batch_conflicts[txid].push_back({tx_hash, {previous_hash, h, block_index, block.header.timestamp}});
            }
            int64_t debit = 0;
            int64_t credit = 0;
            bool valid = true;
            if (!tx.is_coinbase()) {
                for (const auto& input : tx.inputs) {
                    auto it = owned_outputs.find({input.prev_tx_hash, input.vout});
                    if (it != owned_outputs.end()) {
                        valid = checked_accumulate_nonnegative(debit, it->second) && valid;
                        owned_outputs.erase(it);
                    }
                }
            }
            for (uint32_t i = 0; i < tx.outputs.size(); ++i) {
                const auto& output = tx.outputs[i];
                try {
                    if (wallet_addresses.count(Address(output.address))) {
                        valid = checked_accumulate_nonnegative(credit, output.value) && valid;
                        owned_outputs[{tx_hash, i}] = output.value;
                    }
                } catch (...) {}
            }
            result.peak_owned_outputs = std::max(result.peak_owned_outputs, owned_outputs.size());
            Amount net = 0;
            if (!valid || !checked_sub_amount(credit, debit, net) || (debit <= 0 && credit <= 0)) continue;
            WalletTransaction wtx;
            wtx.tx = tx;
            wtx.tx_hash = tx_hash;
            wtx.block_height = h;
            wtx.block_hash = previous_hash;
            wtx.block_index = block_index;
            wtx.block_time = block.header.timestamp;
            wtx.timestamp = block.header.timestamp;
            wtx.received_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            wtx.amount = net;
            wtx.is_coinbase = tx.is_coinbase();
            batch_seen_txids.insert(tx_hash);
            batch_updates.push_back(std::move(wtx));
        }
        ++batch_counter;
        if (batch_counter >= kScanBatchSize || h == result.target_height) {
            if (!merge_batch(h)) {
                chain_changed();
                return finish();
            }
            NotifyScanProgress(this, h);
            batch_counter = 0;
        }
        result.next_height = h == (std::numeric_limits<Height>::max)() ? h : h + 1;
    }
    return finish();
    } catch (const storage::ReadError& error) {
        result.status = WalletScanStatus::READ_ERROR;
        result.next_height = durable_resume;
        result.error = std::string("Wallet history storage read failed: ") + error.what();
        return finish();
    }
}

WalletScanResult Wallet::reconcile_blockchain(
    std::shared_ptr<state::ChainState> chain_state,
    Height from_height
) {
    std::lock_guard<std::recursive_mutex> scan_lock(scan_mutex_);
    if (!chain_state) return scan_blockchain(nullptr, from_height);

    const Height current_height = chain_state->get_height();
    const Height start_height = std::min(from_height, current_height);
    Hash256 prefix_hash{};
    if (start_height > 0) {
        try {
            (void)chain_state->get_active_block_hash(start_height - 1, prefix_hash);
        } catch (const storage::ReadError&) {
            return scan_blockchain(std::move(chain_state), start_height);
        }
    }
    std::vector<Hash256> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& wallet_tx : transactions_) {
            if (wallet_tx.block_height == 0 || wallet_tx.block_height < start_height) {
                continue;
            }
            wallet_tx.disconnect();
            changed.push_back(wallet_tx.tx_hash);
        }
        if (start_height == 0) {
            last_scanned_height_ = 0;
            last_scanned_block_hash_ = {};
        } else {
            // Reconciliation explicitly establishes the active predecessor as
            // the new scan anchor. This also repairs a cursor gap caused by a
            // missed notification before rescanning the affected suffix.
            last_scanned_height_ = start_height - 1;
            last_scanned_block_hash_ = prefix_hash;
        }
    }

    for (const auto& tx_hash : changed) {
        NotifyTransactionChanged(this, tx_hash);
    }
    return scan_blockchain(std::move(chain_state), start_height);
}

WalletScanResult Wallet::get_last_scan_result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_scan_result_;
}

Height Wallet::get_last_scanned_height() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_scanned_height_;
}

Hash256 Wallet::get_last_scanned_block_hash() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_scanned_block_hash_;
}

void Wallet::sync_transaction(const core::Transaction& tx, Height block_height, uint64_t timestamp,
    std::shared_ptr<state::ChainState> chain_state, const Hash256& block_hash, uint32_t block_index) {
    if (!chain_state) {
        return;
    }

    std::lock_guard<std::recursive_mutex> scan_lock(scan_mutex_);
    Hash256 tx_hash = tx.get_hash();

    if (SyncDebugEnabled()) {
        std::ostringstream ss;
        ss << std::hex;
        for (int i = 0; i < 8; ++i) {
            ss << static_cast<int>(tx_hash[i]);
        }
        GEO_LOG_DEBUG("wallet", "sync_transaction: tx_hash=" << ss.str() << ", height=" << block_height);
    }

    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (block_hash != Hash256{} && !tx.is_coinbase()) {
            std::set<std::pair<Hash256, uint32_t>> inputs;
            for (const auto& input : tx.inputs) inputs.emplace(input.prev_tx_hash, input.vout);
            for (auto& other : transactions_) {
                if (other.tx_hash == tx_hash || other.block_height != 0) continue;
                if (std::any_of(other.tx.inputs.begin(), other.tx.inputs.end(), [&](const auto& input) {
                    return inputs.contains({input.prev_tx_hash, input.vout});
                })) {
                    const auto previous = other.conflicts.size();
                    other.note_conflict(tx_hash, {block_hash, block_height, block_index, timestamp});
                    if (other.conflicts.size() != previous) ++wallet_revision_;
                }
            }
        }

        // Проверяем, есть ли уже эта транзакция (как Bitcoin AddToWallet)
        auto existing_it = tx_index_.find(tx_hash);
        bool already_exists = (existing_it != tx_index_.end());
        if (already_exists) {
            auto& wtx = transactions_[existing_it->second];

            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG(
                    "wallet",
                    "sync_transaction: DUPLICATE - updating existing (old_height="
                        << wtx.block_height << ", new_height=" << block_height << ")"
                );
            }

            bool updated = false;
            if (block_hash != Hash256{} && wtx.block_hash != block_hash) {
                if (wtx.block_hash != Hash256{}) wtx.disconnect();
                wtx.block_hash = block_hash;
                wtx.block_index = block_index;
                wtx.block_time = timestamp;
                wtx.block_height = block_height;
                updated = true;
            }

            // Обновляем высоту блока, если изменилась (транзакция подтвердилась)
            if (block_height > 0 && wtx.block_height != block_height) {
                if (SyncDebugEnabled()) {
                    GEO_LOG_DEBUG(
                        "wallet",
                        "sync_transaction: Updating height: " << wtx.block_height
                            << " -> " << block_height
                    );
                }
                wtx.block_height = block_height;
                wtx.mark_dirty();
                updated = true;
            }

            // Обновляем timestamp, если изменился
            if (timestamp > 0 && wtx.timestamp != timestamp) {
                wtx.timestamp = timestamp;
                updated = true;
            }

            if (updated) {
                ++wallet_revision_;
                wtx.tx = tx;

                int64_t debit = get_debit_locked(wtx, chain_state);
                int64_t credit = get_credit_locked(wtx);
                Amount net_amount = 0;
                wtx.amount = checked_sub_amount(credit, debit, net_amount) ? net_amount : 0;
            }

            should_notify = true;
        }

        if (!already_exists) {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("wallet", "sync_transaction: NEW transaction");
            }

            WalletTransaction wtx;
            wtx.tx = tx;
            wtx.tx_hash = tx_hash;
            wtx.block_height = block_height;
            wtx.block_hash = block_hash;
            wtx.block_index = block_index;
            wtx.block_time = block_hash != Hash256{} ? timestamp : 0;
            wtx.timestamp = timestamp;
            wtx.received_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            wtx.is_coinbase = tx.is_coinbase();
            wtx.fee = 0;
            wtx.memo = "";

            int64_t debit = get_debit_locked(wtx, chain_state);
            int64_t credit = get_credit_locked(wtx);

            // Не наша транзакция
            if (debit == 0 && credit == 0) {
                if (SyncDebugEnabled()) {
                    GEO_LOG_DEBUG("wallet", "sync_transaction: Not our transaction (debit=0, credit=0)");
                }
                return;
            }

            Amount net_amount = 0;
            if (!checked_sub_amount(credit, debit, net_amount)) {
                return;
            }
            wtx.amount = net_amount;

            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG(
                    "wallet",
                    "sync_transaction: Adding: debit=" << debit
                        << ", credit=" << credit
                        << ", amount=" << wtx.amount
                );
            }

            transactions_.push_back(wtx);
            ++wallet_revision_;
            tx_index_[tx_hash] = transactions_.size() - 1;
            should_notify = true;
        }

        // A transaction event does not certify that earlier blocks were scanned.
        // Only scan_blockchain advances the durable history cursor.
    }

    if (should_notify) {
        // ВАЖНО: уведомляем вне mutex, чтобы избежать дедлоков UI.
        NotifyTransactionChanged(this, tx_hash);
    }
}

void Wallet::sync_disconnected_block(
    const core::Block& block,
    Height disconnected_height,
    Height new_tip_height,
    const Hash256& new_tip_hash
) {
    std::lock_guard<std::recursive_mutex> scan_lock(scan_mutex_);
    std::vector<Hash256> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& tx : block.transactions) {
            const Hash256 tx_hash = tx.get_hash();
            const auto existing_it = tx_index_.find(tx_hash);
            if (existing_it == tx_index_.end()) {
                continue;
            }

            auto& wallet_tx = transactions_[existing_it->second];
            if (wallet_tx.block_height != disconnected_height) {
                continue;
            }
            if (wallet_tx.block_hash == Hash256{}) {
                wallet_tx.block_hash = block.get_hash();
                wallet_tx.block_time = block.header.timestamp;
                wallet_tx.block_index = static_cast<uint32_t>(&tx - block.transactions.data());
            }
            wallet_tx.disconnect();
            changed.push_back(tx_hash);
        }
        last_scanned_height_ = std::min(last_scanned_height_, new_tip_height);
        ++wallet_revision_;
        if (last_scanned_height_ == new_tip_height) {
            last_scanned_block_hash_ = new_tip_hash;
        } else {
            // The supplied tip does not describe an older retained cursor.
            // Force an anchored reconciliation before incremental scanning.
            last_scanned_block_hash_ = {};
        }
    }

    for (const auto& tx_hash : changed) {
        NotifyTransactionChanged(this, tx_hash);
    }
}

WalletScanResult Wallet::synchronize(std::shared_ptr<state::ChainState> chain_state,
    std::span<const core::Transaction> pending) {
    std::lock_guard<std::recursive_mutex> guard(scan_mutex_);
    if (get_key_count() == 0) return {};
    const auto cursor = get_last_scanned_height();
    const auto anchor = get_last_scanned_block_hash();
    Hash256 active{};
    bool needs_anchors = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        needs_anchors = std::any_of(transactions_.begin(), transactions_.end(),
            [](const auto& tx) { return tx.block_height > 0 && tx.block_hash == Hash256{}; });
    }
    auto result = chain_state && !needs_anchors && anchor != Hash256{} &&
        chain_state->get_active_block_hash(cursor, active) && active == anchor ?
        scan_blockchain(chain_state, cursor + 1) : reconcile_blockchain(chain_state, 0);
    if (!result.complete()) return result;
    for (const auto& tx : pending) sync_transaction(tx, 0, tx.timestamp, chain_state);
    if (is_vault() && !save("")) {
        result.status = WalletScanStatus::READ_ERROR;
        result.error = "Wallet history could not be saved durably";
    }
    return result;
}

} // namespace geocoin::wallet
