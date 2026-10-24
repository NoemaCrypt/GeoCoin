#include "test_utils.hpp"
#include "addresstablemodel.h"
#include "guiutil.h"
#include "corporatepage.h"
#include "miningpage.h"
#include "multisigpage.h"
#include "nftpage.h"
#include "overviewpage.h"
#include "receivecoinspage.h"
#include "sendcoinspage.h"
#include "transactionpage.h"
#include "transactiontablemodel.h"
#include "walletmodel.h"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/wallet/address_funds.hpp"

#include <QApplication>
#include <QFile>
#include <QTemporaryFile>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>

#include <iostream>
#include <limits>
#include <vector>

namespace {

class DialogLog {
public:
    explicit DialogLog(const QString& path) : path_(path) {}

    void clear()
    {
        QFile file(path_);
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    }

    QString text() const
    {
        QFile file(path_);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }

private:
    QString path_;
};

QPushButton* findButton(QWidget& page, const QString& text)
{
    for (QPushButton* button : page.findChildren<QPushButton*>()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

QLabel* findLabelContaining(QWidget& page, const QString& fragment)
{
    for (QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text().contains(fragment)) {
            return label;
        }
    }
    return nullptr;
}

QLineEdit* findLineEdit(QWidget& page, const QString& placeholderFragment)
{
    for (QLineEdit* edit : page.findChildren<QLineEdit*>()) {
        if (edit->placeholderText().contains(placeholderFragment)) {
            return edit;
        }
    }
    return nullptr;
}

void clickButton(QWidget& page, const QString& text)
{
    QPushButton* button = findButton(page, text);
    geocoin::test::expect(button != nullptr, "button exists: " + text.toStdString());
    button->click();
}

void checkAmountAndConfirmationRules()
{
    qint64 parsed = -1;
    qint64 added = 0;
    geocoin::test::expect(GUIUtil::parseAmount(QStringLiteral("0"), parsed) && parsed == 0, "parse zero");
    geocoin::test::expect(GUIUtil::parseAmount(QStringLiteral("0.00000001"), parsed) && parsed == 1, "parse one angle");
    geocoin::test::expect(
        GUIUtil::parseAmount(QStringLiteral("1.00000001"), parsed) && parsed == 100000001,
        "parse one coin plus one angle");
    geocoin::test::expect(!GUIUtil::parseAmount(QStringLiteral("-1"), parsed), "reject a negative amount");
    geocoin::test::expect(!GUIUtil::parseAmount(QStringLiteral("1e-8"), parsed), "reject scientific notation");
    geocoin::test::expect(
        GUIUtil::formatAmount(std::numeric_limits<qint64>::max()) == QStringLiteral("92233720368.54775807"),
        "format the maximum amount");
    geocoin::test::expect(GUIUtil::addAmounts(1, 2, added) && added == 3, "add amounts");
    geocoin::test::expect(!GUIUtil::addAmounts(std::numeric_limits<qint64>::max(), 1, added), "reject amount overflow");

    geocoin::test::expect(TransactionTableModel::confirmationCount(0, 100, false) == 0, "mempool tx has no confirmations");
    geocoin::test::expect(TransactionTableModel::confirmationCount(0, 100, true) == 1, "genesis coinbase is confirmed");
    geocoin::test::expect(TransactionTableModel::confirmationCount(95, 100, false) == 6, "confirmation depth counts the block itself");
}

void checkSpendableOutputsMatchTheWalletRule()
{
    geocoin::state::UTXO utxo;
    utxo.value = 1;
    utxo.height = 0;
    geocoin::test::expect(geocoin::wallet::utxo_is_spendable(utxo, 0), "a confirmed non-coinbase output is spendable");

    utxo.is_coinbase = true;
    geocoin::test::expect(!geocoin::wallet::utxo_is_spendable(utxo, 0), "a fresh coinbase is immature");
    geocoin::test::expect(
        geocoin::wallet::utxo_is_spendable(utxo, geocoin::consensus::params::COINBASE_MATURITY - 1),
        "a coinbase becomes spendable at maturity");

    utxo.value = 0;
    geocoin::test::expect(!geocoin::wallet::utxo_is_spendable(utxo, 1000), "a zero-value output is not spendable");

    utxo.value = 1;
    utxo.is_coinbase = false;
    utxo.height = 5;
    geocoin::test::expect(!geocoin::wallet::utxo_is_spendable(utxo, 4), "an output above the chain tip is not spendable");

    geocoin::core::Transaction pending;
    geocoin::core::TxInput input;
    input.vout = 3;
    pending.inputs.push_back(input);
    const geocoin::wallet::PendingSpendIndex reserved(std::span<const geocoin::core::Transaction>(&pending, 1));
    geocoin::test::expect(reserved.contains(input.prev_tx_hash, 3), "a pending input reserves its outpoint");
    geocoin::test::expect(!reserved.contains(input.prev_tx_hash, 4), "a different output stays free");
}

void checkRelayFees()
{
    const qint64 standard = WalletModel::standardRelayFee(nullptr);
    const qint64 nft = WalletModel::nftRelayFee(nullptr);
    const qint64 smallMultisig = WalletModel::multisigRelayFee(1, nullptr);
    const qint64 largeMultisig = WalletModel::multisigRelayFee(5, nullptr);
    geocoin::test::expect(standard > 0, "a standard payment has a relay fee");
    geocoin::test::expect(nft >= standard, "an NFT fee covers at least a standard payment");
    geocoin::test::expect(smallMultisig >= standard, "a multisig fee covers at least a standard payment");
    geocoin::test::expect(largeMultisig > smallMultisig, "more multisig participants raise the fee");
}

void checkPagesFollowTheWallet(WalletModel& model, DialogLog& dialogs)
{
    SendCoinsPage send;
    ReceiveCoinsPage receive;
    OverviewPage overview;
    MiningPage mining;
    TransactionPage history;
    NFTPage nft;
    MultisigPage multisig;
    CorporatePage corporate;

    send.setWalletModel(&model);
    receive.setWalletModel(&model);
    overview.setWalletModel(&model);
    mining.setWalletModel(&model);
    history.setWalletModel(&model);
    nft.setWalletModel(&model);
    multisig.setWalletModel(&model);
    corporate.setWalletModel(&model);

    QPushButton* sendButton = findButton(send, QStringLiteral("Send"));
    geocoin::test::expect(sendButton && !sendButton->isEnabled(), "Send stays disabled until a wallet exists");
    QLabel* sendNote = findLabelContaining(send, QStringLiteral("Load or create a wallet"));
    geocoin::test::expect(sendNote && !sendNote->isHidden(), "the send page explains that a wallet is required");
    QLineEdit* amountEdit = findLineEdit(send, QStringLiteral("0.00000000"));
    geocoin::test::expect(amountEdit != nullptr, "the amount field exists");
    amountEdit->setText(QStringLiteral("1"));
    geocoin::test::expect(findButton(send, QStringLiteral("Send"))->isEnabled() == false, "typing an amount does not enable Send");

    geocoin::test::expect(findLabelContaining(overview, QStringLiteral("No wallet is loaded")) != nullptr,
                          "the overview tells the user to load a wallet");
    QPushButton* overviewCopy = findButton(overview, QStringLiteral("Copy"));
    geocoin::test::expect(overviewCopy && !overviewCopy->isEnabled(), "overview copy waits for an address");

    QPushButton* receiveCopy = findButton(receive, QStringLiteral("Copy"));
    geocoin::test::expect(receiveCopy && !receiveCopy->isEnabled(), "receive copy waits for an address");
    geocoin::test::expect(findLineEdit(receive, QStringLiteral("No wallet loaded")) != nullptr,
                          "receive shows that no wallet is loaded");

    QPushButton* startMining = findButton(mining, QStringLiteral("Start Mining"));
    geocoin::test::expect(startMining && !startMining->isEnabled(), "mining stays off without a wallet");
    geocoin::test::expect(findLabelContaining(mining, QStringLiteral("No wallet loaded")) != nullptr,
                          "mining names the missing wallet");

    geocoin::test::expect(findLabelContaining(history, QStringLiteral("No transactions yet")) != nullptr,
                          "history starts empty");

    QPushButton* createDraft = findButton(multisig, QStringLiteral("Create draft"));
    geocoin::test::expect(createDraft && !createDraft->isEnabled(), "a multisig draft waits for a configuration");
    geocoin::test::expect(findLabelContaining(corporate, QStringLiteral("Corporate management")) != nullptr,
                          "the corporate page is present");
    QPushButton* founderDraft = findButton(corporate, QStringLiteral("Create draft"));
    geocoin::test::expect(founderDraft && !founderDraft->isEnabled(),
                          "a corporate draft waits for founders and directors");

    dialogs.clear();
    clickButton(nft, QStringLiteral("Create NFT / Cadastr"));
    geocoin::test::expect(dialogs.text().contains(QStringLiteral("load or create a wallet")),
                          "NFT creation asks for a wallet");

    QTemporaryDir directory;
    geocoin::test::expect(directory.isValid(), "wallet directory");
    model.setDataDirectory(directory.path());
    const QString password = QStringLiteral("gui-contract");
    geocoin::test::expect(model.createWallet(model.getWalletFilePath(), password), "create the wallet used by the pages");
    const QString address = model.getAddress();
    geocoin::test::expect(!address.isEmpty(), "created wallet has an address");

    geocoin::test::expect(sendButton->isEnabled(), "Send enables after the wallet is created");
    QLabel* overviewHint = findLabelContaining(overview, QStringLiteral("No wallet is loaded"));
    geocoin::test::expect(overviewHint && overviewHint->isHidden(),
                          "the empty-wallet hint hides after creation");
    QLineEdit* overviewAddress = nullptr;
    for (QLineEdit* edit : overview.findChildren<QLineEdit*>()) {
        if (edit->text() == address) {
            overviewAddress = edit;
        }
    }
    geocoin::test::expect(overviewAddress != nullptr, "overview shows the wallet address");
    geocoin::test::expect(overviewCopy->isEnabled(), "overview copy enables with the address");

    QLineEdit* receiveAddress = nullptr;
    for (QLineEdit* edit : receive.findChildren<QLineEdit*>()) {
        if (edit->text() == address) {
            receiveAddress = edit;
        }
    }
    geocoin::test::expect(receiveAddress != nullptr, "receive shows the wallet address");
    geocoin::test::expect(receiveCopy->isEnabled(), "receive copy enables with the address");

    geocoin::test::expect(findLabelContaining(mining, address) != nullptr, "mining shows the wallet address");
    geocoin::test::expect(startMining->isEnabled(), "mining can start after the wallet is created");

    QLineEdit* payTo = findLineEdit(send, QStringLiteral("GEO"));
    QLineEdit* feeEdit = nullptr;
    for (QLineEdit* edit : send.findChildren<QLineEdit*>()) {
        if (edit != payTo && edit != amountEdit && !edit->text().isEmpty()) {
            feeEdit = edit;
        }
    }
    geocoin::test::expect(payTo && feeEdit, "send form has recipient and fee fields");
    qint64 shownFee = 0;
    geocoin::test::expect(GUIUtil::parseAmount(feeEdit->text(), shownFee) && shownFee >= WalletModel::standardRelayFee(nullptr),
                          "the suggested fee meets the relay minimum");

    dialogs.clear();
    payTo->setText(QStringLiteral("not-an-address"));
    amountEdit->setText(QStringLiteral("1"));
    clickButton(send, QStringLiteral("Send"));
    geocoin::test::expect(dialogs.text().contains(QStringLiteral("not a valid GeoCoin address")),
                          "Send rejects an invalid address");

    dialogs.clear();
    payTo->setText(address);
    feeEdit->setText(QStringLiteral("0.00000001"));
    clickButton(send, QStringLiteral("Send"));
    geocoin::test::expect(dialogs.text().contains(QStringLiteral("relay minimum")),
                          "Send rejects a fee below the relay minimum");

    dialogs.clear();
    amountEdit->setText(QStringLiteral("1"));
    feeEdit->setText(GUIUtil::formatAmount(WalletModel::standardRelayFee(nullptr)));
    clickButton(send, QStringLiteral("Send"));
    geocoin::test::expect(dialogs.text().contains(QStringLiteral("Insufficient balance")),
                          "Send rejects an amount the wallet cannot pay");

    model.closeWallet();
    geocoin::test::expect(!sendButton->isEnabled(), "closing the wallet disables Send");
    geocoin::test::expect(findLabelContaining(send, QStringLiteral("Load or create a wallet")) &&
                              !findLabelContaining(send, QStringLiteral("Load or create a wallet"))->isHidden(),
                          "the send page returns to the empty-wallet note");
    geocoin::test::expect(!startMining->isEnabled(), "closing the wallet disables mining");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setOrganizationName(QStringLiteral("GeoCoinGuiContractTest"));
    QCoreApplication::setApplicationName(QStringLiteral("GeoCoinGuiContractTest"));
    QApplication application(argc, argv);
    QTemporaryDir dialogDirectory;
    geocoin::test::expect(dialogDirectory.isValid(), "dialog directory");
    const QString dialogPath = dialogDirectory.filePath(QStringLiteral("dialogs.log"));
    qputenv("GEOCOIN_GUI_TEST_AUTOCLOSE", dialogPath.toUtf8());
    DialogLog dialogs(dialogPath);

    try {
        checkAmountAndConfirmationRules();
        checkSpendableOutputsMatchTheWalletRule();
        checkRelayFees();
        WalletModel model;
        checkPagesFollowTheWallet(model, dialogs);
        std::cout << "GUI contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
