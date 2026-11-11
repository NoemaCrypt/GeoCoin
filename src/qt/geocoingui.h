// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_GEOCOINGUI_H
#define GEOCOIN_QT_GEOCOINGUI_H

#include <QMainWindow>
#include <QLabel>
#include <QProgressBar>

class ClientModel;
class WalletModel;
class NetworkStyle;
class WalletFrame;

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class QLineEdit;
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

    /** Join an in-flight CPU mining stop before core/node destruction. */
    void waitForPendingMiningStop();

protected:
    void closeEvent(QCloseEvent *event) override;

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
    bool statusCpuMiningEnabled = false;
    int statusCpuMiningThreads = 0;
    double statusCpuHashrate = 0.0;
    bool statusGpuMiningEnabled = false;
    double statusGpuHashrate = 0.0;

    // Status bar
    QLabel *labelConnectionsIcon;
    QLabel *labelConnectionsText;
    QLabel *labelBlocksIcon;
    QLabel *labelBlocksText;
    QProgressBar *progressBar;
    QLabel *progressBarLabel;
    QLabel *labelHashrateStatus;

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
    void createStatusBar();
    void refreshMiningStatusBar();
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
