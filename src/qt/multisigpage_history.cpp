#include "multisigpage_internal.h"

void MultisigPage::addHistoryEntry(const QString& type,
                                   const QString& status,
                                   const QString& details,
                                   const QString& payload)
{
    QJsonObject entry;
    entry.insert("timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    entry.insert("type", type);
    entry.insert("status", status);
    entry.insert("details", details);
    if (!payload.isEmpty()) {
        entry.insert("payload", payload);
    }
    historyEntries_.append(entry);
    constexpr int kMaxHistoryEntries = 200;
    while (historyEntries_.size() > kMaxHistoryEntries) {
        historyEntries_.removeFirst();
    }
    saveHistory();
    refreshHistoryView();
}

void MultisigPage::loadHistory()
{
    historyEntries_ = QJsonArray();
    if (historyFilePath_.isEmpty()) {
        return;
    }
    QFile file(historyFilePath_);
    if (!file.exists()) {
        refreshHistoryView();
        return;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        logError(tr("Could not read history: %1").arg(file.errorString()));
        return;
    }
    QByteArray data = file.readAll();
    file.close();
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (doc.isArray()) {
        historyEntries_ = doc.array();
    } else if (doc.isObject()) {
        historyEntries_ = doc.object().value("entries").toArray();
    } else {
        logError(tr("File history corrupted (%1)").arg(error.errorString()));
    }
    refreshHistoryView();
}

void MultisigPage::saveHistory()
{
    if (historyFilePath_.isEmpty()) {
        return;
    }
    QString error;
    const QJsonDocument doc(historyEntries_);
    if (!GUIUtil::writeFileAtomically(
            historyFilePath_, doc.toJson(QJsonDocument::Compact), error)) {
        logError(tr("Could not save history: %1").arg(error));
        return;
    }
}

void MultisigPage::refreshHistoryView()
{
    if (!historyTable_) {
        return;
    }
    QItemSelectionModel *selection = historyTable_->selectionModel();
    if (selection) {
        selection->blockSignals(true);
    }
    historyTable_->setRowCount(0);
    for (int i = historyEntries_.size() - 1; i >= 0; --i) {
        const QJsonObject obj = historyEntries_.at(i).toObject();
        const int row = historyTable_->rowCount();
        historyTable_->insertRow(row);
        const QString timestamp = obj.value("timestamp").toString();
        QDateTime dt = QDateTime::fromString(timestamp, Qt::ISODate);
        if (dt.isValid()) {
            dt = dt.toLocalTime();
        }
        const QString timeDisplay = dt.isValid()
            ? dt.toString("dd.MM.yyyy HH:mm:ss")
            : timestamp;
        auto *timeItem = new QTableWidgetItem(timeDisplay);
        timeItem->setData(Qt::UserRole, obj.value("payload").toString());
        historyTable_->setItem(row, 0, timeItem);

        const QString type = obj.value("type").toString();
        auto *typeItem = new QTableWidgetItem(type);
        typeItem->setData(Qt::UserRole, type);
        historyTable_->setItem(row, 1, typeItem);

        auto *statusItem = new QTableWidgetItem(obj.value("status").toString());
        historyTable_->setItem(row, 2, statusItem);

        const QString details = obj.value("details").toString();
        auto *detailsItem = new QTableWidgetItem(details);
        detailsItem->setToolTip(details);
        historyTable_->setItem(row, 3, detailsItem);
    }
    if (selection) {
        selection->clear();
        selection->blockSignals(false);
    }
    if (historyLoadButton_) {
        historyLoadButton_->setEnabled(false);
    }
}

void MultisigPage::onHistorySelectionChanged()
{
    if (!historyTable_ || !historyLoadButton_) {
        return;
    }
    QItemSelectionModel *selection = historyTable_->selectionModel();
    if (!selection || selection->selectedRows().isEmpty()) {
        historyLoadButton_->setEnabled(false);
        return;
    }
    const int row = selection->selectedRows().first().row();
    QTableWidgetItem *timeItem = historyTable_->item(row, 0);
    QTableWidgetItem *typeItem = historyTable_->item(row, 1);
    if (!timeItem || !typeItem) {
        historyLoadButton_->setEnabled(false);
        return;
    }
    const QString payload = timeItem->data(Qt::UserRole).toString();
    const QString type = typeItem->data(Qt::UserRole).toString();
    const bool canLoad = !payload.isEmpty() && type == "Draft";
    historyLoadButton_->setEnabled(canLoad);
}

void MultisigPage::onHistoryLoadClicked()
{
    if (!historyTable_) {
        return;
    }
    QItemSelectionModel *selection = historyTable_->selectionModel();
    if (!selection || selection->selectedRows().isEmpty()) {
        return;
    }
    const int row = selection->selectedRows().first().row();
    QTableWidgetItem *timeItem = historyTable_->item(row, 0);
    QTableWidgetItem *typeItem = historyTable_->item(row, 1);
    if (!timeItem || !typeItem) {
        return;
    }
    const QString payload = timeItem->data(Qt::UserRole).toString();
    const QString type = typeItem->data(Qt::UserRole).toString();
    if (type != "Draft" || payload.trimmed().isEmpty()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Information,
            tr("Nothing load"),
            tr("Select record with draft."));
        return;
    }
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8(), &error);
    if (doc.isNull() || !doc.isObject()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("Record history corrupted: %1").arg(error.errorString()));
        return;
    }
    // History lacks the full live-operation provenance (confirmed fields,
    // scanner anchor and pending identity). Load it only into the isolated
    // signer workflow; never splice it into an existing initiator operation.
    signerDraftEdit_->setPlainText(payload);
    onSignerDraftChanged();
    if (roleButtons_) {
        if (QAbstractButton *signerButton = roleButtons_->button(1)) {
            signerButton->setChecked(true);
        }
    }
    roleStack_->setCurrentIndex(1);
    logInfo(tr("The history draft was loaded into the safe signer mode."));
}

void MultisigPage::clearHistory()
{
    const auto answer = GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Warning,
        tr("Clear history?"),
        tr("You sure that want clear all history operations?\n\nThis will delete:\n• All records in history\n• Current spending hash\n• Collected commitments\n• Incomplete transaction\n\nThis action cannot cancel!"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (answer != QMessageBox::Yes) {
        return;
    }

    historyEntries_ = QJsonArray();
    saveHistory();
    refreshHistoryView();
    resetMultisigOperation();

    if (scanStatusMultisigLabel_) {
        scanStatusMultisigLabel_->setText(tr("Auto-scanning disabled"));
        scanStatusMultisigLabel_->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    }

    logInfo(tr("✅ History cleared"));

    GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Ready"), tr("History successfully cleared!"));
}
