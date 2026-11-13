#include "geocoin/network/mempool.hpp"
#include "geocoin/network/mempool_policy.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/state/sidechain_projection.hpp"
#include "geocoin/state/card_projection.hpp"
#include "geocoin/state/nft_projection.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/u64_math.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <limits>
#include <utility>

namespace geocoin::network {
namespace {
constexpr size_t kMaxPendingDelegationProofs = 1024;
constexpr size_t kMaxPendingAngleCommitmentsPerSpend =
    consensus::params::MAX_ANGLE_COMMITMENTS;
constexpr auto kAngleCommitmentExpiry = std::chrono::hours(1);
constexpr auto kZeroFeeProtocolExpiry = std::chrono::hours(6);
constexpr auto kDefaultMempoolExpiry = std::chrono::hours(72);

bool has_real_utxo_inputs(const core::Transaction& tx) {
    return tx.type != core::TransactionType::ANGLE_COMMITMENT;
}

bool consumes_angle_commitments(const core::Transaction& tx) {
    return tx.type == core::TransactionType::MULTISIG_SPEND ||
           tx.type == core::TransactionType::DELEGATION_PROOF ||
           tx.type == core::TransactionType::REVOKE_DELEGATION;
}

bool occupancy_at_least(size_t used, size_t capacity, uint64_t percent) {
    if (capacity == 0 || percent > 100) {
        return false;
    }
    if (used >= capacity) {
        return true;
    }

    // ceil(capacity * percent / 100), without overflowing size_t.
    const size_t whole = capacity / 100;
    const size_t remainder = capacity % 100;
    const size_t threshold = whole * static_cast<size_t>(percent) +
        (remainder * static_cast<size_t>(percent) + 99) / 100;
    return used >= threshold;
}

uint64_t saturating_rate_multiplier(uint64_t rate, uint64_t multiplier) {
    const uint64_t max_value = (std::numeric_limits<uint64_t>::max)();
    if (rate != 0 && multiplier > max_value / rate) {
        return max_value;
    }
    return rate * multiplier;
}
} // namespace


std::string Mempool::hash256_to_hex(const Hash256& h) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : h) {
        oss << std::setw(2) << (int)b;
    }
    return oss.str();
}

bool Mempool::is_null_hash256(const Hash256& h) {
    for (auto b : h) {
        if (b != 0) return false;
    }
    return true;
}

Amount Mempool::hierarchical_amount_spent_locked(
    const core::Transaction& tx,
    const core::Transaction& delegation_proof_tx,
    uint64_t fee
) const {
    // Same debit as ChainState: outgoing payments plus the actual miner fee.
    // Removal uses the fee saved at admission, since confirmation consumes
    // the inputs and makes recalculating their value impossible.
    if (fee > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) return -1;
    Amount amount_spent = static_cast<Amount>(fee);
    for (const auto& output : tx.outputs) {
        if (output.address.str() != delegation_proof_tx.primary_multisig_address.str()) {
            Amount next_amount = 0;
            if (output.value < 0 || !checked_add_amount(amount_spent, output.value, next_amount)) {
                return -1;
            }
            amount_spent = next_amount;
        }
    }
    return amount_spent;
}

bool Mempool::reserve_delegation_budget_locked(
    const MempoolEntry& entry, std::unordered_map<std::string, Amount>& pending,
    const core::Transaction* proof
) const {
    if (entry.tx.type != core::TransactionType::MULTISIG_SPEND ||
        is_null_hash256(entry.tx.delegation_proof_txid)) return true;
    if (!chain_state_ || entry.delegation_debit < 0) return false;

    std::unique_ptr<core::Transaction> loaded_proof;
    if (!proof) {
        loaded_proof = chain_state_->get_transaction(entry.tx.delegation_proof_txid);
        proof = loaded_proof.get();
    }
    if (!proof || proof->type != core::TransactionType::DELEGATION_PROOF ||
        chain_state_->is_delegation_revoked(entry.tx.delegation_proof_txid)) return false;
    const auto key = hash256_to_hex(entry.tx.delegation_proof_txid);
    const auto found = pending.find(key);
    const Amount reserved = found == pending.end() ? 0 : found->second;
    const Amount confirmed = chain_state_->get_delegation_total_spent(entry.tx.delegation_proof_txid);
    Amount next = 0;
    Amount total = 0;
    if (reserved < 0 || confirmed < 0 ||
        !checked_add_amount(reserved, entry.delegation_debit, next) ||
        !checked_add_amount(confirmed, next, total) ||
        (proof->max_total_delegated > 0 && total > proof->max_total_delegated)) return false;
    pending[key] = next;
    return true;
}

Mempool::Mempool(std::shared_ptr<state::ChainState> chain_state)
    : chain_state_(chain_state)
    , max_size_(300 * 1024 * 1024)  // 300 МБ
    , current_size_(0)
    , transactions_updated_(0)
{
}

Mempool::~Mempool() {
    clear();
}

bool Mempool::add_transaction(const core::Transaction& tx) {
    Hash256 tx_hash{};
    try {
        tx_hash = tx.get_hash();
    } catch (...) {
        record_reject(false, "malformed-serialization");
        return false;
    }

    // The global commitment count is enforced by eviction at insertion, after
    // verification, so an invalid commitment can never displace a valid one.
    auto protocol_capacity_available_locked = [&](bool enforce_global_angle_cap) -> bool {
        if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
            const auto per_spend = pending_angle_commitments_by_spend_.find(tx.spending_tx_hash);
            const size_t per_spend_count = per_spend == pending_angle_commitments_by_spend_.end()
                ? 0
                : per_spend->second;
            if ((enforce_global_angle_cap &&
                 angle_commitment_keys_.size() >= kMaxPendingAngleCommitments) ||
                per_spend_count >= kMaxPendingAngleCommitmentsPerSpend) {
                reject_policy_["angle-commitment-capacity"]++;
                return false;
            }
        } else if (tx.type == core::TransactionType::DELEGATION_PROOF &&
                   delegation_proof_keys_.size() >= kMaxPendingDelegationProofs) {
            reject_policy_["delegation-proof-capacity"]++;
            return false;
        }
        if (consumes_angle_commitments(tx)) {
            for (const auto& hash : tx.angle_commitment_hashes) {
                if (claimed_angle_commitments_.count(hash) > 0) {
                    reject_policy_["angle-commitment-already-claimed"]++;
                    return false;
                }
            }
        }
        return true;
    };

    // Replays, protocol-class quotas and obvious double-spends are cheap to
    // identify. Do this before Falcon/state validation, then repeat it under
    // the insertion lock below to close races with concurrent admission.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (transactions_.find(tx_hash) != transactions_.end() ||
            has_conflicts(tx) ||
            !protocol_capacity_available_locked(false)) {
            return false;
        }
    }

    // Validation below intentionally happens without the mempool mutex. Tie
    // its result to one ChainState tip so a block cannot be connected between
    // validation and insertion without either rejecting this add or running
    // the post-connect cleanup after the insertion.
    const Hash256 validated_tip = chain_state_ ? chain_state_->get_tip_hash_snapshot() : Hash256{};

    // Верифицируем транзакцию без блокировки mempool, чтобы не держать mutex
    // на потенциально длительных проверках (chainstate/криптография).
    if (!verify_transaction(tx)) {
        return false;
    }

    // Вычисляем комиссию/размер без блокировки.
    const uint64_t fee = calculate_fee(tx);
    size_t tx_size = 0;
    try {
        tx_size = tx.serialize().size();
    } catch (...) {
        record_reject(false, "malformed-serialization");
        return false;
    }
    const uint64_t fee_per_byte = tx_size > 0 ? fee / tx_size : 0;

    std::lock_guard<std::mutex> lock(mutex_);

    if (chain_state_ && chain_state_->get_tip_hash_snapshot() != validated_tip) {
        reject_consensus_["tip-changed"]++;
        return false;
    }

    // Проверяем, нет ли уже в mempool (повторная проверка после verify).
    if (transactions_.find(tx_hash) != transactions_.end()) {
        return false;
    }

    // Проверяем конфликты
    if (has_conflicts(tx)) {
        return false;
    }
    if (!protocol_capacity_available_locked(false)) {
        return false;
    }
    if (tx.type == core::TransactionType::ANGLE_COMMITMENT &&
        (angle_commitment_keys_.size() >= kMaxPendingAngleCommitments ||
         unclaimed_angle_commitment_count_locked() >= kMaxUnclaimedAngleCommitments) &&
        !evict_oldest_unclaimed_angle_commitment_locked()) {
        reject_policy_["angle-commitment-capacity"]++;
        return false;
    }
    if (!protocol_capacity_available_locked(true)) {
        return false;
    }

    // Проверяем размер mempool
    const bool exceeds_limit = tx_size > max_size_ || current_size_ > max_size_ ||
        current_size_ > max_size_ - tx_size;
    if (exceeds_limit) {
        // A lower/equal-fee transaction must never evict a more valuable one;
        // this also prevents zero-fee protocol messages from flushing a full
        // payment mempool.
        if (tx_size > max_size_ || fee_index_.empty() ||
            fee_per_byte <= std::prev(fee_index_.end())->first) {
            return false;
        }

        // Пытаемся вытеснить транзакции с низкими комиссиями
        const size_t target_current_size = max_size_ - tx_size;
        const size_t bytes_needed = current_size_ - target_current_size;
        const std::set<Hash256> required_commitments(
            tx.angle_commitment_hashes.begin(), tx.angle_commitment_hashes.end());
        if (!evict_low_fee_transactions(bytes_needed, fee_per_byte, required_commitments)) {
            return false;
        }

        if (tx_size > max_size_ || current_size_ > max_size_ - tx_size) {
            return false;
        }
    }

    // Recheck and reserve hierarchical delegation totals under the mempool
    // mutex. The earlier cryptographic/state validation runs unlocked, so two
    // concurrent spends must not both pass using the same pending snapshot.
    std::string hierarchical_dp_key;
    Amount hierarchical_debit = 0;
    Amount hierarchical_pending_total = 0;
    bool reserve_hierarchical_total = false;
    if (tx.type == core::TransactionType::MULTISIG_SPEND &&
        !is_null_hash256(tx.delegation_proof_txid)) {
        const auto dp_tx = chain_state_->get_transaction(tx.delegation_proof_txid);
        if (!dp_tx) {
            reject_consensus_["delegation-not-found"]++;
            return false;
        }

        hierarchical_dp_key = hash256_to_hex(tx.delegation_proof_txid);
        hierarchical_debit = hierarchical_amount_spent_locked(tx, *dp_tx, fee);
        const auto pending_it = mempool_delegation_spent_.find(hierarchical_dp_key);
        const Amount already_pending =
            pending_it == mempool_delegation_spent_.end() ? 0 : pending_it->second;
        if (hierarchical_debit < 0 || already_pending < 0 ||
            !checked_add_amount(already_pending, hierarchical_debit, hierarchical_pending_total)) {
            reject_policy_["max-total-delegated"]++;
            return false;
        }

        if (dp_tx->max_total_delegated > 0) {
            const Amount already_confirmed =
                chain_state_->get_delegation_total_spent(tx.delegation_proof_txid);
            Amount combined_total = 0;
            if (already_confirmed < 0 ||
                !checked_add_amount(already_confirmed, hierarchical_pending_total, combined_total) ||
                combined_total > dp_tx->max_total_delegated) {
                reject_policy_["max-total-delegated"]++;
                return false;
            }
        }
        reserve_hierarchical_total = true;
    }

    // Создаем entry
    MempoolEntry entry;
    entry.tx = tx;
    entry.fee = fee;
    entry.fee_per_byte = fee_per_byte;
    entry.added_at = std::chrono::steady_clock::now();
    entry.size = tx_size;
    entry.delegation_debit = hierarchical_debit;

    // Добавляем в mempool
    transactions_[tx_hash] = entry;
    fee_estimator_.transaction_added(tx_hash, fee, tx_size, chain_state_ ? chain_state_->get_height() : 0);
    fee_index_.insert({fee_per_byte, tx_hash});
    if (const auto authorization = tx.get_protocol_authorization_hash()) {
        protocol_authorization_keys_.insert(*authorization);
    }

    // Обновляем anti-replay индексы mempool
    if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        if (!tx.inputs.empty()) {
            angle_commitment_keys_.insert(tx.inputs[0].prev_tx_hash);
        }
        pending_angle_commitments_by_spend_[tx.spending_tx_hash]++;
    } else if (tx.type == core::TransactionType::MULTISIG_SPEND) {
        if (!is_null_hash256(tx.delegation_proof_txid)) {
            hierarchical_spend_keys_.insert(tx.get_hierarchical_spend_hash());

            // Учет max_total_delegated на уровне mempool.
            if (reserve_hierarchical_total) {
                mempool_delegation_spent_[hierarchical_dp_key] = hierarchical_pending_total;
            }
        } else {
            multisig_spend_keys_.insert(tx.get_hash_without_commitments());
        }
    } else if (tx.type == core::TransactionType::DELEGATION_PROOF) {
        delegation_proof_keys_.insert(tx.get_delegation_proof_hash());
        delegation_nonces_[tx.primary_multisig_address.str()].insert(tx.nonce);
    } else if (tx.type == core::TransactionType::REVOKE_DELEGATION) {
        revoke_delegation_keys_.insert(tx.revoked_delegation_proof_txid);
    } else if (tx.type == core::TransactionType::NFT_CREATE) {
        if (tx.nft_create.has_value()) {
            nft_id_keys_.insert(tx.nft_create->nft_id);
        }
    }
    if (consumes_angle_commitments(tx)) {
        claimed_angle_commitments_.insert(
            tx.angle_commitment_hashes.begin(), tx.angle_commitment_hashes.end());
    }

    // ВАЖНО: Для ANGLE_COMMITMENT добавляем в angle_store сразу!
    // Иначе MULTISIG_SPEND не сможет найти commitments в mempool
    if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        chain_state_->add_angle_commitment_to_store(tx);
    }

    // Отмечаем использованные выходы только для транзакций с реальными inputs.
    // REVOKE_DELEGATION также использует общий синтетический marker-input.
    if (has_real_utxo_inputs(tx)) {
        for (const auto& input : tx.inputs) {
            spent_outputs_[input.prev_tx_hash].insert(input.vout);
        }
    }

    current_size_ += tx_size;

    transactions_updated_++;  // Увеличиваем счетчик обновлений

    // Помечаем транзакцию для relay (как в Bitcoin)
    {
        std::lock_guard<std::mutex> relay_lock(relay_mutex_);
        new_transactions_.insert(tx_hash);
    }

    return true;
}

bool Mempool::test_accept_transaction(const core::Transaction& tx, std::string* reject_reason) {
    auto reject = [reject_reason](const char* reason) {
        if (reject_reason) {
            *reject_reason = reason;
        }
        return false;
    };

    Hash256 tx_hash{};
    size_t tx_size = 0;
    try {
        tx_hash = tx.get_hash();
        tx_size = tx.serialize().size();
    } catch (...) {
        return reject("malformed-serialization");
    }

    auto protocol_capacity_available_locked = [&]() -> bool {
        if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
            const auto per_spend = pending_angle_commitments_by_spend_.find(tx.spending_tx_hash);
            const size_t per_spend_count = per_spend == pending_angle_commitments_by_spend_.end()
                ? 0
                : per_spend->second;
            if (((angle_commitment_keys_.size() >= kMaxPendingAngleCommitments ||
                  unclaimed_angle_commitment_count_locked() >= kMaxUnclaimedAngleCommitments) &&
                 !oldest_unclaimed_angle_commitment_locked()) ||
                per_spend_count >= kMaxPendingAngleCommitmentsPerSpend) {
                return false;
            }
        } else if (tx.type == core::TransactionType::DELEGATION_PROOF &&
                   delegation_proof_keys_.size() >= kMaxPendingDelegationProofs) {
            return false;
        }
        if (consumes_angle_commitments(tx)) {
            for (const auto& hash : tx.angle_commitment_hashes) {
                if (claimed_angle_commitments_.count(hash) > 0) {
                    return false;
                }
            }
        }
        return true;
    };

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (transactions_.find(tx_hash) != transactions_.end()) {
            return reject("txn-already-in-mempool");
        }
        if (has_conflicts(tx)) {
            return reject("txn-mempool-conflict");
        }
        if (!protocol_capacity_available_locked()) {
            return reject("mempool-protocol-capacity");
        }
    }

    const Hash256 validated_tip = chain_state_ ? chain_state_->get_tip_hash_snapshot() : Hash256{};
    if (!verify_transaction(tx, false)) {
        return reject("mempool-policy-or-consensus-reject");
    }
    const uint64_t fee = calculate_fee(tx);
    const uint64_t fee_per_byte = tx_size > 0 ? fee / tx_size : 0;

    std::lock_guard<std::mutex> lock(mutex_);
    if (chain_state_ && chain_state_->get_tip_hash_snapshot() != validated_tip) {
        return reject("tip-changed");
    }
    if (transactions_.find(tx_hash) != transactions_.end()) {
        return reject("txn-already-in-mempool");
    }
    if (has_conflicts(tx)) {
        return reject("txn-mempool-conflict");
    }
    if (!protocol_capacity_available_locked()) {
        return reject("mempool-protocol-capacity");
    }

    const bool exceeds_limit = tx_size > max_size_ || current_size_ > max_size_ ||
        current_size_ > max_size_ - tx_size;
    if (exceeds_limit) {
        if (tx_size > max_size_ || fee_index_.empty() ||
            fee_per_byte <= std::prev(fee_index_.end())->first) {
            return reject("mempool-full");
        }
        const size_t target_current_size = max_size_ - tx_size;
        const size_t bytes_needed = current_size_ - target_current_size;
        const std::set<Hash256> required_commitments(
            tx.angle_commitment_hashes.begin(), tx.angle_commitment_hashes.end());
        if (plan_eviction_locked(bytes_needed, fee_per_byte, required_commitments).empty()) {
            return reject("mempool-full");
        }
    }

    if (reject_reason) {
        reject_reason->clear();
    }
    return true;
}

bool Mempool::remove_transaction(const Hash256& tx_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool ok = remove_transaction_locked(tx_hash);
    if (ok) {
        transactions_updated_++;
    }
    return ok;
}

bool Mempool::remove_transaction_locked(const Hash256& tx_hash) {
    auto it = transactions_.find(tx_hash);
    if (it == transactions_.end()) {
        return false;
    }
    fee_estimator_.transaction_removed(tx_hash);

    // ANGLE_COMMITMENT is a hard dependency of every spend that references
    // it. Removing/evicting the commitment while retaining the spend used to
    // let the miner build a block that the same node would later reject.
    if (it->second.tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        std::vector<Hash256> dependents;
        for (const auto& [candidate_hash, candidate] : transactions_) {
            if (!consumes_angle_commitments(candidate.tx)) {
                continue;
            }
            if (std::find(
                    candidate.tx.angle_commitment_hashes.begin(),
                    candidate.tx.angle_commitment_hashes.end(),
                    tx_hash) != candidate.tx.angle_commitment_hashes.end()) {
                dependents.push_back(candidate_hash);
            }
        }
        for (const auto& dependent_hash : dependents) {
            (void)remove_transaction_locked(dependent_hash);
        }
        it = transactions_.find(tx_hash);
        if (it == transactions_.end()) {
            return true;
        }
    }

    const auto& entry = it->second;

    if (const auto authorization = entry.tx.get_protocol_authorization_hash()) {
        protocol_authorization_keys_.erase(*authorization);
    }

    if (consumes_angle_commitments(entry.tx)) {
        for (const auto& hash : entry.tx.angle_commitment_hashes) {
            claimed_angle_commitments_.erase(hash);
        }
    }

    // Удаляем из anti-replay индексов mempool + bookkeeping.
    if (entry.tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        if (!entry.tx.inputs.empty()) {
            angle_commitment_keys_.erase(entry.tx.inputs[0].prev_tx_hash);
        }
        if (chain_state_) {
            chain_state_->remove_mempool_angle_commitment_from_store(tx_hash);
        }
        auto count_it = pending_angle_commitments_by_spend_.find(entry.tx.spending_tx_hash);
        if (count_it != pending_angle_commitments_by_spend_.end()) {
            if (count_it->second <= 1) {
                pending_angle_commitments_by_spend_.erase(count_it);
            } else {
                --count_it->second;
            }
        }
    } else if (entry.tx.type == core::TransactionType::MULTISIG_SPEND) {
        if (!is_null_hash256(entry.tx.delegation_proof_txid)) {
            hierarchical_spend_keys_.erase(entry.tx.get_hierarchical_spend_hash());

            // Release the exact reservation even if the proof has been disconnected.
            const std::string dp_key = hash256_to_hex(entry.tx.delegation_proof_txid);
            const Amount amt = entry.delegation_debit;
            auto it_amt = mempool_delegation_spent_.find(dp_key);
            if (it_amt != mempool_delegation_spent_.end()) {
                Amount remaining = 0;
                if (it_amt->second > amt && amt >= 0 &&
                    checked_sub_amount(it_amt->second, amt, remaining)) {
                    it_amt->second = remaining;
                } else {
                    mempool_delegation_spent_.erase(it_amt);
                }
            }
        } else {
            multisig_spend_keys_.erase(entry.tx.get_hash_without_commitments());
        }
    } else if (entry.tx.type == core::TransactionType::DELEGATION_PROOF) {
        delegation_proof_keys_.erase(entry.tx.get_delegation_proof_hash());
        const auto nonces = delegation_nonces_.find(entry.tx.primary_multisig_address.str());
        if (nonces != delegation_nonces_.end()) {
            nonces->second.erase(entry.tx.nonce);
            if (nonces->second.empty()) {
                delegation_nonces_.erase(nonces);
            }
        }
    } else if (entry.tx.type == core::TransactionType::REVOKE_DELEGATION) {
        revoke_delegation_keys_.erase(entry.tx.revoked_delegation_proof_txid);
    } else if (entry.tx.type == core::TransactionType::NFT_CREATE) {
        if (entry.tx.nft_create.has_value()) {
            nft_id_keys_.erase(entry.tx.nft_create->nft_id);
        }
    }

    // Удаляем из индекса комиссий
    auto range = fee_index_.equal_range(entry.fee_per_byte);
    for (auto fee_it = range.first; fee_it != range.second; ++fee_it) {
        if (fee_it->second == tx_hash) {
            fee_index_.erase(fee_it);
            break;
        }
    }

    // Удаляем использованные выходы только для транзакций с реальными inputs.
    if (has_real_utxo_inputs(entry.tx)) {
        for (const auto& input : entry.tx.inputs) {
            auto it_spent = spent_outputs_.find(input.prev_tx_hash);
            if (it_spent != spent_outputs_.end()) {
                it_spent->second.erase(input.vout);
                if (it_spent->second.empty()) {
                    spent_outputs_.erase(it_spent);
                }
            }
        }
    }

    // КРИТИЧНО: Проверяем на underflow
    if (entry.size > current_size_) {
        GEO_LOG_ERROR(
            "mempool",
            "Mempool underflow: trying to remove " << entry.size << " bytes, but mempool has " << current_size_
        );
        current_size_ = 0;
    } else {
        current_size_ -= entry.size;
    }

    transactions_.erase(it);
    {
        std::lock_guard<std::mutex> relay_lock(relay_mutex_);
        new_transactions_.erase(tx_hash);
    }
    return true;
}

void Mempool::record_reject(bool policy, const std::string& reason) {
    const std::string key = reason.empty() ? "unknown" : reason;
    std::lock_guard<std::mutex> lock(mutex_);
    if (policy) {
        reject_policy_[key]++;
    } else {
        reject_consensus_[key]++;
    }
}

bool Mempool::has_transaction(const Hash256& tx_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transactions_.find(tx_hash) != transactions_.end();
}

bool Mempool::get_transaction(const Hash256& tx_hash, core::Transaction& tx) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = transactions_.find(tx_hash);
    if (it == transactions_.end()) {
        return false;
    }

    tx = it->second.tx;
    return true;
}

std::vector<core::Transaction> Mempool::get_transactions_for_mining(
    size_t max_size,
    std::optional<Timestamp> block_time
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (max_size == 0 || transactions_.empty()) return {};
    const Timestamp evaluation_time = block_time ? *block_time : static_cast<Timestamp>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    std::vector<core::Transaction> result;
    size_t total_size = 0;
    std::set<Hash256> included;
    std::set<Hash256> consumed_commitments;
    std::unordered_map<std::string, Amount> selected_delegation_spent;
    std::unordered_map<std::string, Amount> balance_deltas;
    std::unordered_map<std::string, Amount> initial_balances;
    std::set<Hash256> selected_revocations;
    std::set<std::pair<std::string, uint32_t>> selected_delegation_nonces;
    std::unique_ptr<state::SidechainProjection> sidechain_projection;
    std::unique_ptr<state::CardProjection> card_projection;
    std::unique_ptr<state::NFTProjection> nft_projection;
    std::optional<Address> selected_foundation_address;
    std::set<std::string> percentage_owners;
    // Resolve each referenced proof once for this selection, including misses.
    // Reuse it for time checks, percentage limits and the aggregate budget.
    std::map<Hash256, std::unique_ptr<core::Transaction>> delegation_proofs;
    const auto find_proof = [&](const Hash256& hash) -> const core::Transaction* {
        const auto [it, inserted] = delegation_proofs.try_emplace(hash);
        if (inserted && chain_state_) it->second = chain_state_->get_transaction(hash);
        return it->second.get();
    };
    if (chain_state_ && !mempool_delegation_spent_.empty()) {
        std::set<Hash256> checked_proofs;
        for (const auto& [hash, entry] : transactions_) {
            (void)hash;
            if (entry.tx.type != core::TransactionType::MULTISIG_SPEND ||
                is_null_hash256(entry.tx.delegation_proof_txid) ||
                !checked_proofs.insert(entry.tx.delegation_proof_txid).second) continue;
            const auto* proof = find_proof(entry.tx.delegation_proof_txid);
            if (proof && proof->max_percent_of_balance > 0) {
                percentage_owners.insert(proof->primary_multisig_address.str());
            }
        }
    }

    auto can_add_size = [max_size, &total_size](size_t size) -> bool {
        return size <= max_size && total_size <= max_size - size;
    };

    auto append_transaction = [&](const Hash256& tx_hash) -> bool {
        if (included.count(tx_hash) > 0) {
            return true;
        }

        auto it = transactions_.find(tx_hash);
        if (it == transactions_.end()) {
            return false;
        }

        const auto& entry = it->second;
        if (!can_add_size(entry.size)) {
            return false;
        }

        result.push_back(entry.tx);
        total_size += entry.size;
        included.insert(tx_hash);
        return true;
    };

    auto angle_package_fits = [&](const MempoolEntry& root) -> bool {
        size_t required_size = root.size;

        for (const auto& commitment_hash : root.tx.angle_commitment_hashes) {
            if (included.count(commitment_hash) > 0 ||
                consumed_commitments.count(commitment_hash) > 0) {
                return false;
            }

            auto dep_it = transactions_.find(commitment_hash);
            if (dep_it == transactions_.end() ||
                dep_it->second.tx.type != core::TransactionType::ANGLE_COMMITMENT) {
                return false;
            }

            const size_t dep_size = dep_it->second.size;
            if (dep_size > max_size || required_size > max_size - dep_size) {
                return false;
            }
            required_size += dep_size;
        }

        return required_size <= max_size && total_size <= max_size - required_size;
    };

    auto reserve_transaction_effects = [&](const MempoolEntry& entry) -> bool {
        const auto& tx = entry.tx;
        const core::Transaction* proof = nullptr;
        if (tx.type == core::TransactionType::MULTISIG_SPEND &&
            !is_null_hash256(tx.delegation_proof_txid)) {
            if (selected_revocations.count(tx.delegation_proof_txid) > 0) return false;
            proof = find_proof(tx.delegation_proof_txid);
            if (!proof || !consensus::is_delegated_spend_valid_at(tx, *proof, evaluation_time)) return false;
            if (proof->max_percent_of_balance > 0) {
                const auto& owner = proof->primary_multisig_address;
                auto initial = initial_balances.find(owner.str());
                if (initial == initial_balances.end()) {
                    initial = initial_balances.emplace(owner.str(), chain_state_->get_balance(owner)).first;
                }
                const auto delta = balance_deltas.find(owner.str());
                Amount balance = 0;
                if (initial->second < 0 || proof->max_percent_of_balance > 100 ||
                    !checked_add_amount(initial->second,
                        delta == balance_deltas.end() ? 0 : delta->second, balance) || balance < 0 ||
                    entry.delegation_debit < 0 || static_cast<uint64_t>(entry.delegation_debit) >
                        utils::mul_div_u64_floor(static_cast<uint64_t>(balance),
                                                proof->max_percent_of_balance, 100)) return false;
            }
        }
        if (!chain_state_) return true;

        // Project only touched addresses. Read a complete address balance only
        // when a percentage-limited delegation actually needs it.
        std::unordered_map<std::string, Amount> changes;
        const auto change_balance = [&](const Address& owner, Amount value, bool debit) {
            if (value < 0) return false;
            if (value == 0 || percentage_owners.count(owner.str()) == 0) return true;
            auto it = changes.find(owner.str());
            if (it == changes.end()) {
                const auto previous = balance_deltas.find(owner.str());
                it = changes.emplace(owner.str(), previous == balance_deltas.end() ? 0 : previous->second).first;
            }
            return debit ? checked_sub_amount(it->second, value, it->second) :
                           checked_add_amount(it->second, value, it->second);
        };
        if (!percentage_owners.empty() && has_real_utxo_inputs(tx)) {
            for (const auto& input : tx.inputs) {
                state::UTXO utxo;
                if (!chain_state_->get_utxo(input.prev_tx_hash, input.vout, utxo) ||
                    !change_balance(utxo.address, utxo.value, true)) return false;
            }
        }
        for (const auto& output : tx.outputs) {
            if (!change_balance(output.address, output.value, false)) return false;
        }
        if (!reserve_delegation_budget_locked(entry, selected_delegation_spent, proof)) return false;
        if (core::is_sidechain_transition_type(tx.type)) {
            if (!sidechain_projection) {
                const Height height = chain_state_->get_height();
                if (height == (std::numeric_limits<Height>::max)()) return false;
                sidechain_projection = std::make_unique<state::SidechainProjection>(*chain_state_, height + 1);
            }
            if (!sidechain_projection->try_apply(tx)) return false;
        }
        if (core::is_card_transition_type(tx.type)) {
            if (!card_projection) {
                const Height height = chain_state_->get_height();
                if (height == (std::numeric_limits<Height>::max)()) return false;
                card_projection = std::make_unique<state::CardProjection>(*chain_state_, height + 1);
            }
            if (!card_projection->try_apply(tx)) return false;
        }
        if (core::is_nft_transition_type(tx.type) || tx.type == core::TransactionType::CADASTRAL_DOC) {
            if (!nft_projection) nft_projection = std::make_unique<state::NFTProjection>(*chain_state_);
            if (!nft_projection->try_apply(tx)) return false;
        }
        if (tx.type == core::TransactionType::UPDATE_FOUNDATION_ADDRESS) {
            if (!selected_foundation_address) selected_foundation_address = chain_state_->get_foundation_address();
            // Admission verified the complete signed old/new policies. Their
            // addresses commit the quorum and members, so only the current
            // authority changes during selection. No-op rotations remain valid.
            if (tx.foundation_old_address != *selected_foundation_address) return false;
            selected_foundation_address = tx.foundation_new_address;
        }
        for (const auto& [owner, delta] : changes) balance_deltas[owner] = delta;
        if (tx.type == core::TransactionType::REVOKE_DELEGATION) {
            selected_revocations.insert(tx.revoked_delegation_proof_txid);
        }
        return true;
    };

    // Берем транзакции с наибольшими комиссиями
    for (const auto& [fee_per_byte, tx_hash] : fee_index_) {
        (void)fee_per_byte;
        if (included.count(tx_hash) > 0) {
            continue;
        }

        auto it = transactions_.find(tx_hash);
        if (it == transactions_.end()) {
            continue;
        }

        const auto& entry = it->second;

        // A commitment is never mined by itself: consensus requires every one
        // to precede exactly one consuming root in this same block. Reward and
        // distribution transactions are synthesized by the block assembler.
        if (entry.tx.is_coinbase() || entry.tx.type == core::TransactionType::ANGLE_COMMITMENT ||
            entry.tx.type == core::TransactionType::INTEREST_DISTRIBUTION) {
            continue;
        }
        if (chain_state_ && !MempoolPolicy::check(
                entry.tx, *chain_state_, entry.fee, policy_config_, nullptr, evaluation_time)) {
            continue;
        }

        if (consumes_angle_commitments(entry.tx)) {
            // Block application records each (primary address, nonce) once, so
            // a second proof with the same nonce would invalidate the block.
            const bool is_delegation_proof =
                entry.tx.type == core::TransactionType::DELEGATION_PROOF;
            const std::pair<std::string, uint32_t> proof_nonce{
                entry.tx.primary_multisig_address.str(), entry.tx.nonce};
            if (is_delegation_proof && selected_delegation_nonces.count(proof_nonce) > 0) {
                continue;
            }
            if (!angle_package_fits(entry) || !reserve_transaction_effects(entry)) {
                continue;
            }

            for (const auto& commitment_hash : entry.tx.angle_commitment_hashes) {
                auto dep_it = transactions_.find(commitment_hash);
                if (dep_it != transactions_.end() &&
                    dep_it->second.tx.type == core::TransactionType::ANGLE_COMMITMENT) {
                    append_transaction(commitment_hash);
                }
            }
            if (!append_transaction(tx_hash)) {
                continue;
            }
            consumed_commitments.insert(
                entry.tx.angle_commitment_hashes.begin(),
                entry.tx.angle_commitment_hashes.end()
            );
            if (is_delegation_proof) {
                selected_delegation_nonces.insert(proof_nonce);
            }
            continue;
        }

        if (can_add_size(entry.size) && reserve_transaction_effects(entry)) {
            append_transaction(tx_hash);
        }
    }

    return result;
}

std::vector<core::Transaction> Mempool::get_all_transactions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<core::Transaction> result;
    result.reserve(transactions_.size());

    for (const auto& [hash, entry] : transactions_) {
        result.push_back(entry.tx);
    }

    return result;
}

std::vector<Hash256> Mempool::get_all_transaction_hashes() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Hash256> result;
    result.reserve(transactions_.size());
    for (const auto& [hash, entry] : transactions_) {
        (void)entry;
        result.push_back(hash);
    }
    return result;
}

std::vector<MempoolEntry> Mempool::get_all_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<MempoolEntry> result;
    result.reserve(transactions_.size());

    for (const auto& [hash, entry] : transactions_) {
        (void)hash;
        result.push_back(entry);
    }

    return result;
}

std::vector<MempoolEntry> Mempool::get_entries_page(
    size_t offset,
    size_t limit,
    size_t* total
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (total) {
        *total = transactions_.size();
    }
    std::vector<MempoolEntry> result;
    if (limit == 0 || offset >= transactions_.size()) {
        return result;
    }

    const size_t available = transactions_.size() - offset;
    result.reserve(std::min(limit, available));
    auto it = transactions_.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(offset));
    while (it != transactions_.end() && result.size() < limit) {
        result.push_back(it->second);
        ++it;
    }
    return result;
}

bool Mempool::get_entry(const Hash256& tx_hash, MempoolEntry& entry) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = transactions_.find(tx_hash);
    if (it == transactions_.end()) {
        return false;
    }
    entry = it->second;
    return true;
}

std::set<Hash256> Mempool::collect_descendants_locked(std::set<Hash256> roots) const {
    if (roots.empty()) return roots;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [hash, entry] : transactions_) {
            if (roots.count(hash) > 0) {
                continue;
            }

            if (consumes_angle_commitments(entry.tx)) {
                const bool depends_on_removed_commitment = std::any_of(
                    entry.tx.angle_commitment_hashes.begin(),
                    entry.tx.angle_commitment_hashes.end(),
                    [&](const Hash256& commitment_hash) {
                        return roots.count(commitment_hash) > 0;
                    }
                );
                if (depends_on_removed_commitment) {
                    roots.insert(hash);
                    changed = true;
                    continue;
                }
            }

            if (!has_real_utxo_inputs(entry.tx)) {
                continue;
            }
            for (const auto& input : entry.tx.inputs) {
                if (roots.count(input.prev_tx_hash) > 0) {
                    roots.insert(hash);
                    changed = true;
                    break;
                }
            }
        }
    }
    return roots;
}

void Mempool::note_block_effects_locked(const core::Block& block, RevalidationScope& scope) const {
    for (size_t i = 0; i < block.transactions.size(); ++i) {
        const auto& tx = block.transactions[i];
        scope.created_txids.insert(tx.get_hash());
        if (i == 0 && tx.is_coinbase()) {
            continue;
        }
        if (has_real_utxo_inputs(tx)) {
            for (const auto& input : tx.inputs) {
                scope.touched_outpoints.emplace(input.prev_tx_hash, input.vout);
            }
        }
        if (tx.type == core::TransactionType::REVOKE_DELEGATION ||
            tx.type == core::TransactionType::DELEGATION_PROOF ||
            (tx.type == core::TransactionType::MULTISIG_SPEND &&
             !is_null_hash256(tx.delegation_proof_txid))) {
            scope.delegation_changed = true;
        }
        if (tx.type != core::TransactionType::STANDARD) {
            scope.registry_changed = true;
        }
    }
}

bool Mempool::entry_needs_recheck_locked(
    const MempoolEntry& entry,
    const RevalidationScope& scope
) const {
    if (scope.full || entry.tx.lock_time != 0) {
        return true;
    }
    const bool delegated_spend =
        entry.tx.type == core::TransactionType::MULTISIG_SPEND &&
        !is_null_hash256(entry.tx.delegation_proof_txid);
    if (scope.delegation_changed && delegated_spend) {
        return true;
    }
    if (scope.registry_changed && entry.tx.type != core::TransactionType::STANDARD) {
        return true;
    }
    if (!has_real_utxo_inputs(entry.tx)) {
        return false;
    }
    for (const auto& input : entry.tx.inputs) {
        if (scope.touched_outpoints.count({input.prev_tx_hash, input.vout}) > 0 ||
            scope.created_txids.count(input.prev_tx_hash) > 0) {
            return true;
        }
    }
    return false;
}

void Mempool::revalidate_locked(const RevalidationScope& scope) {
    // Height, revocation and registry changes can invalidate survivors without
    // consuming their inputs. Apply admission policy and chain validation at
    // the final active tip, then remove dependents of invalid transactions.
    if (chain_state_) {
        auto valid_on_active_tip = [this](const MempoolEntry& entry) {
            const auto& tx = entry.tx;
            if (!MempoolPolicy::check(tx, *chain_state_, entry.fee, policy_config_, nullptr)) return false;
            if (!chain_state_->validate_transaction(tx)) {
                return false;
            }

            consensus::ValidationState validation_state;
            consensus::ContextualTxLookup lookup;
            lookup.get_transaction_by_txid = [this](const Hash256& hash, core::Transaction& out_tx) {
                const auto tx_ptr = chain_state_->get_transaction(hash);
                if (!tx_ptr) {
                    return false;
                }
                out_tx = *tx_ptr;
                return true;
            };
            lookup.has_angle_commitment_key = [this](const Hash256& key) {
                return chain_state_->has_angle_commitment_key(key);
            };
            lookup.has_multisig_spend_key = [this](const Hash256& key) {
                return chain_state_->has_multisig_spend_key(key);
            };
            lookup.has_hierarchical_spend_key = [this](const Hash256& key) {
                return chain_state_->has_hierarchical_spend_key(key);
            };
            lookup.has_delegation_proof_key = [this](const Hash256& key) {
                return chain_state_->has_delegation_proof_key(key);
            };
            lookup.has_revoke_delegation_key = [this](const Hash256& key) {
                return chain_state_->has_revoke_delegation_key(key);
            };
            lookup.has_nft_id_key = [this](const Hash256& key) {
                return chain_state_->has_nft_id_key(key);
            };
            lookup.has_sidechain_deposit_key = [this](const Hash256& key) {
                return chain_state_->has_sidechain_deposit_key(key);
            };
            lookup.has_sidechain_registry_key = [this](const Hash256& key) {
                return chain_state_->has_sidechain_registry_key(key);
            };
            lookup.signatures_verified = true;
            return consensus::contextual_check_transaction(tx, validation_state, lookup);
        };

        std::set<Hash256> invalid_roots;
        for (const auto& [hash, entry] : transactions_) {
            if (!entry_needs_recheck_locked(entry, scope)) {
                continue;
            }
            if (!valid_on_active_tip(entry)) {
                invalid_roots.insert(hash);
            }
        }
        invalid_roots = collect_descendants_locked(std::move(invalid_roots));
        // An external confirmed spend can leave each survivor individually
        // valid while their combined debit exceeds the remaining allowance.
        // Retain the same fee priority used by mining selection.
        std::unordered_map<std::string, Amount> pending_debits;
        if (!scope.full && !scope.delegation_changed) {
            pending_debits.clear();
        }
        for (const auto& [rate, hash] : fee_index_) {
            if (!scope.full && !scope.delegation_changed) {
                break;
            }
            (void)rate;
            const auto it = transactions_.find(hash);
            if (it == transactions_.end() || invalid_roots.count(hash) > 0) continue;
            if (!reserve_delegation_budget_locked(it->second, pending_debits)) {
                invalid_roots.insert(hash);
            }
        }
        const auto invalid_and_descendants = collect_descendants_locked(std::move(invalid_roots));
        for (const auto& hash : invalid_and_descendants) {
            if (remove_transaction_locked(hash)) {
                transactions_updated_++;
            }
        }
    }
}

void Mempool::reconcile_with_chain(
    const std::vector<core::Block>& connected,
    bool had_disconnect
) {
    std::lock_guard<std::mutex> lock(mutex_);
    RevalidationScope scope;
    scope.full = had_disconnect;
    const Height final_height = chain_state_ ? chain_state_->get_height() : connected.size();
    const Height fork_height = final_height >= connected.size() ? final_height - connected.size() : 0;
    if (had_disconnect) fee_estimator_.reorganize(fork_height);
    Height observed_height = fork_height;
    std::set<Hash256> confirmed;
    for (const auto& block : connected) {
        std::vector<Hash256> observed;
        for (const auto& tx : block.transactions) observed.push_back(tx.get_hash());
        fee_estimator_.block_connected(block.get_hash(), ++observed_height, observed);
        if (!scope.full) {
            note_block_effects_locked(block, scope);
        }
        for (const auto& tx : block.transactions) {
            confirmed.insert(tx.get_hash());
        }
    }
    for (const auto& hash : confirmed) {
        if (remove_transaction_locked(hash)) ++transactions_updated_;
    }
    revalidate_locked(scope);
}

void Mempool::remove_block_transactions(const core::Block& block) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::set<Hash256> exact_hashes;
    std::set<std::pair<Hash256, uint32_t>> block_spent_outpoints;
    const Hash256 active_tip = chain_state_ ? chain_state_->get_tip_hash_snapshot() : Hash256{};
    const bool block_is_active_tip =
        !chain_state_ || active_tip == Hash256{} || active_tip == block.get_hash();
    if (block_is_active_tip) {
        for (size_t i = 0; i < block.transactions.size(); ++i) {
            const auto& tx = block.transactions[i];
            exact_hashes.insert(tx.get_hash());
            if (i == 0 && tx.is_coinbase()) {
                continue;
            }
            if (has_real_utxo_inputs(tx)) {
                for (const auto& input : tx.inputs) {
                    block_spent_outpoints.emplace(input.prev_tx_hash, input.vout);
                }
            }
        }
    }

    // Remove transactions that double-spend an input consumed by the block.
    // Descendants of those conflicts are invalid as well. Descendants of an
    // exact confirmed transaction are deliberately retained: their parent is
    // now available from the active chain.
    std::set<Hash256> conflict_roots;
    for (const auto& [hash, entry] : transactions_) {
        if (exact_hashes.count(hash) > 0 || !has_real_utxo_inputs(entry.tx)) {
            continue;
        }
        for (const auto& input : entry.tx.inputs) {
            if (block_spent_outpoints.count({input.prev_tx_hash, input.vout}) > 0) {
                conflict_roots.insert(hash);
                break;
            }
        }
    }

    std::set<Hash256> to_remove = exact_hashes;
    const auto conflicts_and_descendants = collect_descendants_locked(std::move(conflict_roots));
    to_remove.insert(conflicts_and_descendants.begin(), conflicts_and_descendants.end());

    for (const auto& hash : to_remove) {
        if (remove_transaction_locked(hash)) {
            transactions_updated_++;
        }
    }

    RevalidationScope scope;
    if (block_is_active_tip && !block.transactions.empty()) {
        scope.full = false;
        note_block_effects_locked(block, scope);
    }
    revalidate_locked(scope);
}

void Mempool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    fee_estimator_.clear_pending();

    // The shared AngleCommitmentStore is a transient verifier index. Remove
    // mempool-owned entries before dropping the corresponding bookkeeping; the
    // ChainState helper serializes with block application and only touches the
    // transient store, so cleanup also works when historical reads fail.
    if (chain_state_) {
        for (const auto& [hash, entry] : transactions_) {
            if (entry.tx.type == core::TransactionType::ANGLE_COMMITMENT) {
                chain_state_->remove_mempool_angle_commitment_from_store(hash);
            }
        }
    }

    transactions_.clear();
    fee_index_.clear();
    spent_outputs_.clear();
    angle_commitment_keys_.clear();
    pending_angle_commitments_by_spend_.clear();
    claimed_angle_commitments_.clear();
    multisig_spend_keys_.clear();
    protocol_authorization_keys_.clear();
    hierarchical_spend_keys_.clear();
    delegation_proof_keys_.clear();
    delegation_nonces_.clear();
    revoke_delegation_keys_.clear();
    nft_id_keys_.clear();
    mempool_delegation_spent_.clear();
    current_size_ = 0;
    {
        std::lock_guard<std::mutex> relay_lock(relay_mutex_);
        new_transactions_.clear();
    }
}

size_t Mempool::expire_old_transactions() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Hash256> expired;
    expired.reserve(transactions_.size());
    for (const auto& [hash, entry] : transactions_) {
        const auto max_age = entry.tx.type == core::TransactionType::ANGLE_COMMITMENT
            ? kAngleCommitmentExpiry
            : (entry.fee == 0 ? kZeroFeeProtocolExpiry : kDefaultMempoolExpiry);
        if (now >= entry.added_at && now - entry.added_at >= max_age) {
            expired.push_back(hash);
        }
    }

    const size_t before = transactions_.size();
    for (const auto& hash : expired) {
        (void)remove_transaction_locked(hash);
    }
    const size_t removed = before - transactions_.size();
    if (removed > 0) {
        transactions_updated_ += static_cast<uint64_t>(removed);
    }
    return removed;
}

MempoolStats Mempool::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    MempoolStats stats = {};
    stats.tx_count = transactions_.size();
    stats.total_size = current_size_;
    stats.total_fees = 0;
    stats.min_fee_per_byte = UINT64_MAX;
    stats.max_fee_per_byte = 0;

    for (const auto& [hash, entry] : transactions_) {
        (void)hash;
        if (entry.fee > (std::numeric_limits<uint64_t>::max)() - stats.total_fees) {
            stats.total_fees = (std::numeric_limits<uint64_t>::max)();
        } else {
            stats.total_fees += entry.fee;
        }
        stats.min_fee_per_byte = std::min(stats.min_fee_per_byte, entry.fee_per_byte);
        stats.max_fee_per_byte = std::max(stats.max_fee_per_byte, entry.fee_per_byte);
    }

    if (stats.tx_count > 0 && stats.total_size > 0) {
        stats.avg_fee_per_byte = stats.total_fees / stats.total_size;
    }

    if (stats.min_fee_per_byte == UINT64_MAX) {
        stats.min_fee_per_byte = 0;
    }

    return stats;
}

uint64_t fee_policy::recommended_rate_per_kb(const Mempool* mempool) {
    const uint64_t base_rate = consensus::params::FEE_RATE_PER_KB > 0
        ? static_cast<uint64_t>(consensus::params::FEE_RATE_PER_KB)
        : 0;
    if (!mempool) {
        return base_rate;
    }

    const MempoolStats stats = mempool->get_stats();
    const size_t max_size = mempool->max_size_limit();
    uint64_t multiplier = 1;
    if (occupancy_at_least(stats.total_size, max_size, 97)) {
        multiplier = 16;
    } else if (occupancy_at_least(stats.total_size, max_size, 90)) {
        multiplier = 8;
    } else if (occupancy_at_least(stats.total_size, max_size, 75)) {
        multiplier = 4;
    } else if (occupancy_at_least(stats.total_size, max_size, 50)) {
        multiplier = 2;
    }
    multiplier = std::min<uint64_t>(multiplier, MAX_CONGESTION_MULTIPLIER);
    return saturating_rate_multiplier(base_rate, multiplier);
}

Amount fee_policy::scale_fee_to_rate(Amount base_fee, uint64_t rate_per_kb) {
    const Amount minimum = consensus::params::MIN_TX_FEE;
    if (base_fee <= 0) {
        return minimum;
    }

    const uint64_t base_rate = consensus::params::FEE_RATE_PER_KB > 0
        ? static_cast<uint64_t>(consensus::params::FEE_RATE_PER_KB)
        : 0;
    if (base_rate == 0 || rate_per_kb <= base_rate) {
        return std::max(minimum, base_fee);
    }

    const uint64_t max_rate = saturating_rate_multiplier(
        base_rate,
        MAX_CONGESTION_MULTIPLIER
    );
    const uint64_t bounded_rate = std::min(rate_per_kb, max_rate);
    const utils::U128Parts product = utils::mul_u64_wide(
        static_cast<uint64_t>(base_fee),
        bounded_rate
    );
    if (product.hi >= base_rate) {
        return (std::numeric_limits<Amount>::max)();
    }
    uint64_t remainder = 0;
    uint64_t scaled = utils::div_u128_by_u64_fast(
        product.hi,
        product.lo,
        base_rate,
        &remainder
    );
    if (remainder != 0 && scaled != (std::numeric_limits<uint64_t>::max)()) {
        ++scaled;
    }
    const uint64_t amount_max = static_cast<uint64_t>((std::numeric_limits<Amount>::max)());
    if (scaled > amount_max) {
        return (std::numeric_limits<Amount>::max)();
    }
    return std::max(minimum, static_cast<Amount>(scaled));
}

MempoolRejectStats Mempool::get_reject_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MempoolRejectStats out;
    for (const auto& [k, v] : reject_consensus_) {
        out.consensus[k] = v;
    }
    for (const auto& [k, v] : reject_policy_) {
        out.policy[k] = v;
    }
    return out;
}

void Mempool::set_max_size(size_t max_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_size_ = max_size;
}

void Mempool::set_policy_config(const MempoolPolicyConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_config_ = config;
}

size_t Mempool::max_size_limit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_size_;
}

size_t Mempool::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transactions_.size();
}

uint64_t Mempool::get_transactions_updated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transactions_updated_;
}

bool Mempool::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transactions_.empty();
}

bool Mempool::verify_transaction(const core::Transaction& tx, bool record_rejections) {
    auto reject = [this, record_rejections](bool policy, const std::string& reason) {
        if (record_rejections) {
            record_reject(policy, reason);
        }
        return false;
    };
    if (!chain_state_) {
        return reject(false, "no-chainstate");
    }

    // Держим проверки mempool согласованными с проверками консенсуса на уровне блока.
    consensus::ValidationState basic_state;
    if (!consensus::check_transaction(tx, basic_state)) {
        return reject(false, basic_state.get_reject_reason());
    }

    // Cheap UTXO/fee policy precedes expensive signature verification. A peer
    // cannot submit a zero-fee or missing-input transaction merely to consume
    // post-quantum verification time.
    const uint64_t fee = calculate_fee(tx);
    std::string policy_reason;
    MempoolPolicyConfig policy_config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        policy_config = policy_config_;
    }
    if (!MempoolPolicy::check(tx, *chain_state_, fee, policy_config, &policy_reason)) {
        return reject(true, policy_reason.empty() ? "policy-reject" : policy_reason);
    }

    if (consumes_angle_commitments(tx)) {
        size_t package_size = 0;
        bool complete_package = true;
        try {
            package_size = tx.serialize().size();
        } catch (...) {
            complete_package = false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& commitment_hash : tx.angle_commitment_hashes) {
                const auto it = transactions_.find(commitment_hash);
                if (it == transactions_.end() ||
                    it->second.tx.type != core::TransactionType::ANGLE_COMMITMENT ||
                    claimed_angle_commitments_.count(commitment_hash) > 0 ||
                    it->second.size > (std::numeric_limits<size_t>::max)() - package_size) {
                    complete_package = false;
                    break;
                }
                package_size += it->second.size;
            }
        }
        if (!complete_package) {
            return reject(true, "incomplete-or-claimed-angle-package");
        }
        if (fee < MempoolPolicy::required_fee_for_size(package_size, policy_config)) {
            return reject(true, "package-fee-rate-too-low");
        }
    }

    const bool requires_basic_signature =
        (tx.type != core::TransactionType::MULTISIG_SPEND &&
         tx.type != core::TransactionType::DELEGATION_PROOF &&
         tx.type != core::TransactionType::REVOKE_DELEGATION &&
         tx.type != core::TransactionType::UPDATE_FOUNDATION_ADDRESS &&
         tx.type != core::TransactionType::STANDARD);
    if (requires_basic_signature && !tx.verify()) {
        return reject(false, "bad-signature");
    }

    // Проверяем через ChainState
    if (!chain_state_->validate_transaction(tx)) {
        return reject(false, "state-invalid");
    }

    // Дополнительная защита (mempool-only): не принимаем несколько pending hierarchical spends,
    // которые суммарно превысят max_total_delegated.
    if (tx.type == core::TransactionType::MULTISIG_SPEND && !is_null_hash256(tx.delegation_proof_txid)) {
        auto dp_tx = chain_state_->get_transaction(tx.delegation_proof_txid);
        if (!dp_tx) {
            return reject(false, "delegation-not-found");
        }

        if (dp_tx->max_total_delegated > 0) {
            const Amount amount_spent = hierarchical_amount_spent_locked(tx, *dp_tx, fee);
            const Amount already_confirmed = chain_state_->get_delegation_total_spent(tx.delegation_proof_txid);

            const std::string dp_key = hash256_to_hex(tx.delegation_proof_txid);
            const Amount already_pending = [&]() -> Amount {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = mempool_delegation_spent_.find(dp_key);
                return (it == mempool_delegation_spent_.end()) ? 0 : it->second;
            }();

            Amount confirmed_and_pending = 0;
            Amount projected_total = 0;
            if (already_confirmed < 0 || already_pending < 0 || amount_spent < 0 ||
                !checked_add_amount(already_confirmed, already_pending, confirmed_and_pending) ||
                !checked_add_amount(confirmed_and_pending, amount_spent, projected_total) ||
                projected_total > dp_tx->max_total_delegated) {
                return reject(true, "max-total-delegated");
            }
        }
    }

    // ЗАЩИТА ОТ REPLAY АТАК: Проверяем уникальность транзакций в mempool
    // Это предотвращает добавление дубликатов ANGLE_COMMITMENT, MULTISIG_SPEND и других
    // транзакций с теми же параметрами ДО того как они попадут в блок
    consensus::ValidationState validation_state;
    consensus::ContextualTxLookup lookup;
    lookup.get_transaction_by_txid = [this](const Hash256& hash, core::Transaction& out_tx) -> bool {
        // Сначала проверяем в блокчейне
        auto tx_ptr = this->chain_state_->get_transaction(hash);
        if (tx_ptr) {
            out_tx = *tx_ptr;
            return true;
        }
        // Затем проверяем в mempool (по txid)
        return this->get_transaction(hash, out_tx);
    };
    lookup.has_angle_commitment_key = [this](const Hash256& key) -> bool {
        {
            std::lock_guard<std::mutex> lock(this->mutex_);
            if (this->angle_commitment_keys_.count(key) > 0) {
                return true;
            }
        }
        return this->chain_state_->has_angle_commitment_key(key);
    };
    lookup.has_multisig_spend_key = [this](const Hash256& key) -> bool {
        {
            std::lock_guard<std::mutex> lock(this->mutex_);
            if (this->multisig_spend_keys_.count(key) > 0) {
                return true;
            }
        }
        return this->chain_state_->has_multisig_spend_key(key);
    };
    lookup.has_hierarchical_spend_key = [this](const Hash256& key) -> bool {
        {
            std::lock_guard<std::mutex> lock(this->mutex_);
            if (this->hierarchical_spend_keys_.count(key) > 0) {
                return true;
            }
        }
        return this->chain_state_->has_hierarchical_spend_key(key);
    };
    lookup.has_delegation_proof_key = [this](const Hash256& key) -> bool {
        {
            std::lock_guard<std::mutex> lock(this->mutex_);
            if (this->delegation_proof_keys_.count(key) > 0) {
                return true;
            }
        }
        return this->chain_state_->has_delegation_proof_key(key);
    };
    lookup.has_revoke_delegation_key = [this](const Hash256& key) -> bool {
        {
            std::lock_guard<std::mutex> lock(this->mutex_);
            if (this->revoke_delegation_keys_.count(key) > 0) {
                return true;
            }
        }
        return this->chain_state_->has_revoke_delegation_key(key);
    };

    lookup.has_nft_id_key = [this](const Hash256& key) -> bool {
        {
            std::lock_guard<std::mutex> lock(this->mutex_);
            if (this->nft_id_keys_.count(key) > 0) {
                return true;
            }
        }
        return this->chain_state_->has_nft_id_key(key);
    };

    lookup.has_sidechain_deposit_key = [this](const Hash256& key) -> bool {
        return this->chain_state_->has_sidechain_deposit_key(key);
    };

    lookup.has_sidechain_registry_key = [this](const Hash256& key) -> bool {
        return this->chain_state_->has_sidechain_registry_key(key);
    };
    // Для MULTISIG_SPEND/DELEGATION_PROOF/REVOKE_DELEGATION базовый tx.verify()
    // не отражает реальную криптографическую проверку (она в MultisigVerifier).
    lookup.signatures_verified = true;

    if (!consensus::contextual_check_transaction(tx, validation_state, lookup)) {
        // Транзакция не прошла контекстную проверку (дубликат или другая ошибка)
        return reject(false, validation_state.get_reject_reason());
    }

    return true;
}

bool Mempool::has_conflicts(const core::Transaction& tx) const {
    if (const auto authorization = tx.get_protocol_authorization_hash();
        authorization && protocol_authorization_keys_.count(*authorization) > 0) {
        return true;
    }
    if (tx.type == core::TransactionType::DELEGATION_PROOF) {
        const auto nonces = delegation_nonces_.find(tx.primary_multisig_address.str());
        if (nonces != delegation_nonces_.end() && nonces->second.count(tx.nonce) > 0) {
            return true;
        }
    }
    // ANGLE_COMMITMENT has no UTXO input. Every transaction that pays a
    // protocol package fee participates in the normal spent-output index.
    if (!has_real_utxo_inputs(tx)) {
        return false;
    }

    // Проверяем, не использованы ли уже выходы
    for (const auto& input : tx.inputs) {
        auto it_spent = spent_outputs_.find(input.prev_tx_hash);
        if (it_spent != spent_outputs_.end() &&
            it_spent->second.find(input.vout) != it_spent->second.end()) {
            return true;
        }
    }

    return false;
}

uint64_t Mempool::calculate_fee(const core::Transaction& tx) const {
    if (!chain_state_) {
        return 0;
    }
    const Amount fee = chain_state_->calculate_transaction_fee(tx);
    return fee > 0 ? static_cast<uint64_t>(fee) : 0;
}

std::vector<Hash256> Mempool::plan_eviction_locked(
    size_t bytes_needed, uint64_t maximum_fee_rate,
    const std::set<Hash256>& protected_transactions
) const {
    std::vector<Hash256> plan;
    std::set<Hash256> planned;
    size_t available = 0;
    for (auto rate = fee_index_.rbegin(); rate != fee_index_.rend() && available < bytes_needed; ++rate) {
        if (rate->first >= maximum_fee_rate) break;
        const auto entry = transactions_.find(rate->second);
        if (entry == transactions_.end() || planned.count(rate->second) > 0) continue;
        // A claimed commitment inherits its consumer's priority. Evict that
        // package as one unit, never through its zero-fee signature envelope.
        if (entry->second.tx.type == core::TransactionType::ANGLE_COMMITMENT &&
            claimed_angle_commitments_.count(rate->second) > 0) continue;
        std::vector<Hash256> package{rate->second};
        if (consumes_angle_commitments(entry->second.tx)) {
            for (const auto& hash : entry->second.tx.angle_commitment_hashes) {
                const auto commitment = transactions_.find(hash);
                if (commitment != transactions_.end() &&
                    commitment->second.tx.type == core::TransactionType::ANGLE_COMMITMENT) {
                    package.push_back(hash);
                }
            }
        }
        if (std::any_of(package.begin(), package.end(), [&](const auto& hash) {
                return protected_transactions.count(hash) > 0;
            })) continue;
        for (const auto& hash : package) {
            if (!planned.insert(hash).second) continue;
            const size_t size = transactions_.at(hash).size;
            available = size > (std::numeric_limits<size_t>::max)() - available
                ? (std::numeric_limits<size_t>::max)() : available + size;
            plan.push_back(hash);
        }
    }
    return available >= bytes_needed ? plan : std::vector<Hash256>{};
}

bool Mempool::evict_low_fee_transactions(
    size_t bytes_needed, uint64_t maximum_fee_rate,
    const std::set<Hash256>& protected_transactions
) {
    if (bytes_needed == 0) return true;
    while (!fee_index_.empty()) {
        const auto lowest = std::prev(fee_index_.end());
        if (transactions_.count(lowest->second) > 0) break;
        fee_index_.erase(lowest);
    }
    const auto plan = plan_eviction_locked(bytes_needed, maximum_fee_rate, protected_transactions);
    if (plan.empty()) return false;
    const size_t previous_size = current_size_;
    for (const auto& hash : plan) {
        if (remove_transaction_locked(hash)) ++transactions_updated_;
    }
    return previous_size >= current_size_ && previous_size - current_size_ >= bytes_needed;
}

size_t Mempool::unclaimed_angle_commitment_count_locked() const {
    size_t unclaimed = 0;
    for (const auto& [hash, entry] : transactions_) {
        if (entry.tx.type == core::TransactionType::ANGLE_COMMITMENT &&
            claimed_angle_commitments_.count(hash) == 0) {
            ++unclaimed;
        }
    }
    return unclaimed;
}

std::optional<Hash256> Mempool::oldest_unclaimed_angle_commitment_locked() const {
    auto pick = [&](bool isolated_only) -> std::optional<Hash256> {
        auto oldest = transactions_.end();
        for (auto it = transactions_.begin(); it != transactions_.end(); ++it) {
            if (it->second.tx.type != core::TransactionType::ANGLE_COMMITMENT ||
                claimed_angle_commitments_.count(it->first) > 0) {
                continue;
            }
            if (isolated_only) {
                const auto spend = pending_angle_commitments_by_spend_.find(
                    it->second.tx.spending_tx_hash);
                if (spend != pending_angle_commitments_by_spend_.end() && spend->second > 1) {
                    continue;
                }
            }
            if (oldest == transactions_.end() || it->second.added_at < oldest->second.added_at) {
                oldest = it;
            }
        }
        if (oldest == transactions_.end()) {
            return std::nullopt;
        }
        return oldest->first;
    };

    if (const auto isolated = pick(true)) {
        return isolated;
    }
    return pick(false);
}

bool Mempool::evict_oldest_unclaimed_angle_commitment_locked() {
    const auto victim = oldest_unclaimed_angle_commitment_locked();
    if (!victim || !remove_transaction_locked(*victim)) {
        return false;
    }
    ++transactions_updated_;
    return true;
}

} // namespace geocoin::network

std::vector<geocoin::Hash256> geocoin::network::Mempool::get_transactions_for_relay(size_t max_items) {
    std::lock_guard<std::mutex> lock(relay_mutex_);

    std::vector<geocoin::Hash256> result;
    if (new_transactions_.empty()) {
        return result;
    }

    if (max_items == 0 || max_items >= new_transactions_.size()) {
        result.assign(new_transactions_.begin(), new_transactions_.end());
        new_transactions_.clear();
        return result;
    }

    result.reserve(max_items);
    auto it = new_transactions_.begin();
    for (size_t i = 0; i < max_items && it != new_transactions_.end(); ++i) {
        result.push_back(*it);
        it = new_transactions_.erase(it);
    }
    return result;
}

void geocoin::network::Mempool::queue_inventory_for_relay(const std::vector<Hash256>& hashes) {
    if (hashes.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(relay_mutex_);
    new_transactions_.insert(hashes.begin(), hashes.end());
}
