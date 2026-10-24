#include "geocoin/node/node.hpp"
#include "node_connection_plan.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/network/addrman.hpp"
#include "geocoin/network/dns_resolver.hpp"
#include "geocoin/network/dns_seeds.h"
#include "geocoin/node/node_runtime.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using geocoin::utils::LogPrintf;

namespace geocoin::node {

using detail::PeerBootstrapPlan;
using detail::add_bootstrap_endpoints;
using detail::append_runtime_priority_targets;
using detail::build_peer_bootstrap_plan;
using detail::is_self_endpoint;

std::vector<std::string> Node::get_default_seed_nodes() const {
    if (consensus::Params().get_network() == consensus::Network::LOADNET) {
        // Loadnet is intentionally local/manual and must never discover or
        // contact public mainnet/testnet peers.
        return {};
    }
    const bool use_testnet = (consensus::Params().network_id() == "degree-test");

    // 1. Пробуем DNS Seeds
    auto dns_addresses = geocoin::network::DNSResolver::resolve_seeds_with_port(
        geocoin::network::get_dns_seed_hostnames(use_testnet),
        use_testnet ? geocoin::network::DEFAULT_TESTNET_P2P_PORT : geocoin::network::DEFAULT_P2P_PORT
    );

    if (!dns_addresses.empty()) {
        if (detail::node_verbose_enabled()) {
            LogPrintf("Found %zu addresses from DNS seeds\n", dns_addresses.size());
        }
        return dns_addresses;
    }

    // 2. Если DNS не сработал, используем hardcoded seeds
    std::vector<std::string> seeds = geocoin::network::get_hardcoded_seed_nodes(use_testnet);

    if (!seeds.empty()) {
        if (detail::node_verbose_enabled()) {
            LogPrintf("Using %zu hardcoded seed nodes\n", seeds.size());
        }
    } else {
        if (detail::node_verbose_enabled()) {
            LogPrintf("No seed nodes available\n");
        }
    }

    return seeds;
}

void Node::connection_loop() {
    if (detail::node_verbose_enabled()) {
        LogPrintf("Connection thread started\n");
    }

    const int self_check_port = p2p_server_ ? static_cast<int>(config_.p2p_port) : 0;
    const bool use_testnet = (consensus::Params().network_id() == "degree-test");
    const bool connect_only_mode = !config_.connect_nodes.empty();
    const PeerBootstrapPlan bootstrap_plan = build_peer_bootstrap_plan(
        config_,
        connect_only_mode,
        use_testnet
    );

    // Создаем/переиспользуем общий AddrMan (доступен и из обработчика ADDR).
    std::shared_ptr<geocoin::network::AddrMan> addrman;
    {
        std::lock_guard<std::mutex> lock(addrman_mutex_);
        if (!addrman_) {
            addrman_ = std::make_shared<geocoin::network::AddrMan>();
        }
        addrman = addrman_;
    }

    // Загружаем известные адреса из отдельной address-зоны.
    const std::filesystem::path addresses_dir = std::filesystem::path(config_.data_dir) / "addresses";
    std::filesystem::create_directories(addresses_dir);
    std::string peers_file = (addresses_dir / "peers.dat").string();
    addrman->load(peers_file);
    if (detail::node_verbose_enabled()) {
        LogPrintf("Loaded %zu peer addresses from %s\n", addrman->size(), peers_file.c_str());
    }

    // Важно: peers.dat может содержать наши собственные адреса (после смены сети/VM/интерфейсов).
    // Такие адреса удаляем сразу, чтобы не тратить циклы на self-connection.
    {
        size_t removed_self = 0;
        for (const auto& addr : addrman->get_all()) {
            if (is_self_endpoint(addr, self_check_port)) {
                if (addrman->remove(addr)) {
                    removed_self++;
                }
            }
        }
        if (removed_self > 0 && detail::node_verbose_enabled()) {
            LogPrintf("Pruned %zu self addresses from AddrMan cache\n", removed_self);
        }
    }

    // Добавляем seed nodes из конфига (если указаны через -addnode)
    if (!connect_only_mode && !config_.seed_nodes.empty()) {
        if (detail::node_verbose_enabled()) {
            LogPrintf("Adding %zu seed nodes from config\n", config_.seed_nodes.size());
        }
        (void)add_bootstrap_endpoints(
            *addrman,
            config_.seed_nodes,
            "config",
            self_check_port,
            "Skipped self seed from config"
        );
    }

    // Публичные seed endpoints добавляем сразу, а не только после DNS-delay.
    // Иначе локальный fallback 127.0.0.1 делает AddrMan непустым и GUI может
    // ждать 11 секунд до первой реальной попытки подключения к сети.
    if (!connect_only_mode && config_.enable_auto_connect && !bootstrap_plan.default_seed_nodes.empty()) {
        if (detail::node_verbose_enabled()) {
            LogPrintf("Adding %zu default seed endpoints for fast bootstrap\n",
                      bootstrap_plan.default_seed_nodes.size());
        }
        (void)add_bootstrap_endpoints(
            *addrman,
            bootstrap_plan.default_seed_nodes,
            "default-seed",
            self_check_port,
            "Skipped self default seed"
        );
    }

    // Локальные fallback-узлы для сценария "две ноды на одном хосте".
    // Это делает запуск независимым от порядка старта (CLI/QT).
    if (!connect_only_mode) {
        (void)add_bootstrap_endpoints(
            *addrman,
            bootstrap_plan.local_seed_nodes,
            "local-seed",
            self_check_port,
            nullptr
        );
    }

    if (!connect_only_mode && self_check_port == 0) {
        addrman->add(
            std::string("127.0.0.1:") + std::to_string(consensus::Params().default_port()),
            "local-fallback"
        );
    }

    // Запускаем асинхронный DNS поток (как ThreadDNSAddressSeed в Bitcoin)
    std::atomic<bool> dns_thread_running{false};
    std::jthread dns_thread;

    if (config_.enable_auto_connect && !connect_only_mode) {
        dns_thread_running = true;
        dns_thread = std::jthread([this, addrman, &dns_thread_running, self_check_port](std::stop_token stop) {
            try {
                // Как в Bitcoin: headless может подождать, но Qt GUI задаёт
                // нулевую задержку, чтобы не висеть без публичных peers при старте.
                if (addrman->size() > 0 && config_.dns_seed_delay_ms > 0) {
                    if (detail::node_verbose_enabled()) {
                        LogPrintf("DNS: Waiting %u ms to check if peers are available...\n",
                                  static_cast<unsigned>(config_.dns_seed_delay_ms));
                    }

                    const uint32_t delay_ms = config_.dns_seed_delay_ms;
                    for (uint32_t waited_ms = 0; waited_ms < delay_ms && dns_thread_running && !stop.stop_requested(); waited_ms += 100) {
                        const uint32_t step_ms = std::min<uint32_t>(100, delay_ms - waited_ms);
                        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
                    }

                    if (!dns_thread_running || stop.stop_requested()) {
                        return;  // Поток остановлен
                    }

                    // Проверяем количество подключенных пиров (как в Bitcoin: if (vNodes.size() >= 2))
                    auto peer_stats = peer_manager_->get_stats();
                    if (peer_stats.connected_peers >= 2) {
                        if (detail::node_verbose_enabled()) {
                            LogPrintf("P2P peers available (%zu). Skipped DNS seeding.\n", peer_stats.connected_peers);
                        }
                        dns_thread_running = false;
                        return;
                    }
                } else if (addrman->size() > 0 && detail::node_verbose_enabled()) {
                    LogPrintf("DNS: Fast bootstrap enabled, skipping initial DNS delay.\n");
                }

                if (detail::node_verbose_enabled()) {
                    LogPrintf("Loading addresses from DNS seeds (could take a while)...\n");
                }

                // Получаем DNS адреса (уже разрешены)
                std::vector<std::string> dns_seeds = get_default_seed_nodes();

                if (!dns_seeds.empty()) {
                    size_t total_found = 0;
                    for (const auto& addr : dns_seeds) {
                        if (!dns_thread_running || stop.stop_requested()) {
                            break;  // Поток остановлен
                        }
                        if (is_self_endpoint(addr, self_check_port)) {
                            continue;
                        }
                        addrman->add(addr, "dnsseed");
                        total_found++;
                    }

                    if (detail::node_verbose_enabled()) {
                        LogPrintf("%zu addresses found from DNS seeds\n", total_found);
                    }
                }

            } catch (const std::exception& error) {
                GEO_LOG_ERROR("node", "DNS discovery failed: " << error.what());
            } catch (...) {
                GEO_LOG_ERROR("node", "DNS discovery failed");
            }
            dns_thread_running = false;
        });
    }

    if (detail::node_verbose_enabled()) {
        if (connect_only_mode) {
            LogPrintf("Connect-only mode with %zu persistent targets\n", config_.connect_nodes.size());
        } else {
            LogPrintf("AddrMan has %zu addresses available\n", addrman->size());
        }
    }

    if (!connect_only_mode && addrman->size() == 0 && config_.seed_nodes.empty()) {
        LogPrintf("⚠️  No peer addresses available!\n");
        LogPrintf("   Add nodes via <data-dir>/geocoin.conf (addnode=<ip:port>)\n");
        LogPrintf("   or check DNS/default seed availability.\n");
    }

    // Базовые приоритетные цели: addnode/default seeds/local fallback.
    // Runtime reconnect targets добавляются ниже в каждом цикле.
    const std::vector<std::string>& base_priority_targets = bootstrap_plan.base_priority_targets;

    // Главный цикл подключений (как ThreadOpenConnections в Bitcoin)
    const auto connection_loop_started = std::chrono::steady_clock::now();
    auto last_addrman_save = connection_loop_started;
    bool first_outbound_logged = false;
    while (connection_thread_running_) {
        std::vector<std::string> priority_targets = base_priority_targets;
        std::set<std::string> manual_targets(config_.connect_nodes.begin(), config_.connect_nodes.end());
        manual_targets.insert(config_.seed_nodes.begin(), config_.seed_nodes.end());
        if (!connect_only_mode) {
            std::set<std::string> reconnect_targets;
            {
                std::lock_guard<std::mutex> lock(outbound_reconnect_targets_mutex_);
                reconnect_targets = outbound_reconnect_targets_;
            }
            const auto added_nodes = peer_manager_->get_added_nodes();
            manual_targets.insert(added_nodes.begin(), added_nodes.end());
            append_runtime_priority_targets(
                priority_targets,
                added_nodes,
                reconnect_targets
            );
        }

        // Проверяем количество исходящих подключений
        auto peer_stats = peer_manager_->get_stats();
        size_t outbound = peer_stats.outbound_peers;
        const size_t target_outbound = connect_only_mode
            ? std::max<size_t>(1, priority_targets.size())
            : 8;  // Целевое количество исходящих подключений

        // Сначала пробуем приоритетные цели.
        if (outbound < target_outbound) {
            for (const auto& target : priority_targets) {
                if (!connection_thread_running_) {
                    break;
                }
                if (is_self_endpoint(target, self_check_port)) {
                    continue;
                }
                auto existing = peer_manager_->get_peer(target);
                if (existing && existing->is_connected()) {
                    continue;
                }
                const bool manual_target = connect_only_mode || manual_targets.count(target) > 0;
                if (!peer_manager_->should_try_outbound(target, manual_target)) {
                    continue;
                }
                const bool success = peer_manager_->add_outbound_peer(target, private_key_, manual_target);
                if (success && !connect_only_mode) {
                    addrman->add(target, "outbound");
                    addrman->good(target);
                    std::lock_guard<std::mutex> lock(outbound_reconnect_targets_mutex_);
                    outbound_reconnect_targets_.insert(target);
                }
                if (success && !first_outbound_logged) {
                    first_outbound_logged = true;
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - connection_loop_started
                    ).count();
                    GEO_LOG_INFO("node", "First outbound peer connected after " << elapsed_ms
                        << " ms: " << target);
                }
                peer_stats = peer_manager_->get_stats();
                outbound = peer_stats.outbound_peers;
                if (outbound >= target_outbound) {
                    break;
                }
            }
        }

        // A TCP attempt already in progress may finish on its timeout. Do not
        // start any more targets or AddrMan fallback work after cancellation.
        if (!connection_thread_running_) {
            break;
        }

        if (!connect_only_mode && outbound < target_outbound && addrman->size() > 0) {
            // Выбираем адрес для подключения
            std::string addr = addrman->select();

            if (!addr.empty()) {
                if (detail::node_verbose_enabled()) {
                    LogPrintf("Attempting connection to %s (outbound: %zu/%zu)\n",
                        addr.c_str(), outbound, target_outbound);
                }

                // Фильтр на случай если self-адресы попали в кэш после запуска.
                if (is_self_endpoint(addr, self_check_port)) {
                    addrman->remove(addr);
                    if (detail::node_verbose_enabled()) {
                        LogPrintf("Removed self address from AddrMan: %s\n", addr.c_str());
                    }
                    continue;
                }

                auto existing = peer_manager_->get_peer(addr);
                if (existing && existing->is_connected()) {
                    addrman->good(addr);
                } else {
                    if (!peer_manager_->should_try_outbound(addr, false)) {
                        addrman->attempt(addr, false);
                        continue;
                    }

                    // Пытаемся подключиться
                    bool success = peer_manager_->add_outbound_peer(addr, private_key_, false);

                    // Отмечаем попытку
                    addrman->attempt(addr, success);

                    if (success) {
                        {
                            std::lock_guard<std::mutex> lock(outbound_reconnect_targets_mutex_);
                            outbound_reconnect_targets_.insert(addr);
                        }
                        if (detail::node_verbose_enabled()) {
                            LogPrintf("Successfully connected to %s\n", addr.c_str());
                        }
                        if (!first_outbound_logged) {
                            first_outbound_logged = true;
                            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - connection_loop_started
                            ).count();
                            GEO_LOG_INFO("node", "First outbound peer connected after " << elapsed_ms
                                << " ms: " << addr);
                        }
                    } else {
                        if (detail::node_verbose_enabled()) {
                            LogPrintf("Failed to connect to %s\n", addr.c_str());
                        }
                    }
                }
            }
        }

        // Сохраняем адреса каждые 15 минут
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::minutes>(now - last_addrman_save).count() >= 15) {
            if (!addrman->save(peers_file)) GEO_LOG_WARN("node", "Failed to save peer address book");
            last_addrman_save = now;
        }

        // Если пиров нет - пытаемся чаще, чтобы локальные инстансы находили друг друга быстро.
        const auto post_stats = peer_manager_->get_stats();
        const int sleep_ms = (post_stats.outbound_peers < target_outbound) ? 2000 : 30000;
        const int steps = std::max(1, sleep_ms / 500);
        for (int i = 0; i < steps && connection_thread_running_; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // Останавливаем DNS поток
    // ВАЖНО: join нужно делать по joinable(), а не по dns_thread_running.
    // dns_thread может сам завершиться и выставить dns_thread_running=false,
    // но при этом оставаться joinable => деструктор std::thread вызовет terminate.
    if (dns_thread.joinable()) {
        dns_thread_running = false;
        dns_thread.join();
    }

    // Сохраняем адреса при выходе
    if (!addrman->save(peers_file)) GEO_LOG_WARN("node", "Failed to save peer address book");

    if (detail::node_verbose_enabled()) {
        LogPrintf("Connection thread stopped\n");
    }
}


}  // namespace geocoin::node
