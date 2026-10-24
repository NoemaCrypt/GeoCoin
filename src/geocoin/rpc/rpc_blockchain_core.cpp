#include "rpc_geography.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "rpc_blockchain_helpers.hpp"
#include "rpc_mining_helpers.hpp"

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include <algorithm>
#include <utility>

namespace geocoin::rpc {

void RPCServer::register_blockchain_core_methods() {
    register_method({
        .name = "getblockcount",
        .method = [this](const json& params) -> json {
            (void)params;
            return require_blockchain().get_height();
        },
        .category = "blockchain",
        .help = "getblockcount\n\nВозвращает количество блоков в самой длинной цепи.\n\nРезультат:\nn    (numeric) Текущее количество блоков\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "getblockrejectstats",
        .method = [this](const json& params) -> json {
            (void)params;
            auto stats = require_blockchain().get_block_reject_stats();
            json out;
            json consensus = json::object();
            json policy = json::object();
            for (const auto& [k, v] : stats.consensus) {
                consensus[k] = v;
            }
            for (const auto& [k, v] : stats.policy) {
                policy[k] = v;
            }
            out["consensus"] = std::move(consensus);
            out["policy"] = std::move(policy);
            return out;
        },
        .category = "blockchain",
        .help = "getblockrejectstats\n\nВозвращает статистику отклонений блоков с разбивкой по consensus/policy.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "getwitnesspubkeystats",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& chain_state = require_chain_state();

            auto stats = chain_state.get_witness_pubkey_stats();
            json result;
            result["total_keys"] = stats.total_keys;
            result["cache_hits"] = stats.cache_hits;
            result["cache_misses"] = stats.cache_misses;
            result["bytes_saved"] = stats.bytes_saved;
            result["bytes_saved_mb"] = static_cast<double>(stats.bytes_saved) / 1024.0 / 1024.0;
            return result;
        },
        .category = "blockchain",
        .help = "getwitnesspubkeystats\n\nВозвращает статистику witness pubkey cache.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "getinterestpool",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& chain_state = require_chain_state();

            const Amount pool = chain_state.get_interest_pool_balance();
            json result;
            result["balance_angles"] = pool;
            result["balance"] = static_cast<double>(pool) / COIN;
            result["activation_height"] = consensus::params::INTEREST_POOL_ACTIVATION_HEIGHT;
            result["current_height"] = chain_state.get_height();
            return result;
        },
        .category = "blockchain",
        .help = "getinterestpool\n\nВозвращает текущий баланс протокольного interest pool.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "getblockhash",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();

            Height height = required_integer_value<Height>(required_param(params, 0, "Отсутствует параметр height"), "height must be a non-negative integer");
            require_height_available_for_rpc(node_services_.info.get(), height);

            Hash256 hash{};
            if (!blockchain.get_active_block_hash_by_height(height, hash)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Высота блока вне диапазона");
            }

            return hash256_to_hex_string(hash);
        },
        .category = "blockchain",
        .help = "getblockhash height\n\nВозвращает хеш блока на указанной высоте.\n\nАргументы:\n1. height    (numeric, required) Высота блока\n\nРезультат:\n\"hash\"    (string) Хеш блока\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"height"}
    });

    register_method({
        .name = "getblockchaininfo",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& blockchain = require_blockchain();

            json result;
            result["chain"] = consensus::Params().network_id();
            result["consensus_ruleset"] = consensus::Params().consensus_ruleset_id();
            const Height blocks = blockchain.get_height();
            result["geopow"] = geopow_rules_to_json(blocks);
            result["geopow"]["next_selection_rule"] = consensus::geo::selection_rule(
                blocks + 1, consensus::Params().geopow_activation_height());
            Height headers = blocks;
            Height target_height = blocks;
            Height best_peer_height = blocks;
            bool syncing = false;
            bool initial_sync_complete = true;
            bool mining_blocked_by_sync = false;
            std::string mining_block_reason;

            if (node_services_.info) {
                const auto status = node_services_.info->get_status();
                target_height = std::max(status.target_height, blocks);
                best_peer_height = std::max(status.best_peer_height, blocks);
                headers = target_height;
                syncing = status.syncing;
                initial_sync_complete = status.initial_sync_complete;
                mining_blocked_by_sync = status.mining_blocked_by_sync;
                mining_block_reason = status.mining_block_reason;
            }

            result["blocks"] = blocks;
            result["headers"] = headers;
            result["target_height"] = target_height;
            result["best_peer_height"] = best_peer_height;
            result["best_peer_height_is_untrusted_hint"] = true;
            result["syncing"] = syncing;
            result["initial_sync_complete"] = initial_sync_complete;
            result["mining_blocked_by_sync"] = mining_blocked_by_sync;
            if (!mining_block_reason.empty()) {
                result["mining_block_reason"] = mining_block_reason;
            }

            auto* tip = blockchain.get_tip();
            if (tip) {
                auto hash = tip->get_block_hash();
                result["bestblockhash"] = hash256_to_hex_string(hash);
                result["chainwork"] = uint256_to_hex_string(tip->chain_work);
                result["difficulty"] = bits_to_difficulty(tip->get_bits());
            } else {
                result["chainwork"] = uint256_to_hex_string(uint256{});
                result["difficulty"] = 1.0;
            }
            double verification_progress = 1.0;
            if (headers > 0) {
                verification_progress = static_cast<double>(blocks) / static_cast<double>(headers);
                verification_progress = std::clamp(verification_progress, 0.0, 1.0);
            }
            result["verificationprogress"] = verification_progress;

            json chain_selection;
            chain_selection["rule"] = "most_valid_chainwork";
            chain_selection["startingheight_is_hint"] = true;
            chain_selection["seed_nodes_are_authority"] = false;
            chain_selection["active_tip_status"] = "active";
            result["chain_selection"] = std::move(chain_selection);

            const auto checkpoint = blockchain.get_policy_checkpoint_info();
            json reorg_policy;
            reorg_policy["max_reorg_depth"] = consensus::params::MAX_REORG_DEPTH;
            reorg_policy["policy_type"] = "local-node-policy";
            json policy_checkpoint;
            policy_checkpoint["enabled"] = checkpoint.has_checkpoint;
            policy_checkpoint["height"] = checkpoint.height;
            policy_checkpoint["hash"] = hash256_to_hex_string(checkpoint.hash);
            policy_checkpoint["interval"] = checkpoint.interval;
            policy_checkpoint["confirmations"] = checkpoint.confirmations;
            policy_checkpoint["allow_deep_reorg"] = checkpoint.allow_deep_reorg;
            reorg_policy["policy_checkpoint"] = std::move(policy_checkpoint);
            result["reorg_policy"] = std::move(reorg_policy);

            if (node_services_.info) {
                const auto pruned = node_services_.info->get_pruned_data_info();
                result["node_type"] = pruned.enabled ? "pruned" : "full";
                result["pruned"] = pruned.enabled;
                json pruned_data;
                pruned_data["enabled"] = pruned.enabled;
                pruned_data["retention_blocks"] = pruned.retention;
                pruned_data["first_available_height"] = pruned.first_available_height;
                pruned_data["tip_height"] = pruned.tip_height;
                result["pruned_data"] = std::move(pruned_data);
            }

            if (chain_state_) {
                result["prune_depth"] = chain_state_->get_prune_depth();
                result["archive"] = chain_state_->is_archive_mode();
                Height pruned_height = 0;
                if (chain_state_->get_witness_pruned_height(pruned_height)) {
                    result["pruned_height"] = pruned_height;
                }
                storage::BlockStorage::PruningStats stats;
                if (chain_state_->get_pruning_stats(stats)) {
                    json pr;
                    pr["total_blocks"] = stats.total_blocks;
                    pr["pruned_blocks"] = stats.pruned_blocks;
                    pr["body_bytes_reduced"] = stats.body_bytes_reduced;
                    pr["bytes_total"] = stats.bytes_total;
                    double percent = 0.0;
                    if (stats.bytes_total > 0) {
                        percent = (static_cast<double>(stats.body_bytes_reduced) * 100.0) /
                                  static_cast<double>(stats.bytes_total);
                    }
                    pr["body_reduction_percent"] = percent;
                    result["pruning"] = std::move(pr);
                }
            }

            return result;
        },
        .category = "blockchain",
        .help = "getblockchaininfo\n\nВозвращает информацию о состоянии блокчейна.\n\nРезультат:\n{\n  \"chain\": \"xxxx\",        (string) название сети\n  \"blocks\": xxxxxx,         (numeric) текущая высота активной цепи\n  \"headers\": xxxxxx,        (numeric) высота лучшей проверенной header-chain\n  \"target_height\": xxxxxx,  (numeric) проверенная цель sync manager\n  \"best_peer_height\": n,    (numeric) непроверенная VERSION-подсказка пиров\n  \"syncing\": true|false,    (bool) активная загрузка блоков\n  \"initial_sync_complete\": true|false, (bool) локальная цепь догнала проверенные заголовки\n  \"verificationprogress\": n,(numeric) blocks/headers, 0.0..1.0\n  \"bestblockhash\": \"...\", (string) хеш лучшего блока\n  \"chainwork\": \"...\",     (string) накопленная работа активной цепи\n  \"chain_selection\": { ... }, (object) правило выбора публичной цепи\n  \"reorg_policy\": { ... }, (object) локальная reorg-policy ноды\n  \"node_type\": \"full|pruned\", (string) тип ноды\n  \"pruned\": true|false,    (bool) включен ли pruned API window\n  \"pruned_data\": { ... },  (object) границы доступной исторической истории\n  \"prune_depth\": n,         (numeric) глубина pruning witness\n  \"pruned_height\": n,       (numeric) высота, до которой pruned\n  \"archive\": true|false,    (bool) архивный режим\n  \"pruning\": { ... }        (object) статистика экономии\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });
}

} // namespace geocoin::rpc
