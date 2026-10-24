// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "geocoinapplication.h"
#include "geocoincore.h"
#include "geocoingui.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "transactiontablemodel.h"
#include "networkstyle.h"
#include "intro.h"
#include "guiutil.h"
#include "walletsyncworker.h"

#include "geocoin/wallet/wallet.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/types.hpp"

#include <QDebug>
#include <QMessageBox>
#include <QSettings>
#include <QDir>
#include <QLocale>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QSplashScreen>
#include <QPixmap>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <algorithm>
#include <utility>
#include <vector>

static void writeSmokeTrace(const QString& message)
{
    if (!qEnvironmentVariableIsSet("GEOCOIN_QT_SMOKE_TRACE")) {
        return;
    }

    QFile traceFile(QDir::temp().filePath(QStringLiteral("geocoin-core-smoke.log")));
    if (!traceFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream out(&traceFile);
    out << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
        << " pid=" << QCoreApplication::applicationPid()
        << " " << message << '\n';
}

namespace {
bool parseHashHex(const QString& hex, geocoin::Hash256& out)
{
    const QString clean = hex.trimmed();
    if (clean.size() != 64) {
        return false;
    }
    for (int i = 0; i < 32; ++i) {
        bool ok = false;
        const auto value = clean.mid(i * 2, 2).toUInt(&ok, 16);
        if (!ok) {
            return false;
        }
        out[static_cast<size_t>(i)] = static_cast<geocoin::byte>(value);
    }
    return true;
}

int envInt(const char* name, int fallback, int minimum, int maximum)
{
    bool ok = false;
    const int value = QProcessEnvironment::systemEnvironment()
        .value(QString::fromLatin1(name))
        .toInt(&ok);
    if (!ok) {
        return fallback;
    }
    return std::max(minimum, std::min(maximum, value));
}

void writeGuiE2eTrace(const QString& dataDirectory, const QString& message)
{
    QStringList tracePaths;
    if (!dataDirectory.isEmpty()) {
        tracePaths << QDir(dataDirectory).filePath(QStringLiteral("geocoin-core-e2e.log"));
    }
    tracePaths << QDir::temp().filePath(QStringLiteral("geocoin-core-e2e.log"));

    for (const QString& path : tracePaths) {
        QFile traceFile(path);
        if (!traceFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            continue;
        }

        QTextStream out(&traceFile);
        out << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
            << " pid=" << QCoreApplication::applicationPid()
            << " " << message << '\n';
    }
}
}


GeoCoinApplication::GeoCoinApplication(int &argc, char **argv)
    : QApplication(argc, argv)
    , coreThread(nullptr)
    , executor(nullptr)
    , clientModel(nullptr)
    , walletModel(nullptr)
    , window(nullptr)
    , pollTimer(nullptr)
    , miningSha256Backend(QStringLiteral("auto"))
    , noAutoConnect(false)
    , returnValue(0)
    , shutdownRequested(false)
    , smokeTestMode_(false)
    , guiE2eTestMode_(false)
{
    setQuitOnLastWindowClosed(false);

    GUIUtil::applyApplicationTheme(this, GUIUtil::currentTheme());
}

GeoCoinApplication::~GeoCoinApplication()
{
    try {
        prepareManagedWorkersForShutdown();
        if (!shutdownRequested) {
            shutdownCoreThreadSynchronously();
        }
        stopCoreThread();

        delete window;
        window = nullptr;
    } catch (const std::exception& e) {
        // Suppress exceptions in destructor
    } catch (...) {
        // Suppress all exceptions in destructor
    }
}

void GeoCoinApplication::prepareManagedWorkersForShutdown() noexcept
{
    try {
        if (nodeComponents_.services.events) {
            nodeComponents_.services.events->clear_transaction_callback();
            nodeComponents_.services.events->clear_chain_mutation_event_callback();
        }
        if (walletSyncWorker_) {
            walletSyncWorker_->stopAndDrain();
            walletSyncWorker_.reset();
        }

        if (window) {
            window->waitForPendingMiningStop();
        }
        miningStopAction_.wait();
    } catch (...) {
        // Destruction/shutdown must continue even if a worker reports failure.
    }
}

bool GeoCoinApplication::requestMiningStopAsync()
{
    auto mining = nodeComponents_.services.mining;
    if (!mining) {
        return false;
    }
    const bool started = miningStopAction_.start([mining = std::move(mining)]() {
        mining->stop_mining();
    });
    return started || miningStopAction_.isRunning();
}

bool GeoCoinApplication::reapMiningStopIfFinished()
{
    return miningStopAction_.reapIfFinished();
}

void GeoCoinApplication::shutdownCoreThreadSynchronously() noexcept
{
    if (!executor || !coreThread || !coreThread->isRunning()) {
        return;
    }

    // Из GUI-потока синхронно вызываем shutdown() в core-потоке.
    if (QThread::currentThread() == coreThread) {
        return;
    }

    (void)QMetaObject::invokeMethod(executor, "shutdown", Qt::BlockingQueuedConnection);
}

void GeoCoinApplication::stopCoreThread() noexcept
{
    if (!coreThread) {
        return;
    }

    emit stopThread();
    if (!coreThread->wait(10000)) {
        coreThread->quit();
        coreThread->wait();
    }
}

void GeoCoinApplication::setDataDirectory(const QString &dataDir)
{
    dataDirectory = dataDir;
}

void GeoCoinApplication::addNode(const QString &node)
{
    addNodes.append(node);
}

void GeoCoinApplication::addConnectPeer(const QString &node)
{
    connectPeers.append(node);
}

void GeoCoinApplication::setNoAutoConnect(bool value)
{
    noAutoConnect = value;
}

void GeoCoinApplication::setMiningSha256Backend(const QString& backend)
{
    miningSha256Backend = backend;
}

void GeoCoinApplication::setStartupWalletPassword(const QString& password)
{
    startupWalletPassword_ = password;
}

static QLocale localeFromCode(const QString& code) {
    if (code == "ru") {
        return QLocale(QLocale::Russian);
    }
    return QLocale(QLocale::English);
}

bool GeoCoinApplication::setLanguage(const QString &languageCode)
{
    const QString code = languageCode.trimmed().toLower();
    if (code == currentLanguage_) {
        return true;
    }

    if (!currentLanguage_.isEmpty()) {
        removeTranslator(&appTranslator_);
    }

    currentLanguage_.clear();

    if (code == "en" || code.isEmpty()) {
        QLocale::setDefault(localeFromCode("en"));
        currentLanguage_ = "en";

        const QString fileName = QStringLiteral("geocoin_en.qm");
        bool loaded = appTranslator_.load(QStringLiteral(":/translations/%1").arg(fileName));
        if (!loaded) {
            const QString appDir = QCoreApplication::applicationDirPath();
            loaded = appTranslator_.load(QDir(appDir).filePath(QStringLiteral("translations/%1").arg(fileName)));
        }
        if (loaded) {
            installTranslator(&appTranslator_);
        }
        return true;
    }

    const QString fileName = QStringLiteral("geocoin_%1.qm").arg(code);

    bool loaded = appTranslator_.load(QStringLiteral(":/translations/%1").arg(fileName));
    if (!loaded) {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString fsPath = QDir(appDir).filePath(QStringLiteral("translations/%1").arg(fileName));
        loaded = appTranslator_.load(fsPath);
    }

    if (loaded) {
        installTranslator(&appTranslator_);
        QLocale::setDefault(localeFromCode(code));
        currentLanguage_ = code;
        return true;
    }

    QLocale::setDefault(localeFromCode("en"));
    currentLanguage_ = "en";
    return false;
}

void GeoCoinApplication::applyLanguageFromSettings()
{
    QSettings settings;
    const QString code = settings.value(QStringLiteral("ui/language"), QStringLiteral("en")).toString();
    setLanguage(code);
}

void GeoCoinApplication::createWindow(const NetworkStyle *networkStyle)
{
    window = new GeoCoinGUI(networkStyle);
    window->setDataDirectory(dataDirectory);

    // Create poll timer in main thread (before initialization)
    pollTimer = new QTimer(window);
    pollTimer->setInterval(2000);
    pollTimer->setTimerType(Qt::CoarseTimer);

    // Показываем окно сразу: пользователь видит инициализацию в реальном времени.
    window->show();
}

void GeoCoinApplication::createSplashScreen(const NetworkStyle *networkStyle)
{
    // Без splash-экрана: прогресс синхронизации показываем в статус-баре главного окна.
}

void GeoCoinApplication::startThread()
{
    if (coreThread)
        return;

    coreThread = new QThread(this);
    executor = new GeoCoinCore();
    executor->setDataDirectory(dataDirectory);
    executor->setAddNodes(addNodes);
    executor->setConnectPeers(connectPeers);
    executor->setNoAutoConnect(noAutoConnect);
    executor->setMiningSha256Backend(miningSha256Backend.isEmpty() ? QStringLiteral("auto") : miningSha256Backend);
    executor->setWalletPassword(startupWalletPassword_);
    startupWalletPassword_.detach();
    startupWalletPassword_.fill(QChar('\0'));
    startupWalletPassword_.clear();
    startupWalletPassword_.squeeze();
    executor->moveToThread(coreThread);

    // Communication to and from thread
    connect(executor, SIGNAL(initializeResult(bool, const QString&)),
            this, SLOT(initializeResult(bool, const QString&)));
    connect(executor, SIGNAL(shutdownResult()),
            this, SLOT(shutdownResult()));
    connect(executor, SIGNAL(runawayException(const QString&)),
            this, SLOT(handleRunawayException(const QString&)));
    connect(this, SIGNAL(requestedInitialize()),
            executor, SLOT(initialize()));
    connect(this, SIGNAL(requestedShutdown()),
            executor, SLOT(shutdown()));

    // Make sure executor object is deleted in its own thread
    connect(this, SIGNAL(stopThread()), executor, SLOT(deleteLater()));
    connect(this, SIGNAL(stopThread()), coreThread, SLOT(quit()));

    coreThread->start();
}

void GeoCoinApplication::requestInitialize()
{
    startThread();
    emit requestedInitialize();
}

void GeoCoinApplication::requestGuiSmokeTest()
{
    smokeTestMode_ = true;
    writeSmokeTrace(QStringLiteral("requestGuiSmokeTest scheduled"));
    QTimer::singleShot(0, this, &GeoCoinApplication::runGuiSmokeTest);
}

void GeoCoinApplication::requestGuiE2eTest()
{
    guiE2eTestMode_ = true;
    QTimer::singleShot(0, this, &GeoCoinApplication::runGuiE2eTest);
}

void GeoCoinApplication::requestShutdown()
{
    // Prevent multiple shutdown requests
    if (shutdownRequested) {
        return;
    }
    shutdownRequested = true;

    if (smokeTestMode_ || !executor) {
        prepareManagedWorkersForShutdown();
        quit();
        return;
    }

    try {
        // Hide window if still visible
        if (window) {
            window->hide();
        }

        // Stop poll timer
        if (pollTimer) {
            pollTimer->stop();
        }

        // Unsubscribe from wallet signals BEFORE shutdown
        if (walletModel) {
            walletModel->unsubscribeFromCoreSignals();
        }

        // Stop accepting callbacks, drain wallet sync in FIFO order and join
        // any asynchronous mining stop before the core releases node/wallet.
        prepareManagedWorkersForShutdown();

        // Request shutdown from core thread (will emit shutdownResult when done)
        // Models will be deleted in shutdownResult AFTER core shutdown completes
        emit requestedShutdown();
    } catch (const std::exception& e) {
        // Still try to shutdown
        emit requestedShutdown();
    } catch (...) {
        // Still try to shutdown
        emit requestedShutdown();
    }
}

void GeoCoinApplication::runGuiSmokeTest()
{
    writeSmokeTrace(QStringLiteral("runGuiSmokeTest entered"));
    QString error;
    if (!window) {
        returnValue = 1;
        qCritical() << "GeoCoin GUI smoke test failed: main window was not created";
        writeSmokeTrace(QStringLiteral("runGuiSmokeTest failed: no window"));
    } else if (!window->runSmokeChecks(&error)) {
        returnValue = 1;
        qCritical() << "GeoCoin GUI smoke test failed:" << error;
        writeSmokeTrace(QStringLiteral("runGuiSmokeTest failed: %1").arg(error));
    } else {
        returnValue = 0;
        qInfo() << "GeoCoin GUI smoke test passed";
        writeSmokeTrace(QStringLiteral("runGuiSmokeTest passed"));
    }

    if (window) {
        window->hide();
    }
    writeSmokeTrace(QStringLiteral("runGuiSmokeTest exiting"));
    exit(returnValue);
}

void GeoCoinApplication::runGuiE2eTest()
{
    struct E2eState {
        enum Phase {
            WaitPeers,
            WaitInitialSync,
            StartMiningMaturity,
            WaitMaturityBlocks,
            StopMiningAfterMaturity,
            WaitBalance,
            SendTransaction,
            WaitSyncBeforeConfirmation,
            StartMiningConfirmation,
            WaitConfirmationBlock,
            StopMiningAfterConfirmation,
            WaitPropagation,
            VerifyTransaction,
            Done
        };

        Phase phase = WaitPeers;
        QElapsedTimer elapsed;
        qint64 lastTickMs = 0;
        qint64 lastReportMs = 0;
        qint64 syncStartMs = 0;
        qint64 propagationStartMs = 0;
        geocoin::Height startHeight = 0;
        geocoin::Height txHeight = 0;
        geocoin::Height confirmedTxHeight = 0;
        QString txid;
        bool miningStarted = false;
        bool stopRequested = false;
    };

    const auto nodeInfo = nodeComponents_.services.info;
    const auto miningControl = nodeComponents_.services.mining;
    const auto chainState = nodeComponents_.chain_state;
    if (!window || !clientModel || !walletModel || !nodeInfo ||
        !miningControl || !chainState) {
        qCritical() << "GeoCoin GUI e2e failed: GUI/core/wallet was not initialized";
        returnValue = 1;
        requestShutdown();
        return;
    }

    auto state = std::make_shared<E2eState>();
    state->elapsed.start();
    state->lastTickMs = 0;
    state->startHeight = nodeInfo->get_status().current_height;

    const int peerTimeoutSec = envInt("GEOCOIN_QT_E2E_PEER_TIMEOUT_SEC", 180, 15, 1800);
    const int syncTimeoutSec = envInt("GEOCOIN_QT_E2E_SYNC_TIMEOUT_SEC", 600, 30, 3600);
    const int totalTimeoutSec = envInt("GEOCOIN_QT_E2E_TIMEOUT_SEC", 1800, 60, 7200);
    const int watchdogSec = envInt("GEOCOIN_QT_E2E_WATCHDOG_SEC", 30, 10, 300);
    const int extraBlocksAfterTx = envInt("GEOCOIN_QT_E2E_EXTRA_BLOCKS_AFTER_TX", 1, 0, 10);
    const int propagationWaitSec = envInt("GEOCOIN_QT_E2E_PROPAGATION_SEC", 45, 0, 600);
    const int desiredPeers = std::max(1, std::min(2, addNodes.size()));
    const int threadCap = GUIUtil::availableCpuThreads();
    const int miningThreads = envInt(
        "GEOCOIN_QT_E2E_THREADS",
        threadCap,
        1,
        threadCap
    );

    writeGuiE2eTrace(dataDirectory, QStringLiteral("start height=%1 desired_peers=%2 peer_timeout_sec=%3 sync_timeout_sec=%4 total_timeout_sec=%5 watchdog_sec=%6 mining_threads=%7 extra_blocks_after_tx=%8 propagation_wait_sec=%9 addnodes=%10")
        .arg(state->startHeight)
        .arg(desiredPeers)
        .arg(peerTimeoutSec)
        .arg(syncTimeoutSec)
        .arg(totalTimeoutSec)
        .arg(watchdogSec)
        .arg(miningThreads)
        .arg(extraBlocksAfterTx)
        .arg(propagationWaitSec)
        .arg(addNodes.join(QStringLiteral(","))));

    QObject::connect(walletModel, &WalletModel::transactionCreated, this, [state](const QString& txid) {
        state->txid = txid;
    }, Qt::UniqueConnection);

    auto timer = new QTimer(this);
    timer->setInterval(1000);
    timer->setTimerType(Qt::PreciseTimer);

    QObject::connect(timer, &QTimer::timeout, this, [this, timer, state, nodeInfo, miningControl, chainState, desiredPeers, miningThreads, peerTimeoutSec, syncTimeoutSec, totalTimeoutSec, watchdogSec, extraBlocksAfterTx, propagationWaitSec]() {
        const qint64 nowMs = state->elapsed.elapsed();
        if (state->lastTickMs > 0 && nowMs - state->lastTickMs > static_cast<qint64>(watchdogSec) * 1000) {
            qCritical() << "GeoCoin GUI e2e failed: event loop watchdog detected UI stall";
            writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL event loop stall delta_ms=%1").arg(nowMs - state->lastTickMs));
            returnValue = 1;
            timer->stop();
            timer->deleteLater();
            requestShutdown();
            return;
        } else if (state->lastTickMs > 0 && nowMs - state->lastTickMs > 10000) {
            writeGuiE2eTrace(dataDirectory, QStringLiteral("WARN event loop delay delta_ms=%1").arg(nowMs - state->lastTickMs));
        }
        state->lastTickMs = nowMs;

        const auto status = nodeInfo->get_status();
        if (nowMs > static_cast<qint64>(totalTimeoutSec) * 1000) {
            qCritical() << "GeoCoin GUI e2e failed: timeout";
            writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL timeout phase=%1 height=%2 peers=%3 mining=%4 txid=%5")
                .arg(static_cast<int>(state->phase))
                .arg(status.current_height)
                .arg(static_cast<qulonglong>(status.authenticated_peers))
                .arg(status.mining ? 1 : 0)
                .arg(state->txid));
            if (miningControl->is_mining()) {
                (void)requestMiningStopAsync();
            }
            returnValue = 1;
            timer->stop();
            timer->deleteLater();
            requestShutdown();
            return;
        }

        clientModel->updateNumBlocks(status.current_height);
        clientModel->updateNumConnections(static_cast<int>(status.authenticated_peers));
        clientModel->updateMining(status.mining, static_cast<int>(status.mining_threads), status.hashrate, status.mined_blocks_accepted);
        walletModel->pollBalanceChanged();

        if (state->lastReportMs == 0 || nowMs - state->lastReportMs >= 5000) {
            state->lastReportMs = nowMs;
            writeGuiE2eTrace(dataDirectory, QStringLiteral("tick phase=%1 height=%2 target=%3 best_peer=%4 initial_sync_complete=%5 peers=%6 mining=%7 hashrate=%8 balance=%9 txid=%10")
                .arg(static_cast<int>(state->phase))
                .arg(status.current_height)
                .arg(status.target_height)
                .arg(status.best_peer_height)
                .arg(status.initial_sync_complete ? 1 : 0)
                .arg(static_cast<qulonglong>(status.authenticated_peers))
                .arg(status.mining ? 1 : 0)
                .arg(static_cast<double>(status.hashrate))
                .arg(walletModel->getBalance())
                .arg(state->txid));
        }

        switch (state->phase) {
        case E2eState::WaitPeers:
            if (status.authenticated_peers >= static_cast<size_t>(desiredPeers)) {
                qInfo() << "GeoCoin GUI e2e: peers connected" << status.authenticated_peers;
                writeGuiE2eTrace(dataDirectory, QStringLiteral("peers connected count=%1")
                    .arg(static_cast<qulonglong>(status.authenticated_peers)));
                state->syncStartMs = nowMs;
                state->phase = E2eState::WaitInitialSync;
            } else if (nowMs > static_cast<qint64>(peerTimeoutSec) * 1000) {
                qCritical() << "GeoCoin GUI e2e failed: only" << status.authenticated_peers
                            << "authenticated peers, expected" << desiredPeers;
                writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL peers count=%1 expected=%2")
                    .arg(static_cast<qulonglong>(status.authenticated_peers))
                    .arg(desiredPeers));
                returnValue = 1;
                timer->stop();
                timer->deleteLater();
                requestShutdown();
            }
            break;

        case E2eState::WaitInitialSync:
            if (status.initial_sync_complete) {
                state->startHeight = status.current_height;
                qInfo() << "GeoCoin GUI e2e: initial sync complete at height" << status.current_height
                        << "best peer height" << status.best_peer_height;
                writeGuiE2eTrace(dataDirectory, QStringLiteral("initial sync complete height=%1 target=%2 best_peer=%3")
                    .arg(status.current_height)
                    .arg(status.target_height)
                    .arg(status.best_peer_height));
                state->phase = E2eState::StartMiningMaturity;
            } else if (state->syncStartMs > 0 &&
                       nowMs - state->syncStartMs > static_cast<qint64>(syncTimeoutSec) * 1000) {
                qCritical() << "GeoCoin GUI e2e failed: initial sync timeout";
                writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL initial sync timeout height=%1 target=%2 best_peer=%3 syncing=%4 reason=%5")
                    .arg(status.current_height)
                    .arg(status.target_height)
                    .arg(status.best_peer_height)
                    .arg(status.syncing ? 1 : 0)
                    .arg(QString::fromStdString(status.mining_block_reason)));
                returnValue = 1;
                timer->stop();
                timer->deleteLater();
                requestShutdown();
            }
            break;

        case E2eState::StartMiningMaturity: {
            const QString miningAddress = walletModel->getAddress();
            if (miningAddress.isEmpty()) {
                qCritical() << "GeoCoin GUI e2e failed: wallet address is empty";
                writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL wallet address is empty"));
                returnValue = 1;
                timer->stop();
                timer->deleteLater();
                requestShutdown();
                break;
            }
            const geocoin::Address address(miningAddress.toStdString());
            if (!miningControl->start_mining(static_cast<size_t>(miningThreads), address)) {
                if (nodeInfo->get_status().mining_blocked_by_sync) {
                    const auto blockedStatus = nodeInfo->get_status();
                    writeGuiE2eTrace(dataDirectory, QStringLiteral("mining deferred until sync height=%1 best_peer=%2 reason=%3")
                        .arg(blockedStatus.current_height)
                        .arg(blockedStatus.best_peer_height)
                        .arg(QString::fromStdString(blockedStatus.mining_block_reason)));
                    state->syncStartMs = nowMs;
                    state->phase = E2eState::WaitInitialSync;
                    break;
                }
                qCritical() << "GeoCoin GUI e2e failed: GUI/node mining did not start";
                writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL mining did not start"));
                returnValue = 1;
                timer->stop();
                timer->deleteLater();
                requestShutdown();
                break;
            }
            qInfo() << "GeoCoin GUI e2e: mining started, threads" << miningThreads
                    << "start_height" << state->startHeight;
            writeGuiE2eTrace(dataDirectory, QStringLiteral("mining started address=%1 threads=%2 start_height=%3")
                .arg(miningAddress)
                .arg(miningThreads)
                .arg(state->startHeight));
            state->phase = E2eState::WaitMaturityBlocks;
            break;
        }

        case E2eState::WaitMaturityBlocks:
            if (status.current_height >= state->startHeight + geocoin::consensus::params::COINBASE_MATURITY + 1) {
                state->phase = E2eState::StopMiningAfterMaturity;
                state->stopRequested = false;
            }
            break;

        case E2eState::StopMiningAfterMaturity:
            if (!state->stopRequested) {
                state->stopRequested = requestMiningStopAsync();
            } else if (reapMiningStopIfFinished() && !miningControl->is_mining()) {
                walletModel->updateBalance();
                qInfo() << "GeoCoin GUI e2e: maturity mining stopped at height" << status.current_height
                        << "balance" << walletModel->getBalance();
                writeGuiE2eTrace(dataDirectory, QStringLiteral("maturity mining stopped height=%1 balance=%2")
                    .arg(status.current_height)
                    .arg(walletModel->getBalance()));
                state->phase = E2eState::WaitBalance;
            }
            break;

        case E2eState::WaitBalance: {
            walletModel->updateBalance();
            const qint64 relayFee = walletModel->estimateFee(static_cast<qint64>(geocoin::COIN));
            if (walletModel->getBalance() > relayFee + static_cast<qint64>(geocoin::COIN)) {
                state->phase = E2eState::SendTransaction;
            }
            break;
        }

        case E2eState::SendTransaction: {
            const QString toAddress = walletModel->getAddress();
            state->txid.clear();
            const auto result = walletModel->sendCoins(
                toAddress,
                static_cast<qint64>(geocoin::COIN),
                walletModel->estimateFee(static_cast<qint64>(geocoin::COIN)),
                QString()
            );
            if (result != WalletModel::OK || state->txid.isEmpty()) {
                qCritical() << "GeoCoin GUI e2e failed: WalletModel sendCoins failed, code" << result;
                writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL sendCoins code=%1 txid=%2")
                    .arg(static_cast<int>(result))
                    .arg(state->txid));
                returnValue = 1;
                timer->stop();
                timer->deleteLater();
                requestShutdown();
                break;
            }
            state->txHeight = status.current_height;
            qInfo() << "GeoCoin GUI e2e: transaction submitted" << state->txid;
            writeGuiE2eTrace(dataDirectory, QStringLiteral("transaction submitted txid=%1 height=%2")
                .arg(state->txid)
                .arg(state->txHeight));
            state->phase = E2eState::StartMiningConfirmation;
            break;
        }

        case E2eState::WaitSyncBeforeConfirmation:
            if (status.initial_sync_complete) {
                writeGuiE2eTrace(dataDirectory, QStringLiteral("sync before confirmation complete height=%1 best_peer=%2")
                    .arg(status.current_height)
                    .arg(status.best_peer_height));
                state->phase = E2eState::StartMiningConfirmation;
            } else if (state->syncStartMs > 0 &&
                       nowMs - state->syncStartMs > static_cast<qint64>(syncTimeoutSec) * 1000) {
                qCritical() << "GeoCoin GUI e2e failed: confirmation sync timeout";
                writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL confirmation sync timeout height=%1 target=%2 best_peer=%3 syncing=%4")
                    .arg(status.current_height)
                    .arg(status.target_height)
                    .arg(status.best_peer_height)
                    .arg(status.syncing ? 1 : 0));
                returnValue = 1;
                timer->stop();
                timer->deleteLater();
                requestShutdown();
            }
            break;

        case E2eState::StartMiningConfirmation: {
            const geocoin::Address address(walletModel->getAddress().toStdString());
            if (!miningControl->start_mining(static_cast<size_t>(miningThreads), address)) {
                if (nodeInfo->get_status().mining_blocked_by_sync) {
                    const auto blockedStatus = nodeInfo->get_status();
                    writeGuiE2eTrace(dataDirectory, QStringLiteral("confirmation mining deferred until sync height=%1 best_peer=%2 reason=%3")
                        .arg(blockedStatus.current_height)
                        .arg(blockedStatus.best_peer_height)
                        .arg(QString::fromStdString(blockedStatus.mining_block_reason)));
                    state->syncStartMs = nowMs;
                    state->phase = E2eState::WaitSyncBeforeConfirmation;
                    break;
                }
                qCritical() << "GeoCoin GUI e2e failed: confirmation mining did not start";
                writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL confirmation mining did not start"));
                returnValue = 1;
                timer->stop();
                timer->deleteLater();
                requestShutdown();
                break;
            }
            state->phase = E2eState::WaitConfirmationBlock;
            break;
        }

        case E2eState::WaitConfirmationBlock:
            if (!state->txid.isEmpty()) {
                geocoin::Hash256 txHash{};
                if (parseHashHex(state->txid, txHash) && chainState->get_transaction(txHash)) {
                    if (state->confirmedTxHeight == 0) {
                        state->confirmedTxHeight = status.current_height;
                        writeGuiE2eTrace(dataDirectory, QStringLiteral("transaction confirmed txid=%1 height=%2")
                            .arg(state->txid)
                            .arg(status.current_height));
                    }
                    const geocoin::Height targetHeight =
                        state->confirmedTxHeight + static_cast<geocoin::Height>(extraBlocksAfterTx);
                    if (status.current_height >= targetHeight) {
                        state->phase = E2eState::StopMiningAfterConfirmation;
                        state->stopRequested = false;
                        break;
                    }
                    if (state->lastReportMs == nowMs) {
                        writeGuiE2eTrace(dataDirectory, QStringLiteral("mining extra propagation block current_height=%1 target_height=%2")
                            .arg(status.current_height)
                            .arg(targetHeight));
                    }
                }
            }
            if (status.current_height >= state->txHeight + 1 && state->lastReportMs == nowMs) {
                writeGuiE2eTrace(dataDirectory, QStringLiteral("waiting for own tx confirmation txid=%1 height=%2")
                    .arg(state->txid)
                    .arg(status.current_height));
            }
            if (status.current_height >= state->txHeight + 12 + static_cast<geocoin::Height>(extraBlocksAfterTx)) {
                qCritical() << "GeoCoin GUI e2e failed: own transaction was not confirmed" << state->txid;
                writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL own tx not confirmed txid=%1 height=%2 tx_height=%3")
                    .arg(state->txid)
                    .arg(status.current_height)
                    .arg(state->txHeight));
                returnValue = 1;
                state->phase = E2eState::StopMiningAfterConfirmation;
                state->stopRequested = false;
            }
            break;

        case E2eState::StopMiningAfterConfirmation:
            if (!state->stopRequested) {
                state->stopRequested = requestMiningStopAsync();
            } else if (reapMiningStopIfFinished() && !miningControl->is_mining()) {
                state->propagationStartMs = nowMs;
                writeGuiE2eTrace(dataDirectory, QStringLiteral("confirmation mining stopped height=%1 propagation_wait_sec=%2")
                    .arg(status.current_height)
                    .arg(propagationWaitSec));
                state->phase = E2eState::WaitPropagation;
            }
            break;

        case E2eState::WaitPropagation:
            if (state->propagationStartMs <= 0 ||
                nowMs - state->propagationStartMs >= static_cast<qint64>(propagationWaitSec) * 1000) {
                state->phase = E2eState::VerifyTransaction;
            }
            break;

        case E2eState::VerifyTransaction: {
            geocoin::Hash256 hash{};
            geocoin::core::Transaction tx;
            const auto confirmedTx = (parseHashHex(state->txid, hash) && chainState)
                ? chainState->get_transaction(hash)
                : nullptr;
            if (!confirmedTx) {
                qCritical() << "GeoCoin GUI e2e failed: confirmed tx not found" << state->txid;
                writeGuiE2eTrace(dataDirectory, QStringLiteral("FAIL confirmed tx not found txid=%1").arg(state->txid));
                returnValue = 1;
            } else {
                qInfo() << "GeoCoin GUI e2e passed:"
                        << "height" << status.current_height
                        << "peers" << status.authenticated_peers
                        << "txid" << state->txid;
                writeGuiE2eTrace(dataDirectory, QStringLiteral("PASS height=%1 peers=%2 txid=%3")
                    .arg(status.current_height)
                    .arg(static_cast<qulonglong>(status.authenticated_peers))
                    .arg(state->txid));
                returnValue = 0;
            }
            state->phase = E2eState::Done;
            timer->stop();
            timer->deleteLater();
            requestShutdown();
            break;
        }

        case E2eState::Done:
            break;
        }
    });

    timer->start();
}

void GeoCoinApplication::initializeResult(bool success, const QString &error)
{
    // Set exit result
    returnValue = success ? 0 : 1;

    if (success) {
        // Receive one coherent owning runtime bundle. The concrete Node stays
        // inside GeoCoinCore and never leaks into ordinary GUI code.
        auto components = executor->getNodeComponents();
        auto wallet = executor->getWallet();
        if (!components.complete() || !wallet) {
            qCritical() << "GeoCoin initialization returned incomplete runtime components";
            returnValue = 1;
            quit();
            return;
        }
        nodeComponents_ = components;

        // Создаем модели в главном (GUI) потоке.
        clientModel = new ClientModel(this);
        clientModel->setNodeComponents(components);

        walletModel = new WalletModel(this);
        walletModel->setDataDirectory(dataDirectory);
        if (!walletModel->setRuntimeContext(wallet, components)) {
            qCritical() << "GeoCoin initialization could not bind the GUI runtime context";
            returnValue = 1;
            quit();
            return;
        }

        // Production first-run wallet creation is explicit and password-gated.
        // The isolated GUI E2E harness creates a temporary unencrypted wallet so
        // its existing non-interactive transaction scenario remains usable.
        if (guiE2eTestMode_ && !walletModel->isWalletLoaded() &&
            !walletModel->createWallet(walletModel->getWalletFilePath(), QString())) {
            qCritical() << "GeoCoin GUI e2e failed: could not create temporary wallet";
            returnValue = 1;
            requestShutdown();
            return;
        }

        // Подписываемся на сигналы кошелька.
        walletModel->subscribeToCoreSignals();

        // Node callbacks can arrive concurrently from mining/network threads.
        // A single bounded worker preserves FIFO wallet updates without ever
        // blocking the chain writer. Gaps recover from chain and mempool state.
        std::weak_ptr<geocoin::node::ChainQueryService> weakChainQuery =
            components.services.chain_query;
        walletSyncWorker_ = std::make_shared<WalletSyncWorker>(
            wallet,
            components.chain_state,
            [weakChainQuery]() {
                if (const auto query = weakChainQuery.lock()) {
                    return query->get_mempool_transactions();
                }
                return std::vector<geocoin::core::Transaction>{};
            }
        );
        std::weak_ptr<WalletSyncWorker> walletSyncWorker = walletSyncWorker_;
        components.services.events->set_chain_mutation_event_callback([walletSyncWorker](
            const geocoin::services::ChainMutationEvent& event) {
            if (auto worker = walletSyncWorker.lock()) {
                (void)worker->enqueue(event);
            }
        });

        // Set models in window (window is already visible)
        window->setClientModel(clientModel);
        window->setWalletModel(walletModel);

        // Setup update timer (timer was created in main thread in createWindow)
        const auto nodeInfo = components.services.info;
        connect(pollTimer, &QTimer::timeout, [this, nodeInfo]() {
            auto status = nodeInfo->get_status();
            clientModel->updateNumBlocks(status.current_height);
            clientModel->updateNumConnections(status.authenticated_peers);
            clientModel->updateMining(status.mining, status.mining_threads, status.hashrate, status.mined_blocks_accepted);

            // Периодически опрашиваем кошелек на изменения баланса.
            walletModel->pollBalanceChanged();

            if (status.syncing && status.target_height > 0) {
                double progress = (double)status.current_height / (double)status.target_height;
                window->setSyncProgress(progress, status.current_height, status.target_height);
            } else {
                int rescan_current = 0;
                int rescan_target = 0;
                if (walletModel->getRescanProgress(rescan_current, rescan_target)) {
                    double progress = (rescan_target > 0)
                        ? (double)rescan_current / (double)rescan_target
                        : 0.0;
                    window->setSyncProgress(progress, rescan_current, rescan_target);
                } else {
                    window->setSyncProgress(1.0, status.current_height, status.current_height);
                }
            }
        });
        pollTimer->start();

        if (guiE2eTestMode_) {
            QTimer::singleShot(0, this, &GeoCoinApplication::requestGuiE2eTest);
        }

    } else {
        // Initialization failed
        if (smokeTestMode_) {
            qCritical() << "GeoCoin initialization failed during smoke mode:" << error;
        } else {
            QMessageBox::critical(nullptr, tr("GeoCoin"),
                tr("Initialization failed: %1").arg(error));
        }
        quit();
    }
}

void GeoCoinApplication::shutdownResult()
{
    try {
        // Delete models AFTER core shutdown completes
        delete walletModel;
        walletModel = nullptr;

        delete clientModel;
        clientModel = nullptr;

        nodeComponents_ = {};

        quit(); // Exit main loop after shutdown finished
    } catch (const std::exception& e) {
        quit(); // Still exit
    } catch (...) {
        quit(); // Still exit
    }
}

void GeoCoinApplication::handleRunawayException(const QString &message)
{
    if (smokeTestMode_) {
        qCritical() << "GeoCoin fatal error during smoke mode:" << message;
    } else {
        QMessageBox::critical(nullptr, tr("GeoCoin"),
            tr("A fatal error occurred. GeoCoin can no longer continue safely and will quit.") +
            QString("\n\n") + message);
    }
    ::exit(1);
}
