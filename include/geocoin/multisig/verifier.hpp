#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/multisig/angle_store.hpp"
#include "geocoin/multisig/multisig.hpp"
#include "geocoin/types.hpp"

#include <memory>
#include <utility>

namespace geocoin::multisig {

/** Consensus verifier for commitment-backed multisig transactions. */
class MultisigVerifier {
public:
    explicit MultisigVerifier(std::shared_ptr<AngleCommitmentStore> angle_store)
        : angle_store_(std::move(angle_store)) {}

    bool verify_multisig_spend(
        const core::Transaction& tx,
        const MultisigConfig& multisig_config
    ) const;
    bool verify_multisig_spend(const core::Transaction& tx) const;
    bool verify_angle_commitment(const core::Transaction& tx) const;
    bool verify_delegation_proof(const core::Transaction& tx) const;
    bool verify_revoke_delegation(
        const core::Transaction& tx,
        const core::Transaction& delegation_proof_tx
    ) const;
    bool verify_hierarchical_spend(
        const core::Transaction& tx,
        const core::Transaction& delegation_proof_tx
    ) const;

private:
    std::shared_ptr<AngleCommitmentStore> angle_store_;

    bool is_participant(
        const Address& address,
        const MultisigConfig& config
    ) const;
};

} // namespace geocoin::multisig
