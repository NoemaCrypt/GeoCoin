#include "corporatepage_internal.h"

void CorporatePage::updateSignerState()
{
    const bool inputsEnabled = signerDraftValid_ && !commitmentInProgress_;

    if (signerInfoLabel_) signerInfoLabel_->setEnabled(inputsEnabled);
    if (signerHashLabel_) signerHashLabel_->setEnabled(inputsEnabled);

    if (keySourceButtons_) {
        const auto buttons = keySourceButtons_->buttons();
        for (QAbstractButton* button : buttons) {
            if (button) button->setEnabled(inputsEnabled);
        }
    }

    const int sourceId = keySourceButtons_ ? keySourceButtons_->checkedId() : -1;
    updateKeySourceState(sourceId);

    const bool ready = inputsEnabled && isKeyInputReady(sourceId);
    if (signCommitmentButton_) {
        signCommitmentButton_->setEnabled(ready && !commitmentInProgress_);
    }
}

void CorporatePage::updateKeySourceState(int sourceId)
{
    const bool inputsEnabled = !commitmentInProgress_;

    if (walletKeyCombo_) walletKeyCombo_->setEnabled(inputsEnabled && sourceId == 0);
    if (fileKeyPathEdit_) fileKeyPathEdit_->setEnabled(inputsEnabled && sourceId == 1);
    if (browseKeyFileButton_) browseKeyFileButton_->setEnabled(inputsEnabled && sourceId == 1);
    if (manualKeyEdit_) manualKeyEdit_->setEnabled(inputsEnabled && sourceId == 2);

    const bool ready = signerDraftValid_ && inputsEnabled && isKeyInputReady(sourceId);
    if (signCommitmentButton_) signCommitmentButton_->setEnabled(ready);
}

bool CorporatePage::isKeyInputReady(int sourceId) const
{
    switch (sourceId) {
        case 0:
            return walletKeyCombo_ && walletKeyCombo_->count() > 0;
        case 1:
            return fileKeyPathEdit_ && !fileKeyPathEdit_->text().trimmed().isEmpty();
        case 2: {
            QString key = manualKeyEdit_ ? manualKeyEdit_->text().trimmed() : QString();
            return !key.isEmpty() && key.size() >= 32;
        }
        default:
            return false;
    }
}

// ============================================================================
// Helper Methods - Loading/Parsing
// ============================================================================

bool CorporatePage::parseSignerDraft(const QString& jsonText)
{
    if (jsonText.trimmed().isEmpty()) {
        signerInfoLabel_->setText(tr("Type: —\nAddress: —\nDetails: —"));
        signerHashLabel_->setText(tr("Hash for signatures: —"));
        return false;
    }

    if (!walletModel_) {
        signerInfoLabel_->setText(tr("Error: wallet model is unavailable"));
        signerHashLabel_->setText(tr("Hash for signatures: —"));
        return false;
    }
    CommitmentDraftInspection inspection;
    QString error;
    if (!walletModel_->inspectCommitmentDraft(jsonText, inspection, error)) {
        signerInfoLabel_->setText(tr("Error: %1").arg(error));
        signerHashLabel_->setText(tr("Hash for signatures: —"));
        return false;
    }

    signerDraftDoc_ = QJsonDocument::fromJson(jsonText.toUtf8());
    signerDraftHash_ = inspection.signingHash;
    signerInfoLabel_->setText(tr("Type: %1\nAddress: %2\n%3")
        .arg(inspection.operation, inspection.multisigAddress, inspection.details));
    signerHashLabel_->setText(tr("Hash for signatures: %1").arg(inspection.signingHash));

    return true;
}

QWidget* CorporatePage::buildSignerPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setSpacing(15);

    layout->addWidget(buildSignerDraftBox());
    layout->addWidget(buildSignerInfoBox());
    layout->addWidget(buildSignerKeyBox());
    layout->addStretch();

    return page;
}

QGroupBox* CorporatePage::buildSignerDraftBox()
{
    QGroupBox *box = new QGroupBox(tr("Draft for signing"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    QLabel *jsonLabel = new QLabel(tr("Paste the complete JSON with the canonical tx_hex. The type, multisig policy, signing hash and outputs are derived from the transaction and the confirmed proof."), box);
    jsonLabel->setWordWrap(true);
    layout->addWidget(jsonLabel);

    signerDraftEdit_ = new QTextEdit(box);
    signerDraftEdit_->setMinimumHeight(200);
    signerDraftEdit_->setPlaceholderText(tr("Paste JSON draft..."));
    connect(signerDraftEdit_, &QTextEdit::textChanged, this, &CorporatePage::onSignerDraftChanged);
    layout->addWidget(signerDraftEdit_);

    loadSignerDraftButton_ = new QPushButton(tr("Load from file"), box);
    connect(loadSignerDraftButton_, &QPushButton::clicked, this, &CorporatePage::onLoadSignerDraft);
    layout->addWidget(loadSignerDraftButton_);

    return box;
}

QGroupBox* CorporatePage::buildSignerInfoBox()
{
    QGroupBox *box = new QGroupBox(tr("Info about transactions"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    signerInfoLabel_ = new QLabel(tr("Type: —\nAddress: —\nDetails: —"), box);
    layout->addWidget(signerInfoLabel_);

    signerHashLabel_ = new QLabel(tr("Hash for signatures: —"), box);
    signerHashLabel_->setStyleSheet("QLabel { color: #e67e22; font-weight: bold; }");
    signerHashLabel_->setWordWrap(true);
    layout->addWidget(signerHashLabel_);

    return box;
}

QGroupBox* CorporatePage::buildSignerKeyBox()
{
    QGroupBox *box = new QGroupBox(tr("Source private key"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    QRadioButton *walletKeyBtn = new QRadioButton(tr("From wallet"), box);
    QRadioButton *fileKeyBtn = new QRadioButton(tr("From file"), box);
    QRadioButton *manualKeyBtn = new QRadioButton(tr("Enter manually"), box);
    keySourceButtons_->addButton(walletKeyBtn, 0);
    keySourceButtons_->addButton(fileKeyBtn, 1);
    keySourceButtons_->addButton(manualKeyBtn, 2);
    walletKeyBtn->setChecked(true);
    connect(keySourceButtons_, SIGNAL(idClicked(int)), this, SLOT(onKeySourceChanged(int)));

    layout->addWidget(walletKeyBtn);
    walletKeyCombo_ = new QComboBox(box);
    layout->addWidget(walletKeyCombo_);

    layout->addWidget(fileKeyBtn);
    QHBoxLayout *fileLayout = new QHBoxLayout();
    fileKeyPathEdit_ = new QLineEdit(box);
    fileKeyPathEdit_->setPlaceholderText(tr("Path to file..."));
    fileKeyPathEdit_->setEnabled(false);
    browseKeyFileButton_ = new QPushButton(tr("Overview"), box);
    browseKeyFileButton_->setEnabled(false);
    connect(browseKeyFileButton_, &QPushButton::clicked, this, &CorporatePage::onBrowseKeyFile);
    fileLayout->addWidget(fileKeyPathEdit_);
    fileLayout->addWidget(browseKeyFileButton_);
    layout->addLayout(fileLayout);

    layout->addWidget(manualKeyBtn);
    manualKeyEdit_ = new QLineEdit(box);
    manualKeyEdit_->setPlaceholderText(tr("Enter private key (hex)..."));
    manualKeyEdit_->setEchoMode(QLineEdit::Password);
    manualKeyEdit_->setEnabled(false);
    layout->addWidget(manualKeyEdit_);

    broadcastCheck_ = new QCheckBox(tr("Send commitment in network immediately"), box);
    broadcastCheck_->setChecked(true);
    layout->addWidget(broadcastCheck_);

    signCommitmentButton_ = new QPushButton(tr("Create and sign commitment"), box);
    signCommitmentButton_->setStyleSheet("QPushButton { background-color: #27ae60; color: white; font-weight: bold; padding: 10px; }");
    signCommitmentButton_->setEnabled(false);
    connect(signCommitmentButton_, &QPushButton::clicked, this, &CorporatePage::onSignCommitment);
    layout->addWidget(signCommitmentButton_);

    commitmentResultLabel_ = new QLabel(tr("Result: —"), box);
    commitmentResultLabel_->setStyleSheet("QLabel { color: #27ae60; font-weight: bold; }");
    commitmentResultLabel_->setWordWrap(true);
    layout->addWidget(commitmentResultLabel_);

    return box;
}

// ============================================================================
// Build UI - History Box
// ============================================================================

void CorporatePage::onSignerDraftChanged()
{
    signerDraftValid_ = parseSignerDraft(signerDraftEdit_->toPlainText());
    updateSignerState();
}

void CorporatePage::onLoadSignerDraft()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Load draft"),
        QString(), tr("JSON Files (*.json);;All Files (*)"));
    if (fileName.isEmpty()) return;

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Could not open the file"));
        return;
    }

    QString content = file.readAll();
    file.close();
    signerDraftEdit_->setPlainText(content);
    logInfo(tr("Draft loaded from file: %1").arg(fileName));
}

void CorporatePage::onKeySourceChanged(int sourceId)
{
    updateKeySourceState(sourceId);
}

void CorporatePage::onBrowseKeyFile()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Select file with key"),
        QString(), tr("Text Files (*.txt);;All Files (*)"));
    if (!fileName.isEmpty()) {
        fileKeyPathEdit_->setText(fileName);
    }
}

void CorporatePage::onSignCommitment()
{
    if (!signerDraftValid_) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("No draft"),
            tr("Load a valid draft first."));
        return;
    }

    int sourceId = keySourceButtons_->checkedId();
    if (!isKeyInputReady(sourceId)) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Key not selected"),
            tr("Specify source private key."));
        return;
    }

    QString keyData;
    GUIUtil::ScopedQStringCleanser keyDataCleanser(keyData);
    QString source;

    if (sourceId == 0) {
        // Из кошелька - получаем приватный ключ
        if (!walletModel_ || !walletModel_->getWallet()) {
            GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Wallet not loaded."));
            return;
        }

        QString addressStr = walletKeyCombo_->currentText();
        source = "wallet";
        keyData = addressStr;
        logInfo(tr("Used key from wallet for addresses %1").arg(addressStr));

    } else if (sourceId == 1) {
        // Из файла
        source = "file";
        keyData = fileKeyPathEdit_->text().trimmed();
        logInfo(tr("Using the private key from the file"));
    } else {
        // Вручную
        source = "manual";
        keyData = manualKeyEdit_->text().trimmed();
        logInfo(tr("Used key entered manually"));
    }

    // Валидация ключа по источнику
    if (source == "manual") {
        // Должен быть 64 hex символа = 32 байта
        if (keyData.length() != 64 || !keyData.contains(QRegularExpression("^[0-9a-fA-F]+$"))) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("Invalid key"),
                tr("Private key must be 64 hex characters (32 bytes).\nReceived: %1 characters\n\nExample: a1b2c3d4e5f6...").arg(keyData.length()));
            return;
        }
    } else if (source == "file") {
        if (keyData.isEmpty()) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("File key not found"),
                tr("Specify the private key file."));
            return;
        }
    } else if (source == "wallet") {
        if (keyData.isEmpty()) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("Address not selected"),
                tr("Select address from wallet."));
            return;
        }
    }

    logInfo(tr("Signing commitment (%1)...").arg(source));
    setCommitmentInProgress(true);

    emit requestCommitmentSign(
        signerDraftEdit_->toPlainText(),
        source,
        keyData,
        broadcastCheck_->isChecked());
}

// ============================================================================
// Common Slots
// ============================================================================

void CorporatePage::handleCommitmentReady(const QString& commitmentHex, const QString& commitmentHash, const QString& broadcastTxid)
{
    setCommitmentInProgress(false);

    QString txid = broadcastTxid.isEmpty() ? commitmentHash : broadcastTxid;
    commitmentResultLabel_->setText(tr("✅ Commitment created!\nTXID: %1").arg(txid));

    QString message = tr("Commitment created.\n\nHash: %1\nTXID: %2").arg(commitmentHash, txid);
    if (!broadcastTxid.isEmpty()) {
        message += tr("\n\nCommitment published in network.");
    } else {
        message += tr("\n\nPublish its in network and notify coordinator TXID.");
    }

    GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Commitment is ready"), message);

    logInfo(broadcastTxid.isEmpty() ? tr("Signed commitment ready (hash %1).").arg(commitmentHash) : tr("Commitment published (txid %1).").arg(broadcastTxid));
    addHistoryEntry("Commitment", broadcastTxid.isEmpty() ? tr("Prepared") : tr("Sent"),
                   tr("Hash: %1\nTXID: %2").arg(commitmentHash, txid), commitmentHex);
}
