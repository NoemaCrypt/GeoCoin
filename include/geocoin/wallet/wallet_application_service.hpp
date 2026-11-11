#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/services/chain_mutation.hpp"
#include "geocoin/wallet/multisig.hpp"

#include <memory>
#include <span>
#include <string>

namespace geocoin {
namespace state { class ChainState; }

namespace wallet {

class Wallet;

enum class WalletUseCaseError {
    NONE,
    WALLET_UNAVAILABLE,
    CHAIN_STATE_UNAVAILABLE,
    SUBMISSION_UNAVAILABLE,
    INVALID_ARGUMENT,
    CREATION_FAILED,
    SUBMISSION_REJECTED,
};

struct WalletTransactionResult {
    WalletUseCaseError error = WalletUseCaseError::NONE;
    std::string detail;
    core::Transaction transaction;
    Hash256 txid{};

    bool succeeded() const noexcept { return error == WalletUseCaseError::NONE; }
};

/**
 * UI-independent orchestration for wallet transaction use cases.
 *
 * Qt and RPC adapters remain responsible for presentation and credential
 * prompts; creation and submission policy lives here and is shared-testable.
 */
class WalletApplicationService final {
public:
    WalletApplicationService(
        std::shared_ptr<Wallet> wallet,
        std::shared_ptr<state::ChainState> chain_state,
        std::shared_ptr<services::ChainMutationService> mutations
    );

    WalletTransactionResult create_payment(
        const Address& recipient,
        Amount amount,
        Amount fee,
        std::span<const core::Transaction> pending = {}
    ) const;

    WalletTransactionResult create_nft(
        nft::NFTMetadata metadata,
        uint32_t royalty_percent,
        Amount fee,
        std::span<const core::Transaction> pending = {}
    ) const;

    WalletTransactionResult transfer_nft(
        const Hash256& nft_id,
        const Address& recipient,
        Amount fee,
        std::span<const core::Transaction> pending = {}
    ) const;

    WalletTransactionResult burn_nft(
        const Hash256& nft_id,
        Amount fee,
        std::span<const core::Transaction> pending = {}
    ) const;

    WalletTransactionResult submit(const core::Transaction& transaction) const;

    /** Store watch/signing metadata in the wallet, never in consensus state. */
    bool remember_multisig_config(const MultisigConfig& config, std::string& error) const;

private:
    WalletTransactionResult unavailable_result() const;
    WalletTransactionResult creation_failed(std::string detail) const;

    std::shared_ptr<Wallet> wallet_;
    std::shared_ptr<state::ChainState> chain_state_;
    std::shared_ptr<services::ChainMutationService> mutations_;
};

}  // namespace wallet
}  // namespace geocoin
