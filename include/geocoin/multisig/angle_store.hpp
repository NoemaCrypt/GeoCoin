#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/types.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

namespace geocoin::multisig {

/** Transient verifier index for unconsumed ANGLE_COMMITMENT transactions. */
class AngleCommitmentStore {
public:
    AngleCommitmentStore() = default;

    void add_commitment(const core::Transaction& tx);
    std::shared_ptr<core::Transaction> get_by_hash(const Hash256& tx_hash) const;
    std::vector<std::shared_ptr<core::Transaction>> get_by_multisig_address(
        const Address& multisig_address
    ) const;
    std::vector<std::shared_ptr<core::Transaction>> get_by_spending_tx(
        const Hash256& spending_tx_hash
    ) const;
    bool has_commitment(const Hash256& tx_hash) const;
    void remove_commitment(const Hash256& tx_hash);
    void clear();
    size_t size() const { return commitments_by_hash_.size(); }

private:
    std::map<Hash256, std::shared_ptr<core::Transaction>> commitments_by_hash_;
    std::map<std::string, std::vector<Hash256>> index_by_multisig_;
    std::map<Hash256, std::vector<Hash256>> index_by_spending_tx_;
};

} // namespace geocoin::multisig
