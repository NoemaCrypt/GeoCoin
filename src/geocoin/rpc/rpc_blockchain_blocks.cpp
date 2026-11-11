#include "geocoin/rpc/rpc_server.hpp"
#include "rpc_blockchain_format.hpp"
#include "rpc_geography.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "rpc_blockchain_helpers.hpp"

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/consensus/validation.hpp"
#include <functional>
#include <set>
#include <utility>

namespace geocoin::rpc {
namespace {
constexpr uint64_t kMaxBlockTreeDepth = 32;
constexpr uint64_t kMaxBlockTreeNodes = 5000;
}

void RPCServer::register_blockchain_block_methods() {
    register_method({
        .name = "listblockgeography",
        .method = [this](const json& params) -> json {
            auto& chain = require_blockchain();
            const auto first = optional_u64_param(params, 0, 0, "from_height must be non-negative");
            const auto limit = optional_bounded_size_param(params, 1, 100, 1, 1000, "limit");
            json out = json::array();
            for (const auto& record : chain.get_block_geography_page(first, limit)) {
                std::optional<consensus::geo::GeoTagResult> exact;
                if (record.height == 0 && record.hash == consensus::Params().get_genesis_hash())
                    exact = consensus::geo::compute_geo_tag(0, {});
                out.push_back({{"height", record.height}, {"hash", hash256_to_hex_string(record.hash)},
                    {"geo_tag", record.geo_tag}, {"geopow", geopow_rules_to_json(record.height)},
                    {"index_pruned", record.index_pruned},
                    {"geography", geo_tag_to_json(record.geo_tag, exact)}});
            }
            return out;
        },
        .category = "blockchain",
        .help = "listblockgeography ( from_height limit )\nActive-chain geography page, including pruned history. Default limit 100, maximum 1000.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"from_height", "limit"}
    });
    register_method({
        .name = "getblock",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();

            std::string hash_str = required_param(params, 0, "Отсутствует параметр hash").get<std::string>();
            bool verbose = optional_param<bool>(params, 1, true);

            Hash256 hash;
            if (!hex_to_hash256_checked(hash_str, hash)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный хеш блока");
            }
            require_block_available_for_rpc(&blockchain, node_services_.info.get(), hash);

            core::Block block;
            if (!blockchain.get_block(hash, block)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Блок не найден");
            }

            if (!verbose) {
                auto serialized = block.serialize();
                return bytes_to_hex_string(serialized);
            }

            return block_to_verbose_json(block, hash, hash_str, blockchain);
        },
        .category = "blockchain",
        .help = "getblock \"hash\" ( verbose )\n\nВозвращает информацию о блоке с указанным хешем. Подробный ответ включает geo_tag и geography: WGS 84 / OGC:CRS84, GeoJSON [долгота,широта] в градусах, наноградусы и отдельные границы ячейки 0,1°. geography.status показывает доступность точного пересчёта.\n\nАргументы:\n1. \"hash\"    (string, required) Хеш блока\n2. verbose    (boolean, optional, default=true) true для JSON объекта, false для hex данных\n\nРезультат (для verbose = true):\n{\n  \"hash\" : \"hash\",     (string) хеш блока\n  \"confirmations\" : n,   (numeric) количество подтверждений\n  \"height\" : n,          (numeric) высота блока\n  ...\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"hash", "verbose"}
    });

    register_method({
        .name = "getblockbyheight",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();

            const Height height = required_integer_value<Height>(required_param(params, 0, "Отсутствует параметр height"), "height must be a non-negative integer");
            const bool verbose = optional_param<bool>(params, 1, true);

            Hash256 hash;
            if (!blockchain.get_active_block_hash_by_height(height, hash)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Блок на указанной высоте не найден");
            }
            require_block_available_for_rpc(&blockchain, node_services_.info.get(), hash);

            core::Block block;
            if (!blockchain.get_block(hash, block)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Блок не найден");
            }

            const std::string hash_str = hash256_to_hex_string(hash);
            if (!verbose) {
                return bytes_to_hex_string(block.serialize());
            }
            return block_to_verbose_json(block, hash, hash_str, blockchain);
        },
        .category = "blockchain",
        .help = "getblockbyheight height ( verbose )\n\nReturns an active-chain block by height. Equivalent to getblockhash+getblock. Verbose geography uses WGS 84 / OGC:CRS84, GeoJSON [longitude,latitude] in degrees, exact nanodegree fields, and a separate 0.1-degree cell. The exact canonical point is recoverable directly from the tag.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"height", "verbose"}
    });

    register_method({
        .name = "getblockchildren",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();
            std::string hash_str = required_param(params, 0, "Отсутствует параметр hash").get<std::string>();
            Hash256 hash;
            if (!hex_to_hash256_checked(hash_str, hash)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный хеш блока");
            }
            require_block_available_for_rpc(&blockchain, node_services_.info.get(), hash);

            auto storage = blockchain.get_block_storage();
            if (!storage) {
                throw RPCException(RPC_INTERNAL_ERROR, "BlockStorage не инициализирован");
            }

            const auto children = storage->get_children(hash);
            if (children.size() > kMaxBlockTreeNodes) {
                throw RPCException(RPC_MISC_ERROR, "Слишком много дочерних блоков");
            }
            json result = json::array();
            for (const auto& h : children) {
                result.push_back(hash256_to_hex_string(h));
            }
            return result;
        },
        .category = "blockchain",
        .help = "getblockchildren \"hash\"\n\nВозвращает дочерние блоки для указанного родителя.\n\nАргументы:\n1. \"hash\"    (string, required) Хеш родительского блока\n\nРезультат:\n[\n  \"childhash1\",\n  \"childhash2\"\n]\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"hash"}
    });

    register_method({
        .name = "getblocktree",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();
            std::string hash_str = required_param(params, 0, "Отсутствует параметр hash").get<std::string>();
            Hash256 root_hash;
            if (!hex_to_hash256_checked(hash_str, root_hash)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный хеш блока");
            }

            const uint64_t max_depth_raw = optional_u64_param(
                params, 1, 2, "Глубина должна быть неотрицательным целым числом");
            if (max_depth_raw > kMaxBlockTreeDepth) {
                throw RPCException(
                    RPC_INVALID_PARAMETER,
                    "Глубина дерева превышает безопасный лимит " +
                        std::to_string(kMaxBlockTreeDepth));
            }
            const int max_depth = static_cast<int>(max_depth_raw);

            const uint64_t max_nodes_raw = optional_u64_param(
                params, 2, 1000, "Лимит узлов должен быть целым числом");
            if (max_nodes_raw == 0 || max_nodes_raw > kMaxBlockTreeNodes) {
                throw RPCException(
                    RPC_INVALID_PARAMETER,
                    "Лимит узлов должен быть от 1 до " +
                        std::to_string(kMaxBlockTreeNodes));
            }
            const size_t max_nodes = static_cast<size_t>(max_nodes_raw);

            require_block_available_for_rpc(&blockchain, node_services_.info.get(), root_hash);

            auto storage = blockchain.get_block_storage();
            if (!storage) {
                throw RPCException(RPC_INTERNAL_ERROR, "BlockStorage не инициализирован");
            }

            size_t visited = 0;
            std::set<Hash256> seen;
            std::function<json(const Hash256&, int)> build_tree = [&](const Hash256& h, int depth) -> json {
                if (visited >= max_nodes) {
                    return json::object();
                }
                if (!seen.emplace(h).second) {
                    return json{
                        {"hash", hash256_to_hex_string(h)},
                        {"children", json::array()},
                        {"duplicate", true}
                    };
                }
                ++visited;

                json node;
                node["hash"] = hash256_to_hex_string(h);

                if (depth >= max_depth) {
                    node["children"] = json::array();
                    return node;
                }

                const auto children = storage->get_children(h);
                json arr = json::array();
                for (const auto& ch : children) {
                    if (visited >= max_nodes) {
                        break;
                    }
                    arr.push_back(build_tree(ch, depth + 1));
                }
                node["children"] = std::move(arr);
                return node;
            };

            json result;
            result["root"] = build_tree(root_hash, 0);
            result["max_depth"] = max_depth;
            result["max_nodes"] = max_nodes;
            result["nodes_returned"] = visited;
            return result;
        },
        .category = "blockchain",
        .help = "getblocktree \"hash\" (max_depth max_nodes)\n\nВозвращает дерево потомков от указанного блока. Безопасные лимиты: depth <= 32, nodes <= 5000.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"hash", "max_depth", "max_nodes"}
    });

    register_method({
        .name = "switchtotip",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();

            std::string hash_str = required_param(params, 0, "Отсутствует параметр hash").get<std::string>();
            Hash256 hash;
            if (!hex_to_hash256_checked(hash_str, hash)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный хеш блока");
            }
            require_block_available_for_rpc(&blockchain, node_services_.info.get(), hash);

            const auto activation = require_chain_mutations().activate_tip(hash);
            if (!activation.is_accepted()) {
                const std::string reason = activation.reject_reason.empty()
                    ? "chain-tip-activation-failed"
                    : activation.reject_reason;
                throw RPCException(RPC_INTERNAL_ERROR, "Не удалось переключить ветку: " + reason);
            }

            json out;
            out["result"] = "ok";
            out["active_tip"] = hash256_to_hex_string(activation.active_tip_hash);
            out["active_height"] = activation.active_height;
            out["changed"] = activation.active_tip_changed;
            return out;
        },
        .category = "blockchain",
        .help = "switchtotip \"hash\"\n\nПринудительно переключает активную цепь на указанную ветку (tip).\n\nАргументы:\n1. \"hash\"    (string, required) Хеш tip-блока\n\nРезультат:\n{\n  \"result\": \"ok\",\n  \"active_tip\": \"hash\",\n  \"active_height\": n\n}\n",
        .requires_wallet = false,
        .param_names = {"hash"},
        .mutates = true,
        .side_effects = "changes the active local chain tip"
    });
}

} // namespace geocoin::rpc
