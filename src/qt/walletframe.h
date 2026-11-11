// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_WALLETFRAME_H
#define GEOCOIN_QT_WALLETFRAME_H

#include <QFrame>
#include <QStackedWidget>

class ClientModel;
class WalletModel;
class OverviewPage;
class SendCoinsPage;
class ReceiveCoinsPage;
class TransactionPage;
class MiningPage;
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
    void stopGpuMining();
    void waitForPendingMiningStop();
    void applyTheme();
    void rebuildForLanguageChange();

signals:
    void gpuMiningChanged(bool enabled, double hashrate, int acceptedBlocks, int rejectedBlocks);

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

    ClientModel *clientModel;
    WalletModel *walletModel;

    QStackedWidget *walletStack;

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
