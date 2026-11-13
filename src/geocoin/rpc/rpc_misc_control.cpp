#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/utils/version.hpp"

#include "geocoin/core/blockchain.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include <algorithm>
#include <utility>

namespace geocoin::rpc {
namespace {

const char* node_type_to_string(node::NodeType type) {
    switch (type) {
        case node::NodeType::FULL:
            return "full";
        case node::NodeType::PRUNED:
            return "pruned";
    }
    return "unknown";
}

} // namespace

void RPCServer::register_misc_control_methods() {
    register_method({
        .name = "help",
        .method = [this](const json& params) -> json {
            std::string command = params.size() > 0 ? params[0].get<std::string>() : "";
            if (!command.empty() && !is_valid_rpc_method_name(command)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Invalid help method name");
            }
            return get_help(command);
        },
        .category = "control",
        .help = "help ( \"command\" )\n\nСписок команд, доступных текущей RPC-роли, или справка по указанной команде. Справка по чужой роли не раскрывается.\n\nАргументы:\n1. \"command\"    (string, optional) Команда для получения справки\n\nРезультат:\n\"text\"    (string) Текст справки\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"command"}
    });

    register_method({
        .name = "getrpcinfo",
        .method = [this](const json& params) -> json {
            (void)params;

            json commands = json::array();
            json categories = json::object();
            json roles = json::object();

            for (const auto& [name, command] : command_registry_.commands()) {
                const RPCAuthRole viewer = rpc_request_auth_role();
                if (viewer != RPCAuthRole::ADMIN &&
                    !is_rpc_command_authorized(viewer, command)) {
                    continue;
                }
                const char* role = rpc_role_name(command.required_role);

                const auto capabilities = rpc_command_capabilities(command);
                const bool readonly_capable =
                    std::find(capabilities.begin(), capabilities.end(), "readonly") != capabilities.end();

                json item;
                item["name"] = name;
                item["category"] = command.category;
                item["role"] = role;
                item["capabilities"] = capabilities;
                item["requires_wallet"] = command.requires_wallet;
                item["safe_mode"] = command.safe_mode;
                item["readonly"] = readonly_capable;
                item["mutates"] = command.mutates;
                item["side_effect_free"] = !command.mutates;
                item["stability"] = command.stability;
                item["since"] = command.since;
                item["deprecated"] = !command.deprecation.empty();
                if (!command.deprecation.empty()) {
                    item["deprecation"] = command.deprecation;
                }
                if (!command.side_effects.empty()) {
                    item["side_effects"] = command.side_effects;
                }
                item["param_names"] = command.param_names;
                item["variadic_params"] = command.variadic_params;
                commands.push_back(std::move(item));

                categories[command.category] = categories.value(command.category, 0) + 1;
                roles[role] = roles.value(role, 0) + 1;
            }

            json result;
            // Version 2 is reserved for a post-launch API revision.
            result["api_version"] = "1";
            result["jsonrpc"] = "2.0";
            result["command_profile"] = rpc_command_profile_name(command_registry_.profile());
            result["command_count"] = commands.size();
            result["categories"] = std::move(categories);
            result["roles"] = std::move(roles);
            result["role_capabilities"] = {
                {"admin", rpc_role_capabilities(RPCAuthRole::ADMIN)},
                {"readonly", rpc_role_capabilities(RPCAuthRole::READONLY)},
                {"miner", rpc_role_capabilities(RPCAuthRole::MINER)}
            };
            result["commands"] = std::move(commands);
            return result;
        },
        .category = "control",
        .help = "getrpcinfo\n\nMachine-readable catalog of RPC commands visible to the current credential role.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "stop",
        .method = [this](const json& params) -> json {
            (void)params;
            request_shutdown();
            return "GeoCoin server stopping";
        },
        .category = "control",
        .help = "stop\n\nОстановить сервер GeoCoin.\n",
        .requires_wallet = false,
        .mutates = true,
        .side_effects = "requests node shutdown"
    });

    register_method({
        .name = "getinfo",
        .method = [this](const json& params) -> json {
            (void)params;
            json result;
            result["version"] = version::APPLICATION;
            result["protocolversion"] = 1;

            if (blockchain_) {
                result["blocks"] = blockchain_->get_height();
            }

            result["wallet_loaded"] = wallet_ != nullptr;

            result["crypto"] = "FALCON-512";
            result["addressprefix"] = "GEO";
            return result;
        },
        .category = "control",
        .help = "getinfo\n\nВозвращает объект с публичной информацией о состоянии.\n\nРезультат:\n{\n  \"version\": xxxxx,              (numeric) версия сервера\n  \"protocolversion\": xxxxx,      (numeric) версия протокола\n  \"blocks\": xxxxxx,              (numeric) текущее количество блоков\n  \"wallet_loaded\": true|false,   (boolean) кошелек загружен без раскрытия баланса\n  \"crypto\": \"FALCON-512\",      (string) используемая криптосистема\n  ...\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "getnodeinfo",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& node = require_node_info();

            const auto cfg = node.get_config();
            const auto status = node.get_status();
            json result;
            result["version"] = version::APPLICATION;
            result["protocolversion"] = 1;
            result["network"] = consensus::Params().network_id();
            result["running"] = status.running;
            result["node_type"] = node_type_to_string(cfg.node_type);
            result["archive"] = cfg.archive;
            result["p2p_port"] = cfg.p2p_port;
            result["rpc_port"] = cfg.rpc_port;
            result["p2p_listen"] = cfg.enable_p2p_listen;
            result["auto_connect"] = cfg.enable_auto_connect;
            result["db_backend"] = cfg.db_backend;
            result["db_profile"] = cfg.db_profile;
            result["connected_peers"] = status.connected_peers;
            result["authenticated_peers"] = status.authenticated_peers;
            result["mempool_size"] = status.mempool_size;
            result["current_height"] = status.current_height;
            result["target_height"] = status.target_height;
            result["best_peer_height"] = status.best_peer_height;
            result["syncing"] = status.syncing;
            result["initial_sync_complete"] = status.initial_sync_complete;
            result["mining"] = status.mining;
            result["mining_threads"] = status.mining_threads;
            result["hashrate"] = status.hashrate;

            json limits;
            limits["max_inbound_peers"] = cfg.max_inbound_peers;
            limits["max_outbound_peers"] = cfg.max_outbound_peers;
            limits["mempool_max_size"] = cfg.mempool_max_size;
            limits["max_parallel_downloads"] = cfg.max_parallel_downloads;
            result["limits"] = std::move(limits);

            json pruning;
            pruning["retention_blocks"] = cfg.pruned_block_retention;
            pruning["delete_old_blocks"] = cfg.pruned_delete_old_blocks;
            pruning["delete_old_indexes"] = cfg.pruned_delete_old_indexes;
            result["pruned_config"] = std::move(pruning);
            return result;
        },
        .category = "control",
        .help = "getnodeinfo\n\nВозвращает runtime-конфигурацию и базовое состояние ноды.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "getsyncstatus",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& node = require_node_info();

            const auto status = node.get_status();
            json result;
            result["current_height"] = status.current_height;
            result["target_height"] = status.target_height;
            result["best_peer_height"] = status.best_peer_height;
            result["best_peer_height_is_untrusted_hint"] = true;
            result["headers"] = status.target_height;
            result["syncing"] = status.syncing;
            result["initial_sync_complete"] = status.initial_sync_complete;
            result["mining_blocked_by_sync"] = status.mining_blocked_by_sync;
            if (!status.mining_block_reason.empty()) {
                result["mining_block_reason"] = status.mining_block_reason;
            }
            result["connected_peers"] = status.connected_peers;
            result["authenticated_peers"] = status.authenticated_peers;
            double verification_progress = 1.0;
            const Height headers = status.target_height;
            if (headers > 0) {
                verification_progress =
                    static_cast<double>(status.current_height) / static_cast<double>(headers);
                verification_progress = std::clamp(verification_progress, 0.0, 1.0);
            }
            result["verificationprogress"] = verification_progress;
            return result;
        },
        .category = "blockchain",
        .help = "getsyncstatus\n\nВозвращает детальный статус синхронизации без прочих blockchain полей.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "gethealth",
        .method = [this](const json& params) -> json {
            (void)params;
            json result;
            bool ok = true;
            bool live = true;
            bool ready = true;
            json components = json::object();
            json sync = json::object();

            if (!node_services_.info) {
                ok = false;
                ready = false;
                result["node"] = "missing";
                components["node"] = "missing";
            } else {
                const auto status = node_services_.info->get_status();
                result["node"] = status.running ? "running" : "stopped";
                result["syncing"] = status.syncing;
                result["initial_sync_complete"] = status.initial_sync_complete;
                result["current_height"] = status.current_height;
                result["target_height"] = status.target_height;
                result["connected_peers"] = status.connected_peers;
                result["authenticated_peers"] = status.authenticated_peers;
                sync["syncing"] = status.syncing;
                sync["initial_sync_complete"] = status.initial_sync_complete;
                sync["current_height"] = status.current_height;
                sync["target_height"] = status.target_height;
                sync["best_peer_height"] = status.best_peer_height;
                sync["mining_blocked_by_sync"] = status.mining_blocked_by_sync;
                if (!status.mining_block_reason.empty()) {
                    sync["mining_block_reason"] = status.mining_block_reason;
                }
                components["node"] = status.running ? "ok" : "stopped";
                if (!status.running) {
                    ok = false;
                    live = false;
                    ready = false;
                }
            }

            const bool chain_state_ready =
                chain_state_ && chain_state_->has_derived_state_snapshot();
            result["blockchain"] = blockchain_ ? "ok" : "missing";
            result["chain_state"] = !chain_state_
                ? "missing"
                : (chain_state_ready ? "ok" : "rebuild-required");
            result["mempool"] = mempool_ ? "ok" : "missing";
            result["peer_manager"] = peer_manager_ ? "ok" : "missing";
            components["blockchain"] = blockchain_ ? "ok" : "missing";
            components["chain_state"] = !chain_state_
                ? "missing"
                : (chain_state_ready ? "ok" : "rebuild-required");
            components["mempool"] = mempool_ ? "ok" : "missing";
            components["peer_manager"] = peer_manager_ ? "ok" : "missing";
            if (!blockchain_ || !chain_state_ready || !mempool_ || !peer_manager_) {
                ok = false;
                ready = false;
            }

            if (mempool_) {
                const auto stats = mempool_->get_stats();
                json mempool_stats;
                mempool_stats["size"] = stats.tx_count;
                mempool_stats["bytes"] = stats.total_size;
                mempool_stats["maxmempool"] = mempool_->max_size_limit();
                mempool_stats["transactions_updated"] = mempool_->get_transactions_updated();
                result["mempool_stats"] = std::move(mempool_stats);
            }
            result["ok"] = ok;
            result["live"] = live;
            result["ready"] = ready;
            result["status"] = ok ? "ok" : (live ? "degraded" : "down");
            result["components"] = std::move(components);
            if (!sync.empty()) {
                result["sync"] = std::move(sync);
            }
            return result;
        },
        .category = "control",
        .help = "gethealth\n\nВозвращает короткий health-check для мониторинга.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });
}

} // namespace geocoin::rpc
