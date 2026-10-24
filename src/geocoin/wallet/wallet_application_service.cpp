#include "geocoin/wallet/wallet_application_service.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/wallet/wallet.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace geocoin::wallet {

WalletApplicationService::WalletApplicationService(
    std::shared_ptr<Wallet> wallet,
    std::shared_ptr<state::ChainState> chain_state,
    std::shared_ptr<services::ChainMutationService> mutations
)
    : wallet_(std::move(wallet)),
      chain_state_(std::move(chain_state)),
      mutations_(std::move(mutations)) {}

WalletTransactionResult WalletApplicationService::unavailable_result() const {
    if (!wallet_) {
        return {WalletUseCaseError::WALLET_UNAVAILABLE, "Wallet is not available"};
    }
    return {WalletUseCaseError::CHAIN_STATE_UNAVAILABLE, "Chain state is not available"};
}

WalletTransactionResult WalletApplicationService::creation_failed(std::string detail) const {
    return {WalletUseCaseError::CREATION_FAILED, std::move(detail)};
}

WalletTransactionResult WalletApplicationService::create_payment(
    const Address& recipient,
    Amount amount,
    Amount fee,
    std::span<const core::Transaction> pending
) const {
    if (!wallet_ || !chain_state_) {
        return unavailable_result();
    }
    if (amount <= 0 || fee < consensus::params::MIN_TX_FEE) {
        return {WalletUseCaseError::INVALID_ARGUMENT, "Invalid payment amount or fee"};
    }

    try {
        core::Transaction transaction;
        if (!wallet_->create_transaction(recipient, amount, fee, chain_state_, transaction, pending)) {
            return creation_failed("Wallet could not create payment transaction");
        }
        WalletTransactionResult result;
        result.transaction = std::move(transaction);
        result.txid = result.transaction.get_hash();
        return result;
    } catch (const std::exception& error) {
        return creation_failed(error.what());
    }
}

WalletTransactionResult WalletApplicationService::create_nft(
    nft::NFTMetadata metadata,
    uint32_t royalty_percent,
    Amount fee,
    std::span<const core::Transaction> pending
) const {
    if (!wallet_ || !chain_state_) {
        return unavailable_result();
    }
    if (fee < consensus::params::MIN_TX_FEE) {
        return {WalletUseCaseError::INVALID_ARGUMENT, "Invalid NFT fee"};
    }

    try {
        const auto validation = nft::coordinates::validate_geometry(metadata.geometry);
        if (!validation) {
            return {WalletUseCaseError::INVALID_ARGUMENT, validation.issue->message};
        }
        metadata.geometry = nft::coordinates::canonicalize_geometry(metadata.geometry);
        if (metadata.created_at == 0) {
            metadata.created_at = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()
            );
        }
        metadata.block_height = 0;
        metadata.block_hash = Hash256{};
        if (metadata.creator.empty()) {
            metadata.creator = wallet_->get_address().str();
        }

        core::Transaction transaction;
        if (!wallet_->create_nft_create(
                metadata,
                royalty_percent,
                fee,
                chain_state_,
                transaction,
                pending)) {
            return creation_failed("Wallet could not create NFT transaction");
        }
        WalletTransactionResult result;
        result.transaction = std::move(transaction);
        result.txid = result.transaction.get_hash();
        return result;
    } catch (const std::exception& error) {
        return creation_failed(error.what());
    }
}

WalletTransactionResult WalletApplicationService::transfer_nft(
    const Hash256& nft_id,
    const Address& recipient,
    Amount fee,
    std::span<const core::Transaction> pending
) const {
    if (!wallet_ || !chain_state_) {
        return unavailable_result();
    }
    if (fee < consensus::params::MIN_TX_FEE) {
        return {WalletUseCaseError::INVALID_ARGUMENT, "Invalid NFT transfer fee"};
    }

    try {
        core::Transaction transaction;
        if (!wallet_->create_nft_transfer(nft_id, recipient, fee, chain_state_, transaction, pending)) {
            return creation_failed("Wallet could not create NFT transfer");
        }
        WalletTransactionResult result;
        result.transaction = std::move(transaction);
        result.txid = result.transaction.get_hash();
        return result;
    } catch (const std::exception& error) {
        return creation_failed(error.what());
    }
}

WalletTransactionResult WalletApplicationService::burn_nft(
    const Hash256& nft_id,
    Amount fee,
    std::span<const core::Transaction> pending
) const {
    if (!wallet_ || !chain_state_) {
        return unavailable_result();
    }
    if (fee < consensus::params::MIN_TX_FEE) {
        return {WalletUseCaseError::INVALID_ARGUMENT, "Invalid NFT burn fee"};
    }

    try {
        core::Transaction transaction;
        if (!wallet_->create_nft_burn(nft_id, fee, chain_state_, transaction, pending)) {
            return creation_failed("Wallet could not create NFT burn transaction");
        }
        WalletTransactionResult result;
        result.transaction = std::move(transaction);
        result.txid = result.transaction.get_hash();
        return result;
    } catch (const std::exception& error) {
        return creation_failed(error.what());
    }
}

WalletTransactionResult WalletApplicationService::submit(
    const core::Transaction& transaction
) const {
    if (!mutations_) {
        return {
            WalletUseCaseError::SUBMISSION_UNAVAILABLE,
            "Chain mutation service is not available"
        };
    }

    const auto submission = mutations_->admit_transaction(transaction);
    if (!submission.is_accepted()) {
        return {
            WalletUseCaseError::SUBMISSION_REJECTED,
            submission.reject_reason.empty() ? "Transaction was rejected" : submission.reject_reason
        };
    }

    WalletTransactionResult result;
    result.transaction = transaction;
    result.txid = transaction.get_hash();
    return result;
}

bool WalletApplicationService::remember_multisig_config(
    const MultisigConfig& config,
    std::string& error
) const {
    if (!wallet_) {
        error = "Wallet is not available";
        return false;
    }
    if (!validate_multisig_config(config.address, config)) {
        error = "Invalid multisig configuration";
        return false;
    }
    if (!wallet_->add_multisig_address(config)) {
        error = "Failed to store multisig configuration in wallet";
        return false;
    }
    return true;
}

}  // namespace geocoin::wallet
