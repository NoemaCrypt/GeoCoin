#include "multisigpage_internal.h"

void MultisigPage::onSignerDraftChanged()
{
    if (broadcastCheck_) {
        broadcastCheck_->setVisible(true);
    }

    signerDraftValid_ = parseSignerDraft(signerDraftEdit_->toPlainText());
    updateSignerState();
}

void MultisigPage::onSignCommitment()
{
    if (!signerDraftValid_) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("No data"),
            tr("Load a complete JSON draft with the canonical tx_hex first."));
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

    QString keyPath;
    GUIUtil::ScopedQStringCleanser keyPathCleanser(keyPath);
    QString source;
    if (sourceId == 0) {
        source = "wallet";
        keyPath = walletKeyCombo_->currentText();
    } else if (sourceId == 1) {
        source = "file";
        keyPath = fileKeyPathEdit_->text().trimmed();
    } else {
        source = "manual";
        keyPath = manualKeyEdit_->text().trimmed();
    }

    logInfo(tr("Signing the commitment from the verified JSON draft (%1)...").arg(source));
    setCommitmentInProgress(true);
    emit requestCommitmentSign(
        signerDraftEdit_->toPlainText(), source, keyPath,
        broadcastCheck_ && broadcastCheck_->isChecked());
}

void MultisigPage::updateSignerState()
{
    const bool inputsEnabled = signerDraftValid_ && !commitmentInProgress_;
    if (signerSummaryLabel_) {
        signerSummaryLabel_->setEnabled(inputsEnabled);
    }
    if (signerHashLabel_) {
        signerHashLabel_->setEnabled(inputsEnabled);
    }
    if (keySourceButtons_) {
        const auto buttons = keySourceButtons_->buttons();
        for (QAbstractButton* button : buttons) {
            if (button) {
                button->setEnabled(inputsEnabled);
            }
        }
    }
    const int sourceId = keySourceButtons_ ? keySourceButtons_->checkedId() : -1;
    if (walletKeyCombo_) {
        walletKeyCombo_->setEnabled(inputsEnabled && sourceId == 0);
    }
    if (fileKeyPathEdit_) {
        fileKeyPathEdit_->setEnabled(inputsEnabled && sourceId == 1);
    }
    if (manualKeyEdit_) {
        manualKeyEdit_->setEnabled(inputsEnabled && sourceId == 2);
    }
    if (broadcastCheck_) {
        broadcastCheck_->setEnabled(inputsEnabled);
    }
    const bool ready = inputsEnabled && isKeyInputReady(sourceId);
    if (signCommitmentButton_) {
        signCommitmentButton_->setEnabled(ready && !commitmentInProgress_);
    }
}

void MultisigPage::updateKeySourceState(int sourceId)
{
    const bool inputsEnabled = !commitmentInProgress_;
    if (walletKeyCombo_) {
        walletKeyCombo_->setEnabled(inputsEnabled && sourceId == 0);
    }
    if (fileKeyPathEdit_) {
        fileKeyPathEdit_->setEnabled(inputsEnabled && sourceId == 1);
    }
    if (manualKeyEdit_) {
        manualKeyEdit_->setEnabled(inputsEnabled && sourceId == 2);
    }
    const bool ready = signerDraftValid_ && inputsEnabled && isKeyInputReady(sourceId);
    if (signCommitmentButton_) {
        signCommitmentButton_->setEnabled(ready);
    }
}

bool MultisigPage::parseSignerDraft(const QString& jsonText)
{
    if (jsonText.trimmed().isEmpty()) {
        signerSummaryLabel_->setText(tr("Address: —\nRecipient: —\nAmount: —\nType: —"));
        signerHashLabel_->setText(tr("Hash for signatures: —"));
        return false;
    }
    if (!walletModel_) {
        signerSummaryLabel_->setText(tr("Error: wallet model is unavailable"));
        signerHashLabel_->setText(tr("Hash for signatures: —"));
        return false;
    }
    CommitmentDraftInspection inspection;
    QString error;
    if (!walletModel_->inspectCommitmentDraft(jsonText, inspection, error)) {
        signerSummaryLabel_->setText(tr("Error: %1").arg(error));
        signerHashLabel_->setText(tr("Hash for signatures: —"));
        return false;
    }
    signerDraftDoc_ = QJsonDocument::fromJson(jsonText.toUtf8());
    signerDraftHash_ = inspection.signingHash;
    signerSummaryLabel_->setText(tr("Type: %1\nAddress: %2\n%3")
        .arg(inspection.operation, inspection.multisigAddress, inspection.details));
    signerHashLabel_->setText(tr("Hash for signatures: %1").arg(signerDraftHash_));
    return true;
}

bool MultisigPage::isKeyInputReady(int sourceId) const
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

void MultisigPage::handleCommitmentReady(const QString& commitmentHex,
                                         const QString& commitmentHash,
                                         const QString& broadcastTxid)
{
    setCommitmentInProgress(false);
    QString txid = broadcastTxid.isEmpty() ? commitmentHash : broadcastTxid;
    QString message = tr("Commitment created.\n\nHash: %1\nChain TXID: %2")
                          .arg(commitmentHash, txid);
    if (!broadcastTxid.isEmpty()) {
        message += tr("\nCommitment already published in network.");
    } else {
        message += tr("\nPublish its in network and notify coordinator TXID.");
    }

    GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Commitment ready"), message);

    logInfo(broadcastTxid.isEmpty()
                ? tr("Signed commitment ready (hash %1).").arg(commitmentHash)
                : tr("Commitment published (txid %1).").arg(broadcastTxid));
    addHistoryEntry("Commitment",
                    broadcastTxid.isEmpty() ? tr("Prepared") : tr("Sent"),
                    tr("Hash: %1\nTXID: %2").arg(commitmentHash, txid),
                    commitmentHex);
    // Signer results may belong to a different coordinator operation, and an
    // unbroadcast local hash is not an active-chain TXID. The initiator fields
    // are populated only by the confirmed-chain scanner (or explicitly by the
    // user), once target and multisig address have been checked.
}

QWidget* MultisigPage::buildSignerPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setSpacing(15);

    layout->addWidget(buildSignerDraftBox());
    layout->addWidget(buildSignerInfoBox());
    layout->addWidget(buildSignerKeyBox());

    broadcastCheck_ = new QCheckBox(tr("Send in network immediately"), page);
    layout->addWidget(broadcastCheck_);

    signCommitmentButton_ = new QPushButton(tr("Create and sign commitment"), page);
    signCommitmentButton_->setEnabled(false);
    connect(signCommitmentButton_, &QPushButton::clicked, this, &MultisigPage::onSignCommitment);
    layout->addWidget(signCommitmentButton_);
    layout->addStretch();

    return page;
}

QGroupBox* MultisigPage::buildSignerDraftBox()
{
    QGroupBox *box = new QGroupBox(tr("Data for signing"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    QLabel *draftSectionLabel = new QLabel(
        tr("<b>Complete verifiable JSON draft</b>"), box);
    draftSectionLabel->setStyleSheet("QLabel { color: #2980b9; }");
    layout->addWidget(draftSectionLabel);

    QLabel *draftInfoLabel = new QLabel(tr("Paste JSON with the canonical tx_hex. The address, policy, hash and outputs are derived again from the transaction bytes before signing."), box);
    draftInfoLabel->setWordWrap(true);
    layout->addWidget(draftInfoLabel);

    signerDraftEdit_ = new QTextEdit(box);
    signerDraftEdit_->setPlaceholderText(tr("{ \"multisig_address\": \"GEOM...\", ... }"));
    signerDraftEdit_->setMinimumHeight(100);
    signerDraftEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(signerDraftEdit_, &QTextEdit::textChanged, this, &MultisigPage::onSignerDraftChanged);
    layout->addWidget(signerDraftEdit_);

    QHBoxLayout *draftButtons = new QHBoxLayout();
    QPushButton *loadDraft = new QPushButton(tr("Load from file"), box);
    QPushButton *pasteDraft = new QPushButton(tr("Paste from buffer"), box);
    draftButtons->addWidget(loadDraft);
    draftButtons->addWidget(pasteDraft);
    draftButtons->addStretch();
    layout->addLayout(draftButtons);

    connect(loadDraft, &QPushButton::clicked, this, &MultisigPage::onLoadDraftJson);
    connect(pasteDraft, &QPushButton::clicked, [this]() {
        const QClipboard *clipboard = QApplication::clipboard();
        signerDraftEdit_->setPlainText(clipboard->text());
    });

    applyGeoTheme(box);
    return box;
}

QGroupBox* MultisigPage::buildSignerInfoBox()
{
    QGroupBox *box = new QGroupBox(tr("Info about transactions"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    signerSummaryLabel_ = new QLabel(tr("Address: —\nRecipient: —\nAmount: —\nType: —"), box);
    signerHashLabel_ = new QLabel(tr("Hash for signatures: —"), box);

    layout->addWidget(signerSummaryLabel_);
    layout->addWidget(signerHashLabel_);
    applyGeoTheme(box);
    return box;
}

QGroupBox* MultisigPage::buildSignerKeyBox()
{
    QGroupBox *box = new QGroupBox(tr("Your private key"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    keySourceButtons_ = new QButtonGroup(box);
    QRadioButton *walletKeyBtn = new QRadioButton(tr("From wallet"), box);
    QRadioButton *fileKeyBtn = new QRadioButton(tr("From file"), box);
    QRadioButton *manualKeyBtn = new QRadioButton(tr("Manually (HEX)"), box);
    keySourceButtons_->addButton(walletKeyBtn, 0);
    keySourceButtons_->addButton(fileKeyBtn, 1);
    keySourceButtons_->addButton(manualKeyBtn, 2);

    // Выбираем "Из файла" по умолчанию
    fileKeyBtn->setChecked(true);

    walletKeyCombo_ = new QComboBox(box);
    walletKeyCombo_->setEnabled(false);

    fileKeyPathEdit_ = new QLineEdit(box);
    fileKeyPathEdit_->setPlaceholderText(tr("foundation_key_1.txt"));
    fileKeyPathEdit_->setEnabled(true);  // Включаем сразу так как выбрано по умолчанию
    QPushButton *fileBrowse = new QPushButton(tr("Overview…"), box);
    fileBrowse->setEnabled(true);  // Включаем сразу
    connect(fileBrowse, &QPushButton::clicked, [this]() {
        QString file = QFileDialog::getOpenFileName(this, tr("Select file key"), QString(), tr("Key files (*.txt *.dat);;All files (*)"));
        if (!file.isEmpty()) {
            fileKeyPathEdit_->setText(file);
        }
    });

    manualKeyEdit_ = new QLineEdit(box);
    manualKeyEdit_->setPlaceholderText(tr("a1b2c3d4…"));
    manualKeyEdit_->setEnabled(false);

    connect(keySourceButtons_, QOverload<int>::of(&QButtonGroup::idClicked),
            [this, walletKeyBtn, fileKeyBtn, manualKeyBtn, fileBrowse](int id) {
        walletKeyCombo_->setEnabled(id == 0);
        fileKeyPathEdit_->setEnabled(id == 1);
        fileBrowse->setEnabled(id == 1);
        manualKeyEdit_->setEnabled(id == 2);
        updateKeySourceState(id);
    });

    // Добавляем обработчики изменения текста для обновления состояния кнопки
    connect(fileKeyPathEdit_, &QLineEdit::textChanged, [this]() {
        if (keySourceButtons_->checkedId() == 1) {
            updateKeySourceState(1);
        }
    });
    connect(manualKeyEdit_, &QLineEdit::textChanged, [this]() {
        if (keySourceButtons_->checkedId() == 2) {
            updateKeySourceState(2);
        }
    });

    layout->addWidget(walletKeyBtn);
    layout->addWidget(walletKeyCombo_);
    layout->addWidget(fileKeyBtn);

    QHBoxLayout *fileLayout = new QHBoxLayout();
    fileLayout->addWidget(fileKeyPathEdit_);
    fileLayout->addWidget(fileBrowse);
    layout->addLayout(fileLayout);

    layout->addWidget(manualKeyBtn);
    layout->addWidget(manualKeyEdit_);

    applyGeoTheme(box);
    return box;
}
