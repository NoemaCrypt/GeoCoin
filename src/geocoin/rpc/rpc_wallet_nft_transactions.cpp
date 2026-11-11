#include "geocoin/rpc/rpc_amount.hpp"
#include "rpc_nft_metadata.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/utils/secure_memory.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace geocoin::rpc {
namespace {

uint64_t now_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

Amount optional_fee_from_rpc(const json& object) {
    if (!object.is_object()) {
        return consensus::params::MIN_TX_FEE;
    }
    if (object.contains("fee_angles") && !object["fee_angles"].is_null()) {
        Amount fee = rpc_angles_to_positive_amount(object["fee_angles"], "fee_angles must be a positive integer");
        return std::max<Amount>(fee, consensus::params::MIN_TX_FEE);
    }
    if (object.contains("fee") && !object["fee"].is_null()) {
        Amount fee = rpc_coins_to_positive_amount(object["fee"], "fee must be a positive GC amount");
        return std::max<Amount>(fee, consensus::params::MIN_TX_FEE);
    }
    return consensus::params::MIN_TX_FEE;
}

Hash256 required_hash256_field(const json& object, const std::string& field, const std::string& error_message) {
    const std::string value = required_string_field(object, field, error_message).get<std::string>();
    Hash256 out{};
    if (!hex_to_hash256_checked(value, out)) {
        throw RPCException(RPC_INVALID_PARAMETER, error_message);
    }
    return out;
}

json nft_wallet_tx_result(const core::Transaction& tx, const std::string& txid) {
    json out;
    out["txid"] = txid;
    out["tx_hex"] = bytes_to_hex_string(tx.serialize());
    out["type"] = static_cast<int>(tx.type);
    if (tx.nft_create.has_value()) {
        out["nft_id"] = hash256_to_hex_string(tx.nft_create->nft_id);
        out["owner"] = tx.nft_create->creator_address.str();
    } else if (tx.nft_transfer.has_value()) {
        out["nft_id"] = hash256_to_hex_string(tx.nft_transfer->nft_id);
        out["from"] = tx.nft_transfer->from.str();
        out["to"] = tx.nft_transfer->to.str();
    } else if (tx.nft_burn.has_value()) {
        out["nft_id"] = hash256_to_hex_string(tx.nft_burn->nft_id);
        out["owner"] = tx.nft_burn->owner.str();
    }
    return out;
}

} // namespace

void RPCServer::register_wallet_nft_transaction_methods() {
    register_method({
        .name = "createnft",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();
            auto& chain_state = require_chain_state();

            const json& request = required_object_param(params, 0, "createnft requires an object parameter");
            std::string password = optional_object_password(request);
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);
            require_wallet_password(
                wallet,
                password,
                "Кошелек защищен паролем: укажите password в объекте запроса"
            );
            const json& metadata_json = required_object_field(request, "metadata", "metadata is required");
            nft::NFTMetadata defaults;
            defaults.creator = wallet.get_address().str();
            defaults.created_at = now_unix_seconds();
            defaults.block_height = chain_state.get_height() + 1;
            defaults.block_hash = chain_state.get_tip_hash();
            const auto metadata = parse_nft_metadata_from_rpc(metadata_json, defaults);
            const uint32_t royalty_percent = optional_u32_field(
                request,
                "royalty_percent",
                0,
                "royalty_percent must be an integer"
            );
            if (royalty_percent > 10000) {
                throw RPCException(RPC_INVALID_PARAMETER, "royalty_percent must be in range 0..10000");
            }
            const Amount fee = optional_fee_from_rpc(request);

            std::shared_ptr<state::ChainState> chain_state_ptr(&chain_state, [](state::ChainState*){});
            core::Transaction tx;
            const auto pending = pending_mempool_transactions(mempool_.get());
            if (!wallet.create_nft_create(metadata, royalty_percent, fee, chain_state_ptr, tx, pending)) {
                throw RPCException(RPC_WALLET_ERROR, "failed to create NFT_CREATE transaction");
            }

            const std::string txid = submit_wallet_transaction_to_mempool(
                wallet,
                tx,
                chain_state_,
                "createnft",
                true,
                true,
                chain_mutations_
            );
            return nft_wallet_tx_result(tx, txid);
        },
        .category = "nft",
        .help = "createnft {\"metadata\":{...},\"royalty_percent\":n,\"fee\":n,\"password\":\"...\"}\n\nCreates, signs, and submits an NFT_CREATE transaction from the loaded wallet. fee is GC; fee_angles may be used for raw angles. password is required for an encrypted wallet.\n",
        .requires_wallet = true,
        .param_names = {"request"},
        .mutates = true,
        .side_effects = "creates a wallet NFT_CREATE transaction and submits it to the local mempool"
    });

    register_method({
        .name = "transfernft",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();
            auto& chain_state = require_chain_state();

            const json& request = required_object_param(params, 0, "transfernft requires an object parameter");
            std::string password = optional_object_password(request);
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);
            require_wallet_password(
                wallet,
                password,
                "Кошелек защищен паролем: укажите password в объекте запроса"
            );
            const Hash256 nft_id = required_hash256_field(request, "nft_id", "nft_id must be a 32-byte hex string");
            const Address to(required_string_field(request, "to", "to address is required").get<std::string>());
            const Amount fee = optional_fee_from_rpc(request);

            std::shared_ptr<state::ChainState> chain_state_ptr(&chain_state, [](state::ChainState*){});
            core::Transaction tx;
            const auto pending = pending_mempool_transactions(mempool_.get());
            if (!wallet.create_nft_transfer(nft_id, to, fee, chain_state_ptr, tx, pending)) {
                throw RPCException(RPC_WALLET_ERROR, "failed to create NFT_TRANSFER transaction");
            }

            const std::string txid = submit_wallet_transaction_to_mempool(
                wallet,
                tx,
                chain_state_,
                "transfernft",
                true,
                true,
                chain_mutations_
            );
            return nft_wallet_tx_result(tx, txid);
        },
        .category = "nft",
        .help = "transfernft {\"nft_id\":\"hex\",\"to\":\"GEO...\",\"fee\":n,\"password\":\"...\"}\n\nCreates, signs, and submits an NFT_TRANSFER transaction from the loaded wallet. fee is GC; fee_angles may be used for raw angles. password is required for an encrypted wallet.\n",
        .requires_wallet = true,
        .param_names = {"request"},
        .mutates = true,
        .side_effects = "creates a wallet NFT_TRANSFER transaction and submits it to the local mempool"
    });

    register_method({
        .name = "burnnft",
        .method = [this](const json& params) -> json {
            auto& wallet = require_wallet();
            auto& chain_state = require_chain_state();

            const json& request = required_object_param(params, 0, "burnnft requires an object parameter");
            std::string password = optional_object_password(request);
            utils::ScopedContainerCleanser<std::string> passwordCleanser(password);
            require_wallet_password(
                wallet,
                password,
                "Кошелек защищен паролем: укажите password в объекте запроса"
            );
            const Hash256 nft_id = required_hash256_field(request, "nft_id", "nft_id must be a 32-byte hex string");
            const Amount fee = optional_fee_from_rpc(request);

            std::shared_ptr<state::ChainState> chain_state_ptr(&chain_state, [](state::ChainState*){});
            core::Transaction tx;
            const auto pending = pending_mempool_transactions(mempool_.get());
            if (!wallet.create_nft_burn(nft_id, fee, chain_state_ptr, tx, pending)) {
                throw RPCException(RPC_WALLET_ERROR, "failed to create NFT_BURN transaction");
            }

            const std::string txid = submit_wallet_transaction_to_mempool(
                wallet,
                tx,
                chain_state_,
                "burnnft",
                true,
                true,
                chain_mutations_
            );
            return nft_wallet_tx_result(tx, txid);
        },
        .category = "nft",
        .help = "burnnft {\"nft_id\":\"hex\",\"fee\":n,\"password\":\"...\"}\n\nCreates, signs, and submits an NFT_BURN transaction from the loaded wallet. fee is GC; fee_angles may be used for raw angles. password is required for an encrypted wallet.\n",
        .requires_wallet = true,
        .param_names = {"request"},
        .mutates = true,
        .side_effects = "creates a wallet NFT_BURN transaction and submits it to the local mempool"
    });
}

} // namespace geocoin::rpc
