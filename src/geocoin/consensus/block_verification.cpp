#include "geocoin/core/block.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/logger.hpp"

#include <set>

// Block validation belongs to consensus. Keeping this definition beside block
// serialization made linking the value-type library pull in consensus rules.
namespace geocoin::core {

bool Block::verify() const {
    // 1. Проверяем корень Меркла
    std::vector<Hash256> hashes;
    for (const auto& tx : transactions) {
        hashes.push_back(tx.get_hash());
    }

    if (!transactions.empty()) {
        std::set<Hash256> seen_txids;
        for (const auto& hash : hashes) {
            if (!seen_txids.insert(hash).second) {
                GEO_LOG_ERROR("block", "Duplicate txid in block");
                return false;
            }
        }
        auto computed_root = compute_merkle_root(hashes);
        if (computed_root != header.merkle_root) {
            GEO_LOG_ERROR("block", "Merkle root mismatch");
            return false;
        }
    }

    const Hash256 computed_witness_commitment = calculate_witness_commitment();
    if (computed_witness_commitment != header.witness_commitment) {
        GEO_LOG_ERROR("block", "Witness commitment mismatch");
        return false;
    }

    // 2. Подписи независимы и проверяются параллельно. Первая ошибка
    // сообщается в порядке транзакций блока, после синтаксической проверки.
    std::vector<uint8_t> signatures_ok;
    consensus::verify_basic_signatures(transactions, signatures_ok);
    for (size_t i = 0; i < transactions.size(); ++i) {
        const auto& tx = transactions[i];
        consensus::ValidationState state;
        if (!consensus::check_transaction(tx, state)) {
            GEO_LOG_ERROR("block", "Transaction syntax failed (index=" << i
                << "): " << state.get_reject_reason());
            return false;
        }
        if (consensus::transaction_requires_basic_signature(tx) && signatures_ok[i] == 0) {
            GEO_LOG_ERROR("block", "Transaction signature failed (index=" << i << ")");
            return false;
        }
    }

    return true;
}

} // namespace geocoin::core
