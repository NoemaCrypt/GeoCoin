// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_TRANSACTIONTABLEMODEL_H
#define GEOCOIN_QT_TRANSACTIONTABLEMODEL_H

#include <QAbstractTableModel>
#include <QList>
#include <QDateTime>

#include "geocoin/core/transaction.hpp"

class WalletModel;

/** UI model for the transaction table of a wallet */
class TransactionTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum ColumnIndex {
        Status = 0,
        Date = 1,
        Type = 2,
        Address = 3,
        Amount = 4
    };

    enum RoleIndex {
        TypeRole = Qt::UserRole,
        DateRole,
        AmountRole,
        TxHashRole,
        FullAddressRole
    };

    explicit TransactionTableModel(WalletModel *parent = nullptr);
    ~TransactionTableModel();

    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    bool hasPendingTransactions() const;
    static int confirmationCount(geocoin::Height blockHeight,
                                 geocoin::Height chainHeight,
                                 bool isCoinbase);

public Q_SLOTS:
    void updateTransactions();

private:
    WalletModel *walletModel;

    struct TransactionRecord {
        QString txHash;
        QDateTime dateTime;
        int type; // 0=received, 1=sent, 2=self, 3=coinbase
        geocoin::core::TransactionType txType; // authoritative transaction type
        QString address;
        qint64 amount;
        qint64 fee;
        int confirmations;
        QString status;
        bool addressResolved = false;
    };

    QList<TransactionRecord> transactions;

    void refreshWallet();
    QString formatTxStatus(const TransactionRecord &rec) const;
    QString formatTxType(int type) const;
};

#endif // GEOCOIN_QT_TRANSACTIONTABLEMODEL_H
