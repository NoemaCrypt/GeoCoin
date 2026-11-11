// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "addresstablemodel.h"
#include "walletmodel.h"
#include "guiutil.h"
#include "geocoin/wallet/wallet.hpp"

#include <QFont>
#include <QColor>

AddressTableModel::AddressTableModel(WalletModel *parent) :
    QAbstractTableModel(parent),
    walletModel(parent)
{
    // Load initial addresses
    refreshWallet();
}

AddressTableModel::~AddressTableModel()
{
}

void AddressTableModel::refreshWallet()
{
    if (!walletModel || !walletModel->getWallet()) {
        return;
    }

    beginResetModel();
    addresses.clear();

    // Get all addresses from wallet
    auto wallet = walletModel->getWallet();
    auto addrs = wallet->get_all_addresses();

    for (const auto& addr : addrs) {
        geocoin::wallet::KeyInfo info;
        if (wallet->get_key_info(addr, info)) {
            AddressRecord rec;
            rec.label = QString::fromStdString(info.label);
            rec.address = QString::fromStdString(addr);
            rec.used = info.used;

            addresses.append(rec);
        }
    }

    endResetModel();
}

void AddressTableModel::updateAddresses()
{
    refreshWallet();
}

int AddressTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return addresses.size();
}

int AddressTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 2; // Label, Address
}

QVariant AddressTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= addresses.size()) {
        return QVariant();
    }

    const AddressRecord &rec = addresses.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Label:
            return rec.label.isEmpty() ? tr("(no label)") : rec.label;
        case Address:
            return rec.address;
        }
        break;

    case Qt::TextAlignmentRole:
        return (int)(Qt::AlignLeft | Qt::AlignVCenter);
        break;

    case Qt::FontRole:
        if (index.column() == Address) {
            return GUIUtil::geocoinAddressFont();
        }
        break;

    case Qt::ForegroundRole:
        if (rec.used) {
            return QColor(GUIUtil::Theme::ColorGray);
        }
        break;

    case AddressRole:
        return rec.address;

    }

    return QVariant();
}

QVariant AddressTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case Label:
            return tr("Label");
        case Address:
            return tr("Address");
        }
    }
    return QVariant();
}
