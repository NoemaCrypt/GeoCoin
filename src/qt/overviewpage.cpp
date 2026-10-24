// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "overviewpage.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "guiutil.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QApplication>
#include <QClipboard>
#include <QWidget>

OverviewPage::OverviewPage(QWidget *parent) :
    QWidget(parent),
    clientModel(nullptr),
    walletModel(nullptr),
    labelWalletHint(nullptr),
    addressEdit(nullptr),
    buttonCopyAddress(nullptr),
    watchOnlyBox(nullptr),
    labelBalance(nullptr),
    labelUnconfirmed(nullptr),
    labelImmature(nullptr),
    labelTotal(nullptr),
    labelWatchOnlyBalance(nullptr),
    labelWatchOnlyUnconfirmed(nullptr),
    labelWatchOnlyImmature(nullptr),
    labelWatchOnlyTotal(nullptr),
    labelBlocks(nullptr),
    labelConnections(nullptr),
    buttonSend(nullptr),
    buttonReceive(nullptr)
{
    setupUi();
}

OverviewPage::~OverviewPage()
{
}

void OverviewPage::setupUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setSpacing(20);

    // Title
    QLabel *titleLabel = new QLabel(tr("Overview"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    QGroupBox *walletGroup = new QGroupBox(tr("Wallet"), this);
    QVBoxLayout *walletLayout = new QVBoxLayout(walletGroup);
    labelWalletHint = new QLabel(
        tr("No wallet is loaded. Create or load one from the File menu to receive coins."),
        this);
    labelWalletHint->setWordWrap(true);
    walletLayout->addWidget(labelWalletHint);

    QHBoxLayout *addressLayout = new QHBoxLayout();
    addressEdit = new QLineEdit(this);
    addressEdit->setReadOnly(true);
    addressEdit->setFont(GUIUtil::geocoinAddressFont());
    addressEdit->setPlaceholderText(tr("No wallet loaded"));
    addressLayout->addWidget(addressEdit);
    buttonCopyAddress = new QPushButton(tr("Copy"), this);
    buttonCopyAddress->setEnabled(false);
    connect(buttonCopyAddress, &QPushButton::clicked, this, [this]() {
        if (!addressEdit || addressEdit->text().isEmpty()) {
            return;
        }
        QApplication::clipboard()->setText(addressEdit->text());
    });
    addressLayout->addWidget(buttonCopyAddress);
    walletLayout->addLayout(addressLayout);
    layout->addWidget(walletGroup);

    // Balance group
    QGroupBox *balanceGroup = new QGroupBox(tr("Balances"), this);
    QVBoxLayout *balanceLayout = new QVBoxLayout(balanceGroup);

    // Available balance
    QHBoxLayout *availableLayout = new QHBoxLayout();
    availableLayout->addWidget(new QLabel(tr("Available:"), this));
    labelBalance = new QLabel(tr("0.00000000 GC"), this);
    labelBalance->setStyleSheet("QLabel { font-size: 18px; font-weight: bold; color: " + GUIUtil::Theme::ColorGreen + "; }");
    availableLayout->addWidget(labelBalance);
    availableLayout->addStretch();
    balanceLayout->addLayout(availableLayout);

    // Pending balance
    QHBoxLayout *pendingLayout = new QHBoxLayout();
    pendingLayout->addWidget(new QLabel(tr("Pending:"), this));
    labelUnconfirmed = new QLabel(tr("0.00000000 GC"), this);
    pendingLayout->addWidget(labelUnconfirmed);
    pendingLayout->addStretch();
    balanceLayout->addLayout(pendingLayout);

    // Immature balance
    QHBoxLayout *immatureLayout = new QHBoxLayout();
    immatureLayout->addWidget(new QLabel(tr("Immature:"), this));
    labelImmature = new QLabel(tr("0.00000000 GC"), this);
    immatureLayout->addWidget(labelImmature);
    immatureLayout->addStretch();
    balanceLayout->addLayout(immatureLayout);

    // Total balance
    balanceLayout->addSpacing(10);
    QHBoxLayout *totalLayout = new QHBoxLayout();
    totalLayout->addWidget(new QLabel(tr("Total:"), this));
    labelTotal = new QLabel(tr("0.00000000 GC"), this);
    labelTotal->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; }");
    totalLayout->addWidget(labelTotal);
    totalLayout->addStretch();
    balanceLayout->addLayout(totalLayout);

    // Watch-only balances appear only after a multisig address is tracked.
    balanceLayout->addSpacing(12);
    watchOnlyBox = new QWidget(this);
    QVBoxLayout *watchLayout = new QVBoxLayout(watchOnlyBox);
    watchLayout->setContentsMargins(0, 0, 0, 0);
    QLabel* watchTitle = new QLabel(tr("Watch-only (multisig):"), this);
    watchTitle->setStyleSheet("QLabel { font-weight: bold; color: #666; }");
    watchLayout->addWidget(watchTitle);

    QHBoxLayout *watchAvailLayout = new QHBoxLayout();
    watchAvailLayout->addWidget(new QLabel(tr("Available:"), watchOnlyBox));
    labelWatchOnlyBalance = new QLabel(tr("0.00000000 GC"), this);
    watchAvailLayout->addWidget(labelWatchOnlyBalance);
    watchAvailLayout->addStretch();
    watchLayout->addLayout(watchAvailLayout);

    QHBoxLayout *watchPendingLayout = new QHBoxLayout();
    watchPendingLayout->addWidget(new QLabel(tr("Pending:"), watchOnlyBox));
    labelWatchOnlyUnconfirmed = new QLabel(tr("0.00000000 GC"), this);
    watchPendingLayout->addWidget(labelWatchOnlyUnconfirmed);
    watchPendingLayout->addStretch();
    watchLayout->addLayout(watchPendingLayout);

    QHBoxLayout *watchImmatureLayout = new QHBoxLayout();
    watchImmatureLayout->addWidget(new QLabel(tr("Immature:"), watchOnlyBox));
    labelWatchOnlyImmature = new QLabel(tr("0.00000000 GC"), this);
    watchImmatureLayout->addWidget(labelWatchOnlyImmature);
    watchImmatureLayout->addStretch();
    watchLayout->addLayout(watchImmatureLayout);

    QHBoxLayout *watchTotalLayout = new QHBoxLayout();
    watchTotalLayout->addWidget(new QLabel(tr("Total:"), watchOnlyBox));
    labelWatchOnlyTotal = new QLabel(tr("0.00000000 GC"), this);
    labelWatchOnlyTotal->setStyleSheet("QLabel { font-weight: bold; }");
    watchTotalLayout->addWidget(labelWatchOnlyTotal);
    watchTotalLayout->addStretch();
    watchLayout->addLayout(watchTotalLayout);
    watchOnlyBox->setVisible(false);
    balanceLayout->addWidget(watchOnlyBox);

    layout->addWidget(balanceGroup);

    // Network info group
    QGroupBox *networkGroup = new QGroupBox(tr("Network"), this);
    QVBoxLayout *networkLayout = new QVBoxLayout(networkGroup);

    // Blocks
    QHBoxLayout *blocksLayout = new QHBoxLayout();
    blocksLayout->addWidget(new QLabel(tr("Blocks:"), this));
    labelBlocks = new QLabel(tr("0"), this);
    blocksLayout->addWidget(labelBlocks);
    blocksLayout->addStretch();
    networkLayout->addLayout(blocksLayout);

    // Connections
    QHBoxLayout *connectionsLayout = new QHBoxLayout();
    connectionsLayout->addWidget(new QLabel(tr("Connections:"), this));
    labelConnections = new QLabel(tr("0"), this);
    connectionsLayout->addWidget(labelConnections);
    connectionsLayout->addStretch();
    networkLayout->addLayout(connectionsLayout);

    layout->addWidget(networkGroup);

    // Quick actions
    QHBoxLayout *actionsLayout = new QHBoxLayout();

    buttonSend = new QPushButton(tr("Send Coins"), this);
    buttonSend->setMinimumHeight(40);
    connect(buttonSend, &QPushButton::clicked, this, &OverviewPage::handleSendCoinsClicked);
    actionsLayout->addWidget(buttonSend);

    buttonReceive = new QPushButton(tr("Receive Coins"), this);
    buttonReceive->setMinimumHeight(40);
    connect(buttonReceive, &QPushButton::clicked, this, &OverviewPage::handleReceiveCoinsClicked);
    actionsLayout->addWidget(buttonReceive);

    layout->addLayout(actionsLayout);

    layout->addStretch();
    applyTheme();
}

void OverviewPage::applyTheme()
{
    const QString theme = GUIUtil::currentTheme();
    setStyleSheet(QStringLiteral("OverviewPage { background-color: transparent; }")
                  + GUIUtil::glassPanelStyleSheet(theme)
                  + GUIUtil::glassInputStyleSheet(theme)
                  + GUIUtil::glassListStyleSheet(theme));
}

void OverviewPage::setClientModel(ClientModel *model)
{
    if (clientModel == model) {
        return;
    }
    if (clientModel) {
        disconnect(clientModel, nullptr, this, nullptr);
    }

    this->clientModel = model;

    if (model) {
        connect(model, &ClientModel::numBlocksChanged, this, &OverviewPage::setNumBlocks, Qt::UniqueConnection);
        connect(model, &ClientModel::numConnectionsChanged, this, &OverviewPage::setNumConnections, Qt::UniqueConnection);

        setNumBlocks(model->getNumBlocks());
        setNumConnections(model->getNumConnections());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    if (walletModel == model) {
        return;
    }
    if (walletModel) {
        disconnect(walletModel, nullptr, this, nullptr);
    }

    this->walletModel = model;

    if (model) {
        connect(model, &WalletModel::balanceChanged, this, &OverviewPage::setBalance, Qt::UniqueConnection);
        connect(model, &WalletModel::watchOnlyBalanceChanged, this, &OverviewPage::setWatchOnlyBalance, Qt::UniqueConnection);
        connect(model, &WalletModel::walletStateChanged, this, &OverviewPage::refreshWalletIdentity, Qt::UniqueConnection);

        setBalance(model->getBalance(), model->getUnconfirmedBalance(), model->getImmatureBalance());
        setWatchOnlyBalance(model->getWatchOnlyBalance(), model->getWatchOnlyUnconfirmedBalance(), model->getWatchOnlyImmatureBalance());
    }
    refreshWalletIdentity();
}

void OverviewPage::refreshWalletIdentity()
{
    const bool loaded = walletModel && walletModel->isWalletLoaded();
    const QString address = loaded ? walletModel->getAddress() : QString();
    if (addressEdit) {
        addressEdit->setText(address);
        if (!loaded) {
            addressEdit->setPlaceholderText(tr("No wallet loaded"));
        }
    }
    if (buttonCopyAddress) {
        buttonCopyAddress->setEnabled(!address.isEmpty());
    }
    if (labelWalletHint) {
        labelWalletHint->setVisible(!loaded);
    }
}

bool OverviewPage::checkedTotal(qint64 balance, qint64 unconfirmed, qint64 immature, qint64& total) const
{
    qint64 pending = 0;
    if (!GUIUtil::addAmounts(balance, unconfirmed, pending) ||
        !GUIUtil::addAmounts(pending, immature, total)) {
        total = 0;
        return false;
    }
    return true;
}

void OverviewPage::setBalance(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance)
{
    labelBalance->setText(GUIUtil::formatAmountWithUnit(balance));
    labelUnconfirmed->setText(GUIUtil::formatAmountWithUnit(unconfirmedBalance));
    labelImmature->setText(GUIUtil::formatAmountWithUnit(immatureBalance));

    qint64 total = 0;
    if (!checkedTotal(balance, unconfirmedBalance, immatureBalance, total)) {
        labelTotal->setText(tr("unavailable"));
        return;
    }
    labelTotal->setText(GUIUtil::formatAmountWithUnit(total));
}

void OverviewPage::setWatchOnlyBalance(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance)
{
    labelWatchOnlyBalance->setText(GUIUtil::formatAmountWithUnit(balance));
    labelWatchOnlyUnconfirmed->setText(GUIUtil::formatAmountWithUnit(unconfirmedBalance));
    labelWatchOnlyImmature->setText(GUIUtil::formatAmountWithUnit(immatureBalance));

    qint64 total = 0;
    const bool totalOk = checkedTotal(balance, unconfirmedBalance, immatureBalance, total);
    labelWatchOnlyTotal->setText(totalOk ? GUIUtil::formatAmountWithUnit(total) : tr("unavailable"));
    if (watchOnlyBox) {
        watchOnlyBox->setVisible(balance != 0 || unconfirmedBalance != 0 || immatureBalance != 0);
    }
}

void OverviewPage::setNumBlocks(int count)
{
    labelBlocks->setText(QString::number(count));
}

void OverviewPage::setNumConnections(int count)
{
    labelConnections->setText(QString::number(count));
}

void OverviewPage::handleSendCoinsClicked()
{
    emit sendCoinsRequested();
}

void OverviewPage::handleReceiveCoinsClicked()
{
    emit receiveCoinsRequested();
}
