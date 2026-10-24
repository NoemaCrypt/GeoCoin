#include "multisigpage_internal.h"

void MultisigPage::resetMultisigScanner(bool clearCommitments)
{
    if (multisigScanner_) {
        multisigScanner_->stopAutoScan();
        delete multisigScanner_;
        multisigScanner_ = nullptr;
    }
    multisigScannerTarget_.clear();
    multisigScannerAddress_.clear();
    multisigScannerRequired_ = 0;
    scannerMultisigFields_.clear();

    if (clearCommitments) {
        for (QLineEdit* edit : std::as_const(commitmentEdits_)) {
            if (!edit) continue;
            const QSignalBlocker blocker(edit);
            edit->clear();
            edit->setStyleSheet(GUIUtil::themedEditableStyleSheet());
        }
        updateCommitmentState();
    }
}

void MultisigPage::resetMultisigOperation()
{
    if (autoScanMultisigCheck_ && autoScanMultisigCheck_->isChecked()) {
        autoScanMultisigCheck_->setChecked(false);
    }
    resetMultisigScanner(true);
    currentSpendingHash_.clear();
    currentMultisigSpendHex_.clear();
    currentDraftHash_.clear();
    currentDraftDoc_ = QJsonDocument();
    currentDraftJson_.clear();
    if (spendingHashEdit_) spendingHashEdit_->clear();
    if (draftJsonEdit_) draftJsonEdit_->clear();
    if (draftHashLabel_) draftHashLabel_->setText(tr("Hash transactions: —"));
    if (saveDraftButton_) saveDraftButton_->setEnabled(false);
    if (copyDraftButton_) copyDraftButton_->setEnabled(false);
    clearPendingTransaction();
    updateCommitmentState();
}

bool MultisigPage::ensureMultisigScanner()
{
    if (!walletModel_ || !walletModel_->getChainState()) return false;

    const int required = initiatorMSpin_ ? initiatorMSpin_->value() : 2;
    if (multisigScanner_ &&
        (multisigScannerTarget_ != currentSpendingHash_ ||
         multisigScannerAddress_ != currentMultisigAddress_ ||
         multisigScannerRequired_ != required)) {
        resetMultisigScanner(true);
        clearPendingTransaction();
    }

    if (!multisigScanner_) {
        QStringList allowedSigners;
        for (const QJsonValue& value : configParticipants_) {
            const QString address = value.toObject().value(QStringLiteral("address")).toString().trimmed();
            if (!address.isEmpty()) allowedSigners.append(address);
        }
        multisigScanner_ = new CommitmentScanner(
            currentSpendingHash_, currentMultisigAddress_, required,
            walletModel_->getChainState(), this, allowedSigners);
        multisigScannerTarget_ = currentSpendingHash_;
        multisigScannerAddress_ = currentMultisigAddress_;
        multisigScannerRequired_ = required;

        connect(multisigScanner_, &CommitmentScanner::commitmentFound,
                this, &MultisigPage::onMultisigCommitmentFound);
        connect(multisigScanner_, &CommitmentScanner::commitmentInvalidated,
                this, &MultisigPage::onMultisigCommitmentInvalidated);
        connect(multisigScanner_, &CommitmentScanner::allCommitmentsCollected,
                this, &MultisigPage::onAllMultisigCommitmentsCollected);
        connect(multisigScanner_, &CommitmentScanner::scanError,
                this, &MultisigPage::onMultisigScanError);
        connect(multisigScanner_, &CommitmentScanner::statusUpdate,
                this, &MultisigPage::onMultisigScanStatus);
    }
    return true;
}

void MultisigPage::onAutoScanMultisigToggled(bool checked)
{
    if (checked) {
        // Проверяем что есть spending hash для сканирования
        if (currentSpendingHash_.isEmpty()) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("No spending hash"),
                tr("Create and sign the multisig transaction first.\nThe spending hash appears automatically."));
            autoScanMultisigCheck_->setChecked(false);
            return;
        }

        if (!ensureMultisigScanner()) {
            GUIUtil::showStyledMessageBox(
                this, QMessageBox::Warning, tr("Error"),
                tr("Chain state is unavailable. Cannot start scanning."));
            autoScanMultisigCheck_->setChecked(false);
            return;
        }

        multisigScanner_->startAutoScan(30);  // 30 секунд интервал
        scanNowMultisigButton_->setEnabled(true);
        logInfo(tr("Auto-scanning multisig commitments started"));

    } else {
        // Останавливаем сканер
        if (multisigScanner_) {
            multisigScanner_->stopAutoScan();
        }
        scanNowMultisigButton_->setEnabled(false);
        if (scanStatusMultisigLabel_) {
            scanStatusMultisigLabel_->setText(tr("Status: auto-scanning stopped"));
        }
        logInfo(tr("Auto-scanning multisig commitments stopped"));
    }
}

void MultisigPage::onScanNowMultisig()
{
    if (currentSpendingHash_.isEmpty()) {
        GUIUtil::showStyledMessageBox(
            this, QMessageBox::Warning, tr("No spending hash"),
            tr("Create and sign the multisig transaction first."));
        return;
    }
    if (!ensureMultisigScanner()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Chain state is unavailable."));
        return;
    }

    logInfo(tr("Manual check multisig commitments..."));
    multisigScanner_->scanNow();
}

void MultisigPage::onMultisigCommitmentFound(const QString& txid, const QString& signerAddress)
{
    if (!configParticipants_.isEmpty()) {
        bool allowed = false;
        for (const QJsonValue& value : configParticipants_) {
            if (value.toObject().value(QStringLiteral("address")).toString().trimmed() == signerAddress) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            logError(tr("Skipped a commitment from unknown participant %1")
                .arg(signerAddress.left(15) + QStringLiteral("...")));
            return;
        }
    }

    // Сначала проверяем, нет ли уже такого TXID в полях
    for (auto* edit : commitmentEdits_) {
        if (edit->text().trimmed() == txid) {
            scannerMultisigFields_.insert(txid, edit);
            savePendingTransaction();
            logInfo(tr("Commitment %1 is already added.").arg(txid.left(16) + "..."));
            return;  // Уже есть, не добавляем повторно
        }
    }

    // Находим первое пустое поле и вставляем TXID
    bool inserted = false;
    for (auto* edit : commitmentEdits_) {
        if (edit->text().trimmed().isEmpty()) {
            edit->setText(txid);
            edit->setStyleSheet(GUIUtil::themedSuccessEditableStyleSheet());
            scannerMultisigFields_.insert(txid, edit);
            inserted = true;
            break;
        }
    }

    if (!inserted) {
        // Все поля заняты
        logError(tr("All fields used! Found commitment from %1, but nowhere paste")
            .arg(signerAddress.left(15) + "..."));
        return;
    }

    logInfo(tr("✅ Received signature from %1").arg(signerAddress.left(15) + "..."));
    logInfo(tr("   TXID: %1").arg(txid));

    // ВАЖНО: Сохраняем обновленное состояние
    savePendingTransaction();

    updateCommitmentState();

    // Показываем уведомление
    if (scanStatusMultisigLabel_) {
        int found = 0;
        for (auto* edit : commitmentEdits_) {
            if (!edit->text().trimmed().isEmpty()) found++;
        }
        int required = initiatorMSpin_ ? initiatorMSpin_->value() : 2;
        scanStatusMultisigLabel_->setText(
            tr("✅ Found signature! Collected: %1/%2").arg(found).arg(required));
    }
}

void MultisigPage::onMultisigCommitmentInvalidated(const QString& txid, const QString& signerAddress)
{
    const auto it = scannerMultisigFields_.find(txid);
    if (it == scannerMultisigFields_.end()) return;

    const QPointer<QLineEdit> edit = it.value();
    scannerMultisigFields_.erase(it);
    if (edit && edit->text().trimmed() == txid) {
        edit->clear();
        edit->setStyleSheet(GUIUtil::themedEditableStyleSheet());
        updateCommitmentState();
        logError(tr("Commitment %1 from %2 was removed because it is no longer in the active chain")
            .arg(txid.left(16) + QStringLiteral("..."), signerAddress.left(15) + QStringLiteral("...")));
        if (scanStatusMultisigLabel_) {
            scanStatusMultisigLabel_->setText(tr("Reorg: one signature was invalidated; scanning continues"));
        }
    }
    savePendingTransaction();
}

void MultisigPage::onAllMultisigCommitmentsCollected()
{
    logInfo(tr("🎉 All signatures multisig collected! Can finalize."));

    if (scanStatusMultisigLabel_) {
        scanStatusMultisigLabel_->setText(tr("🎉 All signatures collected! Ready to finalization."));
        scanStatusMultisigLabel_->setStyleSheet("QLabel { color: #27ae60; font-weight: bold; font-size: 12px; }");
    }

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Signatures collected"),
        tr("🎉 All required signatures have been collected!\n\nYou can now finalize the multisig transaction.\nPress \"Finalize and send\".\nThe scanner keeps watching the chain until finalization."));
}

void MultisigPage::onMultisigScanError(const QString& error)
{
    logError(tr("Error scanning: %1").arg(error));

    if (scanStatusMultisigLabel_) {
        scanStatusMultisigLabel_->setText(tr("Error: %1").arg(error));
        scanStatusMultisigLabel_->setStyleSheet("QLabel { color: #e74c3c; font-size: 11px; }");
    }
}

void MultisigPage::onMultisigScanStatus(const QString& message)
{
    if (scanStatusMultisigLabel_) {
        scanStatusMultisigLabel_->setText(tr("Status: %1").arg(message));
        scanStatusMultisigLabel_->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    }
}

// ============================================================================
// Pending Transactions (Незавершенные транзакции)
// ============================================================================

void MultisigPage::savePendingTransaction()
{
    if (pendingTransactionFilePath_.isEmpty()) {
        return;
    }

    // Сохраняем только если есть spending hash (новый способ)
    if (currentSpendingHash_.isEmpty()) {
        return;
    }

    QJsonObject pending;
    pending["amount_schema"] = 2;
    pending["spending_hash"] = currentSpendingHash_;
    pending["multisig_address"] = currentMultisigAddress_;
    pending["multisig_spend_hex"] = currentMultisigSpendHex_;
    pending["to_address"] = receiverEdit_ ? receiverEdit_->text() : QString();
    qint64 amount = 0;
    qint64 fee = 0;
    if (!amountEdit_ || !feeEdit_ ||
        !GUIUtil::parseAmount(amountEdit_->text(), amount) ||
        !GUIUtil::parseAmount(feeEdit_->text(), fee)) {
        logError(tr("The unfinished transaction was not saved: the amount is not exact."));
        return;
    }
    pending["amount_angles"] = QString::number(amount);
    pending["fee_angles"] = QString::number(fee);
    pending["amount"] = GUIUtil::formatAmount(amount);
    pending["fee"] = GUIUtil::formatAmount(fee);
    pending["m"] = initiatorMSpin_ ? initiatorMSpin_->value() : 2;
    pending["n"] = initiatorNSpin_ ? initiatorNSpin_->value() : 3;
    pending["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    // Сохраняем собранные commitments
    QJsonArray commitmentsArray;
    for (auto* edit : commitmentEdits_) {
        QString txid = edit->text().trimmed();
        if (!txid.isEmpty()) {
            commitmentsArray.append(txid);
        }
    }
    pending["commitments"] = commitmentsArray;

    QJsonArray scannerCommitments;
    for (auto it = scannerMultisigFields_.cbegin(); it != scannerMultisigFields_.cend(); ++it) {
        if (it.value() && it.value()->text().trimmed() == it.key()) {
            scannerCommitments.append(it.key());
        }
    }
    pending["scanner_commitments"] = scannerCommitments;

    // Сохраняем конфигурацию участников
    pending["participants"] = configParticipants_;

    QString error;
    const QJsonDocument doc(pending);
    if (GUIUtil::writeFileAtomically(
            pendingTransactionFilePath_, doc.toJson(QJsonDocument::Indented), error)) {
        logInfo(tr("Incomplete transaction saved"));
    } else {
        logError(tr("Could not save the unfinished transaction: %1").arg(error));
    }
}

void MultisigPage::loadPendingTransaction()
{
    if (pendingTransactionFilePath_.isEmpty()) {
        return;
    }

    QFile file(pendingTransactionFilePath_);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (doc.isNull() || !doc.isObject()) {
        logError(tr("Could not load the unfinished transaction: %1").arg(error.errorString()));
        return;
    }

    QJsonObject pending = doc.object();

    qint64 amount = 0;
    qint64 fee = 0;
    if (!readExactJsonAmount(pending, QStringLiteral("amount_angles"), QStringLiteral("amount"), amount) ||
        !readExactJsonAmount(pending, QStringLiteral("fee_angles"), QStringLiteral("fee"), fee)) {
        logError(tr("The old pending JSON has inexact numeric amount/fee values; create the transaction again."));
        return;
    }

    // Восстанавливаем данные только после полной проверки amounts.
    currentSpendingHash_ = pending.value("spending_hash").toString();
    currentMultisigAddress_ = pending.value("multisig_address").toString();
    currentMultisigSpendHex_ = pending.value("multisig_spend_hex").toString();

    {
        const QSignalBlocker blockReceiver(receiverEdit_);
        const QSignalBlocker blockAmount(amountEdit_);
        const QSignalBlocker blockFee(feeEdit_);
        if (receiverEdit_) receiverEdit_->setText(pending.value("to_address").toString());
        if (amountEdit_) amountEdit_->setText(GUIUtil::formatAmount(amount));
        if (feeEdit_) feeEdit_->setText(GUIUtil::formatAmount(fee));
    }
    {
        const QSignalBlocker blockM(initiatorMSpin_);
        const QSignalBlocker blockN(initiatorNSpin_);
        if (initiatorMSpin_) initiatorMSpin_->setValue(pending.value("m").toInt());
        if (initiatorNSpin_) initiatorNSpin_->setValue(pending.value("n").toInt());
    }

    configParticipants_ = pending.value("participants").toArray();
    configLoaded_ = true;

    // Восстанавливаем spending hash в поле
    if (spendingHashEdit_) {
        spendingHashEdit_->setText(currentSpendingHash_);
    }

    // Восстанавливаем адрес
    if (multisigAddressLabel_) {
        multisigAddressLabel_->setText(tr("✅ Address multisig: %1").arg(currentMultisigAddress_));
    }

    // Обновляем поля commitments
    updateMultisigCommitmentFields();

    // Восстанавливаем собранные commitments
    QJsonArray commitmentsArray = pending.value("commitments").toArray();
    QSet<QString> scannerCommitments;
    for (const QJsonValue& value : pending.value("scanner_commitments").toArray()) {
        scannerCommitments.insert(value.toString());
    }
    int idx = 0;
    for (const QJsonValue& val : commitmentsArray) {
        if (idx < commitmentEdits_.size()) {
            const QString txid = val.toString();
            commitmentEdits_[idx]->setText(txid);
            commitmentEdits_[idx]->setStyleSheet(GUIUtil::themedSuccessEditableStyleSheet());
            if (scannerCommitments.contains(txid)) {
                scannerMultisigFields_.insert(txid, commitmentEdits_[idx]);
            }
            idx++;
        }
    }

    // Reconcile persisted scanner provenance immediately. The new worker has
    // no in-memory history from the previous process, so an orphan that
    // happened while the GUI was closed would otherwise survive until click.
    if (walletModel_ && walletModel_->getChainState()) {
        QStringList allowedSigners;
        for (const QJsonValue& value : configParticipants_) {
            const QString address = value.toObject().value(QStringLiteral("address")).toString().trimmed();
            if (!address.isEmpty()) allowedSigners.append(address);
        }
        const QStringList persistedScannerTxids = scannerMultisigFields_.keys();
        for (const QString& txid : persistedScannerTxids) {
            if (!CommitmentScanner::isCommitmentActive(
                    txid, currentSpendingHash_, currentMultisigAddress_,
                    walletModel_->getChainState(), allowedSigners)) {
                onMultisigCommitmentInvalidated(txid, tr("unknown signer"));
            }
        }
    }

    // Переключаемся на вкладку инициатора
    if (roleButtons_ && roleButtons_->button(0)) {
        roleButtons_->button(0)->setChecked(true);
        roleStack_->setCurrentIndex(0);
    }

    // Store the config as wallet metadata; consensus checks the on-chain preimage.
    if (walletModel_ && walletModel_->getWallet()) {
        geocoin::wallet::MultisigConfig config;
        config.m = initiatorMSpin_->value();
        config.n = initiatorNSpin_->value();
        config.address = geocoin::Address(currentMultisigAddress_.toStdString());

        for (const QJsonValue& val : configParticipants_) {
            QJsonObject p = val.toObject();
            QString addrStr = p.value("address").toString();
            double area = p.value("area").toDouble();

            geocoin::wallet::MultisigParticipant participant;
            participant.address = geocoin::Address(addrStr.toStdString());
            config.participants.push_back(participant);
        }

        walletModel_->rememberMultisigConfig(config);
    }

    updateInitiatorState();
    updateMultisigBalance();

    QString timestamp = pending.value("timestamp").toString();
    QDateTime dt = QDateTime::fromString(timestamp, Qt::ISODate);
    QString timeAgo = dt.isValid() ? dt.toLocalTime().toString("dd.MM.yyyy HH:mm") : tr("unknown");

    logInfo(tr("✅ Restored incomplete transaction from %1").arg(timeAgo));
    logInfo(tr("   Spending hash: %1...").arg(currentSpendingHash_.left(16)));
    logInfo(tr("   Collected signatures: %1/%2").arg(commitmentsArray.size()).arg(initiatorMSpin_->value()));

    // Continue monitoring even when M is already present: a reorg may orphan
    // one of the persisted scanner-derived commitments before finalization.
    if (autoScanMultisigCheck_ && !autoScanMultisigCheck_->isChecked()) {
        autoScanMultisigCheck_->setChecked(true);
        logInfo(tr("🔄 Autoscan started to watch the active chain"));
    }

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Transaction restored"),
        tr("Incomplete transaction successfully restored!\n\nSpending hash: %1...\nRecipient: %2\nAmount: %3 GC\nCollected signatures: %4/%5\n\n%6")
            .arg(currentSpendingHash_.left(16))
            .arg(receiverEdit_ ? receiverEdit_->text() : "—")
            .arg(amountEdit_ ? amountEdit_->text() : "0")
            .arg(commitmentsArray.size())
            .arg(initiatorMSpin_->value())
            .arg(commitmentsArray.size() < initiatorMSpin_->value()
                 ? tr("🔄 Auto-scanning started")
                 : tr("✅ All signatures collected! Can finalize")));
}

void MultisigPage::clearPendingTransaction()
{
    if (pendingTransactionFilePath_.isEmpty()) {
        return;
    }

    QFile file(pendingTransactionFilePath_);
    if (file.exists()) {
        file.remove();
        logInfo(tr("Incomplete transaction deleted"));
    }
}

bool MultisigPage::hasPendingTransaction() const
{
    if (pendingTransactionFilePath_.isEmpty()) {
        return false;
    }

    QFile file(pendingTransactionFilePath_);
    return file.exists();
}


// ============================================================================
// Clear History (Очистка истории)
// ============================================================================
