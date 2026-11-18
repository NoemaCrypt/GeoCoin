// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include "geocoinapplication.h"
#include "geocoingui.h"
#include "networkstyle.h"
#include "walletmodel.h"
#include "clientmodel.h"
#include "walletframe.h"
#include "miningpage.h"
#include "transactiontablemodel.h"
#include <QPointer>
#include <QLockFile>
#include <QTemporaryDir>
#include <QStandardPaths>
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/wallet/wallet_persist_internal.hpp"
#include "geocoin/node/node.hpp"
#include "geocoin/node/node_key.hpp"
#include "geocoin/utils/filesystem.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include <QMessageBox>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <thread>

class SlowInfo final : public geocoin::node::NodeInfoService {
public:
    mutable std::atomic<int> reads{0};
    bool is_running() const override { return true; }
    geocoin::node::NodeStatus get_status() const override {
        ++reads;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        geocoin::node::NodeStatus status;
        status.current_height = 42;
        return status;
    }
    geocoin::node::NodeConfig get_config() const override { return {}; }
    bool is_initial_block_download() const override { return false; }
    bool is_pruned_mode() const override { return false; }
    geocoin::Height get_pruned_block_retention() const override { return 0; }
    geocoin::node::PrunedDataInfo get_pruned_data_info() const override { return {}; }
    bool is_height_pruned_for_api(geocoin::Height) const override { return false; }
    bool is_block_pruned_for_api(const geocoin::Hash256&, geocoin::Height*) const override { return false; }
    std::string describe_pruned_api_error(geocoin::Height) const override { return {}; }
};

class ThrowingMining final : public geocoin::node::MiningControlService {
public:
    bool start_mining() override { return false; }
    bool start_mining(size_t, const geocoin::Address&) override { return false; }
    void stop_mining() override { throw std::runtime_error("synthetic stop failure"); }
    bool is_mining() const override { return true; }
};

class DelayedChainQuery final : public geocoin::node::ChainQueryService {
public:
    explicit DelayedChainQuery(std::shared_ptr<geocoin::node::ChainQueryService> delegate)
        : delegate_(std::move(delegate)), guiThread_(QThread::currentThread()) {}
    mutable std::atomic<int> reads{0}, active{0}, maximum{0};
    mutable std::atomic<bool> guiRead{false};
    std::atomic<bool> fail{false};
    geocoin::Amount get_balance(const geocoin::Address& address) const override { return delegate_->get_balance(address); }
    bool get_block(const geocoin::Hash256& hash, geocoin::core::Block& block) const override { return delegate_->get_block(hash, block); }
    bool get_block_by_height(geocoin::Height height, geocoin::core::Block& block) const override {
        if (QThread::currentThread() == guiThread_) guiRead = true;
        const int concurrent = ++active;
        if (concurrent > maximum.load()) maximum = concurrent;
        ++reads;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        --active;
        if (fail) throw std::runtime_error("synthetic delayed wallet query failure");
        return delegate_->get_block_by_height(height, block);
    }
    bool get_transaction(const geocoin::Hash256& hash, geocoin::core::Transaction& tx) const override { return delegate_->get_transaction(hash, tx); }
    std::vector<geocoin::core::Transaction> get_mempool_transactions() const override { return delegate_->get_mempool_transactions(); }
private:
    std::shared_ptr<geocoin::node::ChainQueryService> delegate_;
    QThread* guiThread_;
};

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void quiesce(WalletModel& model)
{
    model.beginQuiesce();
    QElapsedTimer timer;
    timer.start();
    while (!model.isQuiesced() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(model.isQuiesced(), "quiesce did not complete");
}

static void checkDelayedWalletSnapshot(geocoin::node::Node& node)
{
    auto components = node.component_handle();
    auto query = std::make_shared<DelayedChainQuery>(components.services.chain_query);
    components.services.chain_query = query;
    auto wallet = std::make_shared<geocoin::wallet::Wallet>();
    geocoin::bytes key(32, 0);
    key.back() = 31;
    require(wallet->initialize(key), "delayed wallet fixture initialization failed");
    geocoin::wallet::WalletTransaction transaction;
    transaction.tx_hash[0] = 1;
    transaction.amount = 7;
    transaction.timestamp = 1700000000;
    wallet->add_transaction(transaction);
    WalletModel model;
    int resets = 0, heartbeats = 0, failures = 0;
    bool guiPublication = true;
    QObject::connect(model.getTransactionTableModel(), &QAbstractItemModel::modelReset, &model, [&] {
        ++resets;
        guiPublication = guiPublication && QThread::currentThread() == model.thread();
    });
    QObject::connect(&model, &WalletModel::walletHistoryError, &model, [&](const QString&) { ++failures; });
    QTimer heartbeat;
    heartbeat.setInterval(5);
#ifdef GEOCOIN_MACOS_ARM64
    heartbeat.setTimerType(Qt::PreciseTimer);
#endif
    QObject::connect(&heartbeat, &QTimer::timeout, &model, [&] { ++heartbeats; });
    heartbeat.start();
    require(model.setRuntimeContext(wallet, std::move(components)), "delayed wallet runtime context failed");
    QElapsedTimer timeout;
    timeout.start();
    for (int i = 0; i < 10; ++i) model.updateBalance();
    while (resets == 0 && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(resets > 0 && query->reads > 0 && !query->guiRead && query->maximum == 1 &&
        guiPublication && heartbeats > 10, "wallet snapshot blocked GUI, overlapped reads, or published off-thread");
    const int retainedRows = model.getTransactionTableModel()->rowCount(QModelIndex());
    require(retainedRows == 1, "wallet snapshot did not publish synthetic history");
    query->fail = true;
    model.updateBalance();
    timeout.restart();
    while (failures == 0 && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(failures > 0 && model.getTransactionTableModel()->rowCount(QModelIndex()) == retainedRows,
        "failed wallet snapshot discarded the previous published history");
    query->fail = false;
    model.updateBalance();
    quiesce(model);
    const int resetsBeforeClose = resets;
    const int readsBeforeClose = query->reads;
    QCoreApplication::processEvents();
    require(resets == resetsBeforeClose && query->reads == readsBeforeClose && query->active == 0,
        "quiesce allowed late wallet snapshot publication or retained its worker");
    model.closeWallet();
    require(model.getTransactionTableModel()->rowCount(QModelIndex()) == 0 && model.getBalance() == 0,
        "wallet close retained snapshot rows or balance");
}

static QByteArray legacyFixture()
{
    using namespace geocoin;
    using namespace geocoin::wallet;
    bytes payload{0};
    append_u32_le(payload, 1);
    bytes key(32, 0);
    key.back() = 17;
    payload.insert(payload.end(), key.begin(), key.end());
    const std::string label = "default";
    append_u32_le(payload, static_cast<uint32_t>(label.size()));
    payload.insert(payload.end(), label.begin(), label.end());
    append_u64_le(payload, 1700000000);
    append_u32_le(payload, 0);
    append_u64_le(payload, 0);
    append_u32_le(payload, 0);
    bytes file;
    append_u32_le(file, 0x47454f43);
    append_u32_le(file, 11);
    file.push_back(0);
    append_u32_le(file, static_cast<uint32_t>(payload.size()));
    file.insert(file.end(), payload.begin(), payload.end());
    return QByteArray(reinterpret_cast<const char*>(file.data()), static_cast<int>(file.size()));
}

static quint16 unusedLocalPort()
{
#ifdef _WIN32
    WSADATA data;
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock initialization failed");
    const SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(socketHandle != INVALID_SOCKET, "port reservation socket failed");
    int size = sizeof(sockaddr_in);
#else
    const int socketHandle = socket(AF_INET, SOCK_STREAM, 0);
    require(socketHandle >= 0, "port reservation socket failed");
    socklen_t size = sizeof(sockaddr_in);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "port reservation bind failed");
    require(getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address), &size) == 0, "port reservation query failed");
#ifdef _WIN32
    closesocket(socketHandle);
    WSACleanup();
#else
    close(socketHandle);
#endif
    return ntohs(address.sin_port);
}

int main(int argc, char** argv)
{
    std::unique_ptr<QTemporaryDir> fixtureDirectory;
    try {
        const QString mode = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
        const bool child = mode == QStringLiteral("--lock-probe") ||
            mode == QStringLiteral("--node-shutdown") || mode == QStringLiteral("--sync-shutdown") ||
            mode == QStringLiteral("--node-relaunch") || mode == QStringLiteral("--failed-stop-shutdown") ||
            mode == QStringLiteral("--destructor-stop-shutdown");
        const QString workRoot = qEnvironmentVariable("GEOCOIN_TEST_WORK_ROOT");
        const QString base = child && argc > 1 ? QString::fromLocal8Bit(argv[1]) :
            (!workRoot.isEmpty() ? QDir(workRoot).filePath(QStringLiteral("gui-wallet-lifecycle")) :
             (argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("out/gui-wallet-lifecycle")));
        require(!base.isEmpty() && QDir().mkpath(base), "cannot create isolated fixture base");
        QString root = QFileInfo(base).absoluteFilePath();
        const QByteArray marker("GeoCoin synthetic Qt wallet lifecycle fixture\n");
        if (!child) {
            fixtureDirectory = std::make_unique<QTemporaryDir>(QDir(root).filePath(QStringLiteral("run-XXXXXX")));
            require(fixtureDirectory->isValid(), "cannot create unique lifecycle fixture directory");
            root = fixtureDirectory->path();
            QFile markerFile(QDir(root).filePath(QStringLiteral(".lifecycle-test")));
            require(markerFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                markerFile.write(marker) == marker.size(), "cannot mark synthetic fixture directory");
        } else {
            QFile markerFile(QDir(root).filePath(QStringLiteral(".lifecycle-test")));
            require(markerFile.open(QIODevice::ReadOnly) && markerFile.readAll() == marker,
                "child probes must use a parent-owned synthetic fixture directory");
        }
        const QString temporary = QDir(root).filePath(QStringLiteral("tmp"));
        require(QDir().mkpath(temporary), "cannot create isolated temporary directory");
        qputenv("TMPDIR", temporary.toLocal8Bit());
        qputenv("TMP", temporary.toLocal8Bit());
        qputenv("TEMP", temporary.toLocal8Bit());
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qunsetenv("GEOCOIN_QT_WALLET_PASSWORD");
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root + "/settings");
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root + "/settings");
        QCoreApplication::setOrganizationName("GeoCoinQtWalletSliceTest");
        QCoreApplication::setApplicationName("QtWalletSlice");
        std::unique_ptr<QLockFile> instanceLock;
        auto appOwner = std::make_unique<GeoCoinApplication>(argc, argv);
        GeoCoinApplication& app = *appOwner;
        app.setProperty("geocoinTestDataDirectory", root);
        if (mode == QStringLiteral("--node-shutdown") || mode == QStringLiteral("--sync-shutdown") ||
            mode == QStringLiteral("--node-relaunch") || mode == QStringLiteral("--failed-stop-shutdown") ||
            mode == QStringLiteral("--destructor-stop-shutdown")) {
            const bool duringSync = mode == QStringLiteral("--sync-shutdown");
            const bool failedStop = mode == QStringLiteral("--failed-stop-shutdown") || mode == QStringLiteral("--destructor-stop-shutdown");
            const bool destructorStop = mode == QStringLiteral("--destructor-stop-shutdown");
            const bool waitForInitialization = duringSync || failedStop || mode == QStringLiteral("--node-relaunch");
            const QString nodeDir = root + QStringLiteral("/\u041A\u043E\u0448\u0435\u043B\u0435\u043A node space/") +
                QString(180, QLatin1Char('x')) + (duringSync ? "/sync-shutdown-node" : "/startup-shutdown-node");
            app.setDataDirectory(nodeDir);
            const QString networkDir = nodeDir + "/degree-load";
            require(QDir().mkpath(networkDir), "isolated loadnet directory creation failed");
            instanceLock = std::make_unique<QLockFile>(nodeDir + QStringLiteral("/geocoin-core.lock"));
            instanceLock->setStaleLockTime(0);
            require(instanceLock->tryLock(0), "previous Qt process retained the node directory lock");
            QFile configFile(networkDir + "/geocoin.conf");
            require(configFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "isolated loadnet config open failed");
            configFile.write("no-auto-connect=1\n");
            configFile.close();
            const QString manifest = root + "/activation-manifest.loadnet.json";
            if (!QFileInfo::exists(manifest)) {
                const QString sourceManifest = qEnvironmentVariable("GEOCOIN_TEST_LOADNET_MANIFEST");
                require(!sourceManifest.isEmpty() && QFile::copy(sourceManifest, manifest),
                    "GEOCOIN_TEST_LOADNET_MANIFEST must name the synthetic loadnet admission manifest");
            }
            app.setNetwork("degree-load");
            app.setP2pPort(unusedLocalPort());
            app.setActivationManifestPath(manifest);
            app.setSmokeTestMode(true);
            app.setNoAutoConnect(true);
            if (failedStop) app.setProperty("geocoinTestCoreStopFailures", 1);
            struct StopObservation { int failures = 0; bool retainedCore = false; };
            const auto observation = std::make_shared<StopObservation>();
            QObject::connect(&app, &GeoCoinApplication::shutdownError, &app, [&, observation](const QString&) {
                ++observation->failures;
                auto* thread = app.findChild<QThread*>();
                observation->retainedCore = !app.isShutdownComplete() && thread && thread->isRunning() && instanceLock->isLocked();
                QTimer::singleShot(150, &app, [] {
                    for (QWidget* widget : QApplication::topLevelWidgets()) {
                        if (auto* message = qobject_cast<QMessageBox*>(widget)) message->done(QMessageBox::Retry);
                    }
                });
            });
            NetworkStyle style("GeoCoin", 0, 0, "");
            app.createWindow(&style);
            app.requestInitialize();
            QTimer waitForWallet;
            if (waitForInitialization) {
                waitForWallet.setInterval(5);
                QObject::connect(&waitForWallet, &QTimer::timeout, &app, [&app, &waitForWallet, root, duringSync, failedStop]() {
                    if (auto* model = app.findChild<WalletModel*>()) {
                        if (duringSync) {
                            const QString path = root + "/during-sync.wallet";
                            QFile::remove(path);
                            if (!model->createWallet(path, QString())) { app.exit(8); return; }
                        }
                        waitForWallet.stop();
                        if (failedStop) { QApplication::exit(0); return; }
                        app.requestShutdown();
                        app.requestShutdown();
                    }
                });
                waitForWallet.start();
            } else {
                // No GUI event loop has processed initializeResult yet.
                app.requestShutdown();
                app.requestShutdown();
            }
            QTimer::singleShot(30000, &app, [&app]() { app.exit(9); });
            const int result = destructorStop ? QApplication::exec() : app.exec();
            require(result == 0 && (failedStop || !waitForInitialization || app.getReturnValue() == 0),
                "core lifecycle probe failed or timed out");
            require(instanceLock->isLocked(), "node directory lock was released before application teardown");
            if (destructorStop) {
                require(!app.isShutdownComplete(), "destructor probe did not bypass normal exec cleanup");
                appOwner.reset();
            }
            if (failedStop) require(observation->failures == 1 && observation->retainedCore,
                "failed core stop destroyed ownership or did not complete an asynchronous retry");
            geocoin::node::NodeConfig config;
            config.data_dir = nodeDir.toStdString();
            config.network = "degree-load";
            config.p2p_port = unusedLocalPort();
            config.rpc_port = 0;
            config.enable_auto_connect = false;
            config.activation_manifest_path = manifest.toStdString();
            auto reopened = std::make_shared<geocoin::node::Node>(config);
            require(reopened->get_config().seed_nodes.empty() && reopened->get_config().p2p_port == config.p2p_port,
                "isolated loadnet config introduced a seed or changed the test port");
            const auto key = geocoin::node::load_or_create_node_key(geocoin::utils::path_from_utf8(nodeDir.toStdString()));
            require(reopened->start_with_result(key).ok(), "node DB remained locked after async shutdown");
            if (duringSync) checkDelayedWalletSnapshot(*reopened);
            reopened->stop();
            return 0;
        }
        if (argc >= 4 && mode == QStringLiteral("--lock-probe")) {
            WalletModel model;
            const bool opened = model.stageWallet(QString::fromLocal8Bit(argv[3]), QString());
            std::cout << "lock-probe opened=" << opened << " code="
                      << model.walletOperationCode().toStdString() << std::endl;
            return !opened && model.walletOperationCode() == "wallet_lock" ? 0 : 2;
        }

        auto slowInfo = std::make_shared<SlowInfo>();
        geocoin::node::NodeComponentHandle partial;
        partial.services.info = slowInfo;
        ClientModel statusModel;
        statusModel.setNodeComponents(partial);
        (void)statusModel.getNumBlocks();
        (void)statusModel.getHashrate();
        require(slowInfo->reads == 0, "ClientModel queried blocking status on the GUI thread");
        geocoin::node::NodeStatus diagnostic;
        diagnostic.current_height = 23;
        diagnostic.mining_threads_requested = 8;
        diagnostic.mining_threads_spawned = 3;
        diagnostic.mining_threads_active = 2;
        diagnostic.mining_error = "synthetic CPU worker failure";
        int errorSignals = 0;
        QObject::connect(&statusModel, &ClientModel::message, [&errorSignals]() { ++errorSignals; });
        statusModel.updateStatus(diagnostic);
        statusModel.updateStatus(diagnostic);
        require(statusModel.getNumBlocks() == 23 && errorSignals == 1 && statusModel.getMiningDiagnostics().contains("spawned 3"),
            "cached status or mining diagnostics were not published correctly");
        ManagedAsyncAction statusAction;
        auto snapshot = std::make_shared<geocoin::node::NodeStatus>();
        require(statusAction.start([slowInfo, snapshot]() { *snapshot = slowInfo->get_status(); }), "status action did not start");
        require(!statusAction.start([]() {}), "status read jobs overlapped");
        int heartbeats = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, [&heartbeats]() { ++heartbeats; });
        heartbeat.start();
        while (!statusAction.reapIfFinished()) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        heartbeat.stop();
        require(heartbeats > 10 && slowInfo->reads == 1 && snapshot->current_height == 42,
            "slow status read blocked the GUI or lost its snapshot");

        const QString gpuFixture = qEnvironmentVariable("GEOCOIN_TEST_GPU_MINER");
        require(!gpuFixture.isEmpty() && QFileInfo::exists(gpuFixture),
            "GEOCOIN_TEST_GPU_MINER must name the offline GPU process fixture");
        {
            WalletFrame frame;
            auto* oldMiningPage = frame.findChild<MiningPage*>();
            require(oldMiningPage != nullptr, "language fixture has no mining page");
            QString error;
            require(oldMiningPage->runProcessLifecycleChecks(gpuFixture, &error), error.toUtf8().constData());
            QProcess* miner = nullptr;
            for (auto* process : oldMiningPage->findChildren<QProcess*>(QString(), Qt::FindDirectChildrenOnly)) {
                if (process->program().isEmpty()) miner = process;
            }
            require(miner != nullptr, "owned miner fixture process is missing");
            miner->setProgram(gpuFixture);
            miner->setArguments({QStringLiteral("--hang-fixture")});
            miner->start();
            QElapsedTimer starting;
            starting.start();
            while (miner->state() != QProcess::Running && starting.elapsed() < 2000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            require(miner->state() == QProcess::Running && frame.isGpuMiningActive(), "mock miner did not start");
            QPointer<MiningPage> oldPage(oldMiningPage);
            int languageHeartbeats = 0;
            QTimer languageHeartbeat;
            languageHeartbeat.setInterval(10);
#ifdef GEOCOIN_MACOS_ARM64
            languageHeartbeat.setTimerType(Qt::PreciseTimer);
#endif
            QObject::connect(&languageHeartbeat, &QTimer::timeout, [&languageHeartbeats]() { ++languageHeartbeats; });
            languageHeartbeat.start();
            QElapsedTimer languageElapsed;
            languageElapsed.start();
            qint64 stoppedAt = -1;
            int heartbeatsAtStop = 0;
            QObject::connect(&frame, &WalletFrame::gpuMiningStopped, &frame, [&] {
                stoppedAt = languageElapsed.elapsed();
                heartbeatsAtStop = languageHeartbeats;
            });
            frame.rebuildForLanguageChange();
            frame.rebuildForLanguageChange();
            require(oldPage && frame.isGpuMiningActive(), "language rebuild destroyed the running mining page");
            while (oldPage && languageElapsed.elapsed() < 6000) {
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            // The final processEvents() can finish a slow rebuild after the loop
            // deadline. Deliver its posted deletions before inspecting QPointer.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            languageHeartbeat.stop();
            if (oldPage || !frame.hasRequiredPages() || frame.isGpuMiningActive()) {
                std::cerr << "language elapsed=" << languageElapsed.elapsed() << " stoppedAt=" << stoppedAt
                          << " oldPage=" << bool(oldPage) << " pages=" << frame.pageCount()
                          << " active=" << frame.isGpuMiningActive() << " cpuPending=" << frame.hasPendingMiningAction()
                          << " heartbeat=" << languageHeartbeats << std::endl;
            }
            require(!oldPage && frame.hasRequiredPages() && !frame.isGpuMiningActive() && !frame.hasPendingMiningAction(),
                "language rebuild did not finish after owned child cleanup");
            require(stoppedAt >= 2500 && stoppedAt < 5000 && heartbeatsAtStop > 80,
                "language rebuild blocked the event loop or exceeded the child stop deadline");
        }

        const QString nodeA = root + "/node-a";
        const QString nodeB = root + "/node-b";
        QDir().mkpath(nodeA);
        QDir().mkpath(nodeB);
        const QString first = root + "/arbitrary-first.wallet";
        const QString second = root + "/not-wallet-dot-dat.bin";
        QFile::remove(first);
        QFile::remove(second);
        WalletModel model;
        model.setDataDirectory(nodeA);
        require(model.getWalletFilePath().isEmpty(), "data directory invented wallet.dat");
        require(!model.stageWallet(root + "/missing.wallet", QString()), "missing file was created");
        require(model.walletOperationCode() == "not_found", "missing path not classified");
        require(model.createWallet(first, QString()), "arbitrary first wallet create failed");
        const auto original = model.getWallet();
        const auto address = model.getAddress();
        require(model.getWalletFilePath() == QFileInfo(first).canonicalFilePath(), "active path not canonical");
        require(WalletModel::initialWalletPath(nodeA) == model.getWalletFilePath(), "successful path not remembered");
        require(WalletModel::initialWalletPath(nodeB).isEmpty(), "last wallet leaked across data dirs");
        require(WalletModel::initialWalletPath(nodeA, "other.bin") == QDir(nodeA).filePath("other.bin"), "explicit relative path not respected");
        require(!model.stageWallet(root + "/missing.wallet", QString()), "bad candidate opened");
        require(model.getWallet() == original && model.getAddress() == address, "bad candidate changed active wallet");
        QFile corrupt(root + "/corrupt.wallet");
        require(corrupt.open(QIODevice::WriteOnly), "corrupt fixture open failed");
        corrupt.write("not a wallet");
        corrupt.close();
        require(!model.stageWallet(corrupt.fileName(), QString()), "corrupt candidate opened");
        require(model.getWallet() == original && model.getWalletFilePath() == QFileInfo(first).canonicalFilePath(), "corrupt candidate replaced active wallet");
        geocoin::wallet::Wallet protectedWallet;
        geocoin::bytes key(32, 0);
        key.back() = 9;
        require(protectedWallet.initialize(key), "protected fixture initialize failed");
        const QString protectedPath = root + "/protected.wallet";
        require(protectedWallet.save_to_file(protectedPath.toStdString(), "fixture passphrase"), "protected fixture save failed");
        require(!model.stageWallet(protectedPath, QString()), "protected wallet opened without passphrase");
        require(model.walletOperationCode() == "password_required", "password-required error not classified");
        require(!model.stageWallet(protectedPath, "incorrect passphrase"), "protected wallet opened with bad passphrase");
        require(model.walletOperationCode() == "authentication_failed", "bad passphrase not classified");
        require(model.getWallet() == original && model.getAddress() == address, "password failure changed active wallet");
        WalletModel contender;
        contender.setDataDirectory(nodeB);
        require(!contender.stageWallet(first, QString()), "same wallet locked by two node directories");
        require(contender.walletOperationCode() == "wallet_lock", "wallet contention misclassified");
        QProcess probe;
        probe.start(QCoreApplication::applicationFilePath(), {root, "--lock-probe", first});
        const bool probeFinished = probe.waitForFinished(10000);
        if (!probeFinished || probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
            std::cerr << "lock-probe finished=" << probeFinished << " exitStatus=" << probe.exitStatus()
                      << " exitCode=" << probe.exitCode() << " processError=" << probe.errorString().toStdString()
                      << " stdout=" << probe.readAllStandardOutput().constData()
                      << " stderr=" << probe.readAllStandardError().constData() << std::endl;
        }
        require(probeFinished && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
            "per-wallet lock did not cross process boundary");
        require(model.stageWallet(second, QString(), true), "second candidate create failed");
        require(model.getWallet() == original, "staging replaced active wallet");
        require(!model.activateStagedWallet(), "active wallet switched without quiesce");
        quiesce(model);
        require(model.activateStagedWallet(), "quiesced candidate activation failed");
        require(model.getWallet() != original && model.getWalletFilePath() == QFileInfo(second).canonicalFilePath(), "candidate not activated");
        require(contender.stageWallet(first, QString()), "prior wallet lock not released after switching");
        contender.discardStagedWallet();

        quiesce(model);
        model.closeWallet();
        require(!model.isWalletLoaded() && model.getWalletFilePath().isEmpty(), "close retained wallet");
        require(WalletModel::initialWalletPath(nodeA) == QFileInfo(second).canonicalFilePath(), "close forgot last successful wallet");
        require(contender.stageWallet(second, QString()), "close retained active lock");
        contender.discardStagedWallet();

        const QString legacy = root + "/legacy-source.wallet";
        QFile::setPermissions(legacy, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile legacyFile(legacy);
        require(legacyFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "legacy fixture open failed");
        const auto legacyBytes = legacyFixture();
        require(legacyFile.write(legacyBytes) == legacyBytes.size(), "legacy fixture write failed");
        legacyFile.close();
        require(!model.loadWallet(legacy, QString()) && model.stagedWalletNeedsMigration(), "legacy activated without confirmation");
        require(!model.isWalletLoaded(), "unconfirmed legacy became active");
        require(QFile::setPermissions(legacy, QFileDevice::ReadOwner), "readonly source setup failed");
        const QString outputDir = root + "/alternate-location";
        QDir().mkpath(outputDir);
        const QString migrated = outputDir + "/migrated.wallet";
        QFile::remove(migrated);
        QFile::remove(migrated + ".legacy.bak");
        require(model.migrateStagedWallet(QString(), migrated), "confirmed alternate-output migration failed");
        require(model.activateStagedWallet(), "migrated candidate activation failed");
        require(model.getWalletFilePath() == QFileInfo(migrated).canonicalFilePath(), "migration ignored selected output");
        require(legacyFile.open(QIODevice::ReadOnly) && legacyFile.readAll() == legacyBytes, "alternate migration changed source bytes");
        legacyFile.close();
        QFile legacyBackup(migrated + ".legacy.bak");
        require(legacyBackup.open(QIODevice::ReadOnly) && legacyBackup.readAll() == legacyBytes, "alternate migration did not preserve backup");
        legacyBackup.close();
        quiesce(model);
        model.closeWallet();
        model.closeWallet();
        require(!model.isWalletLoaded() && model.getWalletFilePath().isEmpty(), "close retained wallet");
        require(WalletModel::initialWalletPath(nodeA) == QFileInfo(migrated).canonicalFilePath(), "close forgot migrated wallet");

        {
            const QByteArray previousAutoclose = qgetenv("GEOCOIN_GUI_TEST_AUTOCLOSE");
            qputenv("GEOCOIN_GUI_TEST_AUTOCLOSE", (root + "/synthetic-stop-error.log").toUtf8());
            WalletModel retained;
            retained.setDataDirectory(nodeA);
            const QString retainedPath = root + "/retained-on-stop-failure.wallet";
            QFile::remove(retainedPath);
            require(retained.createWallet(retainedPath, QString()), "stop-failure fixture create failed");
            const auto retainedWallet = retained.getWallet();
            ClientModel failingClient;
            geocoin::node::NodeComponentHandle failingComponents;
            failingComponents.services.mining = std::make_shared<ThrowingMining>();
            failingClient.setNodeComponents(failingComponents);
            NetworkStyle failureStyle("GeoCoin", 0, 0, "");
            GeoCoinGUI gui(&failureStyle);
            gui.setClientModel(&failingClient);
            gui.setWalletModel(&retained);
            gui.openWalletPath(second);
            QElapsedTimer failureWait;
            failureWait.start();
            while (failureWait.elapsed() < 500) { QCoreApplication::processEvents(); QThread::msleep(1); }
            require(gui.minersStopped(), "throwing CPU stop caused permanent polling");
            require(retained.getWallet() == retainedWallet && retained.getWalletFilePath() == QFileInfo(retainedPath).canonicalFilePath(),
                "throwing CPU stop replaced the active wallet");
            gui.beginShutdown();

            const QString pageErrorPath = root + QStringLiteral("/page-stop-error.log");
            qputenv("GEOCOIN_GUI_TEST_AUTOCLOSE", pageErrorPath.toUtf8());
            MiningPage page;
            page.setClientModel(&failingClient);
            page.setWalletModel(&retained);
            page.updateMiningInfo(true, 1, 0.0, 0);
            QString reportedError;
            QObject::connect(&failingClient, &ClientModel::message, &page,
                [&reportedError](const QString&, const QString& error, unsigned int) { reportedError += error; });
            require(QMetaObject::invokeMethod(&page, "toggleMining", Qt::DirectConnection),
                "cannot trigger page-owned CPU stop regression");
            QElapsedTimer pageStopTimeout;
            pageStopTimeout.start();
            while (page.hasPendingMiningAction() && pageStopTimeout.elapsed() < 5000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            QCoreApplication::processEvents();
            require(!page.hasPendingMiningAction(), "throwing page-owned stop never reaped its worker");
            QFile pageError(pageErrorPath);
            if (pageError.open(QIODevice::ReadOnly)) reportedError += QString::fromUtf8(pageError.readAll());
            require(failingClient.getMiningEnabled() && retained.getWallet() == retainedWallet,
                "failed page stop changed the live mining or wallet state");
            require(reportedError.contains(QStringLiteral("stop"), Qt::CaseInsensitive) &&
                (reportedError.contains(QStringLiteral("fail"), Qt::CaseInsensitive) ||
                 reportedError.contains(QStringLiteral("error"), Qt::CaseInsensitive)),
                "page-owned CPU stop failure was silently discarded");
            if (previousAutoclose.isEmpty()) qunsetenv("GEOCOIN_GUI_TEST_AUTOCLOSE");
            else qputenv("GEOCOIN_GUI_TEST_AUTOCLOSE", previousAutoclose);
        }

        NetworkStyle testStyle("GeoCoin", 0, 0, "");
        {
            GeoCoinGUI gui(&testStyle);
            gui.setDataDirectory(nodeA);
            gui.setWalletModel(&model);
            gui.openWalletPath(root + "/missing-startup.wallet");
            require(!model.isWalletLoaded(), "missing initial wallet activated a fallback");
            gui.openWalletPath(second);
            gui.beginShutdown();
            gui.openWalletPath(first);
            QCoreApplication::processEvents();
            require(!model.isWalletLoaded(), "closing reactivated a staged wallet");
        }

        app.setDataDirectory(nodeA);
        NetworkStyle style("GeoCoin", 0, 0, "");
        app.createWindow(&style);
        app.requestShutdown();
        app.requestShutdown();
        app.initializeResult(true, QString());
        app.initializeResult(false, "late failure must not show a modal");
        app.requestInitialize();
        require(app.isShuttingDown(), "shutdown state not monotonic");
        QTimer::singleShot(5000, &app, [&app]() { app.exit(9); });
        require(app.exec() == 0, "async early shutdown failed or hung");
        QProcess shutdownProbe;
        shutdownProbe.start(QCoreApplication::applicationFilePath(), {root, "--node-shutdown"});
        const bool startupFinished = shutdownProbe.waitForFinished(40000);
        if (!startupFinished || shutdownProbe.exitCode() != 0) std::cerr << shutdownProbe.readAllStandardOutput().constData() << shutdownProbe.readAllStandardError().constData();
        require(startupFinished && shutdownProbe.exitStatus() == QProcess::NormalExit && shutdownProbe.exitCode() == 0,
            "shutdown during actual core initialization failed");
        shutdownProbe.start(QCoreApplication::applicationFilePath(), {root, "--node-relaunch"});
        const bool relaunched = shutdownProbe.waitForFinished(40000);
        if (!relaunched || shutdownProbe.exitCode() != 0)
            std::cerr << shutdownProbe.readAllStandardOutput().constData() << shutdownProbe.readAllStandardError().constData();
        require(relaunched && shutdownProbe.exitStatus() == QProcess::NormalExit && shutdownProbe.exitCode() == 0,
            "new Qt process could not initialize after init-close on the same node directory");
        shutdownProbe.start(QCoreApplication::applicationFilePath(), {root, "--sync-shutdown"});
        const bool syncFinished = shutdownProbe.waitForFinished(40000);
        if (!syncFinished || shutdownProbe.exitCode() != 0) std::cerr << shutdownProbe.readAllStandardOutput().constData() << shutdownProbe.readAllStandardError().constData();
        require(syncFinished && shutdownProbe.exitStatus() == QProcess::NormalExit && shutdownProbe.exitCode() == 0,
            "shutdown during wallet synchronization failed");
        for (const QString& stopMode : {QStringLiteral("--failed-stop-shutdown"), QStringLiteral("--destructor-stop-shutdown")}) {
            shutdownProbe.start(QCoreApplication::applicationFilePath(), {root, stopMode});
            const bool finished = shutdownProbe.waitForFinished(40000);
            if (!finished || shutdownProbe.exitCode() != 0)
                std::cerr << stopMode.toStdString() << ": " << shutdownProbe.readAllStandardOutput().constData()
                          << shutdownProbe.readAllStandardError().constData();
            require(finished && shutdownProbe.exitStatus() == QProcess::NormalExit && shutdownProbe.exitCode() == 0,
                "failed core stop retry or application destructor ownership regression");
        }
        std::cout << "Qt wallet lifecycle: PASS (staging, locks, delayed status, language, failed stop, init-close-relaunch, sync shutdown)\n";
        return 0;
    } catch (const std::exception& exception) {
        if (fixtureDirectory) {
            fixtureDirectory->setAutoRemove(false);
            std::cerr << "Retained synthetic fixtures: " << fixtureDirectory->path().toStdString() << '\n';
        }
        std::cerr << "Qt wallet lifecycle: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
