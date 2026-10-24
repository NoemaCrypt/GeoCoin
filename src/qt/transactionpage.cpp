// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "transactionpage.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "transactiontablemodel.h"
#include "guiutil.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableView>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QMessageBox>

#include <algorithm>

namespace {
class TxFilterProxyModel : public QSortFilterProxyModel
{
public:
    explicit TxFilterProxyModel(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
    }

    void setSearchText(const QString& text)
    {
        searchText_ = text.trimmed().toLower();
        invalidateFilter();
    }

    void setTypeFilter(const QString& typeId)
    {
        typeFilter_ = typeId;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        const auto* model = sourceModel();
        if (!model) {
            return false;
        }

        const QModelIndex statusIndex = model->index(sourceRow, TransactionTableModel::Status, sourceParent);
        const QModelIndex dateIndex = model->index(sourceRow, TransactionTableModel::Date, sourceParent);
        const QModelIndex typeIndex = model->index(sourceRow, TransactionTableModel::Type, sourceParent);
        const QModelIndex addressIndex = model->index(sourceRow, TransactionTableModel::Address, sourceParent);
        const QModelIndex amountIndex = model->index(sourceRow, TransactionTableModel::Amount, sourceParent);

        if (typeFilter_ == QStringLiteral("received")) {
            if (statusIndex.data(TransactionTableModel::TypeRole).toInt() != 0) {
                return false;
            }
        } else if (typeFilter_ == QStringLiteral("sent")) {
            if (statusIndex.data(TransactionTableModel::TypeRole).toInt() != 1) {
                return false;
            }
        } else if (typeFilter_ == QStringLiteral("mined")) {
            if (statusIndex.data(TransactionTableModel::TypeRole).toInt() != 3) {
                return false;
            }
        } else if (typeFilter_ == QStringLiteral("nft")) {
            const QString typeText = typeIndex.data(Qt::DisplayRole).toString();
            if (!typeText.contains(QStringLiteral("NFT"), Qt::CaseInsensitive)) {
                return false;
            }
        }

        if (searchText_.isEmpty()) {
            return true;
        }

        const QString blob = QStringLiteral("%1 %2 %3 %4 %5")
                                 .arg(dateIndex.data(Qt::DisplayRole).toString())
                                 .arg(typeIndex.data(Qt::DisplayRole).toString())
                                 .arg(addressIndex.data(Qt::DisplayRole).toString())
                                 .arg(amountIndex.data(Qt::DisplayRole).toString())
                                 .arg(statusIndex.data(TransactionTableModel::TxHashRole).toString())
                                 .toLower();
        return blob.contains(searchText_);
    }

private:
    QString searchText_;
    QString typeFilter_ = QStringLiteral("all");
};

class AddressElideDelegate : public QStyledItemDelegate
{
public:
    explicit AddressElideDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString fullAddress = index.data(TransactionTableModel::FullAddressRole).toString();
        if (!fullAddress.isEmpty()) {
            const int width = std::max(0, opt.rect.width() - 12);
            opt.text = opt.fontMetrics.horizontalAdvance(fullAddress) <= width
                ? fullAddress
                : opt.fontMetrics.elidedText(fullAddress, Qt::ElideMiddle, width);
        }
        QStyledItemDelegate::paint(painter, opt, index);
    }
};
} // namespace

TransactionPage::TransactionPage(QWidget *parent) :
    QWidget(parent),
    clientModel(nullptr),
    walletModel(nullptr),
    transactionTableModel(nullptr),
    transactionFilterModel(nullptr),
    transactionView(nullptr),
    labelNoTransactions(nullptr),
    searchEdit(nullptr),
    typeFilter(nullptr)
{
    setupUi();
}

TransactionPage::~TransactionPage()
{
}

void TransactionPage::setupUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setSpacing(20);

    // Title
    QLabel *titleLabel = new QLabel(tr("Transaction History"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    QHBoxLayout* filterLayout = new QHBoxLayout();
    searchEdit = new QLineEdit(this);
    searchEdit->setPlaceholderText(tr("Search by address, txid, amount..."));
    filterLayout->addWidget(searchEdit);

    typeFilter = new QComboBox(this);
    typeFilter->addItem(tr("All"), QStringLiteral("all"));
    typeFilter->addItem(tr("Received"), QStringLiteral("received"));
    typeFilter->addItem(tr("Sent"), QStringLiteral("sent"));
    typeFilter->addItem(tr("Mined"), QStringLiteral("mined"));
    typeFilter->addItem(tr("NFT"), QStringLiteral("nft"));
    filterLayout->addWidget(typeFilter);
    layout->addLayout(filterLayout);

    // Transaction table
    transactionView = new QTableView(this);
    transactionView->setAlternatingRowColors(true);
    transactionView->setSelectionBehavior(QAbstractItemView::SelectRows);
    transactionView->setSelectionMode(QAbstractItemView::SingleSelection);
    transactionView->setSortingEnabled(true);
    transactionView->verticalHeader()->hide();
    transactionView->horizontalHeader()->setStretchLastSection(true);
    transactionView->setItemDelegateForColumn(TransactionTableModel::Address, new AddressElideDelegate(transactionView));
    connect(transactionView->horizontalHeader(), &QHeaderView::sectionResized, this, [this](int logicalIndex) {
        if (logicalIndex == TransactionTableModel::Address && transactionView) {
            transactionView->viewport()->update();
        }
    });
    layout->addWidget(transactionView);

    // No transactions label
    labelNoTransactions = new QLabel(tr("No transactions yet"), this);
    labelNoTransactions->setAlignment(Qt::AlignCenter);
    labelNoTransactions->setStyleSheet("QLabel { color: gray; font-size: 14px; }");
    labelNoTransactions->setVisible(true);
    layout->addWidget(labelNoTransactions);

    connect(searchEdit, &QLineEdit::textChanged, this, &TransactionPage::updateFilter);
    connect(typeFilter, qOverload<int>(&QComboBox::currentIndexChanged), this, &TransactionPage::updateFilter);
    connect(transactionView, &QTableView::doubleClicked, this, &TransactionPage::showTransactionDetails);
    applyTheme();
}

void TransactionPage::applyTheme()
{
    const QString theme = GUIUtil::currentTheme();
    setStyleSheet(QStringLiteral("TransactionPage { background-color: transparent; }")
                  + GUIUtil::glassPanelStyleSheet(theme)
                  + GUIUtil::glassInputStyleSheet(theme)
                  + GUIUtil::glassListStyleSheet(theme));
    if (labelNoTransactions) {
        labelNoTransactions->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; font-weight: 700; }")
            .arg(GUIUtil::isDarkTheme(theme) ? QStringLiteral("#B8C0CC") : QStringLiteral("#5D625F")));
    }
}

void TransactionPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
}

void TransactionPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;

    if (transactionTableModel) {
        disconnect(transactionTableModel, nullptr, this, nullptr);
    }

    if (model) {
        transactionTableModel = model->getTransactionTableModel();
        if (!transactionFilterModel) {
            transactionFilterModel = new TxFilterProxyModel(this);
            transactionFilterModel->setDynamicSortFilter(true);
        }
        transactionFilterModel->setSourceModel(transactionTableModel);
        transactionView->setModel(transactionFilterModel);

        // Set column widths
        transactionView->setColumnWidth(0, 80);  // Status
        transactionView->setColumnWidth(1, 150); // Date
        transactionView->setColumnWidth(2, 100); // Type
        transactionView->setColumnWidth(3, 300); // Address
        // Amount column stretches

        if (transactionTableModel) {
            connect(transactionTableModel, &QAbstractItemModel::rowsInserted,
                    this, &TransactionPage::updateTransactionVisibility);
            connect(transactionTableModel, &QAbstractItemModel::rowsRemoved,
                    this, &TransactionPage::updateTransactionVisibility);
            connect(transactionTableModel, &QAbstractItemModel::modelReset,
                    this, &TransactionPage::updateTransactionVisibility);
        }
        updateFilter();
        updateTransactionVisibility();
    } else {
        if (transactionFilterModel) {
            transactionFilterModel->setSourceModel(nullptr);
        }
        transactionView->setModel(nullptr);
        updateTransactionVisibility();
    }
}

void TransactionPage::updateTransactionVisibility()
{
    if (!transactionFilterModel) {
        labelNoTransactions->setVisible(true);
        transactionView->setVisible(false);
        return;
    }

    bool hasTransactions = transactionFilterModel->rowCount(QModelIndex()) > 0;
    labelNoTransactions->setVisible(!hasTransactions);
    transactionView->setVisible(hasTransactions);
}

void TransactionPage::updateFilter()
{
    auto* proxy = static_cast<TxFilterProxyModel*>(transactionFilterModel);
    if (!proxy) {
        return;
    }

    proxy->setSearchText(searchEdit ? searchEdit->text() : QString());
    const QString typeId = typeFilter
        ? typeFilter->currentData().toString()
        : QStringLiteral("all");
    proxy->setTypeFilter(typeId);
    updateTransactionVisibility();
}

void TransactionPage::showTransactionDetails(const QModelIndex& index)
{
    if (!index.isValid() || !transactionFilterModel) {
        return;
    }

    const QModelIndex rowIndex = transactionFilterModel->index(index.row(), TransactionTableModel::Status);
    const QString txHash = rowIndex.data(TransactionTableModel::TxHashRole).toString();
    const QString date = transactionFilterModel->index(index.row(), TransactionTableModel::Date).data(Qt::DisplayRole).toString();
    const QString type = transactionFilterModel->index(index.row(), TransactionTableModel::Type).data(Qt::DisplayRole).toString();
    const QString address = transactionFilterModel->index(index.row(), TransactionTableModel::Address).data(TransactionTableModel::FullAddressRole).toString();
    const QString amount = transactionFilterModel->index(index.row(), TransactionTableModel::Amount).data(Qt::DisplayRole).toString();
    const QString status = transactionFilterModel->index(index.row(), TransactionTableModel::Status).data(Qt::DisplayRole).toString();

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::NoIcon,
        tr("Transaction Details"),
        tr("Status: %1\nDate: %2\nType: %3\nAddress: %4\nAmount: %5\nTxID: %6")
            .arg(status, date, type, address, amount, txHash)
    );
}
