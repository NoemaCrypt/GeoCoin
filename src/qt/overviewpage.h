// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_OVERVIEWPAGE_H
#define GEOCOIN_QT_OVERVIEWPAGE_H

#include <QWidget>

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;
QT_END_NAMESPACE

/** Overview ("home") page widget */
class OverviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit OverviewPage(QWidget *parent = nullptr);
    ~OverviewPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    void applyTheme();

public slots:
    void setBalance(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance);
    void setWatchOnlyBalance(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance);
    void setNumBlocks(int count);
    void setNumConnections(int count);

private:
    ClientModel *clientModel;
    WalletModel *walletModel;

    QLabel *labelWalletHint;
    QLineEdit *addressEdit;
    QPushButton *buttonCopyAddress;
    QWidget *watchOnlyBox;

    // Balance labels
    QLabel *labelBalance;
    QLabel *labelUnconfirmed;
    QLabel *labelImmature;
    QLabel *labelTotal;

    // Watch-only (например, мультисиг без приватных ключей)
    QLabel *labelWatchOnlyBalance;
    QLabel *labelWatchOnlyUnconfirmed;
    QLabel *labelWatchOnlyImmature;
    QLabel *labelWatchOnlyTotal;

    // Network info
    QLabel *labelBlocks;
    QLabel *labelConnections;

    // Quick actions
    QPushButton *buttonSend;
    QPushButton *buttonReceive;

    void setupUi();
    void refreshWalletIdentity();
    bool checkedTotal(qint64 balance, qint64 unconfirmed, qint64 immature, qint64& total) const;

private slots:
    void handleSendCoinsClicked();
    void handleReceiveCoinsClicked();

signals:
    void sendCoinsRequested();
    void receiveCoinsRequested();
};

#endif // GEOCOIN_QT_OVERVIEWPAGE_H
