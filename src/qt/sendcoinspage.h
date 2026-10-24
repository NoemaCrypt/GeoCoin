// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_SENDCOINSPAGE_H
#define GEOCOIN_QT_SENDCOINSPAGE_H

#include <QWidget>

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLineEdit;
class QPushButton;
class QLabel;
QT_END_NAMESPACE

/** Send coins page widget */
class SendCoinsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SendCoinsPage(QWidget *parent = nullptr);
    ~SendCoinsPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    void applyTheme();

private:
    ClientModel *clientModel;
    WalletModel *walletModel;

    QLineEdit *addressEdit;
    QLineEdit *amountEdit;
    QLineEdit *feeEdit;
    QLabel *labelBalance;
    QLabel *labelFundsNote;
    QLabel *infoLabel;
    QPushButton *buttonSend;
    QPushButton *buttonClear;
    bool feeManuallyEdited;

    void setupUi();
    bool validateAddress(const QString &address);
    bool validateAmount(const QString &amount);
    void updateFeeSuggestion();
    void refreshWalletState();
    qint64 parseAmount(const QString &amount) const;
    void showStyledMessageBox(int icon, const QString &title, const QString &text);

private slots:
    void sendClicked();
    void clearClicked();
    void updateBalance(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance);
    void amountChanged(const QString &text);
    void feeEdited(const QString &text);
};

#endif // GEOCOIN_QT_SENDCOINSPAGE_H
