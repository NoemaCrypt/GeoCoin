#include "geocoin/multisig/angle_store.hpp"
#include <algorithm>

namespace geocoin::multisig {

void AngleCommitmentStore::add_commitment(const core::Transaction& tx) {
    // Проверяем тип транзакции
    if (tx.type != core::TransactionType::ANGLE_COMMITMENT) {
        return;
    }

    // Вычисляем хеш транзакции
    Hash256 tx_hash = tx.get_hash();

    // Проверяем, что commitment еще не добавлен
    if (has_commitment(tx_hash)) {
        return;
    }

    // Создаем shared_ptr для транзакции
    auto tx_ptr = std::make_shared<core::Transaction>(tx);

    // Добавляем в основное хранилище
    commitments_by_hash_[tx_hash] = tx_ptr;

    // Добавляем в индекс по multisig_address
    std::string multisig_key = tx.multisig_address.str();
    index_by_multisig_[multisig_key].push_back(tx_hash);

    // Добавляем в индекс по spending_tx_hash
    index_by_spending_tx_[tx.spending_tx_hash].push_back(tx_hash);
}

std::shared_ptr<core::Transaction> AngleCommitmentStore::get_by_hash(
    const Hash256& tx_hash
) const {
    auto it = commitments_by_hash_.find(tx_hash);
    if (it != commitments_by_hash_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<core::Transaction>> AngleCommitmentStore::get_by_multisig_address(
    const Address& multisig_address
) const {
    std::vector<std::shared_ptr<core::Transaction>> result;

    std::string multisig_key = multisig_address.str();
    auto it = index_by_multisig_.find(multisig_key);

    if (it != index_by_multisig_.end()) {
        for (const auto& tx_hash : it->second) {
            auto tx = get_by_hash(tx_hash);
            if (tx) {
                result.push_back(tx);
            }
        }
    }

    return result;
}

std::vector<std::shared_ptr<core::Transaction>> AngleCommitmentStore::get_by_spending_tx(
    const Hash256& spending_tx_hash
) const {
    std::vector<std::shared_ptr<core::Transaction>> result;

    auto it = index_by_spending_tx_.find(spending_tx_hash);

    if (it != index_by_spending_tx_.end()) {
        for (const auto& tx_hash : it->second) {
            auto tx = get_by_hash(tx_hash);
            if (tx) {
                result.push_back(tx);
            }
        }
    }

    return result;
}

bool AngleCommitmentStore::has_commitment(const Hash256& tx_hash) const {
    return commitments_by_hash_.find(tx_hash) != commitments_by_hash_.end();
}

void AngleCommitmentStore::remove_commitment(const Hash256& tx_hash) {
    auto it = commitments_by_hash_.find(tx_hash);
    if (it == commitments_by_hash_.end()) {
        return;
    }

    const auto& tx = *it->second;

    // Cleanup is also used after failures. Find existing secondary entries
    // without allocating an address copy or inserting a missing index bucket.
    const auto remove_from_index = [&](auto& index, const auto& key) {
        const auto entry = index.find(key);
        if (entry == index.end()) return;
        auto& hashes = entry->second;
        hashes.erase(std::remove(hashes.begin(), hashes.end(), tx_hash), hashes.end());
        if (hashes.empty()) index.erase(entry);
    };
    remove_from_index(index_by_multisig_, tx.multisig_address.str());
    remove_from_index(index_by_spending_tx_, tx.spending_tx_hash);

    // Удаляем из основного хранилища
    commitments_by_hash_.erase(it);
}

void AngleCommitmentStore::clear() {
    commitments_by_hash_.clear();
    index_by_multisig_.clear();
    index_by_spending_tx_.clear();
}

} // namespace geocoin::multisig
