// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_WALLETFRAME_H
#define GEOCOIN_QT_WALLETFRAME_H

#include <QFrame>
#include <QHash>
#include <QStackedWidget>

class QScrollArea;
class ClientModel;
class WalletModel;
class OverviewPage;
class SendCoinsPage;
class ReceiveCoinsPage;
class TransactionPage;
class MiningPage;
class QTimer;
class MultisigPage;
class CorporatePage;
class NFTPage;

/** A container for embedding all wallet-related controls */
class WalletFrame : public QFrame
{
    Q_OBJECT

public:
    explicit WalletFrame(QWidget *parent = nullptr);
    ~WalletFrame();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    bool hasRequiredPages() const;
    int pageCount() const;
    bool isGpuMiningActive() const;
    bool hasPendingMiningAction() const;
    void stopGpuMining();
    void waitForPendingMiningStop();
    void applyTheme();
    void rebuildForLanguageChange();
    void cancelPendingLanguageRebuild();

signals:
    void gpuMiningChanged(bool enabled, double hashrate, int acceptedBlocks, int rejectedBlocks);
    void gpuMiningStopped();

public slots:
    void gotoOverviewPage();
    void gotoSendCoinsPage();
    void gotoReceiveCoinsPage();
    void gotoHistoryPage();
    void gotoMiningPage();
    void gotoMultisigPage();
    void gotoCorporatePage();
    void gotoNftPage();

private:
    void buildPages();
    void addPage(QWidget* page, const QString& id);
    QScrollArea* scrollAreaForPage(QWidget* page) const;
    void setCurrentPage(QWidget* page);
    void finishLanguageRebuild();
    QTimer* languageRebuildTimer_ = nullptr;
    bool languageRebuildPending_ = false;
    int languageRebuildIndex_ = 0;

    ClientModel *clientModel;
    WalletModel *walletModel;

    QStackedWidget *walletStack;
    QHash<QWidget*, QScrollArea*> pageScrollAreas;

    // Pages
    OverviewPage *overviewPage;
    SendCoinsPage *sendCoinsPage;
    ReceiveCoinsPage *receiveCoinsPage;
    TransactionPage *transactionPage;
    MiningPage *miningPage;
    MultisigPage *multisigPage;
    CorporatePage *corporatePage;
    NFTPage *nftPage;
};

#endif // GEOCOIN_QT_WALLETFRAME_H
