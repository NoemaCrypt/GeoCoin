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
class WalletSyncWorker;

namespace geocoin { namespace wallet { class Wallet; } }

/** Main GeoCoin application object */
class GeoCoinApplication : public QApplication
{
    Q_OBJECT

public:
    explicit GeoCoinApplication(int &argc, char **argv);
    ~GeoCoinApplication();

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
    void handleRunawayException(const QString &message);
    void runGuiSmokeTest();
    void runGuiE2eTest();

signals:
    void requestedInitialize();
    void requestedShutdown();
    void stopThread();
    void splashFinished(QWidget *window);

private:
    void prepareManagedWorkersForShutdown() noexcept;
    void shutdownCoreThreadSynchronously() noexcept;
    void stopCoreThread() noexcept;
    bool requestMiningStopAsync();
    bool reapMiningStopIfFinished();

    QThread *coreThread;
    GeoCoinCore *executor;
    ClientModel *clientModel;
    WalletModel *walletModel;
    GeoCoinGUI *window;
    QTimer *pollTimer;
    geocoin::node::NodeComponentHandle nodeComponents_{};
    std::shared_ptr<WalletSyncWorker> walletSyncWorker_;
    ManagedAsyncAction miningStopAction_;
    QString dataDirectory;
    QStringList addNodes;
    QStringList connectPeers;
    QString miningSha256Backend;
    QString startupWalletPassword_;
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
