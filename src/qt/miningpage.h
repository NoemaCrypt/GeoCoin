// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_MININGPAGE_H
#define GEOCOIN_QT_MININGPAGE_H

#include <QProcess>
#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>
#include <memory>

#include "managedasyncaction.h"

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
#ifdef GEOCOIN_NATIVE_MACOS
class GeoMiningStatusLabel;
#endif
class QPushButton;
class QSpinBox;
class QCheckBox;
class QListWidget;
class QTabWidget;
class QTimer;
class QMessageBox;
QT_END_NAMESPACE

namespace geocoin {
namespace rpc {
class RPCServer;
}
}

/** Mining page widget - NEW for GeoCoin */
class MiningPage : public QWidget
{
    Q_OBJECT

public:
    explicit MiningPage(QWidget *parent = nullptr);
    ~MiningPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    bool isGpuMiningActive() const;
    bool hasPendingMiningAction() const { return miningActionInProgress || miningStopAction.isRunning(); }
    void waitForPendingMiningStop();
    void applyTheme();
    bool runProcessLifecycleChecks(const QString& fixture, QString* error);
#ifdef GEOCOIN_NATIVE_MACOS
    bool requestNativeCpuMining(int threads, QString* error);
    bool startNativeGpuMining(const QString& backend, const QStringList& devices, int intensity, QString* error);
    QString nativeGpuMiningStatus() const;
    QString nativeGpuBackend() const { return activeNativeGpuBackend; }
    bool isNativeGpuMiningStopping() const { return (gpuStopRequested || gpuSessionFinalizing) && !gpuCleanupFailed; }
#endif

signals:
    void gpuMiningChanged(bool enabled, double hashrate, int acceptedBlocks, int rejectedBlocks);
    void gpuMiningStopped();
    void gpuCleanupError(const QString& error);
#ifdef GEOCOIN_NATIVE_MACOS
    void nativeGpuStatusChanged(const QString& status);
#endif

public slots:
    void updateMiningInfo(bool enabled, int threads, double hashrate, quint64 minedBlocksAccepted);
    void stopGpuMining();

private:
    ClientModel *clientModel;
    WalletModel *walletModel;

    // Mining controls
    QPushButton *buttonStartStop;
    QSpinBox *spinThreads;
    QLabel *labelHashrate;
    QLabel *labelBlocksFound;
    QLabel *labelMiningAddress;
    QTabWidget *modeTabs;

    QCheckBox *checkGpuCuda;
    QCheckBox *checkGpuHip;
    QCheckBox *checkGpuOpenCl;
    QListWidget *listGpuDevices;
    QSpinBox *spinGpuIntensity;
    QPushButton *buttonGpuRefreshDevices;
    QPushButton *buttonGpuStartStop;
    QLabel *labelGpuHashrate;
    QLabel *labelGpuAccepted;
    QLabel *labelGpuRejected;
#ifdef GEOCOIN_NATIVE_MACOS
    GeoMiningStatusLabel *labelGpuStatus;
#else
    QLabel *labelGpuStatus;
#endif
    QProcess *gpuDeviceProbe;
    QProcess *gpuMinerProcess;
    QTimer *miningStopPollTimer;
    std::unique_ptr<geocoin::rpc::RPCServer> gpuRpcServer;
    QString gpuRpcPasswordFile;
    QString gpuRpcUser;
    QString gpuRpcPassword;
    int gpuRpcPort;

    // Mining status
    bool miningEnabled;
    bool miningActionInProgress;
    int miningThreads;
    double currentHashrate;
    int blocksFound;
    bool gpuMiningEnabled;
    int gpuAcceptedBlocks;
    int gpuRejectedBlocks;
    double gpuHashrate;
    ManagedAsyncAction miningStopAction;
    ManagedAsyncAction gpuRpcStopAction;
    struct RpcCleanupState;
    std::shared_ptr<RpcCleanupState> gpuRpcCleanup;
    QTimer *gpuStopPollTimer = nullptr;
    bool gpuStopRequested = false;
    bool gpuSessionFinalizing = false;
    bool gpuCleanupFailed = false;
    QPointer<QMessageBox> gpuCleanupErrorDialog;
    QElapsedTimer gpuStopElapsed;
    bool gpuTerminateSent = false;
    bool gpuKillSent = false;
    QString gpuLastError;
#ifdef GEOCOIN_NATIVE_MACOS
    QString activeNativeGpuBackend;
#endif
    QByteArray gpuOutputBuffer;
    QByteArray gpuErrorBuffer;

    void setupUi();
    void showStyledMessageBox(int icon, const QString &title, const QString &text);
    QString gpuMinerBinaryPath() const;
    QString selectedGpuBackend() const;
#ifdef GEOCOIN_NATIVE_MACOS
    int selectedGpuDeviceRole() const;
#endif
    QStringList selectedGpuDeviceIds() const;
    void setGpuBackendAvailability(bool hasCuda, bool hasHip, bool hasOpenCl);
    void updateGpuDeviceItemsForBackend(bool checkAvailable);
    bool startPrivateGpuRpc(QString *error);
    void stopPrivateGpuRpc();
    void setGpuMiningUi(bool running);
#ifdef GEOCOIN_NATIVE_MACOS
    bool requestCpuMining(int threads, bool stopping, QString* error);
    bool startGpuMiningSession(const QString& backend, const QStringList& devices, int intensity, QString* error);
#endif
    void processGpuStatsLine(const QByteArray& line);
    void updateOverallMiningDisplay();
    void createGpuMinerProcess();
    void resetGpuMinerProcess(bool forceStop);
    void finishGpuSession(int exitCode, QProcess::ExitStatus exitStatus);
    void requestGpuRpcStop();
    void reportGpuCleanupFailure(const QString& error);
    void pollGpuStop();
    void clearGpuRates();
    void recordGpuDiagnostic(const QString& event, const QString& detail = QString());
    void applyModeTabsStyle();
    void updateGpuBackendMinimumSizes();
    void refreshWalletState();

private slots:
    void toggleMining();
    void threadsChanged(int threads);
    void gpuBackendToggled(bool checked);
    void probeGpuDevices();
    void gpuProbeFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void toggleGpuMining();
    void gpuMinerReadyRead();
    void gpuMinerFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void pollMiningStop();
};

#endif // GEOCOIN_QT_MININGPAGE_H
