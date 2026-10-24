// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_TRANSACTIONPAGE_H
#define GEOCOIN_QT_TRANSACTIONPAGE_H

#include <QWidget>

class ClientModel;
class WalletModel;
class TransactionTableModel;
class QSortFilterProxyModel;

QT_BEGIN_NAMESPACE
class QTableView;
class QLabel;
class QLineEdit;
class QComboBox;
QT_END_NAMESPACE

/** Transaction history page widget */
class TransactionPage : public QWidget
{
    Q_OBJECT

public:
    explicit TransactionPage(QWidget *parent = nullptr);
    ~TransactionPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    void applyTheme();

private:
    ClientModel *clientModel;
    WalletModel *walletModel;
    TransactionTableModel *transactionTableModel;
    QSortFilterProxyModel *transactionFilterModel;

    QTableView *transactionView;
    QLabel *labelNoTransactions;
    QLineEdit *searchEdit;
    QComboBox *typeFilter;

    void setupUi();
    void updateFilter();
    void showTransactionDetails(const QModelIndex& index);

private Q_SLOTS:
    void updateTransactionVisibility();
};

#endif // GEOCOIN_QT_TRANSACTIONPAGE_H
