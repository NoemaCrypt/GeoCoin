// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_NFTPAGE_H
#define GEOCOIN_QT_NFTPAGE_H

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QMetaObject>

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QTableWidget;
class QTextEdit;
QT_END_NAMESPACE

class NFTPage : public QWidget
{
    Q_OBJECT

public:
    explicit NFTPage(QWidget *parent = nullptr);
    ~NFTPage() override;

    void setClientModel(ClientModel *model);
    void setWalletModel(WalletModel *model);
    void applyTheme();

private:
    ClientModel *clientModel;
    WalletModel *walletModel;
    QMetaObject::Connection transactionCreatedConnection_;
    QMetaObject::Connection walletStateConnection_;

    QString lastCreatedTxid_;

    struct PendingNft {
        QString nftIdHex;
        QString name;
        QString description;
        QString imageUri;
        QString contentHash;
    };

    QVector<QString> rowNftIds_;
    QHash<QString, PendingNft> pendingById_;

    QLabel *labelWalletAddress;
    QPushButton *buttonRefresh;
    QPushButton *buttonCreate;
    QPushButton *buttonTransfer;
    QPushButton *buttonBurn;
    QTableWidget *tableNfts;

    QLabel *labelNftId;
    QLabel *labelOwner;
    QLabel *labelName;
    QTextEdit *textDescription;
    QLabel *labelImageUri;
    QLabel *labelContentHash;

    void setupUi();
    void clearDetails();

private slots:
    void refresh();
    void handleSelectionChanged();
    void createNftClicked();
    void transferNftClicked();
    void burnNftClicked();
};

#endif // GEOCOIN_QT_NFTPAGE_H
