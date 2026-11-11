#include "geocoin/rpc/rpc_server.hpp"

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/core/blockchain.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace geocoin::rpc {

void RPCServer::register_blockchain_availability_methods() {
    register_method({
        .name = "getprunedrange",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& blockchain = require_blockchain();

            const Height tip = blockchain.get_height();
            node::PrunedDataInfo info;
            if (node_services_.info) {
                info = node_services_.info->get_pruned_data_info();
            } else {
                info.enabled = false;
                info.tip_height = tip;
                info.first_available_height = 0;
            }

            json result;
            result["node_type"] = (info.enabled ? "pruned" : "full");
            result["pruned"] = info.enabled;
            result["retention_blocks"] = info.retention;
            result["first_available_height"] = info.first_available_height;
            result["tip_height"] = tip;
            result["available_blocks"] = (tip >= info.first_available_height)
                ? (tip - info.first_available_height + 1)
                : 0;
            result["range"] = {
                {"start", info.first_available_height},
                {"end", tip}
            };
            return result;
        },
        .category = "blockchain",
        .help = "getprunedrange\n\nReturns the historical block range available through this node's API.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "getblockavailability",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();
            const json& query = required_param(params, 0, "Требуется hash или height");

            bool has_height = false;
            bool has_hash = false;
            Height height = 0;
            Hash256 hash{};

            if (query.is_string()) {
                const std::string value = query.get<std::string>();
                if (!hex_to_hash256_checked(value, hash)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Неверный hash");
                }
                has_hash = true;
                if (const auto* index = blockchain.find_block_index(hash)) {
                    height = index->height;
                    has_height = true;
                }
            } else {
                height = static_cast<Height>(required_u64_value(query, "height должен быть числом"));
                has_height = true;
                if (blockchain.get_active_block_hash_by_height(height, hash)) {
                    has_hash = true;
                }
            }

            const node::PrunedDataInfo info = node_services_.info
                ? node_services_.info->get_pruned_data_info()
                : node::PrunedDataInfo{};
            const bool in_active_chain = has_hash && blockchain.get_block_position(hash).in_active_chain;
            const bool pruned = node_services_.info && has_height &&
                node_services_.info->is_height_pruned_for_api(height);

            json result;
            result["query"] = query;
            result["found"] = in_active_chain;
            result["in_active_chain"] = in_active_chain;
            result["available"] = in_active_chain && !pruned;
            result["pruned"] = pruned;
            result["node_type"] = info.enabled ? "pruned" : "full";
            result["first_available_height"] = info.first_available_height;
            result["tip_height"] = blockchain.get_height();
            result["retention_blocks"] = info.retention;
            if (has_height) {
                result["height"] = height;
            }
            if (has_hash) {
                result["hash"] = hash256_to_hex_string(hash);
            }
            if (!in_active_chain) {
                result["reason"] = "not_found_in_active_chain";
            } else if (pruned && node_services_.info) {
                result["reason"] = node_services_.info->describe_pruned_api_error(height);
            } else {
                result["reason"] = "available";
            }
            return result;
        },
        .category = "blockchain",
        .help = "getblockavailability <hash|height>\n\nChecks whether a block is available through this node's API window.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"hash_or_height"}
    });

    register_method({
        .name = "getavailableblocks",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();

            const Height tip = blockchain.get_height();
            const node::PrunedDataInfo info = node_services_.info
                ? node_services_.info->get_pruned_data_info()
                : node::PrunedDataInfo{};
            const Height first = info.enabled ? info.first_available_height : 0;

            Height start = first;
            if (has_param(params, 0)) {
                start = static_cast<Height>(required_u64_value(params[0], "start должен быть числом"));
            }
            if (start < first) {
                start = first;
            }
            if (start > tip + 1) {
                start = tip + 1;
            }

            uint64_t count = 1000;
            if (has_param(params, 1)) {
                count = required_u64_value(params[1], "count должен быть числом");
            }
            if (count > 5000) {
                throw RPCException(RPC_INVALID_PARAMETER, "count must be <= 5000");
            }

            const bool empty_range = (count == 0 || start > tip);
            const Height end = empty_range
                ? (start == 0 ? 0 : start - 1)
                : std::min<Height>(tip, start + static_cast<Height>(count) - 1);

            json blocks = json::array();
            for (Height h = start; !empty_range && h <= end && h <= tip; ++h) {
                Hash256 hash{};
                json item;
                item["height"] = h;
                item["available"] = !node_services_.info ||
                    !node_services_.info->is_height_pruned_for_api(h);
                if (blockchain.get_active_block_hash_by_height(h, hash)) {
                    item["hash"] = hash256_to_hex_string(hash);
                }
                blocks.push_back(std::move(item));
                if (h == std::numeric_limits<Height>::max()) {
                    break;
                }
            }

            json result;
            result["node_type"] = info.enabled ? "pruned" : "full";
            result["first_available_height"] = first;
            result["tip_height"] = tip;
            result["retention_blocks"] = info.retention;
            result["start"] = start;
            result["end"] = end;
            result["count"] = blocks.size();
            result["truncated"] = (!empty_range && end < tip);
            result["blocks"] = std::move(blocks);
            return result;
        },
        .category = "blockchain",
        .help = "getavailableblocks [start] [count]\n\nLists active-chain blocks available through this node's API window.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"start", "count"}
    });
}

} // namespace geocoin::rpc
