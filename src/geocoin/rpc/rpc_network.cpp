#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/network/peer_manager.hpp"
#include "geocoin/utils/logger.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdlib>

namespace geocoin::rpc {

namespace {
inline bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}
}

void RPCServer::register_network_methods() {

    // getpeerinfo - Возвращает данные о каждом подключенном узле
    register_method({
        .name = "getpeerinfo",
        .method = [this](const json& params) -> json {
            json result = json::array();

            if (!peer_manager_) {
                return result;
            }

            // Получаем статистику всех пиров
            auto peer_stats = peer_manager_->get_peer_stats();

            for (const auto& stat : peer_stats) {
                json peer;
                peer["id"] = stat.id;
                peer["addr"] = stat.address;
                peer["state"] = stat.state;
                peer["authenticated"] = stat.authenticated;
                peer["transport_encrypted"] = stat.encrypted;
                peer["secure_wire_version"] = stat.secure_wire_version;
                peer["secure_suite"] = stat.secure_suite;
                peer["secure_suite_name"] = stat.encrypted
                    ? "ML-KEM-768+X25519/ChaCha20-Poly1305"
                    : "";
                peer["inbound"] = stat.inbound;
                peer["conntime"] = stat.conntime;
                peer["bytessent"] = stat.bytes_sent;
                peer["bytesrecv"] = stat.bytes_recv;
                peer["version"] = stat.version;
                peer["subver"] = stat.subver;
                peer["startingheight"] = stat.starting_height;
                peer["auth_address"] = stat.auth_address.str();
                peer["lastsend"] = stat.last_send;
                peer["lastrecv"] = stat.last_recv;
                peer["policy_score"] = stat.policy_score;
                peer["last_message_type"] = stat.last_message_type;
                peer["last_message_age_ms"] = stat.last_message_age_ms;
                peer["last_useful_age_ms"] = stat.last_useful_age_ms;
                peer["ping_rtt_ms"] = stat.ping_rtt_ms;
                peer["last_disconnect_reason"] = stat.last_disconnect_reason;

                result.push_back(peer);
            }

            return result;
        },
        .category = "network",
        .help = "getpeerinfo\n\nВозвращает данные о каждом подключенном узле, включая transport_encrypted, secure_wire_version и secure_suite.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::ADMIN,
        .capabilities = {"diagnostics"}
    });

    // getconnectioncount - Возвращает количество соединений с другими узлами
    register_method({
        .name = "getconnectioncount",
        .method = [this](const json& params) -> json {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("rpc", "getconnectioncount called");
            }

            if (!peer_manager_) {
                GEO_LOG_WARN("rpc", "getconnectioncount: peer_manager is null");
                return 0;
            }

            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("rpc", "getconnectioncount: calling get_peer_count()");
            }
            size_t count = peer_manager_->get_peer_count();
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("rpc", "getconnectioncount: get_peer_count returned " << count);
            }

            // Возвращаем количество подключенных пиров
            return static_cast<int>(count);
        },
        .category = "network",
        .help = "getconnectioncount\n\nВозвращает количество соединений с другими узлами.\n\nРезультат:\nn          (numeric) Количество соединений\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    // addnode - Попытаться добавить или удалить узел из списка добавленных узлов
    register_method({
        .name = "addnode",
        .method = [this](const json& params) -> json {
            require_min_params(params, 2, "Отсутствуют параметры: node, command");

            std::string node = params[0].get<std::string>();
            std::string command = params[1].get<std::string>();
            std::string host;
            uint16_t port = 0;
            if (!network::NetworkUtils::parse_peer_address(node, host, port)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Некорректный peer address");
            }
            node = network::NetworkUtils::make_endpoint(host, port);

            if (command != "add" && command != "remove" && command != "onetry") {
                throw RPCException(RPC_INVALID_PARAMETER, "Команда должна быть 'add', 'remove' или 'onetry'");
            }

            auto& peer_manager = require_peer_manager();

            // Реализация как в Bitcoin (rpcnet.cpp:addnode)
            if (command == "onetry") {
                // Попытка подключения один раз
                // В Bitcoin: OpenNetworkConnection(addr, NULL, strNode.c_str());
                if (!peer_manager.connect_to_node(node, bytes())) {
                    throw RPCException(RPC_CLIENT_NODE_NOT_CONNECTED, "Ошибка: не удалось подключиться к узлу");
                }
                return nullptr;
            }

            // Получаем список добавленных узлов
            auto added_nodes = peer_manager.get_added_nodes();
            auto it = std::find(added_nodes.begin(), added_nodes.end(), node);

            if (command == "add") {
                // Добавляем узел в список
                // В Bitcoin: vAddedNodes.push_back(strNode);
                if (it != added_nodes.end()) {
                    throw RPCException(RPC_CLIENT_NODE_ALREADY_ADDED, "Ошибка: узел уже добавлен");
                }
                peer_manager.add_node(node);
            }
            else if (command == "remove") {
                // Удаляем узел из списка
                // В Bitcoin: vAddedNodes.erase(it);
                if (it == added_nodes.end()) {
                    throw RPCException(RPC_CLIENT_NODE_NOT_ADDED, "Ошибка: узел не был добавлен");
                }
                peer_manager.remove_node(node);
            }

            return nullptr;
        },
        .category = "network",
        .help = "addnode \"node\" \"add|remove|onetry\"\n\nПопытаться добавить или удалить узел из списка добавленных узлов.\nИли попробовать соединение с узлом один раз.\n\nАргументы:\n1. \"node\"     (string, required) Узел (IP:port)\n2. \"command\"  (string, required) 'add' для добавления узла в список, 'remove' для удаления узла из списка, 'onetry' для попытки соединения с узлом один раз\n\nПример:\n> geocoin-cli addnode \"192.168.0.6:3030\" \"onetry\"\n",
        .requires_wallet = false,
        .param_names = {"node", "command"},
        .mutates = true,
        .side_effects = "changes manual peer targets or opens a one-shot connection"
    });

    register_method({
        .name = "getaddednodeinfo",
        .method = [this](const json& params) -> json {
            (void)params;
            json result = json::array();
            if (!peer_manager_) {
                return result;
            }

            const auto added_nodes = peer_manager_->get_added_nodes();
            const auto peer_stats = peer_manager_->get_peer_stats();
            for (const auto& node : added_nodes) {
                json row;
                row["addr"] = node;
                row["connected"] = false;
                row["authenticated"] = false;
                row["state"] = "disconnected";
                for (const auto& stat : peer_stats) {
                    if (stat.address == node) {
                        row["connected"] = true;
                        row["authenticated"] = stat.authenticated;
                        row["state"] = stat.state;
                        row["inbound"] = stat.inbound;
                        break;
                    }
                }
                result.push_back(std::move(row));
            }
            return result;
        },
        .category = "network",
        .help = "getaddednodeinfo\n\nВозвращает список persistent addnode targets и их текущее состояние.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::ADMIN,
        .capabilities = {"diagnostics"}
    });

    // getp2ppolicystats - Статистика policy-отклонений P2P
    register_method({
        .name = "getp2ppolicystats",
        .method = [this](const json& params) -> json {
            json result;
            if (!peer_manager_) {
                result["policy_rejects_total"] = 0;
                result["active_temp_bans"] = 0;
                result["reject_reasons"] = json::object();
                return result;
            }

            const auto stats = peer_manager_->get_stats();
            result["policy_rejects_total"] = stats.policy_rejects;
            result["active_temp_bans"] = stats.active_temp_bans;

            json reasons = json::object();
            for (const auto& [reason, cnt] : peer_manager_->get_policy_reject_counters()) {
                reasons[reason] = cnt;
            }
            result["reject_reasons"] = std::move(reasons);
            return result;
        },
        .category = "network",
        .help = "getp2ppolicystats\n\nВозвращает статистику policy-отклонений P2P (rate-limit, malformed INV/GETDATA, временные баны).\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    // getconnectiondiagnostics - Расширенная диагностика P2P соединений
    register_method({
        .name = "getconnectiondiagnostics",
        .method = [this](const json& params) -> json {
            (void)params;
            json result;
            if (!peer_manager_) {
                result["stats"] = json::object();
                result["peers"] = json::array();
                result["outbound_attempts"] = json::array();
                result["recent_disconnects"] = json::array();
                result["disconnect_reasons"] = json::object();
                result["policy_rejects"] = json::object();
                return result;
            }

            const auto diagnostics = peer_manager_->get_connection_diagnostics();
            result["stats"] = {
                {"total_peers", diagnostics.stats.total_peers},
                {"connected_peers", diagnostics.stats.connected_peers},
                {"authenticated_peers", diagnostics.stats.authenticated_peers},
                {"encrypted_peers", diagnostics.stats.encrypted_peers},
                {"inbound_peers", diagnostics.stats.inbound_peers},
                {"outbound_peers", diagnostics.stats.outbound_peers},
                {"total_bytes_sent", diagnostics.stats.total_bytes_sent},
                {"total_bytes_received", diagnostics.stats.total_bytes_received},
                {"policy_rejects", diagnostics.stats.policy_rejects},
                {"active_temp_bans", diagnostics.stats.active_temp_bans}
            };

            result["peers"] = json::array();
            for (const auto& stat : diagnostics.peers) {
                result["peers"].push_back({
                    {"id", stat.id},
                    {"addr", stat.address},
                    {"state", stat.state},
                    {"authenticated", stat.authenticated},
                    {"transport_encrypted", stat.encrypted},
                    {"secure_wire_version", stat.secure_wire_version},
                    {"secure_suite", stat.secure_suite},
                    {"inbound", stat.inbound},
                    {"policy_score", stat.policy_score},
                    {"last_message_type", stat.last_message_type},
                    {"last_message_age_ms", stat.last_message_age_ms},
                    {"last_useful_age_ms", stat.last_useful_age_ms},
                    {"ping_rtt_ms", stat.ping_rtt_ms},
                    {"last_disconnect_reason", stat.last_disconnect_reason},
                    {"bytessent", stat.bytes_sent},
                    {"bytesrecv", stat.bytes_recv}
                });
            }

            result["outbound_attempts"] = json::array();
            for (const auto& attempt : diagnostics.outbound_attempts) {
                result["outbound_attempts"].push_back({
                    {"addr", attempt.address},
                    {"fail_count", attempt.fail_count},
                    {"last_attempt_age_ms", attempt.last_attempt_age_ms},
                    {"last_success_age_ms", attempt.last_success_age_ms},
                    {"next_attempt_in_ms", attempt.next_attempt_in_ms},
                    {"last_error", attempt.last_error},
                    {"manual", attempt.manual},
                    {"in_backoff", attempt.in_backoff},
                    {"in_progress", attempt.in_progress}
                });
            }

            result["recent_disconnects"] = json::array();
            for (const auto& disconnect : diagnostics.recent_disconnects) {
                result["recent_disconnects"].push_back({
                    {"addr", disconnect.address},
                    {"reason", disconnect.reason},
                    {"disconnected_at", disconnect.disconnected_at},
                    {"policy_score", disconnect.policy_score},
                    {"inbound", disconnect.inbound}
                });
            }

            json disconnect_reasons = json::object();
            for (const auto& [reason, count] : diagnostics.disconnect_reasons) {
                disconnect_reasons[reason] = count;
            }
            result["disconnect_reasons"] = std::move(disconnect_reasons);

            json policy_rejects = json::object();
            for (const auto& [reason, count] : diagnostics.policy_rejects) {
                policy_rejects[reason] = count;
            }
            result["policy_rejects"] = std::move(policy_rejects);
            return result;
        },
        .category = "network",
        .help = "getconnectiondiagnostics\n\nВозвращает расширенную диагностику P2P: peers, outbound backoff, recent disconnects, disconnect reasons и policy counters.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::ADMIN,
        .capabilities = {"diagnostics"}
    });

    // disconnectnode - Разорвать соединение с конкретным пиром
    register_method({
        .name = "disconnectnode",
        .method = [this](const json& params) -> json {
            auto& peer_manager = require_peer_manager();
            const std::string address = required_param(params, 0, "Нужен параметр address").get<std::string>();
            if (!peer_manager.disconnect_peer(address)) {
                throw RPCException(RPC_CLIENT_NODE_NOT_CONNECTED, "Узел не подключен");
            }
            return true;
        },
        .category = "network",
        .help = "disconnectnode \"address\"\n\nРазрывает соединение с указанным пиром (по IP:port или IP).\n",
        .requires_wallet = false,
        .param_names = {"address"},
        .mutates = true,
        .side_effects = "disconnects a peer"
    });

    // setban - Ручной бан пира
    register_method({
        .name = "setban",
        .method = [this](const json& params) -> json {
            auto& peer_manager = require_peer_manager();
            require_min_params(params, 2, "Нужны параметры: subnet action [bantime]");
            const std::string subnet = params[0].get<std::string>();
            const std::string action = params[1].get<std::string>();
            const uint64_t bantime = has_param(params, 2)
                ? required_u64_value(params[2], "bantime должен быть числом")
                : 24ull * 60ull * 60ull;

            if (action == "add") {
                if (!peer_manager.set_manual_ban(subnet, bantime)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Некорректный адрес/подсеть для бана");
                }
                return true;
            }
            if (action == "remove") {
                if (!peer_manager.remove_manual_ban(subnet)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Бан для адреса не найден");
                }
                return true;
            }
            throw RPCException(RPC_INVALID_PARAMETER, "action должен быть 'add' или 'remove'");
        },
        .category = "network",
        .help = "setban \"subnet\" \"add|remove\" (bantime)\n\nДобавляет или удаляет ручной бан по IP/endpoint.\n",
        .requires_wallet = false,
        .param_names = {"subnet", "action", "bantime"},
        .mutates = true,
        .side_effects = "changes manual peer bans"
    });

    register_method({
        .name = "listbans",
        .method = [this](const json& params) -> json {
            json result = json::array();
            if (!peer_manager_) {
                return result;
            }
            for (const auto& b : peer_manager_->list_bans()) {
                json row;
                row["address"] = b.address;
                row["banned_until"] = b.banned_until;
                row["manual"] = b.manual;
                result.push_back(std::move(row));
            }
            return result;
        },
        .category = "network",
        .help = "listbans\n\nПоказывает активные баны пиров.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::ADMIN,
        .capabilities = {"diagnostics"}
    });

    register_method({
        .name = "clearbans",
        .method = [this](const json& params) -> json {
            require_peer_manager().clear_manual_bans();
            return true;
        },
        .category = "network",
        .help = "clearbans\n\nОчищает все ручные баны.\n",
        .requires_wallet = false,
        .mutates = true,
        .side_effects = "clears manual peer bans"
    });
}

} // namespace geocoin::rpc
