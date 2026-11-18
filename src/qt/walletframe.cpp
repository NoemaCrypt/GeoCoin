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
#include "guiutil.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QTimer>

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
    walletStack->setObjectName(QStringLiteral("walletStack"));
    walletStack->setStyleSheet("QStackedWidget { background-color: transparent; }");
    layout->addWidget(walletStack);
    buildPages();
}

void WalletFrame::buildPages()
{
    overviewPage = new OverviewPage(this);
    addPage(overviewPage, QStringLiteral("overview"));

    // Real pages
    sendCoinsPage = new SendCoinsPage(this);
    addPage(sendCoinsPage, QStringLiteral("send"));

    receiveCoinsPage = new ReceiveCoinsPage(this);
    addPage(receiveCoinsPage, QStringLiteral("receive"));

    transactionPage = new TransactionPage(this);
    addPage(transactionPage, QStringLiteral("history"));

    miningPage = new MiningPage(this);
    connect(miningPage, &MiningPage::gpuMiningChanged,
            this, &WalletFrame::gpuMiningChanged);
    connect(miningPage, &MiningPage::gpuMiningStopped,
            this, &WalletFrame::gpuMiningStopped);
    connect(miningPage, &MiningPage::gpuMiningStopped, this, &WalletFrame::finishLanguageRebuild);
    addPage(miningPage, QStringLiteral("mining"));

    multisigPage = new MultisigPage(this);
    addPage(multisigPage, QStringLiteral("multisig"));

    corporatePage = new CorporatePage(this);
    addPage(corporatePage, QStringLiteral("corporate"));

    nftPage = new NFTPage(this);
    addPage(nftPage, QStringLiteral("nft"));

    setCurrentPage(overviewPage);
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

void WalletFrame::addPage(QWidget* page, const QString& id)
{
    QScrollArea* scroll = GUIUtil::createPageScrollArea(page, walletStack);
    scroll->setObjectName(QStringLiteral("pageScroll_") + id);
    pageScrollAreas.insert(page, scroll);
    walletStack->addWidget(scroll);
}

QScrollArea* WalletFrame::scrollAreaForPage(QWidget* page) const
{
    return pageScrollAreas.value(page, nullptr);
}

void WalletFrame::setCurrentPage(QWidget* page)
{
    if (QScrollArea* scroll = scrollAreaForPage(page)) {
        walletStack->setCurrentWidget(scroll);
    }
}

void WalletFrame::rebuildForLanguageChange()
{
    if (!walletStack || languageRebuildPending_) return;
    languageRebuildPending_ = true;
    languageRebuildIndex_ = walletStack->currentIndex();
    walletStack->setEnabled(false);
    if (!languageRebuildTimer_) {
        languageRebuildTimer_ = new QTimer(this);
        languageRebuildTimer_->setInterval(25);
        connect(languageRebuildTimer_, &QTimer::timeout, this, &WalletFrame::finishLanguageRebuild);
    }
    languageRebuildTimer_->start();
    stopGpuMining();
    finishLanguageRebuild();
}

void WalletFrame::cancelPendingLanguageRebuild()
{
    languageRebuildPending_ = false;
    if (languageRebuildTimer_) languageRebuildTimer_->stop();
}

void WalletFrame::finishLanguageRebuild()
{
    if (!languageRebuildPending_ || isGpuMiningActive() || hasPendingMiningAction()) return;
    const int index = languageRebuildIndex_;
    cancelPendingLanguageRebuild();
    pageScrollAreas.clear();
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
    walletStack->setEnabled(true);
#ifdef GEOCOIN_NATIVE_MACOS
    emit nativePagesRebuilt();
#endif
}

WalletFrame::~WalletFrame()
{
    cancelPendingLanguageRebuild();
    for (MiningPage* page : findChildren<MiningPage*>()) {
        disconnect(page, nullptr, this, nullptr);
    }
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
           walletStack->count() == 8 &&
           pageScrollAreas.size() == 8;
}

int WalletFrame::pageCount() const
{
    return walletStack ? walletStack->count() : 0;
}

#ifdef GEOCOIN_NATIVE_MACOS
QScrollArea* WalletFrame::nativeCurrentScrollArea() const
{
    return qobject_cast<QScrollArea*>(walletStack->currentWidget());
}
#endif

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

bool WalletFrame::hasPendingMiningAction() const
{
    return miningPage && miningPage->hasPendingMiningAction();
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
    setCurrentPage(overviewPage);
}

void WalletFrame::gotoSendCoinsPage()
{
    setCurrentPage(sendCoinsPage);
}

void WalletFrame::gotoReceiveCoinsPage()
{
    setCurrentPage(receiveCoinsPage);
}

void WalletFrame::gotoHistoryPage()
{
    setCurrentPage(transactionPage);
}

void WalletFrame::gotoMiningPage()
{
    setCurrentPage(miningPage);
}

void WalletFrame::gotoMultisigPage()
{
    setCurrentPage(multisigPage);
}

void WalletFrame::gotoCorporatePage()
{
    setCurrentPage(corporatePage);
}

void WalletFrame::gotoNftPage()
{
    setCurrentPage(nftPage);
}
