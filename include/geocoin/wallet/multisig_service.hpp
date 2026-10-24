#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/state/state.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace geocoin::wallet {

/**
 * Estimate the fee for a root transaction and all Falcon commitments that
 * may be attached to it. The estimate is deliberately conservative; the
 * network still enforces the exact serialized package size at admission.
 */
bool estimate_commitment_package_fee(
    const core::Transaction& root,
    size_t maximum_commitments,
    Amount requested_fee,
    Amount& fee,
    std::string& error
);

enum class CommitmentIntentKind {
    DIRECT_SPEND,
    DELEGATION_PROOF,
    HIERARCHICAL_SPEND,
    REVOKE_DELEGATION
};

struct CommitmentSigningIntent {
    CommitmentIntentKind kind = CommitmentIntentKind::DIRECT_SPEND;
    Hash256 signing_hash{};
    MultisigConfig signer_config;
};

/**
 * Derive the signing hash and signer policy from a complete, unsigned root
 * transaction. Hierarchical/revoke drafts also require the referenced,
 * confirmed delegation proof. Hash-only inputs are intentionally unsupported.
 */
bool derive_commitment_signing_intent(
    const core::Transaction& draft,
    const core::Transaction* delegation_proof,
    CommitmentSigningIntent& intent,
    std::string& error
);

struct MultisigDraftResult {
    core::Transaction transaction;
    Hash256 spending_tx_hash;
    Amount total_input = 0;
    Amount total_output = 0;
    Amount fee_used = 0;
};

/**
 * @brief Создает мультиподпись-черновик (MULTISIG_SPEND без commitment-ов)
 *
 * @param config Конфигурация мультиподписи (m-of-n)
 * @param to_address Адрес получателя
 * @param amount Сумма в монетах (angles)
 * @param fee Комиссия в монетах (angles)
 * @param chain_state ChainState для выборки UTXO
 * @param out_result Результат с транзакцией и хешем
 * @param error Строка с описанием ошибки
 * @return true при успехе
 */
bool create_multisig_draft(
    const MultisigConfig& config,
    const Address& to_address,
    Amount amount,
    Amount fee,
    std::shared_ptr<state::ChainState> chain_state,
    MultisigDraftResult& out_result,
    std::string& error,
    std::span<const core::Transaction> pending = {}
);

struct CommitmentResult {
    core::Transaction commitment_tx;
    Hash256 commitment_hash;
    Address signer_address;
};

struct DelegationProofRequest {
    MultisigConfig primary_config;
    MultisigConfig delegated_config;
    uint64_t valid_from = 0;
    uint64_t valid_until = 0;
    uint32_t nonce = 0;
    Amount max_total = 0;
    Amount max_per_transaction = 0;
    uint8_t max_percent_of_balance = 0;
    /** Miner fee in angles; values below the network minimum are raised. */
    Amount fee = 0;
    /** Zero selects the current wall-clock time. */
    uint64_t timestamp = 0;
};

struct DelegationProofResult {
    core::Transaction transaction;
    Hash256 delegation_hash{};
    Amount total_input = 0;
    Amount fee_used = 0;
};

/** Build the canonical unsigned transaction signed by a delegation quorum. */
bool create_delegation_proof(
    const DelegationProofRequest& request,
    std::shared_ptr<state::ChainState> chain_state,
    DelegationProofResult& out_result,
    std::string& error,
    std::span<const core::Transaction> pending = {}
);

/** Replace draft commitments with a validated quorum. */
bool finalize_delegation_proof(
    const core::Transaction& draft,
    const std::vector<Hash256>& commitment_hashes,
    core::Transaction& final_transaction,
    std::string& error
);

struct HierarchicalSpendRequest {
    Hash256 delegation_proof_txid{};
    MultisigConfig delegated_config;
    Address recipient;
    Amount amount = 0;
    Amount fee = 0;
    /** Zero selects the current wall-clock time. */
    uint64_t timestamp = 0;
};

struct HierarchicalSpendDraftResult {
    core::Transaction transaction;
    Hash256 spending_hash{};
    Address primary_address;
    Amount total_input = 0;
    Amount fee_used = 0;
    Amount change = 0;
};

/**
 * Build a complete hierarchical draft, including change, before calculating
 * its signing hash. This prevents signing a different output set than the one
 * eventually submitted.
 */
bool create_hierarchical_spend_draft(
    const HierarchicalSpendRequest& request,
    std::shared_ptr<state::ChainState> chain_state,
    HierarchicalSpendDraftResult& out_result,
    std::string& error,
    std::span<const core::Transaction> pending = {}
);

/** Add delegated commitments after rechecking that the proof is still active. */
bool finalize_hierarchical_spend(
    const core::Transaction& draft,
    const std::vector<Hash256>& commitment_hashes,
    std::shared_ptr<state::ChainState> chain_state,
    core::Transaction& final_transaction,
    std::string& error
);

/**
 * @brief Подписывает angle commitment для указанного draft
 */
bool sign_angle_commitment(
    const Address& multisig_address,
    const Hash256& spending_tx_hash,
    const bytes& private_key,
    CommitmentResult& out_result,
    std::string& error
);

/** Sign only when the private key belongs to the supplied multisig policy. */
bool sign_configured_angle_commitment(
    const MultisigConfig& config,
    const Hash256& spending_tx_hash,
    const bytes& private_key,
    CommitmentResult& out_result,
    std::string& error
);

/**
 * @brief Финализирует MULTISIG_SPEND, добавляя commitment-хеши
 */
bool finalize_multisig_spend(
    const core::Transaction& draft_tx,
    const std::vector<Hash256>& commitment_hashes,
    uint8_t required_signatures,
    core::Transaction& final_tx,
    std::string& error
);

} // namespace geocoin::wallet
