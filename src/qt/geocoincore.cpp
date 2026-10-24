// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "geocoincore.h"
#include "guiutil.h"
#include "qtlogging.h"
#include "geocoin/node/node.hpp"
#include "geocoin/node/node_key.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/utils/secure_memory.hpp"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <cstdlib>
#include <unordered_set>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace {
#ifdef _WIN32
void ensure_winsock_initialized() {
    static const bool initialized = []() {
        WSADATA wsa_data;
        return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
    }();
    (void)initialized;
}

void close_socket_portable(SOCKET sock) {
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
}
#else
void ensure_winsock_initialized() {}

void close_socket_portable(int sock) {
    if (sock >= 0) {
        close(sock);
    }
}
#endif

QString describeNodeStartupContext(const QString& dataDirectory,
                                   const QStringList& addNodes,
                                   const QStringList& connectPeers,
                                   bool noAutoConnect,
                                   const QString& reason)
{
    const QString networkDir = QDir(dataDirectory).filePath(QStringLiteral("degree"));
    return QStringLiteral(
        "%1\n\n"
        "Context:\n"
        "- network: degree\n"
        "- base datadir: %2\n"
        "- network datadir: %3\n"
        "- p2p port: 3030 (or outbound-only if already busy)\n"
        "- rpc port: disabled in Qt full-node mode\n"
        "- addnode count: %4\n"
        "- connect count: %5\n"
        "- auto-connect: %6"
    ).arg(reason,
          dataDirectory,
          networkDir,
          QString::number(addNodes.size()),
          QString::number(connectPeers.size()),
          noAutoConnect ? QStringLiteral("off") : QStringLiteral("on"));
}

}

// Helper function to check if port is available
static bool is_port_available(int port) {
    ensure_winsock_initialized();
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    // Set SO_REUSEADDR to avoid "Address already in use" errors
    int opt = 1;
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bool available = (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    close_socket_portable(sock);

    return available;
}

// Helper function to find available port starting from default_port
static int find_available_port(int default_port, int max_attempts = 10) {
    // Try default port first
    if (is_port_available(default_port)) {
        return default_port;
    }

    GEO_QT_LOG_DEBUG << "Port" << default_port << "is busy, searching for available port...";

    // Try next ports
    for (int i = 1; i < max_attempts; i++) {
        int port = default_port + i;
        if (is_port_available(port)) {
            GEO_QT_LOG_DEBUG << "Found available port:" << port;
            return port;
        }
    }

    // If all attempts failed, return default and let it fail with proper error
    return default_port;
}

GeoCoinCore::GeoCoinCore(QObject *parent)
    : QObject(parent)
{
}

GeoCoinCore::~GeoCoinCore()
{
}

geocoin::node::NodeComponentHandle GeoCoinCore::getNodeComponents()
{
    return node ? node->component_handle() : geocoin::node::NodeComponentHandle{};
}

void GeoCoinCore::setDataDirectory(const QString &dataDir)
{
    dataDirectory = dataDir;
}

void GeoCoinCore::setAddNodes(const QStringList& nodes)
{
    addNodes = nodes;
}

void GeoCoinCore::setConnectPeers(const QStringList& peers)
{
    connectPeers = peers;
}

void GeoCoinCore::setNoAutoConnect(bool value)
{
    noAutoConnect = value;
}

void GeoCoinCore::setMiningSha256Backend(const QString& backend)
{
    miningSha256Backend = backend;
}

void GeoCoinCore::setWalletPassword(const QString& password)
{
    walletPassword = password;
}

void GeoCoinCore::initialize()
{
    try {
        // Initialize node
        geocoin::node::NodeConfig config;
        config.p2p_port = 0;
        config.rpc_port = 0;
        config.data_dir = dataDirectory.toStdString();
        config.network = "degree";
        config.enable_mining = false;
        config.mining_threads = 1;
        // Qt is an interactive full-node wallet. Keep mining fully multi-core,
        // but use shorter runtime-only batches than headless CLI so the event
        // loop, wallet refresh and P2P processing remain responsive.
        config.mining_cooperative_batch_iterations = 131072;
        config.mining_cooperative_pause_us = 0;
        config.mining_sha256_backend = miningSha256Backend.toStdString();
        // Qt startup must stay responsive while peers are discovered. These
        // runtime-only values do not affect consensus, wire bytes or wallet data.
        config.dns_seed_delay_ms = 0;
        config.outbound_connect_timeout_ms = 1500;

        // Семантика подключения узлов:
        // -addnode: постоянные узлы для авто-подключения
        // -connect: режим connect-only (отключает авто-подключение)
        config.seed_nodes.clear();
        for (const auto& n : addNodes) {
            config.seed_nodes.push_back(n.toStdString());
        }
        config.connect_nodes.clear();
        for (const auto& p : connectPeers) {
            config.connect_nodes.push_back(p.toStdString());
        }

        if (!config.connect_nodes.empty()) {
            config.enable_auto_connect = true;
        }
        if (noAutoConnect && config.connect_nodes.empty()) {
            config.enable_auto_connect = false;
        }

        node = std::make_shared<geocoin::node::Node>(config);

        geocoin::bytes node_private_key = geocoin::node::load_or_create_node_key(
            std::filesystem::path(dataDirectory.toStdString())
        );
        geocoin::utils::ScopedContainerCleanser<geocoin::bytes> nodeKeyCleanser(node_private_key);

        const auto startResult = node->start_with_result(node_private_key);
        if (!startResult.ok()) {
            QString reason = QStringLiteral("Node startup failed [%1]: %2")
                .arg(
                    QString::fromLatin1(geocoin::node::node_start_failure_name(startResult.failure)),
                    QString::fromStdString(startResult.message)
                );
            if (startResult.chain_data_may_need_attention()) {
                reason += QStringLiteral(
                    "\n\nLocal chain data was not modified. Automatic recovery is disabled; "
                    "inspect the node log and make an explicit backup/reindex decision."
                );
            }
            const QString startupError = describeNodeStartupContext(
                dataDirectory,
                addNodes,
                connectPeers,
                noAutoConnect,
                reason
            );
            node.reset();
            emit initializeResult(false, startupError);
            return;
        }

        // Load the wallet from the selected data directory.  Wallet creation is
        // an explicit GUI action so first run can ask the user for a passphrase.
        const QString walletPath = QDir(dataDirectory).filePath(QStringLiteral("wallet.dat"));
        QFile walletFile(walletPath);

        std::string walletPasswordBytes = walletPassword.toStdString();
        geocoin::utils::ScopedContainerCleanser<std::string> walletPasswordCleanser(walletPasswordBytes);
        GUIUtil::cleanseQString(walletPassword);

        if (!walletFile.exists() && !QFileInfo::exists(walletPath + QStringLiteral(".bak"))) {
            wallet = std::make_shared<geocoin::wallet::Wallet>();
        } else {
            try {
                wallet = std::make_shared<geocoin::wallet::Wallet>();

                const bool load_result = wallet->load_from_file(
                    walletPath.toStdString(), walletPasswordBytes);
                geocoin::utils::cleanse_container(walletPasswordBytes);

                if (!load_result) {
                    emit initializeResult(false, tr("Failed to load wallet. The password is incorrect or wallet.dat is damaged."));
                    return;
                }
            } catch (const std::exception& e) {
                emit initializeResult(false, QString("Exception: ") + e.what());
                return;
            } catch (...) {
                emit initializeResult(false, "Unknown exception while loading wallet");
                return;
            }
        }

        // Сканируем блокчейн на транзакции кошелька в фоновом потоке.
        // Это делается ПОСЛЕ старта ноды и ДО сигнала об успешной инициализации.
        auto chain_state = node->get_chain_state();

        const geocoin::Height current_height = chain_state->get_height();
        const geocoin::Height wallet_best_height = wallet->get_last_scanned_height();
        const geocoin::Hash256 wallet_anchor = wallet->get_last_scanned_block_hash();
        geocoin::Hash256 active_anchor{};
        const bool anchor_matches = wallet_best_height <= current_height &&
            chain_state->get_active_block_hash(wallet_best_height, active_anchor) &&
            active_anchor == wallet_anchor;

        if (!anchor_matches) {
            // This also covers legacy wallets that only stored a height and
            // same-height reorgs that happened while the application was off.
            wallet->reconcile_blockchain(chain_state, 0);
        } else if (wallet_best_height < current_height) {
            wallet->scan_blockchain(chain_state, wallet_best_height + 1);
        }

        emit initializeResult(true, QString());

    } catch (const std::exception& e) {
        emit initializeResult(false, QString::fromStdString(e.what()));
    }
}

void GeoCoinCore::shutdown()
{
    try {
        if (node) {
            node->stop();
            node.reset();
        }

        wallet.reset();

        emit shutdownResult();
    } catch (const std::exception& e) {
        // Log error but don't crash
        emit shutdownResult();  // Still signal completion
    } catch (...) {
        emit shutdownResult();  // Still signal completion
    }
}
