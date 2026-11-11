// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "sendcoinspage.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "guiutil.h"
#include "geocoin/types.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/consensus/consensus.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QRegularExpression>
#include <QInputDialog>
#include <QStringList>

SendCoinsPage::SendCoinsPage(QWidget *parent) :
    QWidget(parent),
    clientModel(nullptr),
    walletModel(nullptr),
    addressEdit(nullptr),
    amountEdit(nullptr),
    feeEdit(nullptr),
    labelBalance(nullptr),
    labelFundsNote(nullptr),
    infoLabel(nullptr),
    buttonSend(nullptr),
    buttonClear(nullptr),
    feeManuallyEdited(false)
{
    setupUi();
}

SendCoinsPage::~SendCoinsPage()
{
}

void SendCoinsPage::setupUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setSpacing(20);

    // Title
    QLabel *titleLabel = new QLabel(tr("Send Coins"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    // Balance display
    QHBoxLayout *balanceLayout = new QHBoxLayout();
    balanceLayout->addWidget(new QLabel(tr("Available Balance:"), this));
    labelBalance = new QLabel(tr("0.00000000 GC"), this);
    labelBalance->setStyleSheet("QLabel { font-weight: bold; color: " + GUIUtil::Theme::ColorBlue + "; }");
    balanceLayout->addWidget(labelBalance);
    balanceLayout->addStretch();
    layout->addLayout(balanceLayout);
    labelFundsNote = new QLabel(tr("Load or create a wallet before sending."), this);
    labelFundsNote->setWordWrap(true);
    layout->addWidget(labelFundsNote);

    // Send form
    QGroupBox *formGroup = new QGroupBox(tr("Payment Details"), this);
    QVBoxLayout *formLayout = new QVBoxLayout(formGroup);

    // Pay To
    QHBoxLayout *addressLayout = new QHBoxLayout();
    QLabel *addressLabel = new QLabel(tr("Pay To:"), this);
    addressLabel->setMinimumWidth(100);
    addressLayout->addWidget(addressLabel);
    addressEdit = new QLineEdit(this);
    addressEdit->setPlaceholderText(tr("Enter a GeoCoin address (GEO... or GEOM...)"));
    addressEdit->setFont(GUIUtil::geocoinAddressFont());
    addressLayout->addWidget(addressEdit);
    formLayout->addLayout(addressLayout);

    // Amount
    QHBoxLayout *amountLayout = new QHBoxLayout();
    QLabel *amountLabel = new QLabel(tr("Amount:"), this);
    amountLabel->setMinimumWidth(100);
    amountLayout->addWidget(amountLabel);
    amountEdit = new QLineEdit(this);
    amountEdit->setPlaceholderText(tr("0.00000000"));
    amountLayout->addWidget(amountEdit);
    connect(amountEdit, &QLineEdit::textChanged, this, &SendCoinsPage::amountChanged);
    amountLayout->addWidget(new QLabel(tr("GC"), this));
    formLayout->addLayout(amountLayout);

    // Fee
    QHBoxLayout *feeLayout = new QHBoxLayout();
    QLabel *feeLabel = new QLabel(tr("Fee:"), this);
    feeLabel->setMinimumWidth(100);
    feeLayout->addWidget(feeLabel);
    feeEdit = new QLineEdit(this);
    QString defaultFee = GUIUtil::formatAmount(WalletModel::standardRelayFee(nullptr));
    feeEdit->setPlaceholderText(defaultFee);
    feeEdit->setToolTip(tr(
        "Automatic size-based fee estimate. A manual value applies only to this form."));
    connect(feeEdit, &QLineEdit::textEdited, this, &SendCoinsPage::feeEdited);
    feeEdit->setText(defaultFee);
    feeManuallyEdited = false;
    feeLayout->addWidget(feeEdit);
    feeLayout->addWidget(new QLabel(tr("GC"), this));
    formLayout->addLayout(feeLayout);

    layout->addWidget(formGroup);

    // Buttons
    QHBoxLayout *buttonsLayout = new QHBoxLayout();

    buttonClear = new QPushButton(tr("Clear"), this);
    connect(buttonClear, &QPushButton::clicked, this, &SendCoinsPage::clearClicked);
    buttonsLayout->addWidget(buttonClear);

    buttonsLayout->addStretch();

    buttonSend = new QPushButton(tr("Send"), this);
    buttonSend->setMinimumHeight(40);
    buttonSend->setMinimumWidth(120);
    buttonSend->setStyleSheet(
        "QPushButton { "
        "  background-color: " + GUIUtil::Theme::ColorBlue + "; "
        "  color: white; "
        "  font-weight: bold; "
        "  border-radius: 5px; "
        "} "
        "QPushButton:hover { "
        "  background-color: #5570DB; "
        "}"
        "QPushButton:disabled { "
        "  background-color: #8A93A8; "
        "  color: #F4F6F8; "
        "}"
    );
    buttonSend->setEnabled(false);
    connect(buttonSend, &QPushButton::clicked, this, &SendCoinsPage::sendClicked);
    buttonsLayout->addWidget(buttonSend);

    layout->addLayout(buttonsLayout);

    // Info text
    infoLabel = new QLabel(
        tr("<b>Note:</b> Transactions are irreversible. "
           "Please verify the recipient address carefully before sending."),
        this
    );
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet(GUIUtil::warningLabelStyleSheet(GUIUtil::currentTheme()));
    layout->addWidget(infoLabel);

    layout->addStretch();
    applyTheme();
}

void SendCoinsPage::applyTheme()
{
    const QString theme = GUIUtil::currentTheme();
    setStyleSheet(QStringLiteral("SendCoinsPage { background-color: transparent; }")
                  + GUIUtil::glassPanelStyleSheet(theme)
                  + GUIUtil::glassInputStyleSheet(theme)
                  + GUIUtil::glassListStyleSheet(theme)
                  + GUIUtil::spinBoxStyleSheet(theme));
    if (infoLabel) {
        infoLabel->setStyleSheet(GUIUtil::warningLabelStyleSheet(theme));
    }
}

void SendCoinsPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
}

void SendCoinsPage::setWalletModel(WalletModel *model)
{
    if (walletModel == model) {
        return;
    }
    if (walletModel) {
        disconnect(walletModel, nullptr, this, nullptr);
    }

    this->walletModel = model;

    if (model) {
        connect(model, &WalletModel::balanceChanged, this, &SendCoinsPage::updateBalance, Qt::UniqueConnection);
        connect(model, &WalletModel::walletStateChanged, this, &SendCoinsPage::refreshWalletState, Qt::UniqueConnection);
    }
    refreshWalletState();
}

bool SendCoinsPage::validateAddress(const QString &address)
{
    try {
        geocoin::Address addr(address.toStdString());
        return geocoin::crypto::verify_address(addr);
    } catch (const std::exception&) {
        return false;
    }
}

bool SendCoinsPage::validateAmount(const QString &amount)
{
    qint64 value = 0;
    return GUIUtil::parseAmount(amount, value) && value > 0;
}

void SendCoinsPage::showStyledMessageBox(int icon, const QString &title, const QString &text)
{
    GUIUtil::showStyledMessageBox(this, static_cast<QMessageBox::Icon>(icon), title, text);
}

void SendCoinsPage::sendClicked()
{
    if (!walletModel || !walletModel->isWalletLoaded()) {
        showStyledMessageBox(2, tr("Send Coins"),
            tr("Please load or create a wallet first."));
        return;
    }

    QString address = addressEdit->text().trimmed();
    QString amountStr = amountEdit->text().trimmed();
    QString feeStr = feeEdit->text().trimmed();

    // Validate address
    if (!validateAddress(address)) {
        showStyledMessageBox(2, tr("Send Coins"),
            tr("The recipient address is not a valid GeoCoin address."));
        addressEdit->setFocus();
        return;
    }

    // Validate amount
    if (!validateAmount(amountStr)) {
        showStyledMessageBox(2, tr("Send Coins"),
            tr("Invalid amount. Please enter a valid number."));
        amountEdit->setFocus();
        return;
    }

    // Validate fee
    if (!validateAmount(feeStr)) {
        showStyledMessageBox(2, tr("Send Coins"),
            tr("Invalid fee. Please enter a valid number."));
        feeEdit->setFocus();
        return;
    }

    // Convert to angles (1 GC = 100,000,000 angles)
    qint64 amount = parseAmount(amountStr);
    qint64 fee = parseAmount(feeStr);

    if (amount <= 0) {
        showStyledMessageBox(2, tr("Send Coins"), tr("Invalid amount."));
        amountEdit->setFocus();
        return;
    }

    if (fee <= 0) {
        showStyledMessageBox(2, tr("Send Coins"), tr("Invalid fee."));
        feeEdit->setFocus();
        return;
    }

    const qint64 requiredFee = walletModel->estimateFee(amount);
    if (requiredFee <= 0 || fee < requiredFee) {
        showStyledMessageBox(
            2,
            tr("Send Coins"),
            tr("Fee is below the current relay minimum. Enter at least %1 GC.")
                .arg(GUIUtil::formatAmount(
                    qMax(requiredFee, walletModel->standardRelayFee())))
        );
        feeEdit->setFocus();
        return;
    }

    qint64 total = 0;
    if (!GUIUtil::addAmounts(amount, fee, total)) {
        showStyledMessageBox(2, tr("Send Coins"), tr("Amount plus fee is too large."));
        return;
    }

    // Check balance
    if (total > walletModel->getBalance()) {
        showStyledMessageBox(2, tr("Send Coins"),
            tr("Insufficient balance. You need %1 GC (including fee) but only have %2 GC available.")
                .arg(GUIUtil::formatAmount(total))
                .arg(GUIUtil::formatAmount(walletModel->getBalance())));
        return;
    }

    // Confirm transaction
    QString confirmMsg = tr("Are you sure you want to send?\n\n"
                           "Amount: %1 GC\n"
                           "Fee: %2 GC\n"
                           "Total: %3 GC\n\n"
                           "To: %4")
        .arg(GUIUtil::formatAmount(amount))
        .arg(GUIUtil::formatAmount(fee))
        .arg(GUIUtil::formatAmount(total))
        .arg(address);

    if (GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Question,
            tr("Confirm Send"),
            confirmMsg,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        ) != QMessageBox::Yes) {
        return;
    }

    // Request password if wallet is encrypted
    QString password;
    GUIUtil::ScopedQStringCleanser passwordCleanser(password);
    if (walletModel->isWalletEncrypted()) {
        // Создаем кастомный QInputDialog с явным стилем
        QInputDialog inputDialog(this);
        inputDialog.setWindowTitle(tr("Wallet Password"));
        inputDialog.setLabelText(tr("Enter wallet password to sign transaction:"));
        inputDialog.setTextEchoMode(QLineEdit::Password);
        inputDialog.setStyleSheet(GUIUtil::dialogStyleSheet(GUIUtil::currentTheme()));

        if (inputDialog.exec() != QDialog::Accepted) {
            return; // User cancelled
        }

        password = inputDialog.textValue();
        inputDialog.setTextValue(QString(password.size(), QChar(0)));
        if (password.isEmpty()) {
            return;
        }

        // Verify password
        if (!walletModel->verifyPassword(password)) {
            showStyledMessageBox(3, tr("Send Coins"),
                tr("Incorrect password. Transaction cancelled."));
            return;
        }
    }

    // Send transaction
    WalletModel::StatusCode status = walletModel->sendCoins(address, amount, fee, password);
    GUIUtil::cleanseQString(password);

    if (status == WalletModel::OK) {
        showStyledMessageBox(1, tr("Send Coins"),
            tr("Transaction sent successfully!"));
        clearClicked();
    } else if (status == WalletModel::WalletSaveFailed) {
        showStyledMessageBox(3, tr("Transaction Sent — Wallet Save Failed"),
            tr("The transaction was already broadcast, but the wallet file was not saved. "
               "Do not send it again. Keep the application open and save the wallet before exiting."));
        clearClicked();
    } else {
        QString errorMsg;
        switch (status) {
            case WalletModel::InvalidAmount:
                errorMsg = tr("Invalid amount");
                break;
            case WalletModel::InvalidAddress:
                errorMsg = tr("Invalid address");
                break;
            case WalletModel::AmountExceedsBalance:
                errorMsg = tr("Amount exceeds balance");
                break;
            case WalletModel::TransactionCreationFailed:
                errorMsg = tr("Transaction creation failed");
                break;
            case WalletModel::TransactionCommitFailed:
                errorMsg = tr("Transaction commit failed");
                break;
            default:
                errorMsg = tr("Unknown error");
        }

        showStyledMessageBox(3, tr("Send Coins"),
            tr("Error sending transaction: %1").arg(errorMsg));
    }
}

void SendCoinsPage::clearClicked()
{
    addressEdit->clear();
    amountEdit->clear();
    feeManuallyEdited = false;
    updateFeeSuggestion();
    addressEdit->setFocus();
}

void SendCoinsPage::refreshWalletState()
{
    const bool loaded = walletModel && walletModel->isWalletLoaded();
    if (buttonSend) {
        buttonSend->setEnabled(loaded);
    }
    if (!loaded) {
        if (labelBalance) {
            labelBalance->setText(tr("0.00000000 GC"));
        }
        if (labelFundsNote) {
            labelFundsNote->setText(tr("Load or create a wallet before sending."));
            labelFundsNote->setVisible(true);
        }
        updateFeeSuggestion();
        return;
    }
    updateBalance(walletModel->getBalance(), walletModel->getUnconfirmedBalance(), walletModel->getImmatureBalance());
}

void SendCoinsPage::updateBalance(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance)
{
    if (!walletModel || !walletModel->isWalletLoaded()) {
        refreshWalletState();
        return;
    }
    labelBalance->setText(GUIUtil::formatAmountWithUnit(balance));
    QStringList notes;
    if (unconfirmedBalance > 0) {
        notes << tr("Pending: %1").arg(GUIUtil::formatAmountWithUnit(unconfirmedBalance));
    }
    if (immatureBalance > 0) {
        notes << tr("Immature: %1").arg(GUIUtil::formatAmountWithUnit(immatureBalance));
    }
    if (labelFundsNote) {
        labelFundsNote->setText(notes.join(QStringLiteral("   ")));
        labelFundsNote->setVisible(!notes.isEmpty());
    }
    if (!feeManuallyEdited) {
        updateFeeSuggestion();
    }
}

void SendCoinsPage::amountChanged(const QString &text)
{
    Q_UNUSED(text);
    if (!feeManuallyEdited) {
        updateFeeSuggestion();
    }
}

void SendCoinsPage::feeEdited(const QString &text)
{
    Q_UNUSED(text);
    feeManuallyEdited = true;
}

qint64 SendCoinsPage::parseAmount(const QString &amount) const
{
    qint64 value = 0;
    return GUIUtil::parseAmount(amount, value) ? value : -1;
}

void SendCoinsPage::updateFeeSuggestion()
{
    if (!walletModel || !walletModel->isWalletLoaded()) {
        const qint64 relayFee = WalletModel::standardRelayFee(
            walletModel ? walletModel->getMempool().get() : nullptr);
        QString minFee = GUIUtil::formatAmount(relayFee);
        feeEdit->setPlaceholderText(minFee);
        if (!feeManuallyEdited) {
            feeEdit->setText(minFee);
        }
        return;
    }

    qint64 amount = parseAmount(amountEdit->text());
    if (amount <= 0) {
        QString minFee = GUIUtil::formatAmount(walletModel->standardRelayFee());
        feeEdit->setPlaceholderText(minFee);
        if (!feeManuallyEdited) {
            feeEdit->setText(minFee);
        }
        return;
    }

    qint64 suggestedFee = walletModel->estimateFee(amount);
    QString feeText = GUIUtil::formatAmount(suggestedFee);

    if (!feeManuallyEdited) {
        feeEdit->setText(feeText);
    } else {
        feeEdit->setPlaceholderText(feeText);
    }
}
