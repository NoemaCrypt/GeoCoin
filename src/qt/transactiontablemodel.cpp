// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "transactiontablemodel.h"
#include "walletmodel.h"
#include "guiutil.h"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/types.hpp"
#include "geocoin/crypto/geo_area.hpp"

#include <QIcon>
#include <QColor>
#include <QHash>

TransactionTableModel::TransactionTableModel(WalletModel *parent) :
    QAbstractTableModel(parent),
    walletModel(parent)
{
    // Load initial transactions
    refreshWallet();
}

TransactionTableModel::~TransactionTableModel()
{
}

int TransactionTableModel::confirmationCount(geocoin::Height blockHeight,
                                              geocoin::Height chainHeight,
                                              bool isCoinbase)
{
    if (blockHeight == 0) {
        // Height zero is the wallet's mempool sentinel for ordinary
        // transactions. The only confirmed height-zero transaction is the
        // genesis coinbase.
        return isCoinbase ? 1 : 0;
    }
    if (blockHeight > chainHeight) {
        return 0;
    }
    return static_cast<int>(chainHeight - blockHeight + 1);
}

void TransactionTableModel::refreshWallet()
{
    if (!walletModel || !walletModel->getWallet()) {
        return;
    }

    QHash<QString, TransactionRecord> cachedByHash;
    cachedByHash.reserve(transactions.size());
    for (const auto& cached : transactions) {
        cachedByHash.insert(cached.txHash, cached);
    }

    // ChainState нужен, чтобы восстановить адрес отправителя для входящих транзакций.
    // В UTXO-модели адрес отправителя хранится в предыдущих выходах (prevout), поэтому
    // без доступа к UTXO set мы не можем корректно заполнить колонку "Address".
    auto chainState = walletModel->getChainState();

    beginResetModel();
    transactions.clear();

    // Get transactions from wallet
    auto wallet = walletModel->getWallet();
    auto txs = wallet->get_transactions("", 0); // All transactions
    transactions.reserve(static_cast<qsizetype>(txs.size()));

    for (const auto& wtx : txs) {
        TransactionRecord rec;
        // Convert Hash256 (array) to bytes (vector) for bytes_to_hex
        geocoin::bytes hash_bytes(wtx.tx_hash.begin(), wtx.tx_hash.end());
        rec.txHash = QString::fromStdString(geocoin::crypto::bytes_to_hex(hash_bytes));
        rec.dateTime = QDateTime::fromSecsSinceEpoch(wtx.timestamp);
        rec.amount = wtx.amount;
        rec.fee = wtx.fee;
        rec.confirmations = confirmationCount(
            wtx.block_height,
            chainState ? chainState->get_height() : 0,
            wtx.is_coinbase);
        rec.txType = wtx.tx.type;

        // Determine type
        if (wtx.is_coinbase) {
            rec.type = 3; // Coinbase
            rec.address = tr("Mining Reward");
            rec.addressResolved = true;
        } else if (wtx.amount > 0) {
            rec.type = 0; // Received

            // Повторно используем уже найденный адрес, чтобы не делать дорогой lookup
            // по цепочке при каждом обновлении модели.
            const auto cachedIt = cachedByHash.constFind(rec.txHash);
            if (cachedIt != cachedByHash.constEnd() && cachedIt->addressResolved) {
                rec.address = cachedIt->address;
                rec.addressResolved = true;
            } else {
                // Адрес отправителя пытаемся восстановить через первый input:
                // input -> (prev_tx_hash, prev_vout) -> UTXO -> address.
                // Ограничение: при множественных inputs отправителей может быть несколько.
                // Для UI достаточно показать первый обнаруженный адрес.
                rec.address = tr("Unknown");
                if (chainState && !wtx.tx.inputs.empty()) {
                    const auto& in0 = wtx.tx.inputs[0];
                    geocoin::state::UTXO utxo;
                    if (chainState->get_utxo(in0.prev_tx_hash, in0.vout, utxo)) {
                        rec.address = QString::fromStdString(utxo.address.str());
                        rec.addressResolved = true;
                    } else {
                        // Если UTXO уже потрачен, он может отсутствовать в текущем UTXO set.
                        // Тогда пробуем восстановить адрес через саму предыдущую транзакцию,
                        // прочитав адрес из её выхода (vout).
                        std::unique_ptr<geocoin::core::Transaction> prevTx = chainState->get_transaction(in0.prev_tx_hash);
                        if (prevTx && in0.vout < prevTx->outputs.size()) {
                            rec.address = QString::fromStdString(prevTx->outputs[in0.vout].address);
                            rec.addressResolved = true;
                        }
                    }
                }
            }
        } else {
            rec.type = 1; // Sent
            // For NFT transactions, outputs may contain only change back to sender.
            // Prefer payload address fields for a meaningful "Address" column.
            if (wtx.tx.type == geocoin::core::TransactionType::NFT_TRANSFER && wtx.tx.nft_transfer.has_value()) {
                rec.address = QString::fromStdString(wtx.tx.nft_transfer->to.str());
            } else if (wtx.tx.type == geocoin::core::TransactionType::NFT_BURN && wtx.tx.nft_burn.has_value()) {
                rec.address = QString::fromStdString(wtx.tx.nft_burn->owner.str());
            } else if (!wtx.tx.outputs.empty()) {
                rec.address = QString::fromStdString(wtx.tx.outputs[0].address);
            }
            rec.addressResolved = true;
        }

        rec.status = formatTxStatus(rec);
        transactions.append(rec);
    }

    endResetModel();
}

void TransactionTableModel::updateTransactions()
{
    refreshWallet();
}

bool TransactionTableModel::hasPendingTransactions() const
{
    for (const auto& tx : transactions) {
        if (tx.confirmations == 0) {
            return true;
        }
    }
    return false;
}

int TransactionTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return transactions.size();
}

int TransactionTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 5; // Status, Date, Type, Address, Amount
}

QVariant TransactionTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= transactions.size()) {
        return QVariant();
    }

    const TransactionRecord &rec = transactions.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Status:
            return rec.status;
        case Date:
            return rec.dateTime.toString("yyyy-MM-dd hh:mm");
        case Type:
            if (rec.txType == geocoin::core::TransactionType::NFT_CREATE) {
                return tr("NFT Create");
            }
            if (rec.txType == geocoin::core::TransactionType::NFT_TRANSFER) {
                return tr("NFT Transfer");
            }
            if (rec.txType == geocoin::core::TransactionType::NFT_BURN) {
                return tr("NFT Burn");
            }
            return formatTxType(rec.type);
        case Address:
            return rec.address;
        case Amount:
            return GUIUtil::formatAmountWithUnit(rec.amount, 0);
        }
        break;

    case Qt::TextAlignmentRole:
        switch (index.column()) {
        case Amount:
            return (int)(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return (int)(Qt::AlignLeft | Qt::AlignVCenter);
        }
        break;

    case Qt::ForegroundRole:
        // Color code amounts
        if (index.column() == Amount) {
            if (rec.amount < 0) {
                return QColor(GUIUtil::Theme::ColorRed);
            } else if (rec.amount > 0) {
                return QColor(GUIUtil::Theme::ColorGreen);
            }
        }
        break;

    case Qt::FontRole:
        if (index.column() == Address) {
            return GUIUtil::geocoinAddressFont();
        }
        break;

    case TypeRole:
        return rec.type;

    case DateRole:
        return rec.dateTime;

    case AmountRole:
        return (qint64)rec.amount;

    case TxHashRole:
        return rec.txHash;

    case FullAddressRole:
        return rec.address;
    }

    return QVariant();
}

QVariant TransactionTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case Status:
            return tr("Status");
        case Date:
            return tr("Date");
        case Type:
            return tr("Type");
        case Address:
            return tr("Address");
        case Amount:
            return tr("Amount");
        }
    }
    return QVariant();
}

QString TransactionTableModel::formatTxStatus(const TransactionRecord &rec) const
{
    if (rec.confirmations == 0) {
        return tr("Pending");
    } else if (rec.confirmations < 6) {
        return tr("%1 conf").arg(rec.confirmations);
    } else {
        return tr("Confirmed");
    }
}

QString TransactionTableModel::formatTxType(int type) const
{
    switch (type) {
    case 0: return tr("Received");
    case 1: return tr("Sent");
    case 2: return tr("Self");
    case 3: return tr("Mined");
    default: return tr("Unknown");
    }
}
