#include "geocoin/rpc/rpc_server.hpp"
#include "rpc_mining_helpers.hpp"
#include "rpc_geography.hpp"

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/validation.hpp"
#include <limits>
#include <optional>
#include <utility>

namespace geocoin::rpc {

void RPCServer::register_mining_template_methods() {
    register_method({
        .name = "getblocktemplate",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();
            auto& chain_state = require_chain_state();
            auto& mempool = require_mempool();

            if (node_services_.info) {
                const auto status = node_services_.info->get_status();
                if (status.mining_blocked_by_sync || !status.initial_sync_complete) {
                    std::string reason = status.mining_block_reason;
                    if (reason.empty()) {
                        reason = "node is still syncing";
                    }
                    throw RPCException(
                        RPC_CLIENT_IN_INITIAL_DOWNLOAD,
                        "Mining is blocked until sync completes: " + reason
                    );
                }
            }

            Address mining_address;
            std::optional<Timestamp> requested_timestamp;
            if (has_param(params, 0)) {
                if (params[0].is_object()) {
                    require_only_fields(params[0], {"address", "mining_address", "timestamp"});
                    if (params[0].contains("address") && params[0].contains("mining_address")) {
                        throw RPCException(RPC_INVALID_PARAMETER, "Supply one mining address field");
                    }
                    std::string addr = params[0].value("address", params[0].value("mining_address", ""));
                    if (!addr.empty()) {
                        mining_address = addr;
                    }
                    if (params[0].contains("timestamp")) {
                        const auto& timestamp = params[0]["timestamp"];
                        if (!timestamp.is_number_unsigned()) {
                            throw RPCException(
                                RPC_INVALID_PARAMETER,
                                "timestamp must be an unsigned Unix timestamp"
                            );
                        }
                        const uint64_t value = timestamp.get<uint64_t>();
                        if (value > std::numeric_limits<uint32_t>::max()) {
                            throw RPCException(RPC_INVALID_PARAMETER, "timestamp exceeds uint32 range");
                        }
                        requested_timestamp = value;
                    }
                } else if (params[0].is_string()) {
                    mining_address = params[0].get<std::string>();
                } else {
                    throw RPCException(RPC_INVALID_PARAMETER, "request must be an object or address string");
                }
            }

            if (mining_address.empty()) {
                // MINER must not learn or spend to the node wallet address.
                throw RPCException(RPC_INVALID_PARAMS, "Укажите адрес майнинга (param.address)");
            }

            if (!crypto::verify_address(mining_address)) {
                throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "Invalid mining address");
            }

            if (requested_timestamp.has_value()) {
                consensus::ValidationState timestamp_state;
                const auto tip_snapshot = blockchain.get_active_tip_snapshot();
                if (!consensus::check_timestamp(*requested_timestamp, timestamp_state) ||
                    !consensus::check_timestamp_against_prev(
                        *requested_timestamp,
                        tip_snapshot.index,
                        timestamp_state
                    )) {
                    throw RPCException(
                        RPC_INVALID_PARAMETER,
                        "invalid block template timestamp: " + timestamp_state.get_reject_reason()
                    );
                }
            }

            core::Block block = build_block_template(
                &blockchain,
                &chain_state,
                &mempool,
                mining_address,
                requested_timestamp
            );
            const Hash256 block_hash = block.get_hash();
            const auto target = consensus::ProofOfWork::bits_to_target(block.header.bits);

            json result;
            result["capabilities"] = json::array();
            result["version"] = block.header.version;
            result["previousblockhash"] = hash256_to_hex_string(block.header.prev_block_hash);
            const auto parent = blockchain.get_block_position(block.header.prev_block_hash);
            const Height template_height = parent.indexed ? parent.height + 1 : 0;
            result["height"] = template_height;
            result["geopow"] = geopow_rules_to_json(template_height);
            result["bits"] = block.header.bits;
            result["target"] = uint256_to_hex_string(target);
            result["curtime"] = block.header.timestamp;
            result["mintime"] = block.header.timestamp;
            result["nonce"] = block.header.nonce;
            result["geo_tag"] = block.header.geo_tag;
            result["geography"] = block_geography_to_json(block.header, blockchain);
            result["coinbasevalue"] = block.transactions.front().outputs.empty() ? 0 : block.transactions.front().outputs[0].value;
            result["blockhash"] = hash256_to_hex_string(block_hash);
            result["blockhex"] = bytes_to_hex_string(block.serialize());

            json txs = json::array();
            const auto fees = chain_state.project_block_transaction_fees(block.transactions, 1, template_height);
            for (size_t i = 1; i < block.transactions.size(); ++i) {
                const auto& tx = block.transactions[i];
                json item;
                item["txid"] = hash256_to_hex_string(tx.get_hash());
                item["data"] = bytes_to_hex_string(tx.serialize());
                item["fee"] = fees[i - 1];
                item["weight"] = tx.serialize().size();
                txs.push_back(std::move(item));
            }
            result["transactions"] = std::move(txs);
            return result;
        },
        .category = "mining",
        .help = "getblocktemplate {\"address\":\"...\",\"timestamp\":n} | \"address\"\n\nВозвращает шаблон блока для внешнего майнинга. Адрес награды обязателен и не подставляется из кошелька ноды. Необязательный timestamp проходит обычные consensus-проверки времени. geography содержит точку WGS 84 в GeoJSON [долгота,широта] и границы ячейки geo_tag; точка пересчитывается из хеша и MTP родителя шаблона.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::MINER,
        .param_names = {"request"}
    });

    register_method({
        .name = "submitblock",
        .method = [this](const json& params) -> json {
            const std::string hex = required_param(params, 0, "Нужен hex блока").get<std::string>();
            if (hex.size() > consensus::params::MAX_BLOCK_SIZE * 2) {
                throw RPCException(RPC_INVALID_PARAMETER, "Размер блока превышает максимальный лимит");
            }
            bytes raw;
            if (!hex_to_bytes_checked(hex, raw)) {
                throw RPCException(RPC_DESERIALIZATION_ERROR, "Некорректный hex блока");
            }
            if (raw.size() > consensus::params::MAX_BLOCK_SIZE) {
                throw RPCException(RPC_INVALID_PARAMETER, "Размер блока превышает максимальный лимит");
            }

            core::Block block;
            try {
                block = core::Block::deserialize(raw);
            } catch (...) {
                throw RPCException(RPC_DESERIALIZATION_ERROR, "Не удалось декодировать блок");
            }

            const auto submission = require_chain_mutations().process_block(block);
            if (!submission.is_accepted()) {
                return submission.reject_reason.empty()
                    ? "block-submission-failed"
                    : submission.reject_reason;
            }
            return nullptr;
        },
        .category = "mining",
        .help = "submitblock \"hex\"\n\nОтправляет найденный блок в ноду.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::MINER,
        .param_names = {"hex"},
        .mutates = true,
        .side_effects = "validates and submits a block to the local chain"
    });
}

} // namespace geocoin::rpc
