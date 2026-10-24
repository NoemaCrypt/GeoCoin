#pragma once

#include "geocoin/network/mempool.hpp"
#include "geocoin/services/chain_mutation.hpp"

namespace geocoin::test {

/** Test-only mutation port for RPC handler tests without a running Node. */
class MempoolMutationService final : public services::ChainMutationService {
public:
    explicit MempoolMutationService(network::Mempool& mempool)
        : mempool_(mempool) {}

    services::BlockSubmissionResult process_block(const core::Block&) override {
        services::BlockSubmissionResult result;
        result.outcome = services::SubmissionOutcome::UNAVAILABLE;
        result.reject_reason = "unsupported-by-test-service";
        return result;
    }

    services::ChainTipActivationResult activate_tip(const Hash256& tip_hash) override {
        services::ChainTipActivationResult result;
        result.requested_tip_hash = tip_hash;
        result.outcome = services::SubmissionOutcome::UNAVAILABLE;
        result.reject_reason = "unsupported-by-test-service";
        return result;
    }

    services::TransactionSubmissionResult admit_transaction(
        const core::Transaction& tx
    ) override {
        services::TransactionSubmissionResult result;
        result.txid = tx.get_hash();
        if (mempool_.add_transaction(tx)) {
            result.outcome = services::SubmissionOutcome::ACCEPTED;
        } else {
            result.outcome = services::SubmissionOutcome::REJECTED;
            result.reject_reason = "mempool-rejected";
        }
        return result;
    }

private:
    network::Mempool& mempool_;
};

} // namespace geocoin::test
