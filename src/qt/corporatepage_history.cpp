#include "corporatepage_internal.h"
#include "geocoin/network/mempool.hpp"
#include "geocoin/wallet/address_funds.hpp"

void CorporatePage::loadSavedMultisigAddresses()
{
    if (!walletModel_ || !walletModel_->getWallet()) {
        if (::geocoin::qt::debugEnabled()) {
            GEO_QT_LOG_DEBUG << "[CorporatePage] loadSavedMultisigAddresses: wallet not ready";
        }
        return;
    }

    // Очищаем списки
    if (primaryMultisigCombo_) {
        primaryMultisigCombo_->clear();
        primaryMultisigCombo_->addItem(tr("-- Select saved address --"), "");
    }

    if (delegatedMultisigCombo_) {
        delegatedMultisigCombo_->clear();
        delegatedMultisigCombo_->addItem(tr("-- Select saved address --"), "");
    }

    // Загружаем сохраненные адреса из кошелька
    auto configs = walletModel_->getWallet()->get_multisig_addresses();
    if (::geocoin::qt::debugEnabled()) {
        GEO_QT_LOG_DEBUG << "[CorporatePage] Loaded" << configs.size() << "multisig addresses from wallet";
    }

    for (const auto& config : configs) {
        QString address = QString::fromStdString(config.address.str());
        QString displayText = QString("%1-of-%2: %3...")
            .arg(config.m)
            .arg(config.n)
            .arg(address.left(20));

        if (primaryMultisigCombo_) {
            primaryMultisigCombo_->addItem(displayText, address);
        }

        if (delegatedMultisigCombo_) {
            delegatedMultisigCombo_->addItem(displayText, address);
        }

        if (::geocoin::qt::debugEnabled()) {
            GEO_QT_LOG_DEBUG << "[CorporatePage] Added address:" << address.left(30) << "M=" << config.m << "N=" << config.n;
        }
    }

    if (configs.empty()) {
        logInfo(tr("No saved multisig addresses"));
    } else {
        logInfo(tr("Loaded %1 saved addresses").arg(configs.size()));
    }
}

void CorporatePage::loadHistory()
{
    QFile file(historyFilePath_);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (doc.isNull() || !doc.isArray()) {
        logError(tr("Could not load history: %1").arg(error.errorString()));
        return;
    }

    historyEntries_ = doc.array();
    refreshHistoryView();
    logInfo(tr("History loaded: %1 records").arg(historyEntries_.size()));
}

void CorporatePage::saveHistory()
{
    QString error;
    const QJsonDocument doc(historyEntries_);
    if (!GUIUtil::writeFileAtomically(historyFilePath_, doc.toJson(), error)) {
        logError(tr("Could not save history: %1").arg(error));
        return;
    }
}

void CorporatePage::refreshHistoryView()
{
    if (!historyTable_) return;

    historyTable_->setRowCount(0);
    QString filter = historyFilterCombo_ ? historyFilterCombo_->currentData().toString() : "all";

    for (int sourceRow = 0; sourceRow < historyEntries_.size(); ++sourceRow) {
        QJsonObject entry = historyEntries_[sourceRow].toObject();
        QString type = entry["type"].toString();

        if (filter != "all") {
            if (filter == "delegation" && !type.contains("Delegation")) continue;
            if (filter == "hierarchical" && !type.contains("Hierarchical")) continue;
            if (filter == "commitment" && !type.contains("Commitment")) continue;
        }

        int row = historyTable_->rowCount();
        historyTable_->insertRow(row);
        auto* timestampItem = new QTableWidgetItem(entry["timestamp"].toString());
        timestampItem->setData(Qt::UserRole, sourceRow);
        historyTable_->setItem(row, 0, timestampItem);
        historyTable_->setItem(row, 1, new QTableWidgetItem(type));
        historyTable_->setItem(row, 2, new QTableWidgetItem(entry["status"].toString()));
        historyTable_->setItem(row, 3, new QTableWidgetItem(entry["details"].toString()));
    }
}

void CorporatePage::addHistoryEntry(const QString& type, const QString& status, const QString& details, const QString& payload)
{
    QJsonObject entry;
    entry["timestamp"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    entry["type"] = type;
    entry["status"] = status;
    entry["details"] = details;
    entry["payload"] = payload;

    historyEntries_.append(entry);
    saveHistory();
    refreshHistoryView();
}

// ============================================================================
// Helper Methods - Utility
// ============================================================================

QGroupBox* CorporatePage::buildHistoryBox()
{
    QGroupBox *box = new QGroupBox(tr("History operations"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    QHBoxLayout *filterLayout = new QHBoxLayout();
    filterLayout->addWidget(new QLabel(tr("Filter:"), box));
    historyFilterCombo_ = new QComboBox(box);
    historyFilterCombo_->addItem(tr("All"), "all");
    historyFilterCombo_->addItem(tr("Delegation proofs"), "delegation");
    historyFilterCombo_->addItem(tr("Delegated spend"), "hierarchical");
    historyFilterCombo_->addItem(tr("Commitments"), "commitment");
    connect(historyFilterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CorporatePage::onHistoryFilterChanged);
    filterLayout->addWidget(historyFilterCombo_);

    // Кнопка для ручной загрузки истории
    QPushButton *refreshHistoryButton = new QPushButton(tr("🔄 Refresh history"), box);
    refreshHistoryButton->setToolTip(tr("Load history operations from file"));
    connect(refreshHistoryButton, &QPushButton::clicked, this, &CorporatePage::loadHistory);
    filterLayout->addWidget(refreshHistoryButton);

    filterLayout->addStretch();
    layout->addLayout(filterLayout);

    historyTable_ = new QTableWidget(0, 4, box);
    historyTable_->setHorizontalHeaderLabels({tr("Time"), tr("Type"), tr("Status"), tr("Details")});
    historyTable_->horizontalHeader()->setStretchLastSection(true);
    historyTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    historyTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    historyTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyTable_->setMinimumHeight(150);
    historyTable_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(historyTable_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &CorporatePage::onHistorySelectionChanged);
    layout->addWidget(historyTable_);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    historyLoadButton_ = new QPushButton(tr("Load selected"), box);
    historyLoadButton_->setEnabled(false);
    connect(historyLoadButton_, &QPushButton::clicked, this, &CorporatePage::onHistoryLoadClicked);
    buttonLayout->addWidget(historyLoadButton_);

    historyCopyHashButton_ = new QPushButton(tr("Copy hash"), box);
    historyCopyHashButton_->setEnabled(false);
    connect(historyCopyHashButton_, &QPushButton::clicked, this, &CorporatePage::onHistoryCopyHash);
    buttonLayout->addWidget(historyCopyHashButton_);

    historyExportButton_ = new QPushButton(tr("Export CSV"), box);
    connect(historyExportButton_, &QPushButton::clicked, this, &CorporatePage::onExportHistory);
    buttonLayout->addWidget(historyExportButton_);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    return box;
}


// ============================================================================
// Founder Slots
// ============================================================================

void CorporatePage::updatePrimaryBalance()
{
    if (currentPrimaryAddress_.isEmpty() || !walletModel_) {
        primaryBalanceLabel_->setText(tr("Balance: —"));
        updateDelegationAllowance();
        return;
    }

    // Получаем ChainState из WalletModel
    auto chainState = walletModel_->getChainState();
    if (!chainState) {
        primaryBalanceLabel_->setText(tr("Balance: Unavailable"));
        updateDelegationAllowance();
        return;
    }

    try {
        const geocoin::Address addr(currentPrimaryAddress_.toStdString());
        const auto funds = geocoin::wallet::classify_address_funds(
            *chainState, addr, walletModel_->pendingTransactions());
        if (!funds.valid) {
            primaryBalanceLabel_->setText(tr("Balance: invalid data"));
            if (availableBalanceLabel_) {
                availableBalanceLabel_->setText(tr("Error"));
            }
            static bool accountingErrorLogged = false;
            if (!accountingErrorLogged) {
                logError(tr("Corporate balance was not calculated: negative value or overflow."));
                accountingErrorLogged = true;
            }
            updateDelegationAllowance();
            return;
        }

        QString balanceText = tr("Balance: %1 GC").arg(GUIUtil::formatAmount(funds.confirmed));
        if (funds.unconfirmed > 0) {
            balanceText += tr(" (+%1 unconfirmed.)").arg(GUIUtil::formatAmount(funds.unconfirmed));
        }
        if (funds.immature > 0) {
            balanceText += tr(" (+%1 immature.)").arg(GUIUtil::formatAmount(funds.immature));
        }
        primaryBalanceLabel_->setText(balanceText);
        if (availableBalanceLabel_) {
            availableBalanceLabel_->setText(tr("%1 GC").arg(GUIUtil::formatAmount(funds.available)));
        }

        // Также обновляем отображение лимитов/делегированного баланса.
        updateDelegationAllowance();

    } catch (const std::exception& ex) {
        primaryBalanceLabel_->setText(tr("Balance: Error"));
        if (availableBalanceLabel_) availableBalanceLabel_->setText(tr("Error"));
        logError(tr("Error receiving balance: %1").arg(ex.what()));
        updateDelegationAllowance();
    }
}

void CorporatePage::onHistoryFilterChanged(int index)
{
    Q_UNUSED(index);
    refreshHistoryView();
}

void CorporatePage::onHistorySelectionChanged()
{
    bool hasSelection = historyTable_->selectionModel()->hasSelection();
    historyLoadButton_->setEnabled(hasSelection);
    if (historyCopyHashButton_) historyCopyHashButton_->setEnabled(hasSelection);
}

void CorporatePage::onHistoryCopyHash()
{
    int row = historyTable_->currentRow();
    QTableWidgetItem* sourceItem = row >= 0 ? historyTable_->item(row, 0) : nullptr;
    bool sourceOk = false;
    const int sourceRow = sourceItem ? sourceItem->data(Qt::UserRole).toInt(&sourceOk) : -1;
    if (!sourceOk || sourceRow < 0 || sourceRow >= historyEntries_.size()) return;

    QJsonObject entry = historyEntries_[sourceRow].toObject();
    QString type = entry["type"].toString();
    QString payload = entry["payload"].toString().trimmed();

    QString hash;

    // Если payload уже является hash (например CreateAndSign сохраняет delegation_hash)
    if (isHexTxid(payload)) {
        hash = payload;
    } else if (!payload.isEmpty()) {
        // Пробуем распарсить JSON черновика
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8(), &parseError);
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();
            hash = obj.value("delegation_hash").toString().trimmed();
            if (hash.isEmpty()) {
                hash = obj.value("spending_hash").toString().trimmed();
            }
            if (hash.isEmpty()) {
                hash = obj.value("spending_tx_hash").toString().trimmed();
            }
        }
    }

    if (hash.isEmpty()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("No hash"),
            tr("In selected records (%1) not found delegation/spending hash for copying.").arg(type));
        return;
    }

    QApplication::clipboard()->setText(hash);
    logInfo(tr("Hash copied in buffer exchange: %1...").arg(hash.left(16)));
}

void CorporatePage::onHistoryLoadClicked()
{
    int row = historyTable_->currentRow();
    QTableWidgetItem* sourceItem = row >= 0 ? historyTable_->item(row, 0) : nullptr;
    bool sourceOk = false;
    const int sourceRow = sourceItem ? sourceItem->data(Qt::UserRole).toInt(&sourceOk) : -1;
    if (!sourceOk || sourceRow < 0 || sourceRow >= historyEntries_.size()) return;

    QJsonObject entry = historyEntries_[sourceRow].toObject();
    QString type = entry["type"].toString();
    QString payload = entry["payload"].toString();

    if (payload.isEmpty()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Information,
            tr("No data"),
            tr("For this records no saved data."));
        return;
    }

    // A history row does not preserve scanner/provenance and full operation
    // identity. Keep live founder/director state untouched and load historical
    // payloads only into the isolated signer parser.
    roleButtons_->button(2)->setChecked(true);
    onRoleChanged(2);
    signerDraftEdit_->setPlainText(payload);
    onSignerDraftChanged();

    logInfo(tr("Loaded record from history: %1").arg(type));
}

void CorporatePage::onExportHistory()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Export history"),
        QString("corporate_history_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("CSV Files (*.csv)"));
    if (fileName.isEmpty()) return;

    QByteArray csv;
    QTextStream out(&csv, QIODevice::WriteOnly);
    out << tr("Time,Type,Status,Details") << '\n';
    for (const QJsonValue& val : historyEntries_) {
        QJsonObject entry = val.toObject();
        out << entry["timestamp"].toString() << ","
            << entry["type"].toString() << ","
            << entry["status"].toString() << ","
            << "\"" << entry["details"].toString().replace("\"", "\"\"") << "\"\n";
    }
    out.flush();
    QString error;
    if (!GUIUtil::writeFileAtomically(fileName, csv, error)) {
        GUIUtil::showStyledMessageBox(
            this, QMessageBox::Warning, tr("Error"),
            tr("Could not save the file: %1").arg(error));
        return;
    }
    logInfo(tr("History exported: %1").arg(fileName));
    GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Success"), tr("History exported in CSV"));
}


// ============================================================================
// Handlers
// ============================================================================
