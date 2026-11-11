#include "rpc_blockchain_format.hpp"
#include "rpc_geography.hpp"
#include "rpc_nft_metadata.hpp"

#include "geocoin/core/block.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/state/state.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

namespace geocoin::rpc {
namespace {

bool is_null_hash256(const Hash256& hash) {
    for (const auto byte : hash) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

json addresses_to_json(const std::vector<Address>& addresses) {
    json out = json::array();
    for (const auto& address : addresses) {
        out.push_back(address.str());
    }
    return out;
}

json transaction_inputs_to_json(const core::Transaction& tx) {
    json out = json::array();
    for (const auto& input : tx.inputs) {
        json input_obj;
        input_obj["prev_tx_hash"] = hash256_to_hex_string(input.prev_tx_hash);
        input_obj["vout"] = input.vout;
        input_obj["sequence"] = input.sequence;
        input_obj["script_sig_hex"] = bytes_to_hex_string(input.script_sig);
        if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
            input_obj["signature_hex"] = bytes_to_hex_string(input.signature);
        } else {
            input_obj["signature_hash"] = hash256_to_hex_string(input.signature_hash);
        }
        out.push_back(std::move(input_obj));
    }
    return out;
}

json transaction_outputs_to_json(const std::vector<core::TxOutput>& outputs) {
    json out = json::array();
    for (const auto& output : outputs) {
        json output_obj;
        output_obj["value"] = output.value;
        output_obj["address"] = output.address.str();
        output_obj["script_pubkey_hex"] = bytes_to_hex_string(output.script_pubkey);
        out.push_back(std::move(output_obj));
    }
    return out;
}

json transaction_witnesses_to_json(const core::Transaction& tx) {
    json out = json::array();
    const core::TxWitness empty_witness;
    const size_t count = std::max(tx.inputs.size(), tx.witnesses.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& witness = i < tx.witnesses.size() ? tx.witnesses[i] : empty_witness;
        out.push_back({{"public_key_hash", hash256_to_hex_string(witness.public_key_hash)},
            {"public_key_hex", bytes_to_hex_string(witness.public_key)},
            {"signature_hex", bytes_to_hex_string(witness.signature)}});
    }
    return out;
}

void add_cadastral_doc_fields(json& result, const core::Transaction& tx) {
    result["doc_hash"] = hash256_to_hex_string(tx.cadastral_doc_hash);
    if (!tx.cadastral_number.empty()) {
        result["cadastral_number"] = tx.cadastral_number;
    }
    if (!tx.cadastral_owner.str().empty()) {
        result["owner_address"] = tx.cadastral_owner.str();
    }
    result["nft_id"] = hash256_to_hex_string(tx.cadastral_nft_id);
}

void add_card_fields(json& result, const core::Transaction& tx) {
    auto authority_to_json = [](const core::CardAuthority& authority) {
        json value;
        value["quorum"] = authority.quorum;
        value["members"] = addresses_to_json(authority.members);
        return value;
    };
    auto policy_to_json = [&authority_to_json](const core::CardCorporatePolicy& policy) {
        json value;
        value["version"] = policy.version;
        value["mint"] = authority_to_json(policy.mint);
        value["freeze"] = authority_to_json(policy.freeze);
        value["operations"] = authority_to_json(policy.operations);
        value["recovery"] = authority_to_json(policy.recovery);
        value["compliance_policy_hash"] = hash256_to_hex_string(policy.compliance_policy_hash);
        value["max_active_cards"] = policy.max_active_cards;
        return value;
    };

    if (tx.type == core::TransactionType::CARD_PROGRAM_DEFINE) {
        json card;
        card["program_id"] = hash256_to_hex_string(tx.card_program_id);
        card["issuer"] = tx.card_issuer.str();
        card["chip_vendor"] = tx.card_chip_vendor;
        card["chip_os"] = tx.card_chip_os;
        card["applet_aid"] = tx.card_applet_aid;
        card["applet_version"] = tx.card_applet_version;
        card["applet_package_hash"] = hash256_to_hex_string(tx.card_applet_package_hash);
        card["policy_id"] = tx.card_policy_id;
        card["scope_id"] = hash256_to_hex_string(tx.card_scope_id);
        card["corporate_policy"] = policy_to_json(tx.card_corporate_policy);
        result["card_program_define"] = std::move(card);
    } else if (tx.type == core::TransactionType::CARD_ISSUE) {
        json card;
        card["program_id"] = hash256_to_hex_string(tx.card_program_id);
        card["issuer"] = tx.card_issuer.str();
        card["policy_id"] = tx.card_policy_id;
        card["scope_id"] = hash256_to_hex_string(tx.card_scope_id);
        card["uid_hash"] = hash256_to_hex_string(tx.card_uid_hash);
        card["pubkey_hash"] = hash256_to_hex_string(tx.card_pubkey_hash);
        card["valid_from"] = tx.card_valid_from;
        card["valid_to"] = tx.card_valid_to;
        card["status"] = tx.card_status;
        card["metadata_hash"] = hash256_to_hex_string(tx.card_metadata_hash);
        result["card_issue"] = std::move(card);
    } else if (tx.type == core::TransactionType::CARD_UPDATE_STATUS) {
        json card;
        card["program_id"] = hash256_to_hex_string(tx.card_program_id);
        card["issuer"] = tx.card_issuer.str();
        card["uid_hash"] = hash256_to_hex_string(tx.card_uid_hash);
        card["status"] = tx.card_status;
        card["metadata_hash"] = hash256_to_hex_string(tx.card_metadata_hash);
        result["card_update_status"] = std::move(card);
    } else if (tx.type == core::TransactionType::CARD_UPDATE_POLICY) {
        json card;
        card["program_id"] = hash256_to_hex_string(tx.card_program_id);
        card["issuer"] = tx.card_issuer.str();
        card["policy_id"] = tx.card_policy_id;
        card["scope_id"] = hash256_to_hex_string(tx.card_scope_id);
        card["corporate_policy"] = policy_to_json(tx.card_corporate_policy);
        result["card_update_policy"] = std::move(card);
    }
}

void add_multisig_spend_fields(json& result, const core::Transaction& tx) {
    result["m"] = tx.m;
    result["n"] = tx.n;

    json commitments = json::array();
    for (const auto& h : tx.angle_commitment_hashes) {
        commitments.push_back(hash256_to_hex_string(h));
    }
    result["angle_commitment_hashes"] = std::move(commitments);

    if (!is_null_hash256(tx.delegation_proof_txid)) {
        result["delegation_proof_txid"] = hash256_to_hex_string(tx.delegation_proof_txid);
    }

    if (tx.version >= 2 && is_null_hash256(tx.delegation_proof_txid) && !tx.spend_participants.empty()) {
        result["spend_participants"] = addresses_to_json(tx.spend_participants);
    }
}

void add_delegation_fields_angles(
    json& result,
    const core::Transaction& tx,
    const Hash256& txid,
    const state::ChainState& chain_state
) {
    result["primary_multisig_address"] = tx.primary_multisig_address.str();
    result["delegated_multisig_address"] = tx.delegated_multisig_address.str();
    result["primary_m"] = tx.primary_m;
    result["primary_n"] = tx.primary_n;
    result["delegated_m"] = tx.delegated_m;
    result["delegated_n"] = tx.delegated_n;
    result["valid_from"] = tx.valid_from;
    result["valid_until"] = tx.valid_until;
    result["nonce"] = tx.nonce;
    result["max_total_delegated"] = tx.max_total_delegated;
    result["max_per_transaction"] = tx.max_per_transaction;
    result["max_percent_of_balance"] = tx.max_percent_of_balance;
    result["total_spent"] = chain_state.get_delegation_total_spent(txid);
    result["primary_participants"] = addresses_to_json(tx.primary_participants);
    result["delegated_participants"] = addresses_to_json(tx.delegated_participants);
}

void add_delegation_fields_coins(
    json& result,
    const core::Transaction& tx,
    const Hash256& txid,
    const state::ChainState& chain_state
) {
    result["primary_multisig_address"] = tx.primary_multisig_address.str();
    result["delegated_multisig_address"] = tx.delegated_multisig_address.str();
    result["primary_m"] = tx.primary_m;
    result["primary_n"] = tx.primary_n;
    result["delegated_m"] = tx.delegated_m;
    result["delegated_n"] = tx.delegated_n;
    result["valid_from"] = tx.valid_from;
    result["valid_until"] = tx.valid_until;
    result["nonce"] = tx.nonce;
    result["max_total_delegated"] = tx.max_total_delegated / static_cast<double>(COIN);
    result["max_per_transaction"] = tx.max_per_transaction / static_cast<double>(COIN);
    result["max_percent_of_balance"] = tx.max_percent_of_balance;
    result["total_spent"] = chain_state.get_delegation_total_spent(txid) / static_cast<double>(COIN);
    result["primary_participants"] = addresses_to_json(tx.primary_participants);
    result["delegated_participants"] = addresses_to_json(tx.delegated_participants);
}

void add_nft_fields(json& result, const core::Transaction& tx) {
    if (tx.type == core::TransactionType::NFT_CREATE && tx.nft_create.has_value()) {
        const auto& nft_tx = tx.nft_create.value();
        json nft;
        nft["nft_id"] = hash256_to_hex_string(nft_tx.nft_id);
        nft["creator_address"] = nft_tx.creator_address.str();
        nft["royalty_percent"] = nft_tx.royalty_percent;
        nft["metadata"] = nft_metadata_to_json(nft_tx.metadata);
        nft["geography"] = nft_geography_to_json(nft_tx.metadata);

        if (nft_tx.pol_data.has_value()) {
            const auto& pol = nft_tx.pol_data.value();
            json polj;
            polj["latitude_ndeg"] = pol.latitude_ndeg;
            polj["longitude_ndeg"] = pol.longitude_ndeg;
            polj["timestamp_ms"] = pol.timestamp_ms;
            polj["gps_accuracy_cm"] = pol.gps_accuracy_cm;
            polj["source_type"] = pol.source_type;
            nft["pol_data"] = std::move(polj);
        }

        result["nft_create"] = std::move(nft);
    } else if (tx.type == core::TransactionType::NFT_TRANSFER && tx.nft_transfer.has_value()) {
        const auto& nft_tx = tx.nft_transfer.value();
        json nft;
        nft["nft_id"] = hash256_to_hex_string(nft_tx.nft_id);
        nft["from"] = nft_tx.from.str();
        nft["to"] = nft_tx.to.str();
        nft["fee"] = nft_tx.fee;
        result["nft_transfer"] = std::move(nft);
    } else if (tx.type == core::TransactionType::NFT_BURN && tx.nft_burn.has_value()) {
        const auto& nft_tx = tx.nft_burn.value();
        json nft;
        nft["nft_id"] = hash256_to_hex_string(nft_tx.nft_id);
        nft["owner"] = nft_tx.owner.str();
        nft["fee"] = nft_tx.fee;
        result["nft_burn"] = std::move(nft);
    }
}

} // namespace

json block_to_verbose_json(
    const core::Block& block,
    const Hash256& block_hash,
    const std::string& block_hash_hex,
    const core::Blockchain& blockchain
) {
    json result;
    result["hash"] = block_hash_hex;

    const auto position = blockchain.get_block_position(block_hash);
    const Height height = position.height;
    const bool height_known = position.indexed;
    result["in_active_chain"] = position.in_active_chain;

    if (height_known) {
        result["confirmations"] = position.in_active_chain ? json(position.confirmations) : json(-1);
        result["height"] = height;
    } else {
        result["confirmations"] = 0;
        result["height"] = nullptr;
    }

    result["version"] = block.header.version;
    result["merkleroot"] = hash256_to_hex_string(block.header.merkle_root);
    result["witnesscommitment"] = hash256_to_hex_string(block.header.witness_commitment);
    result["time"] = block.header.timestamp;
    result["nonce"] = block.header.nonce;
    result["bits"] = block.header.bits;
    result["geo_tag"] = block.header.geo_tag;
    result["geography"] = block_geography_to_json(block.header, blockchain);
    if (height_known) result["geopow"] = geopow_rules_to_json(height);

    json tx_array = json::array();
    for (const auto& tx : block.transactions) {
        tx_array.push_back(hash256_to_hex_string(tx.get_hash()));
    }
    result["tx"] = std::move(tx_array);

    if (height_known && height > 0) {
        result["previousblockhash"] = hash256_to_hex_string(block.header.prev_block_hash);
    }

    if (position.next_hash != Hash256{}) {
        result["nextblockhash"] = hash256_to_hex_string(position.next_hash);
    }

    return result;
}

json transaction_to_verbose_json(
    const core::Transaction& tx,
    const Hash256& txid,
    const std::string& txid_hex,
    const state::ChainState& chain_state
) {
    json result;
    result["txid"] = txid_hex;
    result["version"] = tx.version;
    result["type"] = static_cast<int>(tx.type);
    result["timestamp"] = tx.timestamp;
    result["lock_time"] = tx.lock_time;
    result["inputs"] = transaction_inputs_to_json(tx);
    result["outputs"] = transaction_outputs_to_json(tx.outputs);
    result["witness_version"] = tx.witness_version;
    result["witnesses"] = transaction_witnesses_to_json(tx);

    if (tx.type == core::TransactionType::CADASTRAL_DOC) {
        add_cadastral_doc_fields(result, tx);
    }

    add_card_fields(result, tx);

    if (tx.type == core::TransactionType::ANGLE_COMMITMENT) {
        result["network_id"] = tx.commitment_network_id;
        result["multisig_address"] = tx.multisig_address.str();
        result["spending_tx_hash"] = hash256_to_hex_string(tx.spending_tx_hash);
        result["signer_address"] = tx.signer_address.str();
    }

    if (tx.type == core::TransactionType::MULTISIG_SPEND) {
        add_multisig_spend_fields(result, tx);
    }

    if (tx.type == core::TransactionType::DELEGATION_PROOF) {
        add_delegation_fields_angles(result, tx, txid, chain_state);
    }

    if (tx.type == core::TransactionType::REVOKE_DELEGATION && !is_null_hash256(tx.revoked_delegation_proof_txid)) {
        result["revoked_delegation_proof_txid"] = hash256_to_hex_string(tx.revoked_delegation_proof_txid);
    }

    add_nft_fields(result, tx);
    return result;
}

json delegation_proof_to_json(
    const core::Transaction& tx,
    const Hash256& txid,
    const std::string& txid_hex,
    const state::ChainState& chain_state
) {
    json result;
    result["txid"] = txid_hex;
    add_delegation_fields_coins(result, tx, txid, chain_state);

    const auto current_time = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count());

    bool is_valid = true;
    if (tx.valid_from > 0 && current_time < tx.valid_from) {
        is_valid = false;
    }
    if (tx.valid_until > 0 && current_time > tx.valid_until) {
        is_valid = false;
    }

    result["is_valid"] = is_valid;
    result["is_expired"] = (tx.valid_until > 0 && current_time > tx.valid_until);
    return result;
}

} // namespace geocoin::rpc
