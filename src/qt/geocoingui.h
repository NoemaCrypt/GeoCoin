// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_GEOCOINGUI_H
#define GEOCOIN_QT_GEOCOINGUI_H

#include <QMainWindow>
#include <QLabel>
#include <QProgressBar>
#include <functional>
#include "managedasyncaction.h"

class ClientModel;
class WalletModel;
class NetworkStyle;
class WalletFrame;
#ifdef GEOCOIN_NATIVE_MACOS
class MiningPage;
#endif

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class QLineEdit;
class QToolButton;
class QFrame;
QT_END_NAMESPACE

/**
 * Main GeoCoin GUI window
 * Главное окно GeoCoin Qt с вкладкой майнинга
 */
class GeoCoinGUI : public QMainWindow
{
    Q_OBJECT

public:
    explicit GeoCoinGUI(const NetworkStyle *networkStyle, QWidget *parent = nullptr);
    ~GeoCoinGUI();

    /** Set the client model */
    void setClientModel(ClientModel *clientModel);

    /** Set the wallet model */
    void setWalletModel(WalletModel *walletModel);

    /** Set the data directory */
    void setDataDirectory(const QString &dataDir);

    /** Set sync progress */
    void setSyncProgress(double progress, int current, int target);

    /** Run offscreen/headless GUI smoke checks without starting node/wallet init. */
    bool runSmokeChecks(QString *error = nullptr);
    bool runLayoutChecks(QString *error = nullptr);

    /** Join an in-flight CPU mining stop before core/node destruction. */
    void waitForPendingMiningStop();
    void beginShutdown();
    bool minersStopped();
    void openWalletPath(const QString& filename, const QString& password = QString());
#ifdef GEOCOIN_NATIVE_MACOS
    void setNativeWorkflowsHosted() { nativeWorkflowsHosted_ = true; }
    void invokeNativeMenuAction(const QString& command);
    void syncNativeTheme(const QString& theme);
    void refreshNativeLanguage();
    MiningPage* nativeMiningController() const;
    WalletFrame* nativeWalletFrame() const { return walletFrame; }
#endif

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void persistLastPage(const QString& pageId);
    void restoreUiSettings();
    void gotoPageById(const QString& pageId);

    ClientModel *clientModel;
    WalletModel *walletModel;
    WalletFrame *walletFrame;
    QString dataDirectory;  // Data directory for wallet and blocks
    QString lastVisitedPage_;
    QString titleAddText_;
    QString lastWalletHistoryError_;
#ifdef GEOCOIN_NATIVE_MACOS
    bool nativeWorkflowsHosted_ = false;
#endif
    bool closing_ = false;
    bool walletTransition_ = false;
    bool walletTransitionFinalizing_ = false;
    ManagedAsyncAction walletMiningStop_;
    void finishWalletTransition(bool closeOnly);
    void startWalletTransition(bool closeOnly);
    void showWalletOperationError(const QString& title);
    bool statusCpuMiningEnabled = false;
    int statusCpuMiningThreads = 0;
    double statusCpuHashrate = 0.0;
    bool statusGpuMiningEnabled = false;
    double statusGpuHashrate = 0.0;
    int statusConnectionsCount_ = 0;
    int statusBlocksCount_ = 0;
    bool statusSyncActive_ = false;
    QString statusMiningText_;

    // Status bar
    QLabel *labelConnectionsIcon = nullptr;
    QLabel *labelConnectionsText = nullptr;
    QLabel *labelBlocksIcon = nullptr;
    QLabel *labelBlocksText = nullptr;
    QProgressBar *progressBar = nullptr;
    QLabel *progressBarLabel = nullptr;
    QLabel *labelHashrateStatus = nullptr;
    QToolButton *themeToggleButton = nullptr;
    QFrame *clientAreaFrame_ = nullptr;

    // Menu bar
    QMenu *fileMenu;
    QMenu *settingsMenu;
    QMenu *helpMenu;
    QMenu *languageMenu;

    // Actions
    QAction *quitAction;
    QAction *aboutAction;
    QAction *optionsAction;
    QAction *langEnglishAction;
    QAction *langRussianAction;
    QAction *createWalletAction;
    QAction *loadWalletAction;
    QAction *closeWalletAction;
    QAction *encryptWalletAction;
    QAction *backupWalletAction;
    QAction *exportPrivateKeyAction;
    QAction *importPrivateKeyAction;
    QAction *changePassphraseAction;

    // Tabs
    QAction *overviewAction;
    QAction *sendCoinsAction;
    QAction *receiveCoinsAction;
    QAction *historyAction;
    QAction *miningAction;
    QAction *multisigAction;
    QAction *corporateAction;
    QAction *nftAction;

    void createActions();
    void createMenuBar();
    void createToolBars();
    void updateNavigationWidths();
    void createStatusBar();
    void refreshMiningStatusBar();
    void updateStatusBarLayout();
    void updateMiningStatusText();
    void applyCurrentTheme();
    void retranslateUi();
    void showStyledMessageBox(int icon, const QString &title, const QString &text);
    int showStyledQuestionBox(const QString &title, const QString &text);
    QString showStyledInputDialog(const QString &title, const QString &label, int echoMode = 0);
    bool ensureWalletSaved();
    void updateWalletActions();

private slots:
    void setNumConnections(int count);
    void setNumBlocks(int count);
    void updateMiningStatus(bool enabled, int threads, double hashrate, quint64 minedBlocksAccepted);
    void updateGpuMiningStatus(bool enabled, double hashrate, int acceptedBlocks, int rejectedBlocks);
    void aboutClicked();
    void optionsClicked();
    void gotoOverviewPage();
    void gotoSendCoinsPage();
    void gotoReceiveCoinsPage();
    void gotoHistoryPage();
    void gotoMiningPage();
    void gotoMultisigPage();
    void gotoCorporatePage();
    void gotoNftPage();
    void encryptWallet();
    void backupWallet();
    void exportPrivateKey();
    void importPrivateKey();
    void changePassphrase();
    void createWallet();
    void loadWallet();
    void closeWallet();
};

#endif // GEOCOIN_QT_GEOCOINGUI_H
