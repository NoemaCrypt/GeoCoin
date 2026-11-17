// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_GEOCOINAPPLICATION_H
#define GEOCOIN_QT_GEOCOINAPPLICATION_H

#include <QApplication>
#include <QTranslator>
#include <QString>
#include <memory>

#include "geocoin/node/node_components.hpp"
#include "managedasyncaction.h"

class GeoCoinGUI;
class ClientModel;
class WalletModel;
class NetworkStyle;
class QThread;
class GeoCoinCore;
class QTimer;

namespace geocoin { namespace wallet { class Wallet; } }

/** Main GeoCoin application object */
class GeoCoinApplication : public QApplication
{
    Q_OBJECT

public:
    explicit GeoCoinApplication(int &argc, char **argv);
    ~GeoCoinApplication();
    int exec();
    bool isShutdownComplete() const { return shutdownComplete_; }

    /** Create main window */
    void createWindow(const NetworkStyle *networkStyle);

    /** Create splash screen */
    void createSplashScreen(const NetworkStyle *networkStyle);

    /** Request core initialization */
    void requestInitialize();

    /** Request a UI-only smoke test without starting the core thread. */
    void requestGuiSmokeTest();

    /** Request a full GUI/core/network e2e test. */
    void requestGuiE2eTest();

    /** Get process return value */
    int getReturnValue() { return returnValue; }

    /** Set data directory */
    void setDataDirectory(const QString &dataDir);

    /** Add node to connect to */
    void addNode(const QString &node);

    /** Add explicit connect peer (connect-only semantics) */
    void addConnectPeer(const QString &node);

    /** Disable auto-connect */
    void setNoAutoConnect(bool value);

    /** Select mining SHA256 backend: auto/native/legacy/openssl */
    void setMiningSha256Backend(const QString& backend);

    /** Password supplied by the startup wallet prompt. */
    void setStartupWalletPassword(const QString& password);
    void setStartupWalletPath(const QString& path) { startupWalletPath_ = path; }
    void setNetwork(const QString& network) { network_ = network; }
    void setP2pPort(quint16 port) { p2pPort_ = port; }
    void setActivationManifestPath(const QString& path) { activationManifestPath_ = path; }
    bool isShuttingDown() const { return shutdownRequested; }

    /** Load UI language (en/ru/be). Returns true if translation loaded. */
    bool setLanguage(const QString &languageCode);

    /** Load language from QSettings (ui/language) */
    void applyLanguageFromSettings();

    void setSmokeTestMode(bool value) { smokeTestMode_ = value; }
    bool isSmokeTestMode() const { return smokeTestMode_; }
    void setGuiE2eTestMode(bool value) { guiE2eTestMode_ = value; }
    bool isGuiE2eTestMode() const { return guiE2eTestMode_; }

public slots:
    /** Request core shutdown (public slot for signal handler) */
    void requestShutdown();

    void initializeResult(bool success, const QString &error);
    void shutdownResult();
    void shutdownFailed(const QString& error);
    void handleRunawayException(const QString &message);
    void runGuiSmokeTest();
    void runGuiE2eTest();

signals:
    void requestedInitialize();
    void requestedShutdown();
    void stopThread();
    void splashFinished(QWidget *window);
    void shutdownCompleted();
    void shutdownError(const QString& error);

private:
    void drainShutdown();
    void stopCoreThread() noexcept;
    bool requestMiningStopAsync();
    bool reapMiningStopIfFinished();
    void pollNodeStatus();

    QThread *coreThread;
    GeoCoinCore *executor;
    ClientModel *clientModel;
    WalletModel *walletModel;
    GeoCoinGUI *window;
    QTimer *pollTimer;
    geocoin::node::NodeComponentHandle nodeComponents_{};
    ManagedAsyncAction miningStopAction_;
    ManagedAsyncAction statusPollAction_;
    std::shared_ptr<geocoin::node::NodeStatus> statusSnapshot_;
    quint64 statusGeneration_ = 0;
    quint64 statusSnapshotGeneration_ = 0;
    qint64 lastStatusRequestMs_ = 0;
    bool statusPending_ = false;
    bool statusFailureReported_ = false;
    QString dataDirectory;
    QStringList addNodes;
    QStringList connectPeers;
    QString miningSha256Backend;
    QString startupWalletPassword_;
    QString startupWalletPath_;
    QString network_ = QStringLiteral("degree");
    quint16 p2pPort_ = 0;
    QString activationManifestPath_;
    QTimer* shutdownPollTimer_ = nullptr;
    bool coreShutdownQueued_ = false;
    bool shutdownMiningStarted_ = false;
    bool shutdownComplete_ = false;
    bool noAutoConnect;
    int returnValue;
    bool shutdownRequested;
    bool smokeTestMode_;
    bool guiE2eTestMode_;

    QTranslator appTranslator_;
    QString currentLanguage_;

    void startThread();
};

#endif // GEOCOIN_QT_GEOCOINAPPLICATION_H
