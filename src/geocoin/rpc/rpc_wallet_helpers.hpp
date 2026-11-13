#pragma once

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/u64_math.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <vector>
#include <openssl/rand.h>
#include <openssl/crypto.h>

namespace geocoin::rpc {

bool SyncDebugEnabled();
bool is_printable_ascii(const std::string& value);
core::Transaction deserialize_transaction_exact(const bytes& tx_data, const std::string& context_label);
core::Transaction deserialize_transaction_hex_or_throw(
    const std::string& hex,
    const std::string& invalid_hex_message,
    const std::string& context_label
);
std::string transaction_txid_hex(const core::Transaction& tx);
bool requires_basic_signature_verification(core::TransactionType type);

struct RawTransactionPreflight {
    std::string txid;
    int type = 0;
    bool verified = false;
    bool valid = false;
    bool contextual = false;
    std::string reject_reason;

    bool allowed() const {
        return verified && valid && contextual;
    }
};

RawTransactionPreflight validate_raw_transaction_for_mempool(
    const core::Transaction& tx,
    state::ChainState& chain_state,
    network::Mempool& mempool
);
json raw_transaction_preflight_to_json(
    const RawTransactionPreflight& preflight,
    bool include_type = true
);
std::string submit_transaction_to_mempool(
    const core::Transaction& tx,
    const std::shared_ptr<state::ChainState>& chain_state,
    const std::string& label,
    bool verify_signature,
    bool include_type_in_validation_error,
    const std::shared_ptr<services::ChainMutationService>& chain_mutations
);
// Submits a transaction the wallet has just built and recorded in its history.
// A rejected transaction never reaches the chain, so its history entry is
// dropped instead of lingering as a permanently unconfirmed send.
std::string submit_wallet_transaction_to_mempool(
    wallet::Wallet& wallet,
    const core::Transaction& tx,
    const std::shared_ptr<state::ChainState>& chain_state,
    const std::string& label,
    bool verify_signature,
    bool include_type_in_validation_error,
    const std::shared_ptr<services::ChainMutationService>& chain_mutations
);
// Pending mempool spends the wallet must not select again.
std::vector<core::Transaction> pending_mempool_transactions(const network::Mempool* mempool);
std::string submit_raw_transaction_hex_to_mempool(
    const std::string& hex,
    const std::shared_ptr<state::ChainState>& chain_state,
    const std::string& label,
    bool verify_signature,
    bool include_type_in_validation_error,
    const std::shared_ptr<services::ChainMutationService>& chain_mutations
);
json submit_commitment_package_from_rpc(
    const json& payload,
    const std::string& final_tx_field,
    const std::string& final_result_field,
    const std::shared_ptr<state::ChainState>& chain_state,
    bool include_type_in_validation_error,
    const std::shared_ptr<services::ChainMutationService>& chain_mutations
);
json transaction_hex_result(const core::Transaction& tx);
void set_committee_signatures_from_rpc(
    std::vector<bytes>& signatures,
    const json& signatures_json,
    const std::string& invalid_shape_message = "signatures должен быть массивом или строкой"
);
void set_batch_checkpoint_signatures_from_rpc(core::Transaction& tx, const json& signatures_json);
json finalize_committee_signed_transaction(
    const json& params,
    core::TransactionType expected_type,
    const std::string& invalid_hex_message,
    const std::string& wrong_type_message
);
json finalize_nft_transaction_signature(
    const json& params,
    core::TransactionType expected_type,
    const std::string& expected_type_name
);

struct SidechainCommonFields {
    Hash256 sidechain_id{};
    Hash256 genesis_hash{};
    bool has_genesis = false;
    std::string owner_address;
};

struct SidechainCommitteeFields {
    uint32_t m = 0;
    uint32_t n = 0;
    std::vector<Address> members;
    Hash256 committee_id{};
};

struct ResolvedFraudRef {
    Hash256 sidechain_id{};
    Hash256 checkpoint{};
    Height checkpoint_height = 0;
    uint16_t index = 0xFFFF;
};

bool resolve_fraud_checkpoint_ref(
    state::ChainState* chain_state,
    const Hash256& sidechain_id,
    const Hash256& ref_txid,
    bool index_provided,
    uint16_t requested_index,
    ResolvedFraudRef& out_ref,
    std::string& error
);
bool get_validate_flag(const json& p, const char* context_label);
Amount calc_sidechain_fee_rpc(Amount amount, uint32_t fee_bps);
Amount required_amount_field(const json& p, const std::string& field, const std::string& error_message);
Amount optional_amount_field(
    const json& p,
    const std::string& field,
    Amount default_value,
    const std::string& error_message
);
SidechainCommitteeFields parse_sidechain_committee_from_rpc(const json& p);
void append_transaction_inputs_from_rpc(const json& inputs_json, std::vector<core::TxInput>& inputs);
Amount append_transaction_outputs_from_rpc(const json& outputs_json, std::vector<core::TxOutput>& outputs);
void apply_optional_sidechain_checkpoint_from_rpc(const json& p, core::Transaction& tx);
void apply_optional_sidechain_committee_id_from_rpc(const json& p, core::Transaction& tx);
void apply_active_sidechain_committee_id_or_throw(
    Hash256& committee_id,
    const state::SidechainRegistryEntry& entry
);
void apply_active_sidechain_committee_id_or_throw(
    core::Transaction& tx,
    const state::SidechainRegistryEntry& entry
);
void fill_active_sidechain_committee_id_if_available(
    core::Transaction& tx,
    state::ChainState* chain_state,
    const Hash256& sidechain_id
);
void apply_registry_sidechain_committee_id_if_present(
    Hash256& committee_id,
    state::ChainState* chain_state,
    const Hash256& sidechain_id
);
void apply_registry_sidechain_committee_id_if_present(
    core::Transaction& tx,
    state::ChainState* chain_state,
    const Hash256& sidechain_id
);
SidechainCommonFields build_sidechain_tx_common(
    const json& p,
    core::Transaction& tx,
    const char* context_label
);
json finalize_sidechain_tx(
    const json& params,
    core::TransactionType expected_type,
    const char* context_label
);
uint64_t compute_recommended_fee_rate_per_kb(const network::Mempool* mempool);
Amount compute_dynamic_wallet_fee(
    wallet::Wallet* wallet,
    std::shared_ptr<state::ChainState> chain_state,
    Amount amount,
    const network::Mempool* mempool,
    std::span<const core::Transaction> pending = {}
);

inline void require_wallet_password(
    wallet::Wallet& wallet,
    const std::string& password,
    const char* missing_message
) {
    if (wallet.is_vault()) {
        if (!password.empty() && !wallet.unlock(password, 60))
            throw RPCException(RPC_WALLET_PASSPHRASE_INCORRECT, "Incorrect wallet passphrase");
        if (wallet.is_locked())
            throw RPCException(RPC_WALLET_UNLOCK_NEEDED, "Wallet is locked; use walletpassphrase first");
        return;
    }
    if (!wallet.has_password_protection()) {
        return;
    }
    if (password.empty()) {
        throw RPCException(RPC_WALLET_ERROR, missing_message);
    }
    if (!wallet.verify_password(password)) {
        throw RPCException(RPC_WALLET_ERROR, "Неверный пароль кошелька");
    }
}

inline std::string optional_object_password(const json& object) {
    if (!object.contains("password") || object["password"].is_null()) {
        return {};
    }
    if (!object["password"].is_string()) {
        throw RPCException(RPC_INVALID_PARAMETER, "password must be a string");
    }
    return object["password"].get<std::string>();
}

} // namespace geocoin::rpc
