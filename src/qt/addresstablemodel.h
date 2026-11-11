// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_ADDRESSTABLEMODEL_H
#define GEOCOIN_QT_ADDRESSTABLEMODEL_H

#include <QAbstractTableModel>
#include <QStringList>

class WalletModel;

/** UI model for the address book */
class AddressTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum ColumnIndex {
        Label = 0,
        Address = 1
    };

    enum RoleIndex {
        TypeRole = Qt::UserRole,
        AddressRole
    };

    explicit AddressTableModel(WalletModel *parent = nullptr);
    ~AddressTableModel();

    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

public Q_SLOTS:
    void updateAddresses();

private:
    WalletModel *walletModel;

    struct AddressRecord {
        QString label;
        QString address;
        bool used;
    };

    QList<AddressRecord> addresses;

    void refreshWallet();
};

#endif // GEOCOIN_QT_ADDRESSTABLEMODEL_H
