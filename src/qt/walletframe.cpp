// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "walletframe.h"
#include "overviewpage.h"
#include "sendcoinspage.h"
#include "receivecoinspage.h"
#include "transactionpage.h"
#include "miningpage.h"
#include "multisigpage.h"
#include "corporatepage.h"
#include "nftpage.h"
#include "clientmodel.h"
#include "walletmodel.h"

#include <QVBoxLayout>
#include <QLabel>

WalletFrame::WalletFrame(QWidget *parent) :
    QFrame(parent),
    clientModel(nullptr),
    walletModel(nullptr),
    walletStack(nullptr),
    overviewPage(nullptr),
    sendCoinsPage(nullptr),
    receiveCoinsPage(nullptr),
    transactionPage(nullptr),
    miningPage(nullptr),
    multisigPage(nullptr),
    corporatePage(nullptr),
    nftPage(nullptr)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    walletStack = new QStackedWidget(this);
    walletStack->setStyleSheet("QStackedWidget { background-color: transparent; }");
    layout->addWidget(walletStack);
    buildPages();
}

void WalletFrame::buildPages()
{
    overviewPage = new OverviewPage(this);
    walletStack->addWidget(overviewPage);

    // Real pages
    sendCoinsPage = new SendCoinsPage(this);
    walletStack->addWidget(sendCoinsPage);

    receiveCoinsPage = new ReceiveCoinsPage(this);
    walletStack->addWidget(receiveCoinsPage);

    transactionPage = new TransactionPage(this);
    walletStack->addWidget(transactionPage);

    miningPage = new MiningPage(this);
    connect(miningPage, &MiningPage::gpuMiningChanged,
            this, &WalletFrame::gpuMiningChanged);
    walletStack->addWidget(miningPage);

    multisigPage = new MultisigPage(this);
    walletStack->addWidget(multisigPage);

    corporatePage = new CorporatePage(this);
    walletStack->addWidget(corporatePage);

    nftPage = new NFTPage(this);
    walletStack->addWidget(nftPage);

    walletStack->setCurrentWidget(overviewPage);
    applyTheme();
    if (clientModel) {
        ClientModel* model = clientModel;
        clientModel = nullptr;
        setClientModel(model);
    }
    if (walletModel) {
        WalletModel* model = walletModel;
        walletModel = nullptr;
        setWalletModel(model);
    }
}

void WalletFrame::rebuildForLanguageChange()
{
    if (!walletStack) {
        return;
    }
    const int index = walletStack->currentIndex();
    stopGpuMining();
    waitForPendingMiningStop();
    while (walletStack->count() > 0) {
        QWidget* page = walletStack->widget(0);
        walletStack->removeWidget(page);
        page->deleteLater();
    }
    overviewPage = nullptr;
    sendCoinsPage = nullptr;
    receiveCoinsPage = nullptr;
    transactionPage = nullptr;
    miningPage = nullptr;
    multisigPage = nullptr;
    corporatePage = nullptr;
    nftPage = nullptr;
    buildPages();
    if (index >= 0 && index < walletStack->count()) {
        walletStack->setCurrentIndex(index);
    }
}

WalletFrame::~WalletFrame()
{
}

void WalletFrame::setClientModel(ClientModel *model)
{
    if (clientModel == model) {
        return;
    }
    this->clientModel = model;

    if (overviewPage) {
        overviewPage->setClientModel(model);
    }
    if (sendCoinsPage) {
        sendCoinsPage->setClientModel(model);
    }
    if (receiveCoinsPage) {
        receiveCoinsPage->setClientModel(model);
    }
    if (transactionPage) {
        transactionPage->setClientModel(model);
    }
    if (miningPage) {
        miningPage->setClientModel(model);
    }
    if (multisigPage) {
        multisigPage->setClientModel(model);
    }
    if (corporatePage) {
        corporatePage->setClientModel(model);
    }
    if (nftPage) {
        nftPage->setClientModel(model);
    }
}

void WalletFrame::setWalletModel(WalletModel *model)
{
    if (walletModel == model) {
        return;
    }
    this->walletModel = model;

    if (overviewPage) {
        overviewPage->setWalletModel(model);
        // Connect signals for page switching
        connect(overviewPage, &OverviewPage::sendCoinsRequested, this, &WalletFrame::gotoSendCoinsPage, Qt::UniqueConnection);
        connect(overviewPage, &OverviewPage::receiveCoinsRequested, this, &WalletFrame::gotoReceiveCoinsPage, Qt::UniqueConnection);
    }
    if (sendCoinsPage) {
        sendCoinsPage->setWalletModel(model);
    }
    if (receiveCoinsPage) {
        receiveCoinsPage->setWalletModel(model);
    }
    if (transactionPage) {
        transactionPage->setWalletModel(model);
    }
    if (miningPage) {
        miningPage->setWalletModel(model);
    }
    if (multisigPage) {
        multisigPage->setWalletModel(model);
    }
    if (corporatePage) {
        corporatePage->setWalletModel(model);
    }
    if (nftPage) {
        nftPage->setWalletModel(model);
    }
}

bool WalletFrame::hasRequiredPages() const
{
    return walletStack != nullptr &&
           overviewPage != nullptr &&
           sendCoinsPage != nullptr &&
           receiveCoinsPage != nullptr &&
           transactionPage != nullptr &&
           miningPage != nullptr &&
           multisigPage != nullptr &&
           corporatePage != nullptr &&
           nftPage != nullptr &&
           walletStack->count() >= 8;
}

int WalletFrame::pageCount() const
{
    return walletStack ? walletStack->count() : 0;
}

bool WalletFrame::isGpuMiningActive() const
{
    return miningPage && miningPage->isGpuMiningActive();
}

void WalletFrame::stopGpuMining()
{
    if (miningPage) {
        miningPage->stopGpuMining();
    }
}

void WalletFrame::waitForPendingMiningStop()
{
    if (miningPage) {
        miningPage->waitForPendingMiningStop();
    }
}

void WalletFrame::applyTheme()
{
    setStyleSheet("WalletFrame { background-color: transparent; }");
    if (overviewPage) {
        overviewPage->applyTheme();
    }
    if (sendCoinsPage) {
        sendCoinsPage->applyTheme();
    }
    if (receiveCoinsPage) {
        receiveCoinsPage->applyTheme();
    }
    if (transactionPage) {
        transactionPage->applyTheme();
    }
    if (miningPage) {
        miningPage->applyTheme();
    }
    if (multisigPage) {
        multisigPage->applyTheme();
    }
    if (corporatePage) {
        corporatePage->applyTheme();
    }
    if (nftPage) {
        nftPage->applyTheme();
    }
}

void WalletFrame::gotoOverviewPage()
{
    walletStack->setCurrentWidget(overviewPage);
}

void WalletFrame::gotoSendCoinsPage()
{
    walletStack->setCurrentWidget(sendCoinsPage);
}

void WalletFrame::gotoReceiveCoinsPage()
{
    walletStack->setCurrentWidget(receiveCoinsPage);
}

void WalletFrame::gotoHistoryPage()
{
    walletStack->setCurrentWidget(transactionPage);
}

void WalletFrame::gotoMiningPage()
{
    walletStack->setCurrentWidget(miningPage);
}

void WalletFrame::gotoMultisigPage()
{
    walletStack->setCurrentWidget(multisigPage);
}

void WalletFrame::gotoCorporatePage()
{
    walletStack->setCurrentWidget(corporatePage);
}

void WalletFrame::gotoNftPage()
{
    walletStack->setCurrentWidget(nftPage);
}
