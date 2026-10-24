#include "multisigpage_internal.h"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/wallet/address_funds.hpp"

void MultisigPage::onCreateDraft()
{
    QString error;
    qint64 amount = 0;
    qint64 fee = 0;
    if (!validateInitiatorInputs(error, &amount, &fee)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Invalid data"), error);
        return;
    }

    // Создаем JSON конфигурацию из текущих данных
    QJsonObject configObj;
    configObj["m"] = initiatorMSpin_->value();
    configObj["n"] = initiatorNSpin_->value();
    configObj["address"] = currentMultisigAddress_;
    configObj["participants"] = configParticipants_;
    QJsonDocument configDoc(configObj);
    QString configJson = QString::fromUtf8(configDoc.toJson(QJsonDocument::Compact));

    logInfo(tr("Requested creation draft (automatic mode)..."));
    setDraftInProgress(true);
    emit requestDraftCreation(configJson,
                              receiverEdit_->text().trimmed(),
                              amount,
                              fee);
}

void MultisigPage::onCreateAndSignMultisig()
{
    QString error;
    qint64 amount = 0;
    qint64 fee = 0;
    if (!validateInitiatorInputs(error, &amount, &fee)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Invalid data"), error);
        return;
    }

    // Создаем диалог выбора источника ключа
    QDialog keyDialog(this);
    keyDialog.setWindowTitle(tr("Select private key"));
    keyDialog.setMinimumWidth(500);

    QVBoxLayout *mainLayout = new QVBoxLayout(&keyDialog);

    QLabel *infoLabel = new QLabel(tr("This key signs the commitment.\nThe commitment is published on the chain immediately."), &keyDialog);
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("QLabel { color: #e67e22; font-weight: bold; padding: 10px; background-color: #fef5e7; border-radius: 5px; }");
    mainLayout->addWidget(infoLabel);

    // Группа выбора источника
    QGroupBox *sourceBox = new QGroupBox(tr("Source private key"), &keyDialog);
    QVBoxLayout *sourceLayout = new QVBoxLayout(sourceBox);

    QButtonGroup *sourceButtons = new QButtonGroup(&keyDialog);
    QRadioButton *walletKeyBtn = new QRadioButton(tr("From wallet"), &keyDialog);
    QRadioButton *fileKeyBtn = new QRadioButton(tr("From file"), &keyDialog);
    QRadioButton *manualKeyBtn = new QRadioButton(tr("Manually (HEX)"), &keyDialog);

    sourceButtons->addButton(walletKeyBtn, 0);
    sourceButtons->addButton(fileKeyBtn, 1);
    sourceButtons->addButton(manualKeyBtn, 2);

    // По умолчанию выбираем кошелек если есть адреса
    if (walletModel_ && !walletModel_->getAddress().isEmpty()) {
        walletKeyBtn->setChecked(true);
    } else {
        fileKeyBtn->setChecked(true);
    }

    // Комбобокс для выбора адреса из кошелька
    QComboBox *walletKeyCombo = new QComboBox(&keyDialog);
    walletKeyCombo->setEnabled(walletKeyBtn->isChecked());
    if (walletModel_) {
        QString primaryAddress = walletModel_->getAddress();
        if (!primaryAddress.isEmpty()) {
            walletKeyCombo->addItem(primaryAddress);
        }
    }

    // Поле для файла
    QWidget *fileWidget = new QWidget(&keyDialog);
    QHBoxLayout *fileLayout = new QHBoxLayout(fileWidget);
    fileLayout->setContentsMargins(0, 0, 0, 0);
    QLineEdit *fileKeyPathEdit = new QLineEdit(&keyDialog);
    fileKeyPathEdit->setPlaceholderText(tr("foundation_key_1.txt"));
    fileKeyPathEdit->setEnabled(fileKeyBtn->isChecked());
    QPushButton *fileBrowseBtn = new QPushButton(tr("Overview…"), &keyDialog);
    fileBrowseBtn->setEnabled(fileKeyBtn->isChecked());
    connect(fileBrowseBtn, &QPushButton::clicked, [&keyDialog, fileKeyPathEdit]() {
        QString file = QFileDialog::getOpenFileName(&keyDialog, tr("Select file key"),
                                                     QString(), tr("Key files (*.txt *.dat);;All files (*)"));
        if (!file.isEmpty()) {
            fileKeyPathEdit->setText(file);
        }
    });
    fileLayout->addWidget(fileKeyPathEdit);
    fileLayout->addWidget(fileBrowseBtn);

    // Поле для ручного ввода
    QLineEdit *manualKeyEdit = new QLineEdit(&keyDialog);
    manualKeyEdit->setPlaceholderText(tr("a1b2c3d4e5f6... (64 hex characters)"));
    manualKeyEdit->setEchoMode(QLineEdit::Password);
    manualKeyEdit->setEnabled(manualKeyBtn->isChecked());

    // Подключаем переключение источников
    connect(sourceButtons, QOverload<int>::of(&QButtonGroup::idClicked),
            [walletKeyCombo, fileKeyPathEdit, fileBrowseBtn, manualKeyEdit](int id) {
        walletKeyCombo->setEnabled(id == 0);
        fileKeyPathEdit->setEnabled(id == 1);
        fileBrowseBtn->setEnabled(id == 1);
        manualKeyEdit->setEnabled(id == 2);
    });

    sourceLayout->addWidget(walletKeyBtn);
    sourceLayout->addWidget(walletKeyCombo);
    sourceLayout->addWidget(fileKeyBtn);
    sourceLayout->addWidget(fileWidget);
    sourceLayout->addWidget(manualKeyBtn);
    sourceLayout->addWidget(manualKeyEdit);

    mainLayout->addWidget(sourceBox);

    // Кнопки диалога
    QDialogButtonBox *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &keyDialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &keyDialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &keyDialog, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    // Применяем текущую тему приложения.
    GUIUtil::applyDialogStyle(&keyDialog);

    if (keyDialog.exec() != QDialog::Accepted) {
        logInfo(tr("Creation cancelled user"));
        return;
    }

    // Получаем выбранный источник и ключ
    int sourceId = sourceButtons->checkedId();
    QString privateKeyHex;
    GUIUtil::ScopedQStringCleanser privateKeyHexCleanser(privateKeyHex);
    QString walletAddress;
    QString walletPassword;
    GUIUtil::ScopedQStringCleanser walletPasswordCleanser(walletPassword);

    if (sourceId == 0) {
        // Из кошелька
        if (!walletModel_ || walletKeyCombo->count() == 0) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("Error"),
                tr("Wallet not loaded or no available addresses."));
            return;
        }

        walletAddress = walletKeyCombo->currentText();
        if (walletModel_->isWalletEncrypted()) {
            if (!GUIUtil::getStyledText(this,
                                        tr("Password wallet"),
                                        tr("Enter the password to sign:"),
                                        walletPassword,
                                        QLineEdit::Password) || walletPassword.isEmpty()) {
                return;
            }
        }
        logInfo(tr("Used key from wallet for addresses %1")
                    .arg(walletAddress.left(15) + "..."));

    } else if (sourceId == 1) {
        // Из файла
        QString filePath = fileKeyPathEdit->text().trimmed();
        if (filePath.isEmpty()) {
            GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Specify path to file with key."));
            return;
        }

        QString keyError;
        if (!GUIUtil::readPrivateKeyFileHex(filePath, privateKeyHex, keyError)) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("Error"),
                keyError);
            return;
        }
        logInfo(tr("Using the key from the protected file"));

    } else {
        // Вручную
        privateKeyHex = manualKeyEdit->text().trimmed();
        logInfo(tr("Used key entered manually"));
    }

    // External key sources remain supported, but wallet keys never leave WalletModel.
    if (sourceId != 0 &&
        (privateKeyHex.length() != 64 ||
         !privateKeyHex.contains(QRegularExpression("^[0-9a-fA-F]+$")))) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Invalid key"),
            tr("Private key must be 64 hex characters (32 bytes).\nReceived: %1 characters\n\nExample: a1b2c3d4e5f6...").arg(privateKeyHex.length()));
        return;
    }

    logInfo(tr("Creation and signing multisig transactions..."));
    setDraftInProgress(true);

    // Создаем JSON конфигурацию из текущих данных
    QJsonObject configObj;
    configObj["m"] = initiatorMSpin_->value();
    configObj["n"] = initiatorNSpin_->value();
    configObj["address"] = currentMultisigAddress_;
    configObj["participants"] = configParticipants_;
    QJsonDocument configDoc(configObj);
    QString configJson = QString::fromUtf8(configDoc.toJson(QJsonDocument::Compact));

    if (sourceId == 0) {
        emit requestCreateAndSignMultisigWithWalletKey(
            configJson,
            receiverEdit_->text().trimmed(),
            amount,
            fee,
            walletAddress,
            walletPassword);
    } else {
        emit requestCreateAndSignMultisig(
            configJson,
            receiverEdit_->text().trimmed(),
            amount,
            fee,
            privateKeyHex);
    }
}

void MultisigPage::onCommitmentChanged()
{
    updateCommitmentState();
}

void MultisigPage::onFinalizeDraft()
{
    QStringList txids;
    QSet<QString> uniqueTxids;
    for (QLineEdit *edit : std::as_const(commitmentEdits_)) {
        QString value = edit->text().trimmed();
        if (!value.isEmpty()) {
            if (uniqueTxids.contains(value)) {
                GUIUtil::showStyledMessageBox(
                    this, QMessageBox::Warning, tr("Duplicate TXID"),
                    tr("Each commitment TXID can be used only once."));
                return;
            }
            uniqueTxids.insert(value);
            txids.append(value);
        }
    }
    if (txids.size() < initiatorMSpin_->value()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Not enough signatures"),
            tr("At least %1 commitment transaction ids are required.").arg(initiatorMSpin_->value()));
        return;
    }

    // Для финализации всегда нужна полная каноническая транзакция.
    QString draftToFinalize = currentDraftJson_;

    if (draftToFinalize.isEmpty()) {
        // Восстанавливаем JSON-контейнер только из сохранённого tx_hex.
        if (!currentSpendingHash_.isEmpty() && !currentMultisigSpendHex_.isEmpty()) {
            QJsonObject draftObj;
            draftObj["type"] = "multisig_spend";
            draftObj["spending_tx_hash"] = currentSpendingHash_;
            draftObj["multisig_spend_hex"] = currentMultisigSpendHex_;
            draftObj["multisig_address"] = currentMultisigAddress_;
            draftObj["m"] = initiatorMSpin_->value();
            draftObj["n"] = initiatorNSpin_->value();

            QJsonDocument draftDoc(draftObj);
            draftToFinalize = draftDoc.toJson(QJsonDocument::Compact);

            logInfo(tr("Used saved transaction for finalization"));
        } else {
            // Нет ни черновика, ни spending hash
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("No data transactions"),
                tr("Create a draft, or use Create and sign."));
            return;
        }
    }

    if (currentSpendingHash_.isEmpty() || currentMultisigAddress_.isEmpty() ||
        !walletModel_ || !walletModel_->getChainState()) {
        GUIUtil::showStyledMessageBox(
            this, QMessageBox::Warning, tr("Signatures cannot be checked"),
            tr("Missing the spending hash, multisig address, or access to the active chain."));
        return;
    }
    QStringList inactiveTxids;
    QStringList allowedSigners;
    for (const QJsonValue& value : configParticipants_) {
        const QString address = value.toObject().value(QStringLiteral("address")).toString().trimmed();
        if (!address.isEmpty()) allowedSigners.append(address);
    }
    QSet<QString> activeSigners;
    for (const QString& txid : std::as_const(txids)) {
        QString signerAddress;
        if (!CommitmentScanner::isCommitmentActive(
                txid, currentSpendingHash_, currentMultisigAddress_,
                walletModel_->getChainState(), allowedSigners, &signerAddress) ||
            activeSigners.contains(signerAddress)) {
            inactiveTxids.append(txid);
        } else {
            activeSigners.insert(signerAddress);
        }
    }
    if (!inactiveTxids.isEmpty()) {
        for (const QString& txid : std::as_const(inactiveTxids)) {
            if (scannerMultisigFields_.contains(txid)) {
                onMultisigCommitmentInvalidated(txid, tr("unknown signer"));
            }
        }
        GUIUtil::showStyledMessageBox(
            this, QMessageBox::Warning, tr("Signatures were not confirmed"),
            tr("Some commitments are not in the active chain, do not match the draft or participants, or repeat a signer:\n%1\n\nWait for confirmation or scan again.")
                .arg(inactiveTxids.join(QLatin1Char('\n'))));
        return;
    }

    logInfo(tr("Finalization: waiting %1 TXID.").arg(txids.size()));
    setFinalizeInProgress(true);
    emit requestDraftFinalize(draftToFinalize, txids, finalizeBroadcastCheck_ && finalizeBroadcastCheck_->isChecked());
}

void MultisigPage::updateInitiatorState()
{
    bool hasConfig = configLoaded_;
    bool hasBalance = multisigBalanceKnown_;
    bool inputsReady = hasConfig &&
        hasBalance &&
        !receiverEdit_->text().trimmed().isEmpty() &&
        !amountEdit_->text().trimmed().isEmpty() &&
        !feeEdit_->text().trimmed().isEmpty();

    // Обе кнопки доступны при наличии всех данных
    if (createDraftButton_) {
        createDraftButton_->setEnabled(inputsReady && !draftInProgress_);
    }
    if (createAndSignButton_) {
        createAndSignButton_->setEnabled(inputsReady && !draftInProgress_);
    }

    // Кнопки для spending hash
    const bool hasHash = spendingHashEdit_ && !spendingHashEdit_->text().isEmpty();
    if (copySpendingHashButton_) copySpendingHashButton_->setEnabled(hasHash);
    if (showSpendingHashQRButton_) showSpendingHashQRButton_->setEnabled(hasHash);

    // Кнопки для черновика (старый способ)
    const bool hasDraft = draftJsonEdit_ && !draftJsonEdit_->toPlainText().isEmpty();
    if (saveDraftButton_) {
        saveDraftButton_->setEnabled(hasDraft);
    }
    if (copyDraftButton_) {
        copyDraftButton_->setEnabled(hasDraft);
    }
    updateCommitmentState();
}

void MultisigPage::updateCommitmentState()
{
    int filled = 0;
    for (QLineEdit *edit : std::as_const(commitmentEdits_)) {
        if (!edit->text().trimmed().isEmpty()) {
            filled++;
        }
    }
    const bool ready = filled >= initiatorMSpin_->value();
    if (finalizeButton_) {
        finalizeButton_->setEnabled(ready && !finalizeInProgress_);
    }
    if (finalizeBroadcastCheck_) {
        finalizeBroadcastCheck_->setEnabled(!finalizeInProgress_);
    }
}

void MultisigPage::updateMultisigCommitmentFields()
{
    // Получаем контейнер
    QWidget *container = findChild<QWidget*>("multisigCommitmentsContainer");
    if (!container) return;

    QGridLayout *grid = qobject_cast<QGridLayout*>(container->layout());
    if (!grid) return;

    // Очищаем старые поля
    QLayoutItem *item;
    while ((item = grid->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    commitmentEdits_.clear();
    scannerMultisigFields_.clear();

    // Создаем новые поля в зависимости от N
    int n = initiatorNSpin_ ? initiatorNSpin_->value() : 1;
    for (int i = 0; i < n; ++i) {
        QLabel *label = new QLabel(tr("TXID %1:").arg(i + 1), container);
        QLineEdit *edit = new QLineEdit(container);
        edit->setPlaceholderText(tr("Paste TXID signatures %1...").arg(i + 1));
        connect(edit, &QLineEdit::textChanged, this, &MultisigPage::onCommitmentChanged);

        commitmentEdits_.append(edit);
        grid->addWidget(label, i, 0);
        grid->addWidget(edit, i, 1);
    }

    // Обновляем информационную метку
    QLabel *infoLabel = findChild<QLabel*>("multisigCommitmentsInfo");
    if (infoLabel) {
        int m = initiatorMSpin_ ? initiatorMSpin_->value() : 1;
        infoLabel->setText(tr("Required signatures: %1 from %2").arg(m).arg(n));
    }
}

bool MultisigPage::loadConfigFromFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        configLoaded_ = false;
        return false;
    }
    QByteArray data = file.readAll();
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (doc.isNull() || !doc.isObject()) {
        configLoaded_ = false;
        return false;
    }
    QJsonObject obj = doc.object();
    resetMultisigOperation();
    {
        const QSignalBlocker blockM(initiatorMSpin_);
        const QSignalBlocker blockN(initiatorNSpin_);
        initiatorMSpin_->setValue(obj.value("m").toInt(1));
        initiatorNSpin_->setValue(obj.value("n").toInt(1));
    }
    updateMultisigCommitmentFields();
    currentMultisigAddress_ = obj.value("address").toString();
    multisigAddressLabel_->setText(tr("Address multisig: %1").arg(currentMultisigAddress_));

    QStringList participants;
    QJsonArray arr = obj.value("participants").toArray();
    configParticipants_ = arr;
    for (const QJsonValue& val : arr) {
        QJsonObject p = val.toObject();
        const QString addr = p.value("address").toString();
        if (p.contains("area")) {
            participants << QString("%1 (area: %2)")
                            .arg(addr)
                            .arg(p.value("area").toDouble(), 0, 'f', 8);
        } else {
            participants << addr;
        }
    }
    participantsEdit_->setPlainText(participants.join('\n'));
    configLoaded_ = true;

    // Loading a file must not modify the wallet. The explicit "Add to wallet"
    // action below the file selector performs the validated, durable save.
    logInfo(tr("Multisig configuration loaded. Add the address to the wallet to save it."));

    // Обновляем баланс после загрузки конфига
    updateMultisigBalance();

    return true;
}

bool MultisigPage::validateInitiatorInputs(QString& error,
                                           qint64* parsedAmount,
                                           qint64* parsedFee) const
{
    if (!configLoaded_) {
        error = tr("Multisig configuration is not loaded.\n\nLoad an existing configuration or create a new address.");
        return false;
    }

    if (currentMultisigAddress_.isEmpty()) {
        error = tr("The multisig address is missing.\n\nLoad a configuration or create a new address.");
        return false;
    }

    QString recipient = receiverEdit_->text().trimmed();
    if (recipient.isEmpty()) {
        error = tr("Enter a recipient address.");
        return false;
    }

    bool recipientOk = false;
    try {
        const geocoin::Address recipientAddress(recipient.toStdString());
        recipientOk = geocoin::crypto::verify_address(recipientAddress);
    } catch (const std::exception&) {
        recipientOk = false;
    }
    if (!recipientOk) {
        error = tr("The recipient address is not a valid GeoCoin address.");
        return false;
    }

    if (recipient == currentMultisigAddress_) {
        error = tr("The recipient is the multisig address itself.\n\nChoose a different address. Change is added automatically.");
        return false;
    }

    qint64 amountVal = 0;
    if (!GUIUtil::parseAmount(amountEdit_->text(), amountVal) || amountVal <= 0) {
        error = tr("Enter an amount greater than zero.\n\nExample: 10.5 or 0.001");
        return false;
    }

    if (amountVal < 1000) {
        error = tr("The amount is below the minimum of 0.00001000 GC.\n\nEntered: %1 GC").arg(GUIUtil::formatAmount(amountVal));
        return false;
    }

    qint64 feeVal = 0;
    if (!GUIUtil::parseAmount(feeEdit_->text(), feeVal) ||
        feeVal < WalletModel::multisigRelayFee(
            initiatorNSpin_ ? initiatorNSpin_->value() : 3,
            walletModel_ ? walletModel_->getMempool().get() : nullptr)) {
        error = tr("The fee is below the minimum of %1 GC.")
                    .arg(GUIUtil::formatAmount(WalletModel::multisigRelayFee(
                        initiatorNSpin_ ? initiatorNSpin_->value() : 3,
                        walletModel_ ? walletModel_->getMempool().get() : nullptr)));
        return false;
    }
    qint64 totalRequired = 0;
    if (!GUIUtil::addAmounts(amountVal, feeVal, totalRequired)) {
        error = tr("Amount plus fee is out of range.");
        return false;
    }

    if (multisigBalanceKnown_ && totalRequired > multisigAvailableBalance_) {
        const qint64 shortfall = totalRequired - multisigAvailableBalance_;
        error = tr("The multisig address does not have enough funds.\n\nRequired: %1 GC + %2 GC fee = %3 GC\nAvailable: %4 GC\nShortfall: %5 GC")
            .arg(GUIUtil::formatAmount(amountVal))
            .arg(GUIUtil::formatAmount(feeVal))
            .arg(GUIUtil::formatAmount(totalRequired))
            .arg(GUIUtil::formatAmount(multisigAvailableBalance_))
            .arg(GUIUtil::formatAmount(shortfall));
        return false;
    }

    if (initiatorMSpin_ && initiatorNSpin_) {
        int m = initiatorMSpin_->value();
        int n = initiatorNSpin_->value();

        if (m > n) {
            error = tr("Invalid multisig configuration.\n\nRequired signatures (M) cannot exceed participants (N).\nCurrent values: M=%1, N=%2").arg(m).arg(n);
            return false;
        }

        if (m < 1 || n < 1) {
            error = tr("Invalid multisig configuration.\n\nM and N must be greater than zero.\nCurrent values: M=%1, N=%2").arg(m).arg(n);
            return false;
        }

        if (n > 15) {
            error = tr("Too many participants.\n\nThe maximum is 15.\nEntered: %1").arg(n);
            return false;
        }
    }

    if (parsedAmount) *parsedAmount = amountVal;
    if (parsedFee) *parsedFee = feeVal;
    return true;
}

QString MultisigPage::computeDraftHash(const QJsonDocument& doc) const
{
    QByteArray raw = doc.toJson(QJsonDocument::Compact);
    QByteArray hash = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
    return hash.toHex();
}

void MultisigPage::handleDraftReady(const QString& draftJson, const QString& txHash)
{
    setDraftInProgress(false);
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(draftJson.toUtf8(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        handleDraftFailed(tr("Invalid draft JSON from wallet: %1").arg(parseError.errorString()));
        return;
    }
    resetMultisigOperation();
    currentDraftDoc_ = doc;
    currentDraftJson_ = draftJson;
    currentDraftHash_ = txHash;
    currentSpendingHash_ = doc.object().value(QStringLiteral("spending_tx_hash")).toString(txHash);
    currentMultisigSpendHex_ = doc.object().value(QStringLiteral("tx_hex")).toString();
    if (spendingHashEdit_) spendingHashEdit_->setText(currentSpendingHash_);
    draftJsonEdit_->setPlainText(doc.toJson(QJsonDocument::Indented));
    draftHashLabel_->setText(tr("Hash transactions: %1").arg(txHash));
    saveDraftButton_->setEnabled(true);
    copyDraftButton_->setEnabled(true);

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Draft created"),
        tr("Draft successfully created. Send JSON signers."));

    logInfo(tr("Draft created (hash %1).").arg(txHash));
    addHistoryEntry("Draft",
                    tr("Successfully"),
                    tr("Path: %1\nHash: %2").arg(configPathEdit_->text().trimmed(), txHash),
                    draftJson);
}

void MultisigPage::handleCreateAndSignReady(const QString& spendingHash, const QString& myCommitmentTxid, const QString& multisigSpendHex)
{
    setDraftInProgress(false);

    // A scanner is immutable for one operation. Retire its state and every
    // commitment from the previous target before accepting a new one.
    resetMultisigOperation();

    // Сохраняем spending hash и транзакцию
    currentSpendingHash_ = spendingHash;
    currentMultisigSpendHex_ = multisigSpendHex;
    if (spendingHashEdit_) {
        spendingHashEdit_->setText(spendingHash);
    }

    // Вставляем свою подпись в первое поле commitments
    if (!commitmentEdits_.isEmpty()) {
        commitmentEdits_[0]->setText(myCommitmentTxid);
        commitmentEdits_[0]->setStyleSheet(GUIUtil::themedSuccessEditableStyleSheet());
    }

    // ВАЖНО: Сохраняем незавершенную транзакцию
    savePendingTransaction();

    // Обновляем состояние кнопок
    updateInitiatorState();

    // Автоматически включаем автосканирование
    if (autoScanMultisigCheck_) {
        if (!autoScanMultisigCheck_->isChecked()) {
            autoScanMultisigCheck_->setChecked(true);
        } else {
            onAutoScanMultisigToggled(true);
        }
    }

    // Показываем информационное сообщение
    QString message = tr("✅ Your commitment sent in blockchain!\n\nTXID: %1\n\nSpending hash for other participants:\n%2\n\n📋 Send spending hash other participants.\nThey can sign its on tab \"Signer\".\n\n🔄 Auto-scanning started!\nSystem automatically will collect signatures from blockchain.")
        .arg(myCommitmentTxid, spendingHash);

    GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Commitment created"), message);

    logInfo(tr("✅ Your commitment sent! TXID: %1").arg(myCommitmentTxid));
    logInfo(tr("📋 Spending hash: %1").arg(spendingHash));
    logInfo(tr("🔄 Auto-scanning started"));
    logInfo(tr("⏳ Waiting signatures from other participants..."));

    // Сохраняем в историю
    addHistoryEntry("CreateAndSign", tr("Successfully"),
                   tr("Spending hash: %1\nCommitment TXID: %2").arg(spendingHash, myCommitmentTxid),
                   spendingHash);
}

void MultisigPage::handleFinalizeReady(const QString& finalHex, const QString& broadcastTxid)
{
    setFinalizeInProgress(false);

    // КРИТИЧНО (UX/безопасность): после успешной финализации очищаем все подписанные/временные поля,
    // чтобы нельзя было случайно повторно "финализировать" следующую транзакцию старыми TXID/данными.
    currentDraftHash_.clear();
    currentSpendingHash_.clear();
    currentMultisigSpendHex_.clear();
    currentDraftDoc_ = QJsonDocument();
    currentDraftJson_.clear();
    draftJsonEdit_->clear();
    draftHashLabel_->setText(tr("Hash transactions: —"));
    saveDraftButton_->setEnabled(false);
    copyDraftButton_->setEnabled(false);

    if (spendingHashEdit_) {
        QSignalBlocker blocker(spendingHashEdit_);
        spendingHashEdit_->clear();
    }

    // Очищаем TXID commitments
    for (QLineEdit *edit : std::as_const(commitmentEdits_)) {
        if (!edit) continue;
        QSignalBlocker blocker(edit);
        edit->clear();
    }

    // Останавливаем автоскан после успешной отправки
    if (autoScanMultisigCheck_ && autoScanMultisigCheck_->isChecked()) {
        autoScanMultisigCheck_->setChecked(false);
    }
    resetMultisigScanner(false);
    if (scanStatusMultisigLabel_) {
        scanStatusMultisigLabel_->setText(tr("Status: —"));
    }

    updateCommitmentState();
    updateInitiatorState();

    // ВАЖНО: Удаляем незавершенную транзакцию после успешной финализации
    clearPendingTransaction();

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Final transaction ready"),
        broadcastTxid.isEmpty()
            ? tr("Transaction formed.\nHEX:\n%1").arg(finalHex)
            : tr("Transaction sent.\nTXID: %1").arg(broadcastTxid));

    logInfo(broadcastTxid.isEmpty()
                ? tr("Final transaction formed.")
                : tr("Final transaction sent (txid %1).").arg(broadcastTxid));
    addHistoryEntry("Finalize",
                    broadcastTxid.isEmpty() ? tr("Prepared") : tr("Sent"),
                    broadcastTxid.isEmpty() ? tr("HEX length: %1").arg(finalHex.length()) : tr("TXID: %1").arg(broadcastTxid),
                    finalHex);
}

QWidget* MultisigPage::buildInitiatorPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setSpacing(15);

    layout->addWidget(buildInitiatorConfigBox());
    layout->addWidget(buildInitiatorTxBox());
    layout->addWidget(buildDraftInfoBox());
    layout->addWidget(buildCommitmentsBox());
    layout->addStretch();
    return page;
}

QGroupBox* MultisigPage::buildInitiatorConfigBox()
{
    QGroupBox *box = new QGroupBox(tr("Configuration multisig"), this);
    QGridLayout *grid = new QGridLayout(box);
    grid->setColumnStretch(1, 1);

    // Секция 0: Сохраненные мультиподпись адреса
    savedMultisigCombo_ = new QComboBox(box);
    savedMultisigCombo_->addItem(tr("-- Select saved address --"), "");
    connect(savedMultisigCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MultisigPage::onSavedMultisigSelected);

    grid->addWidget(new QLabel(tr("Saved addresses:")), 0, 0);
    grid->addWidget(savedMultisigCombo_, 0, 1, 1, 2);

    // Разделитель
    QFrame *line0 = new QFrame(box);
    line0->setFrameShape(QFrame::HLine);
    line0->setFrameShadow(QFrame::Sunken);
    grid->addWidget(line0, 1, 0, 1, 3);

    // Секция 1: Загрузка существующего конфига
    configPathEdit_ = new QLineEdit(box);
    QPushButton *browseBtn = new QPushButton(tr("Load config"), box);
    connect(browseBtn, &QPushButton::clicked, this, &MultisigPage::onBrowseConfig);

    QPushButton *addToWalletBtn = new QPushButton(tr("➕ Add to wallet"), box);
    addToWalletBtn->setToolTip(tr("Add current multisig address in wallet for tracking"));
    connect(addToWalletBtn, &QPushButton::clicked, this, &MultisigPage::onAddMultisigToWallet);

    grid->addWidget(new QLabel(tr("File configuration:")), 2, 0);
    grid->addWidget(configPathEdit_, 2, 1);

    QHBoxLayout *configButtonsLayout = new QHBoxLayout();
    configButtonsLayout->addWidget(browseBtn);
    configButtonsLayout->addWidget(addToWalletBtn);
    configButtonsLayout->setSpacing(5);
    QWidget *configButtonsWidget = new QWidget(box);
    configButtonsWidget->setLayout(configButtonsLayout);
    grid->addWidget(configButtonsWidget, 2, 2);

    // Разделитель
    QFrame *line1 = new QFrame(box);
    line1->setFrameShape(QFrame::HLine);
    line1->setFrameShadow(QFrame::Sunken);
    grid->addWidget(line1, 3, 0, 1, 3);

    // Секция 2: Создание нового адреса
    QLabel *createLabel = new QLabel(tr("<b>Or create new address:</b>"), box);
    grid->addWidget(createLabel, 4, 0, 1, 3);

    initiatorMSpin_ = new QSpinBox(box);
    initiatorMSpin_->setRange(1, 15);
    initiatorMSpin_->setValue(2);
    initiatorNSpin_ = new QSpinBox(box);
    initiatorNSpin_->setRange(1, 15);
    initiatorNSpin_->setValue(3);

    connect(initiatorMSpin_, QOverload<int>::of(&QSpinBox::valueChanged), [this]() {
        resetMultisigOperation();
        updateMultisigCommitmentFields();
        updateInitiatorState();
    });
    connect(initiatorNSpin_, QOverload<int>::of(&QSpinBox::valueChanged), [this]() {
        resetMultisigOperation();
        updateMultisigCommitmentFields();
        updateInitiatorState();
    });

    grid->addWidget(new QLabel(tr("Type:")), 5, 0);
    QHBoxLayout *mnLayout = new QHBoxLayout();
    mnLayout->addWidget(initiatorMSpin_);
    mnLayout->addWidget(new QLabel(tr("from"), box));
    mnLayout->addWidget(initiatorNSpin_);
    mnLayout->addStretch();
    QWidget *mnWidget = new QWidget(box);
    mnWidget->setLayout(mnLayout);
    grid->addWidget(mnWidget, 5, 1, 1, 2);

    participantsEdit_ = new QTextEdit(box);
    participantsEdit_->setPlaceholderText(tr("Enter addresses participants (one on line):\nGEO1abc...xyz\nGEO2def...uvw\n..."));
    participantsEdit_->setMinimumHeight(100);
    participantsEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(participantsEdit_, &QTextEdit::textChanged, this, &MultisigPage::updateInitiatorState);

    grid->addWidget(new QLabel(tr("Addresses participants:")), 6, 0);
    grid->addWidget(participantsEdit_, 6, 1, 1, 2);
    grid->setRowStretch(6, 1);

    QPushButton *createAddressBtn = new QPushButton(tr("Create multisig address"), box);
    createAddressBtn->setStyleSheet("QPushButton { background-color: #3498db; color: white; font-weight: bold; padding: 8px; }");
    connect(createAddressBtn, &QPushButton::clicked, this, &MultisigPage::onCreateMultisigAddress);
    grid->addWidget(createAddressBtn, 7, 0, 1, 3);

    // Результат
    multisigAddressLabel_ = new QLabel(tr("Address multisig: —"), box);
    multisigAddressLabel_->setStyleSheet("QLabel { color: #27ae60; font-weight: bold; }");
    multisigAddressLabel_->setWordWrap(true);
    grid->addWidget(multisigAddressLabel_, 8, 0, 1, 3);

    applyGeoTheme(box);
    return box;
}

QGroupBox* MultisigPage::buildInitiatorTxBox()
{
    QGroupBox *box = new QGroupBox(tr("Settings transactions"), this);
    QFormLayout *form = new QFormLayout(box);

    receiverEdit_ = new QLineEdit(box);
    amountEdit_ = new QLineEdit(box);
    feeEdit_ = new QLineEdit(box);
    feeEdit_->setText(GUIUtil::formatAmount(WalletModel::multisigRelayFee(
        initiatorNSpin_ ? initiatorNSpin_->value() : 3, nullptr)));

    balanceLabel_ = new QLabel(tr("Available balance: —"), box);

    // Economic inputs are part of the signed target. Editing them after draft
    // creation must retire that operation instead of leaving a ready button
    // which would finalize the old recipient/amount.
    const auto semanticInputChanged = [this]() {
        if (!currentSpendingHash_.isEmpty() || !currentDraftJson_.isEmpty()) {
            resetMultisigOperation();
        }
        updateInitiatorState();
    };
    connect(receiverEdit_, &QLineEdit::textChanged, this, semanticInputChanged);
    connect(amountEdit_, &QLineEdit::textChanged, this, semanticInputChanged);
    connect(feeEdit_, &QLineEdit::textChanged, this, semanticInputChanged);

    form->addRow(tr("Recipient:"), receiverEdit_);
    form->addRow(tr("Amount (GC):"), amountEdit_);
    form->addRow(tr("Fee (GC):"), feeEdit_);
    form->addRow(balanceLabel_);

    // Две кнопки: старый способ (черновик) и новый (создать и подписать)
    QHBoxLayout *buttonRow = new QHBoxLayout();

    createDraftButton_ = new QPushButton(tr("Create draft"), box);
    createDraftButton_->setStyleSheet("QPushButton { background-color: #95a5a6; color: white; font-weight: bold; padding: 8px; }");
    createDraftButton_->setEnabled(false);
    createDraftButton_->setToolTip(tr("Old method: create JSON draft for manual transfer"));
    connect(createDraftButton_, &QPushButton::clicked, this, &MultisigPage::onCreateDraft);
    buttonRow->addWidget(createDraftButton_);

    createAndSignButton_ = new QPushButton(tr("Create and sign"), box);
    createAndSignButton_->setStyleSheet("QPushButton { background-color: #3498db; color: white; font-weight: bold; padding: 8px; }");
    createAndSignButton_->setEnabled(false);
    createAndSignButton_->setToolTip(tr("New method: create, sign and publish commitment in blockchain"));
    connect(createAndSignButton_, &QPushButton::clicked, this, &MultisigPage::onCreateAndSignMultisig);
    buttonRow->addWidget(createAndSignButton_);

    form->addRow(buttonRow);

    applyGeoTheme(box);
    return box;
}

QGroupBox* MultisigPage::buildDraftInfoBox()
{
    QGroupBox *box = new QGroupBox(tr("Draft transactions"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    // Секция для spending hash (новый способ)
    QLabel *hashSectionLabel = new QLabel(tr("Spending Hash (for automatic collection):"), box);
    hashSectionLabel->setStyleSheet("QLabel { font-weight: bold; color: #2980b9; }");
    layout->addWidget(hashSectionLabel);

    spendingHashEdit_ = new QLineEdit(box);
    spendingHashEdit_->setReadOnly(true);
    spendingHashEdit_->setPlaceholderText(tr("Spending hash will appear here after creation and signing..."));
    spendingHashEdit_->setStyleSheet(GUIUtil::themedMonoEditableStyleSheet());
    layout->addWidget(spendingHashEdit_);

    QHBoxLayout *hashButtonLayout = new QHBoxLayout();
    copySpendingHashButton_ = new QPushButton(tr("📋 Copy hash"), box);
    copySpendingHashButton_->setEnabled(false);
    copySpendingHashButton_->setToolTip(tr("Copy spending hash for sending other signers"));
    connect(copySpendingHashButton_, &QPushButton::clicked, [this]() {
        if (!spendingHashEdit_ || spendingHashEdit_->text().isEmpty()) {
            return;
        }
        QString hash = spendingHashEdit_->text();
        QApplication::clipboard()->setText(hash);
        logInfo(tr("Spending hash copied in buffer exchange"));

        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Information,
            tr("Hash copied"),
            tr("Spending hash copied in buffer exchange:\n\n%1\n\nSend its other participants for signing.").arg(hash));
    });
    hashButtonLayout->addWidget(copySpendingHashButton_);

    showSpendingHashQRButton_ = new QPushButton(tr("📱 QR-code"), box);
    showSpendingHashQRButton_->setEnabled(false);
    showSpendingHashQRButton_->setToolTip(tr("Show QR-code for scanning"));
    connect(showSpendingHashQRButton_, &QPushButton::clicked, [this]() {
        if (!spendingHashEdit_ || spendingHashEdit_->text().isEmpty()) {
            return;
        }
        QString hash = spendingHashEdit_->text();
        GUIUtil::showQRCodeDialog(this,
            tr("QR-code Spending Hash"),
            hash,
            tr("Scan this QR-code for receiving spending hash"));
        logInfo(tr("Shown QR-code for spending hash"));
    });
    hashButtonLayout->addWidget(showSpendingHashQRButton_);
    hashButtonLayout->addStretch();
    layout->addLayout(hashButtonLayout);

    QLabel *hashInfoLabel = new QLabel(tr("Send the spending hash to the other participants so they can sign."), box);
    hashInfoLabel->setWordWrap(true);
    hashInfoLabel->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    layout->addWidget(hashInfoLabel);

    // Разделитель
    QFrame *separator = new QFrame(box);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    // Секция для черновика (старый способ)
    QLabel *draftSectionLabel = new QLabel(tr("JSON Draft (old method):"), box);
    draftSectionLabel->setStyleSheet("QLabel { font-weight: bold; color: #95a5a6; }");
    layout->addWidget(draftSectionLabel);

    draftHashLabel_ = new QLabel(tr("Hash transactions: —"), box);
    layout->addWidget(draftHashLabel_);

    draftJsonEdit_ = new QTextEdit(box);
    draftJsonEdit_->setPlaceholderText(tr("{ JSON draft will appear here }"));
    draftJsonEdit_->setReadOnly(true);
    draftJsonEdit_->setMinimumHeight(120);
    draftJsonEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(draftJsonEdit_);

    QHBoxLayout *buttons = new QHBoxLayout();
    saveDraftButton_ = new QPushButton(tr("Save in file"), box);
    copyDraftButton_ = new QPushButton(tr("Copy in buffer"), box);
    saveDraftButton_->setEnabled(false);
    copyDraftButton_->setEnabled(false);
    connect(saveDraftButton_, &QPushButton::clicked, this, &MultisigPage::onSaveDraft);
    connect(copyDraftButton_, &QPushButton::clicked, this, &MultisigPage::onCopyDraftJson);

    buttons->addWidget(saveDraftButton_);
    buttons->addWidget(copyDraftButton_);
    buttons->addStretch();
    layout->addLayout(buttons);

    applyGeoTheme(box);
    return box;
}

QGroupBox* MultisigPage::buildCommitmentsBox()
{
    QGroupBox *box = new QGroupBox(tr("Collection signatures from participants"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    QLabel *infoLabel = new QLabel(tr("Required signatures: —"), box);
    infoLabel->setObjectName("multisigCommitmentsInfo");
    layout->addWidget(infoLabel);

    // Создаем контейнер для динамических полей
    QWidget *commitmentsContainer = new QWidget(box);
    commitmentsContainer->setObjectName("multisigCommitmentsContainer");
    QGridLayout *grid = new QGridLayout(commitmentsContainer);
    grid->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(commitmentsContainer);

    // Изначально создаем поля для текущего N
    updateMultisigCommitmentFields();

    // Разделитель
    QFrame *separator = new QFrame(box);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    // Секция автоматического сканирования
    QLabel *scanLabel = new QLabel(tr("<b>Automatic collection signatures:</b>"), box);
    layout->addWidget(scanLabel);

    autoScanMultisigCheck_ = new QCheckBox(tr("🔄 Auto-scanning blockchain"), box);
    autoScanMultisigCheck_->setToolTip(tr("Automatically search commitments in new blocks (interval: 30 sec)"));
    autoScanMultisigCheck_->setChecked(false);
    connect(autoScanMultisigCheck_, &QCheckBox::toggled, this, &MultisigPage::onAutoScanMultisigToggled);
    layout->addWidget(autoScanMultisigCheck_);

    QHBoxLayout *scanButtonLayout = new QHBoxLayout();
    scanNowMultisigButton_ = new QPushButton(tr("🔍 Check now"), box);
    scanNowMultisigButton_->setToolTip(tr("Check blockchain directly now (without auto-scanning)"));
    scanNowMultisigButton_->setEnabled(false);
    connect(scanNowMultisigButton_, &QPushButton::clicked, this, &MultisigPage::onScanNowMultisig);
    scanButtonLayout->addWidget(scanNowMultisigButton_);
    scanButtonLayout->addStretch();
    layout->addLayout(scanButtonLayout);

    scanStatusMultisigLabel_ = new QLabel(tr("Status: ready to scan"), box);
    scanStatusMultisigLabel_->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    scanStatusMultisigLabel_->setWordWrap(true);
    layout->addWidget(scanStatusMultisigLabel_);

    QLabel *scanInfoLabel = new QLabel(tr("Scanning starts automatically after Create and sign."), box);
    scanInfoLabel->setWordWrap(true);
    scanInfoLabel->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 10px; }");
    layout->addWidget(scanInfoLabel);

    // Разделитель
    QFrame *separator2 = new QFrame(box);
    separator2->setFrameShape(QFrame::HLine);
    separator2->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator2);

    // Финализация
    finalizeBroadcastCheck_ = new QCheckBox(tr("Send final transaction immediately"), box);
    finalizeBroadcastCheck_->setChecked(true);
    layout->addWidget(finalizeBroadcastCheck_);

    finalizeButton_ = new QPushButton(tr("Finalize and send"), box);
    finalizeButton_->setStyleSheet("QPushButton { background-color: #27ae60; color: white; font-weight: bold; padding: 10px; }");
    finalizeButton_->setEnabled(false);
    connect(finalizeButton_, &QPushButton::clicked, this, &MultisigPage::onFinalizeDraft);
    layout->addWidget(finalizeButton_);

    applyGeoTheme(box);
    return box;
}

void MultisigPage::onBrowseConfig()
{
    QString file = QFileDialog::getOpenFileName(this, tr("Select configuration multisig"), QString(), tr("JSON files (*.json);;All files (*)"));
    if (!file.isEmpty()) {
        configPathEdit_->setText(file);
        if (!loadConfigFromFile(file)) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("Configuration error"),
                tr("Could not read the configuration file."));
        }
    }
    updateInitiatorState();
}

void MultisigPage::onAddMultisigToWallet()
{
    if (currentMultisigAddress_.isEmpty()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("No addresses"),
            tr("Load a configuration or create a multisig address first."));
        return;
    }

    if (!walletModel_ || !walletModel_->isWalletLoaded()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Wallet not loaded"),
            tr("Create or load a wallet before saving the multisig address."));
        return;
    }

    // Запрашиваем метку у пользователя
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Add address to wallet"));
    dialog.setLabelText(tr("Label for %1:").arg(currentMultisigAddress_));
    dialog.setTextValue(QString("%1-of-%2 Multisig").arg(initiatorMSpin_->value()).arg(initiatorNSpin_->value()));
    GUIUtil::applyDialogStyle(&dialog);

    if (dialog.exec() != QDialog::Accepted || dialog.textValue().isEmpty()) {
        return;
    }

    QString label = dialog.textValue();

    // Создаем конфигурацию
    geocoin::wallet::MultisigConfig config;
    config.m = initiatorMSpin_->value();
    config.n = initiatorNSpin_->value();
    config.address = geocoin::Address(currentMultisigAddress_.toStdString());

    // Добавляем участников из сохраненного JSON
    for (const QJsonValue& val : configParticipants_) {
        QJsonObject p = val.toObject();
        QString addrStr = p.value("address").toString();
        geocoin::Address addr(addrStr.toStdString());
        config.addresses.push_back(addr);
        config.participants.emplace_back(addr);
    }

    // Добавляем в кошелек. The wallet operation is idempotent, so adding an
    // already known address refreshes its configuration rather than failing.
    QString rememberError;
    if (walletModel_->rememberMultisigConfig(config, &rememberError)) {

        if (!saveWalletChanges()) return;

        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Information,
            tr("Success"),
            tr("Multisig address added in wallet!\n\nAddress: %1\nLabel: %2\n\nNow you can track balance and create transactions with this addresses.")
                .arg(currentMultisigAddress_)
                .arg(label));

        logInfo(tr("Address %1 added in wallet").arg(currentMultisigAddress_));

        // Обновляем список сохраненных адресов
        loadSavedMultisigAddresses();
    } else {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("Could not add the multisig address to the wallet:\n%1")
                .arg(rememberError.isEmpty()
                    ? tr("Unknown error")
                    : rememberError));
        logError(tr("Could not add address %1: %2")
                     .arg(currentMultisigAddress_, rememberError));
    }
}

void MultisigPage::onCreateMultisigAddress()
{
    // Получаем параметры
    uint8_t m = static_cast<uint8_t>(initiatorMSpin_->value());
    uint8_t n = static_cast<uint8_t>(initiatorNSpin_->value());

    // Парсим адреса из текстового поля
    QString participantsText = participantsEdit_->toPlainText();
    QStringList lines = participantsText.split('\n', Qt::SkipEmptyParts);

    QStringList addresses;
    for (const QString& line : lines) {
        QString addr = line.trimmed();
        // Убираем комментарии
        if (addr.startsWith('#')) continue;
        // Извлекаем только адрес (если есть дополнительная информация)
        if (addr.contains(' ')) {
            addr = addr.split(' ').first();
        }
        if (!addr.isEmpty() && addr.startsWith("GEO")) {
            addresses.append(addr);
        }
    }

    // Валидация
    if (addresses.size() != n) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("Pending %1 addresses, found %2.\n\nEnter by one address on line.")
                .arg(n).arg(addresses.size()));
        return;
    }

    if (m > n) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("M not can be more N.\nM=%1, N=%2").arg(m).arg(n));
        return;
    }

    // Создаем мультиподпись адрес через библиотеку
    try {
        std::vector<geocoin::Address> addrVec;
        for (const QString& addr : addresses) {
            addrVec.emplace_back(addr.toStdString());
        }

        geocoin::wallet::MultisigAddress multisig =
            geocoin::wallet::create_multisig_address(m, n, addrVec);

        resetMultisigOperation();
        currentMultisigAddress_ = QString::fromStdString(multisig.address);
        multisigAddressLabel_->setText(tr("✅ Address multisig: %1").arg(currentMultisigAddress_));

        // Сохраняем конфигурацию в JSON
        QJsonObject configObj;
        configObj["address"] = currentMultisigAddress_;
        configObj["m"] = m;
        configObj["n"] = n;

        QJsonArray participantsArray;
        for (const QString& addr : addresses) {
            QJsonObject p;
            p["address"] = addr;
            participantsArray.append(p);
        }
        configObj["participants"] = participantsArray;

        configParticipants_ = participantsArray;

        // Предлагаем сохранить конфигурацию
        QString suggestedName = QString("multisig_%1.json")
            .arg(currentMultisigAddress_.left(10));

        QString saveFile = QFileDialog::getSaveFileName(
            this,
            tr("Save configuration multisig"),
            suggestedName,
            tr("JSON files (*.json);;All files (*)")
        );

        if (!saveFile.isEmpty()) {
            const QJsonDocument doc(configObj);
            QString writeError;
            if (GUIUtil::writeFileAtomically(
                    saveFile, doc.toJson(QJsonDocument::Indented), writeError)) {

                configPathEdit_->setText(saveFile);
                configLoaded_ = true;

                // Добавляем мультиподпись адрес в кошелек для отслеживания
                if (walletModel_ && walletModel_->getWallet()) {
                    // Создаем конфигурацию
                    geocoin::wallet::MultisigConfig config;
                    config.m = m;
                    config.n = n;
                    config.address = geocoin::Address(currentMultisigAddress_.toStdString());

                    // Добавляем участников
                    for (const auto& addr : addrVec) {
                        config.addresses.push_back(addr);
                        config.participants.emplace_back(addr);
                    }

                    QString rememberError;
                    if (!walletModel_->rememberMultisigConfig(config, &rememberError)) {
                        GUIUtil::showStyledMessageBox(
                            this,
                            QMessageBox::Warning,
                            tr("Could not save the address"),
                            tr("The configuration was created but was not added to the wallet:\n%1")
                                .arg(rememberError));
                        return;
                    }
                    if (!saveWalletChanges()) return;
                }

                GUIUtil::showStyledMessageBox(
                    this,
                    QMessageBox::Information,
                    tr("Success"),
                    tr("Multisig address created!\n\nAddress: %1\nType: %2-of-%3\n\nConfiguration saved in:\n%4\n\nSend this file all participants!")
                        .arg(currentMultisigAddress_)
                        .arg(m).arg(n)
                        .arg(saveFile));

                logInfo(tr("Created multisig address %1 (%2-of-%3)")
                    .arg(currentMultisigAddress_).arg(m).arg(n));
            } else {
                GUIUtil::showStyledMessageBox(
                    this,
                    QMessageBox::Warning,
                    tr("Error"),
                    tr("Could not save the configuration file: %1").arg(writeError));
            }
        } else {
            // Пользователь отменил сохранение, но адрес создан
            configLoaded_ = true;
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Information,
                tr("Address created"),
                tr("Multisig address created:\n%1\n\nSave the configuration file before leaving this page.")
                    .arg(currentMultisigAddress_));
        }

        updateInitiatorState();
        updateMultisigBalance();  // Обновляем баланс после создания адреса

    } catch (const std::exception& e) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Critical,
            tr("Could not create the address"),
            tr("Could not create the multisig address:\n%1")
                .arg(QString::fromStdString(e.what())));

        logError(tr("Could not create the multisig address: %1")
            .arg(QString::fromStdString(e.what())));
    }
}

void MultisigPage::onLoadDraftJson()
{
    QString file = QFileDialog::getOpenFileName(this, tr("Load draft"), QString(), tr("JSON files (*.json);;All files (*)"));
    if (file.isEmpty()) {
        return;
    }
    QFile draftFile(file);
    if (!draftFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Could not open the file draft"));
        return;
    }
    signerDraftEdit_->setPlainText(QString::fromUtf8(draftFile.readAll()));
    onSignerDraftChanged();
}

void MultisigPage::onSaveDraft()
{
    if (draftJsonEdit_->toPlainText().isEmpty()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Empty draft"), tr("Create a draft first."));
        return;
    }
    QString file = QFileDialog::getSaveFileName(this, tr("Save draft"), QStringLiteral("multisig_draft.json"), tr("JSON files (*.json);;All files (*)"));
    if (file.isEmpty()) {
        return;
    }
    QString error;
    if (!GUIUtil::writeFileAtomically(file, draftJsonEdit_->toPlainText().toUtf8(), error)) {
        GUIUtil::showStyledMessageBox(
            this, QMessageBox::Warning, tr("Error"),
            tr("Could not save the file: %1").arg(error));
        return;
    }
}

void MultisigPage::onCopyDraftJson()
{
    if (draftJsonEdit_->toPlainText().isEmpty()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Empty draft"), tr("No data for copying."));
        return;
    }
    QApplication::clipboard()->setText(draftJsonEdit_->toPlainText());

    GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Copied"), tr("Draft copied in buffer exchange."));
}

void MultisigPage::onSavedMultisigSelected(int index) {
    if (index <= 0 || !savedMultisigCombo_ || !walletModel_ || !walletModel_->getWallet()) {
        return;
    }

    QString address = savedMultisigCombo_->itemData(index).toString();
    if (address.isEmpty()) {
        return;
    }

    // Получаем конфигурацию из кошелька
    auto configs = walletModel_->getWallet()->get_multisig_addresses();
    geocoin::wallet::MultisigConfig* foundConfig = nullptr;

    for (auto& config : configs) {
        if (QString::fromStdString(config.address.str()) == address) {
            foundConfig = &config;
            break;
        }
    }

    if (!foundConfig) {
        logInfo(tr("Error: configuration not found for addresses %1").arg(address));
        return;
    }

    resetMultisigOperation();

    currentMultisigAddress_ = address;
    multisigAddressLabel_->setText(tr("✅ Address multisig: %1").arg(currentMultisigAddress_));
    configLoaded_ = true;

    // Устанавливаем M и N
    if (foundConfig->m > 0 && foundConfig->n > 0) {
        {
            const QSignalBlocker blockM(initiatorMSpin_);
            const QSignalBlocker blockN(initiatorNSpin_);
            initiatorMSpin_->setValue(foundConfig->m);
            initiatorNSpin_->setValue(foundConfig->n);
        }
        updateMultisigCommitmentFields();

        // Сохраняем участников в configParticipants_
        QJsonArray participantsArray;
        for (const auto& participant : foundConfig->participants) {
            QJsonObject p;
            p["address"] = QString::fromStdString(participant.address.str());
            participantsArray.append(p);
        }
        configParticipants_ = participantsArray;

        logInfo(tr("Loaded saved address: %1 (%2-of-%3)").arg(address).arg(foundConfig->m).arg(foundConfig->n));
    } else {
        logInfo(tr("Loaded saved address: %1 (M/N unknown - old format)").arg(address));
    }

    updateMultisigBalance();
}

void MultisigPage::loadSavedMultisigAddresses() {
    if (!savedMultisigCombo_ || !walletModel_ || !walletModel_->getWallet()) {
        if (::geocoin::qt::debugEnabled()) {
            GEO_QT_LOG_DEBUG << "[MultisigPage] loadSavedMultisigAddresses: combo or wallet not ready";
        }
        return;
    }

    // Очищаем список
    savedMultisigCombo_->clear();
    savedMultisigCombo_->addItem(tr("-- Select saved address --"), "");

    // Загружаем сохраненные адреса из кошелька
    auto configs = walletModel_->getWallet()->get_multisig_addresses();
    if (::geocoin::qt::debugEnabled()) {
        GEO_QT_LOG_DEBUG << "[MultisigPage] Loaded" << configs.size() << "multisig addresses from wallet";
    }

    for (const auto& config : configs) {
        QString address = QString::fromStdString(config.address.str());
        QString displayText = QString("%1-of-%2: %3...")
            .arg(config.m)
            .arg(config.n)
            .arg(address.left(20));
        savedMultisigCombo_->addItem(displayText, address);
        if (::geocoin::qt::debugEnabled()) {
            GEO_QT_LOG_DEBUG << "[MultisigPage] Added address:" << address.left(30) << "M=" << config.m << "N=" << config.n;
        }
    }

    if (configs.empty()) {
        logInfo(tr("No saved multisig addresses"));
    } else {
        logInfo(tr("Loaded %1 saved addresses").arg(configs.size()));
    }
}

void MultisigPage::updateMultisigBalance()
{
    multisigBalanceKnown_ = false;
    multisigAvailableBalance_ = 0;

    if (!walletModel_ || !walletModel_->getChainState()) {
        balanceLabel_->setText(tr("Available balance: —"));
        updateInitiatorState();
        return;
    }

    if (currentMultisigAddress_.isEmpty()) {
        balanceLabel_->setText(tr("Available balance: —"));
        updateInitiatorState();
        return;
    }

    try {
        const geocoin::Address multisigAddr(currentMultisigAddress_.toStdString());
        const auto funds = geocoin::wallet::classify_address_funds(
            *walletModel_->getChainState(),
            multisigAddr,
            walletModel_->pendingTransactions());
        if (!funds.valid) {
            balanceLabel_->setText(tr("Available balance: invalid data"));
            static bool accountingErrorLogged = false;
            if (!accountingErrorLogged) {
                logError(tr("Multisig balance was not calculated: negative value or overflow."));
                accountingErrorLogged = true;
            }
            updateInitiatorState();
            return;
        }

        multisigAvailableBalance_ = funds.available;
        multisigBalanceKnown_ = true;
        QString labelText = tr("Available balance: %1 GC")
            .arg(GUIUtil::formatAmount(funds.available));
        if (funds.unconfirmed > 0) {
            labelText += tr(" (+%1 unconfirmed.)").arg(GUIUtil::formatAmount(funds.unconfirmed));
        }
        if (funds.immature > 0) {
            labelText += tr(" (+%1 immature.)").arg(GUIUtil::formatAmount(funds.immature));
        }
        balanceLabel_->setText(labelText);

        // НЕ выводим в лог при каждом обновлении - только обновляем label

    } catch (const std::exception& e) {
        balanceLabel_->setText(tr("Available balance: error"));
        // Ошибку выводим только один раз
        static QString lastError;
        QString currentError = QString::fromStdString(e.what());
        if (lastError != currentError) {
            logError(tr("Error receiving balance: %1").arg(currentError));
            lastError = currentError;
        }
    }

    updateInitiatorState();
}

// ============================================================================
// Scanner Handlers для Multisig
// ============================================================================
