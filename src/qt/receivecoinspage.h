// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_RECEIVECOINSPAGE_H
#define GEOCOIN_QT_RECEIVECOINSPAGE_H

#include <QWidget>

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLineEdit;
class QPushButton;
class QLabel;
QT_END_NAMESPACE

/** Receive coins page widget */
class ReceiveCoinsPage : public QWidget
{
    Q_OBJECT

public:
    explicit ReceiveCoinsPage(QWidget *parent = nullptr);
    ~ReceiveCoinsPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    void applyTheme();

private:
    ClientModel *clientModel;
    WalletModel *walletModel;

    QLineEdit *addressEdit;
    QLabel *labelQR;
    QLabel *infoLabel;
    QPushButton *buttonCopy;

    void setupUi();
    void updateAddress();
    void generateQRCode(const QString &address);
    void showStyledMessageBox(int icon, const QString &title, const QString &text);

private slots:
    void copyAddress();
};

#endif // GEOCOIN_QT_RECEIVECOINSPAGE_H
