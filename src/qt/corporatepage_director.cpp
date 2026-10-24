#include "corporatepage_internal.h"

void CorporatePage::resetHierarchicalScanner(bool clearCommitments)
{
    if (hierarchicalScanner_) {
        hierarchicalScanner_->stopAutoScan();
        delete hierarchicalScanner_;
        hierarchicalScanner_ = nullptr;
    }
    hierarchicalScannerTarget_.clear();
    hierarchicalScannerAddress_.clear();
    hierarchicalScannerRequired_ = 0;
    scannerHierarchicalFields_.clear();
    if (clearCommitments) {
        for (QLineEdit* edit : std::as_const(hierarchicalCommitmentEdits_)) {
            if (!edit) continue;
            edit->clear();
            edit->setStyleSheet(GUIUtil::themedEditableStyleSheet());
        }
        updateHierarchicalCommitmentState();
    }
}

void CorporatePage::resetHierarchicalOperation()
{
    if (autoScanHierarchicalCheck_ && autoScanHierarchicalCheck_->isChecked()) {
        autoScanHierarchicalCheck_->setChecked(false);
    }
    resetHierarchicalScanner(true);
    currentHierarchicalSpendingHash_.clear();
    currentHierarchicalDraftDoc_ = QJsonDocument();
    currentHierarchicalDraftJson_.clear();
    if (hierarchicalSpendingHashEdit_) hierarchicalSpendingHashEdit_->clear();
    if (hierarchicalDraftEdit_) hierarchicalDraftEdit_->clear();
    if (hierarchicalDraftHashLabel_) hierarchicalDraftHashLabel_->setText(tr("Hash for signatures: —"));
    if (saveHierarchicalDraftButton_) saveHierarchicalDraftButton_->setEnabled(false);
    if (copyHierarchicalDraftButton_) copyHierarchicalDraftButton_->setEnabled(false);
    updateHierarchicalCommitmentState();
}

bool CorporatePage::ensureHierarchicalScanner()
{
    if (!walletModel_ || !walletModel_->getChainState()) return false;
    const int required = delegatedMSpin_ ? delegatedMSpin_->value() : 3;
    if (hierarchicalScanner_ &&
        (hierarchicalScannerTarget_ != currentHierarchicalSpendingHash_ ||
         hierarchicalScannerAddress_ != currentDelegatedAddress_ ||
         hierarchicalScannerRequired_ != required)) {
        resetHierarchicalScanner(true);
    }
    if (!hierarchicalScanner_) {
        QStringList allowedSigners;
        for (const QJsonValue& value : delegatedConfigParticipants_) {
            const QString address = value.toObject().value(QStringLiteral("address")).toString().trimmed();
            if (!address.isEmpty()) allowedSigners.append(address);
        }
        hierarchicalScanner_ = new CommitmentScanner(
            currentHierarchicalSpendingHash_, currentDelegatedAddress_, required,
            walletModel_->getChainState(), this, allowedSigners);
        hierarchicalScannerTarget_ = currentHierarchicalSpendingHash_;
        hierarchicalScannerAddress_ = currentDelegatedAddress_;
        hierarchicalScannerRequired_ = required;
        connect(hierarchicalScanner_, &CommitmentScanner::commitmentFound,
                this, &CorporatePage::onHierarchicalCommitmentFound);
        connect(hierarchicalScanner_, &CommitmentScanner::commitmentInvalidated,
                this, &CorporatePage::onHierarchicalCommitmentInvalidated);
        connect(hierarchicalScanner_, &CommitmentScanner::allCommitmentsCollected,
                this, &CorporatePage::onAllHierarchicalCommitmentsCollected);
        connect(hierarchicalScanner_, &CommitmentScanner::scanError,
                this, &CorporatePage::onHierarchicalScanError);
        connect(hierarchicalScanner_, &CommitmentScanner::statusUpdate,
                this, &CorporatePage::onHierarchicalScanStatus);
    }
    return true;
}

void CorporatePage::updateDirectorCommitmentFields()
{
    // Получаем контейнер
    QWidget *container = findChild<QWidget*>("directorCommitmentsContainer");
    if (!container) return;

    QGridLayout *grid = qobject_cast<QGridLayout*>(container->layout());
    if (!grid) return;

    // Очищаем старые поля
    QLayoutItem *item;
    while ((item = grid->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    hierarchicalCommitmentEdits_.clear();
    scannerHierarchicalFields_.clear();

    // Создаем новые поля в зависимости от N
    int n = delegatedNSpin_ ? delegatedNSpin_->value() : 5;
    for (int i = 0; i < n; ++i) {
        QLabel *label = new QLabel(tr("TXID %1:").arg(i + 1), container);
        QLineEdit *edit = new QLineEdit(container);
        edit->setPlaceholderText(tr("Paste TXID signatures %1...").arg(i + 1));
        connect(edit, &QLineEdit::textChanged, this, &CorporatePage::onHierarchicalCommitmentChanged);

        QPushButton *loadBtn = new QPushButton(tr("📁"), container);
        loadBtn->setToolTip(tr("Load commitment from file"));
        loadBtn->setMaximumWidth(40);
        loadBtn->setProperty("commitmentIndex", i);
        connect(loadBtn, &QPushButton::clicked, this, &CorporatePage::onLoadHierarchicalCommitmentFile);

        hierarchicalCommitmentEdits_.append(edit);
        grid->addWidget(label, i, 0);
        grid->addWidget(edit, i, 1);
        grid->addWidget(loadBtn, i, 2);
    }
}

// ============================================================================
// Helper Methods - Update State
// ============================================================================

void CorporatePage::updateDirectorState()
{
    bool hasProof = delegationProofLoaded_;
    bool hasInputs = !recipientEdit_->text().trimmed().isEmpty() &&
                    !spendAmountEdit_->text().trimmed().isEmpty() &&
                    !spendFeeEdit_->text().trimmed().isEmpty();
    bool inputsReady = hasProof && hasInputs;

    if (createHierarchicalDraftButton_) {
        createHierarchicalDraftButton_->setEnabled(inputsReady && !directorDraftInProgress_);
    }

    const bool hasDraft = hierarchicalDraftEdit_ && !hierarchicalDraftEdit_->toPlainText().isEmpty();
    if (saveHierarchicalDraftButton_) saveHierarchicalDraftButton_->setEnabled(hasDraft);
    if (copyHierarchicalDraftButton_) copyHierarchicalDraftButton_->setEnabled(hasDraft);

    updateHierarchicalCommitmentState();
}

void CorporatePage::updateHierarchicalCommitmentState()
{
    int filled = 0;
    for (QLineEdit *edit : std::as_const(hierarchicalCommitmentEdits_)) {
        if (!edit->text().trimmed().isEmpty()) filled++;
    }

    int required = delegatedMSpin_ ? delegatedMSpin_->value() : 3;
    const bool ready = filled >= required;

    if (finalizeHierarchicalButton_) {
        finalizeHierarchicalButton_->setEnabled(ready && !directorDraftInProgress_ && !currentHierarchicalDraftJson_.isEmpty());
    }
    if (finalizeHierarchicalBroadcastCheck_) {
        finalizeHierarchicalBroadcastCheck_->setEnabled(!directorDraftInProgress_);
    }

    QLabel *infoLabel = findChild<QLabel*>("directorCommitmentsInfo");
    if (infoLabel) {
        infoLabel->setText(tr("Required signatures: %1 from %2 (collected: %3)")
                          .arg(required).arg(delegatedNSpin_ ? delegatedNSpin_->value() : 5).arg(filled));
    }
}

bool CorporatePage::validateDirectorInputs(QString& error,
                                           qint64* parsedAmount,
                                           qint64* parsedFee) const
{
    if (!delegationProofLoaded_) {
        error = tr("The delegation is not loaded.");
        return false;
    }
    if (!isCurrentDelegationProofActive(error)) return false;
    if (currentDelegationProofTxid_.isEmpty()) {
        error = tr("The delegation transaction id is missing.");
        return false;
    }

    QString recipient = recipientEdit_ ? recipientEdit_->text().trimmed() : QString();
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

    qint64 amountVal = 0;
    if (!spendAmountEdit_ || !GUIUtil::parseAmount(spendAmountEdit_->text(), amountVal) || amountVal <= 0) {
        error = tr("Enter an amount greater than zero.");
        return false;
    }

    qint64 feeVal = 0;
    if (!spendFeeEdit_ || !GUIUtil::parseAmount(spendFeeEdit_->text(), feeVal) ||
        feeVal < WalletModel::multisigRelayFee(
            delegatedNSpin_ ? delegatedNSpin_->value() : 5,
            walletModel_ ? walletModel_->getMempool().get() : nullptr)) {
        error = tr("The fee is below the minimum of %1 GC.")
                    .arg(GUIUtil::formatAmount(WalletModel::multisigRelayFee(
                        delegatedNSpin_ ? delegatedNSpin_->value() : 5,
                        walletModel_ ? walletModel_->getMempool().get() : nullptr)));
        return false;
    }
    qint64 total = 0;
    if (!GUIUtil::addAmounts(amountVal, feeVal, total)) {
        error = tr("Amount plus fee is out of range.");
        return false;
    }

    if (parsedAmount) *parsedAmount = amountVal;
    if (parsedFee) *parsedFee = feeVal;
    return true;
}

bool CorporatePage::parseDelegationProofInfo(const QString& txid)
{
    if (!walletModel_) {
        return false;
    }

    auto chainState = walletModel_->getChainState();
    if (!chainState) {
        return false;
    }

    try {
        // Преобразуем txid в Hash256
        geocoin::Hash256 hash;
        geocoin::bytes hashBytes;
        QString cleanTxid = txid.trimmed();

        // Конвертируем hex в bytes
        for (int i = 0; i < cleanTxid.length(); i += 2) {
            bool ok;
            hashBytes.push_back(static_cast<geocoin::byte>(
                cleanTxid.mid(i, 2).toUInt(&ok, 16)));
            if (!ok) return false;
        }

        if (hashBytes.size() != 32) return false;
        std::copy(hashBytes.begin(), hashBytes.end(), hash.begin());

        // Получаем транзакцию из блокчейна
        auto txOpt = chainState->get_transaction(hash);
        if (!txOpt) {  // unique_ptr проверяется на nullptr
            return false;
        }

        geocoin::core::Transaction tx = *txOpt;  // разыменовываем unique_ptr

        // Проверяем тип транзакции
        if (tx.type != geocoin::core::TransactionType::DELEGATION_PROOF) {
            return false;
        }
        const uint64_t now = static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch());
        if ((tx.valid_from > 0 && now < tx.valid_from) ||
            (tx.valid_until > 0 && now > tx.valid_until) ||
            chainState->is_delegation_revoked(hash)) {
            return false;
        }

        // Форматируем лимиты
        QString limitsInfo;
        if (tx.max_total_delegated > 0) {
            limitsInfo += tr("\nMax. total amount: %1 GC")
                .arg(GUIUtil::formatAmount(tx.max_total_delegated));
        }
        if (tx.max_per_transaction > 0) {
            limitsInfo += tr("\nMax. on transaction: %1 GC")
                .arg(GUIUtil::formatAmount(tx.max_per_transaction));
        }
        if (tx.max_percent_of_balance > 0) {
            limitsInfo += tr("\nMax. % from balance: %1%").arg(tx.max_percent_of_balance);
        }

        const auto formatValidity = [this](uint64_t timestamp, const QString& zeroText) {
            return timestamp == 0
                ? zeroText
                : QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp), Qt::UTC)
                      .toString(QStringLiteral("dd.MM.yyyy HH:mm 'UTC'"));
        };
        QString info = tr("Primary: %1\nDelegated: %2\nValid: with %3 until %4\nNumber: %5%6")
            .arg(QString::fromStdString(tx.primary_multisig_address.str()))
            .arg(QString::fromStdString(tx.delegated_multisig_address.str()))
            .arg(formatValidity(tx.valid_from, tr("immediately")))
            .arg(formatValidity(tx.valid_until, tr("∞")))
            .arg(tx.nonce)
            .arg(limitsInfo);

        delegationInfoLabel_->setText(info);
        delegationStatusLabel_->setText(tr("Status: ✅ Active"));

        // Загружаем конфигурацию делегированного мультисига если она есть
        QString delegatedAddr = QString::fromStdString(tx.delegated_multisig_address.str());
        if (cleanTxid != currentDelegationProofTxid_.trimmed() ||
            delegatedAddr != currentDelegatedAddress_) {
            resetHierarchicalOperation();
        }
        currentDelegationProofTxid_ = cleanTxid;
        delegationProofLoaded_ = true;
        currentDelegatedAddress_ = delegatedAddr;
        delegatedAddressLabel_->setText(tr("Address: %1").arg(delegatedAddr));

        // Обновляем баланс основного мультисига
        currentPrimaryAddress_ = QString::fromStdString(tx.primary_multisig_address.str());
        updatePrimaryBalance();

        return true;

    } catch (const std::exception& ex) {
        logError(tr("Error parsing delegation proof: %1").arg(ex.what()));
        return false;
    }
}

bool CorporatePage::isCurrentDelegationProofActive(QString& error) const
{
    if (!walletModel_ || !walletModel_->getChainState() || currentDelegationProofTxid_.size() != 64) {
        error = tr("Delegation proof is not loaded or chain state is unavailable.");
        return false;
    }
    geocoin::Hash256 hash{};
    for (int i = 0; i < 32; ++i) {
        bool ok = false;
        const uint value = currentDelegationProofTxid_.mid(i * 2, 2).toUInt(&ok, 16);
        if (!ok || value > 0xffU) {
            error = tr("Invalid delegation proof TXID.");
            return false;
        }
        hash[static_cast<size_t>(i)] = static_cast<geocoin::byte>(value);
    }
    const auto chainState = walletModel_->getChainState();
    const auto tx = chainState->get_transaction(hash);
    if (!tx || tx->type != geocoin::core::TransactionType::DELEGATION_PROOF ||
        chainState->is_delegation_revoked(hash)) {
        error = tr("Delegation proof is not in the active chain or has been revoked.");
        return false;
    }
    const uint64_t now = static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch());
    if ((tx->valid_from > 0 && now < tx->valid_from) ||
        (tx->valid_until > 0 && now > tx->valid_until)) {
        error = tr("The delegation proof is not valid yet or has already expired.");
        return false;
    }
    if (!currentDelegatedAddress_.isEmpty() &&
        QString::fromStdString(tx->delegated_multisig_address.str()) != currentDelegatedAddress_) {
        error = tr("Delegation proof does not match the current delegated address.");
        return false;
    }
    return true;
}

void CorporatePage::loadActiveDelegationProofs()
{
    if (!activeDelegationProofsCombo_ || !walletModel_) {
        return;
    }

    activeDelegationProofsCombo_->clear();
    activeDelegationProofsCombo_->addItem(tr("-- Select active delegation proof --"), "");

    // Пользователь может ввести TXID вручную или получить список через RPC getdelegationproof
    // В будущем можно добавить автоматическую загрузку из блокчейна
}

// ============================================================================
// Helper Methods - History
// ============================================================================

QWidget* CorporatePage::buildDirectorPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setSpacing(15);

    layout->addWidget(buildDirectorDelegationProofBox());
    layout->addWidget(buildDirectorSpendParametersBox());
    layout->addWidget(buildDirectorDraftBox());
    layout->addWidget(buildDirectorCommitmentsBox());
    layout->addStretch();

    return page;
}

QGroupBox* CorporatePage::buildDirectorDelegationProofBox()
{
    QGroupBox *box = new QGroupBox(tr("Proof delegation"), this);
    QGridLayout *grid = new QGridLayout(box);

    delegatedAddressDirectorEdit_ = new QLineEdit(box);
    delegatedAddressDirectorEdit_->setPlaceholderText(tr("Enter delegated address..."));
    connect(delegatedAddressDirectorEdit_, &QLineEdit::textChanged,
            this, &CorporatePage::onDelegatedAddressChanged);
    grid->addWidget(new QLabel(tr("Delegated address:"), box), 0, 0);
    grid->addWidget(delegatedAddressDirectorEdit_, 0, 1, 1, 2);

    delegatedConfigDirectorEdit_ = new QLineEdit(box);
    delegatedConfigDirectorEdit_->setPlaceholderText(tr("File configuration directors (JSON)..."));
    QPushButton *loadDirectorConfigBtn = new QPushButton(tr("Load config"), box);
    connect(loadDirectorConfigBtn, &QPushButton::clicked, this, &CorporatePage::onLoadDirectorDelegatedConfig);
    grid->addWidget(new QLabel(tr("OR file:"), box), 1, 0);
    grid->addWidget(delegatedConfigDirectorEdit_, 1, 1);
    grid->addWidget(loadDirectorConfigBtn, 1, 2);

    activeDelegationProofsCombo_ = new QComboBox(box);
    activeDelegationProofsCombo_->addItem(tr("-- Select active delegation proof --"), "");
    connect(activeDelegationProofsCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CorporatePage::onActiveDelegationProofSelected);
    grid->addWidget(new QLabel(tr("Active proofs:"), box), 2, 0);
    grid->addWidget(activeDelegationProofsCombo_, 2, 1, 1, 2);

    QFrame *line = new QFrame(box);
    line->setFrameShape(QFrame::HLine);
    grid->addWidget(line, 3, 0, 1, 3);

    delegationProofTxidEdit_ = new QLineEdit(box);
    delegationProofTxidEdit_->setPlaceholderText(tr("Enter TXID..."));
    QPushButton *loadBtn = new QPushButton(tr("Load"), box);
    connect(loadBtn, &QPushButton::clicked, this, &CorporatePage::onLoadDelegationProof);
    grid->addWidget(new QLabel(tr("Or TXID:"), box), 4, 0);
    grid->addWidget(delegationProofTxidEdit_, 4, 1);
    grid->addWidget(loadBtn, 4, 2);

    QFrame *line2 = new QFrame(box);
    line2->setFrameShape(QFrame::HLine);
    grid->addWidget(line2, 5, 0, 1, 3);

    delegationInfoLabel_ = new QLabel(tr("Primary: —\nDelegated: —\nValid: —\nNumber: —"), box);
    grid->addWidget(delegationInfoLabel_, 6, 0, 1, 3);

    delegationStatusLabel_ = new QLabel(tr("Status: —"), box);
    delegationStatusLabel_->setStyleSheet("QLabel { font-weight: bold; }");
    grid->addWidget(delegationStatusLabel_, 7, 0, 1, 3);

    return box;
}

void CorporatePage::onLoadDirectorDelegatedConfig()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Select configuration file"),
        QString(), tr("JSON Files (*.json);;All Files (*)"));
    if (fileName.isEmpty()) return;

    if (delegatedConfigDirectorEdit_) {
        delegatedConfigDirectorEdit_->setText(fileName);
    }

    if (!loadConfigFromFile(fileName, false)) {
        logError(tr("Could not load the directors configuration"));
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("Could not load the directors configuration file"));
        return;
    }

    if (delegatedAddressDirectorEdit_ && !currentDelegatedAddress_.isEmpty()) {
        QSignalBlocker blocker(delegatedAddressDirectorEdit_);
        delegatedAddressDirectorEdit_->setText(currentDelegatedAddress_);
    }

    onDelegatedAddressChanged();

    // Если нашли ровно один активный proof, подставляем его автоматически.
    if (activeDelegationProofsCombo_ && activeDelegationProofsCombo_->count() == 2) {
        activeDelegationProofsCombo_->setCurrentIndex(1);
    }

    logInfo(tr("Configuration directors loaded: %1").arg(currentDelegatedAddress_));
    updateDirectorState();
}

void CorporatePage::onDelegatedAddressChanged()
{
    if (!activeDelegationProofsCombo_ || !walletModel_ || !walletModel_->getChainState()) {
        return;
    }

    const QString addrText = delegatedAddressDirectorEdit_ ? delegatedAddressDirectorEdit_->text().trimmed() : QString();
    activeDelegationProofsCombo_->blockSignals(true);
    activeDelegationProofsCombo_->clear();
    activeDelegationProofsCombo_->addItem(tr("-- Select active delegation proof --"), "");
    activeDelegationProofsCombo_->blockSignals(false);

    if (addrText.isEmpty()) {
        return;
    }

    geocoin::Address delegatedAddr;
    try {
        delegatedAddr = geocoin::Address(addrText.toStdString());
    } catch (...) {
        return;
    }

    auto chainState = walletModel_->getChainState();
    const geocoin::Height height = chainState->get_height();
    const uint64_t now = static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch());
    const geocoin::Height scanWindow = 10000;
    const geocoin::Height start = (height > scanWindow) ? (height - scanWindow) : 0;

    activeDelegationProofsCombo_->blockSignals(true);
    for (geocoin::Height h = height; h >= start; --h) {
        geocoin::core::Block block;
        if (!chainState->get_block_by_height(h, block)) {
            if (h == 0) break;
            continue;
        }

        for (const auto& tx : block.transactions) {
            if (tx.type != geocoin::core::TransactionType::DELEGATION_PROOF) {
                continue;
            }
            if (tx.delegated_multisig_address.str() != delegatedAddr.str()) {
                continue;
            }

            if ((tx.valid_from > 0 && now < tx.valid_from) ||
                (tx.valid_until > 0 && now > tx.valid_until)) {
                continue;
            }

            const geocoin::Hash256 txid = tx.get_hash();
            if (chainState->is_delegation_revoked(txid)) {
                continue;
            }

            geocoin::bytes txidBytes(txid.begin(), txid.end());
            const QString txidHex = QString::fromStdString(geocoin::crypto::bytes_to_hex(txidBytes));
            const QString label = tr("%1 (h=%2)").arg(txidHex.left(16)).arg(static_cast<qulonglong>(h));
            activeDelegationProofsCombo_->addItem(label, txidHex);
        }

        if (h == 0) break;
    }
    activeDelegationProofsCombo_->blockSignals(false);
}

QGroupBox* CorporatePage::buildDirectorSpendParametersBox()
{
    QGroupBox *box = new QGroupBox(tr("Settings delegated spend"), this);
    QFormLayout *form = new QFormLayout(box);

    availableBalanceLabel_ = new QLabel(tr("—"), box);
    availableBalanceLabel_->setStyleSheet("QLabel { color: #2980b9; font-weight: bold; }");
    form->addRow(tr("Available balance:"), availableBalanceLabel_);

    delegatedUtxoBalanceLabel_ = new QLabel(tr("—"), box);
    delegatedUtxoBalanceLabel_->setStyleSheet("QLabel { color: #7f8c8d; }");
    form->addRow(tr("Balance directors (UTXO):"), delegatedUtxoBalanceLabel_);

    delegationAllowanceLabel_ = new QLabel(tr("—"), box);
    delegationAllowanceLabel_->setWordWrap(true);
    delegationAllowanceLabel_->setStyleSheet("QLabel { color: #8e44ad; }");
    form->addRow(tr("Limit by delegation:"), delegationAllowanceLabel_);

    recipientEdit_ = new QLineEdit(box);
    recipientEdit_->setPlaceholderText(tr("GEO..."));
    const auto semanticInputChanged = [this]() {
        if (!currentHierarchicalSpendingHash_.isEmpty() || !currentHierarchicalDraftJson_.isEmpty()) {
            resetHierarchicalOperation();
        }
        updateDirectorState();
    };
    connect(recipientEdit_, &QLineEdit::textChanged, this, semanticInputChanged);
    form->addRow(tr("Recipient:"), recipientEdit_);

    spendAmountEdit_ = new QLineEdit(box);
    spendAmountEdit_->setPlaceholderText(tr("0.0"));
    connect(spendAmountEdit_, &QLineEdit::textChanged, this, semanticInputChanged);
    form->addRow(tr("Amount (GC):"), spendAmountEdit_);

    spendFeeEdit_ = new QLineEdit(box);
    spendFeeEdit_->setText(GUIUtil::formatAmount(WalletModel::multisigRelayFee(
        delegatedNSpin_ ? delegatedNSpin_->value() : 5, nullptr)));
    connect(spendFeeEdit_, &QLineEdit::textChanged, this, semanticInputChanged);
    form->addRow(tr("Fee (GC):"), spendFeeEdit_);

    createHierarchicalDraftButton_ = new QPushButton(tr("Create draft delegated spend"), box);
    createHierarchicalDraftButton_->setStyleSheet("QPushButton { background-color: #3498db; color: white; font-weight: bold; padding: 10px; }");
    createHierarchicalDraftButton_->setEnabled(false);
    connect(createHierarchicalDraftButton_, &QPushButton::clicked, this, &CorporatePage::onCreateHierarchicalSpendDraft);
    form->addRow(createHierarchicalDraftButton_);

    return box;
}

QGroupBox* CorporatePage::buildDirectorDraftBox()
{
    QGroupBox *box = new QGroupBox(tr("Draft delegated spend"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    // Секция для spending hash (для автоматического сбора)
    QLabel *hashSectionLabel = new QLabel(tr("Spending Hash (for automatic collection):"), box);
    hashSectionLabel->setStyleSheet("QLabel { font-weight: bold; color: #2980b9; }");
    layout->addWidget(hashSectionLabel);

    hierarchicalSpendingHashEdit_ = new QLineEdit(box);
    hierarchicalSpendingHashEdit_->setReadOnly(true);
    hierarchicalSpendingHashEdit_->setPlaceholderText(tr("Spending hash will appear here after creation draft..."));
    hierarchicalSpendingHashEdit_->setStyleSheet(GUIUtil::themedMonoEditableStyleSheet());
    layout->addWidget(hierarchicalSpendingHashEdit_);

    QHBoxLayout *hashButtonLayout = new QHBoxLayout();
    copyHierarchicalHashButton_ = new QPushButton(tr("📋 Copy hash"), box);
    copyHierarchicalHashButton_->setEnabled(false);
    copyHierarchicalHashButton_->setToolTip(tr("Copy spending hash for sending directors"));
    connect(copyHierarchicalHashButton_, &QPushButton::clicked, [this]() {
        if (!hierarchicalSpendingHashEdit_ || hierarchicalSpendingHashEdit_->text().isEmpty()) {
            return;
        }
        QString hash = hierarchicalSpendingHashEdit_->text();
        QApplication::clipboard()->setText(hash);
        logInfo(tr("Spending hash copied in buffer exchange"));

        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Information,
            tr("Hash copied"),
            tr("Spending hash copied in buffer exchange:\n\n%1\n\nSend its directors for signing.").arg(hash));
    });
    hashButtonLayout->addWidget(copyHierarchicalHashButton_);

    showHierarchicalHashQRButton_ = new QPushButton(tr("📱 QR-code"), box);
    showHierarchicalHashQRButton_->setEnabled(false);
    showHierarchicalHashQRButton_->setToolTip(tr("Show QR-code for scanning"));
    connect(showHierarchicalHashQRButton_, &QPushButton::clicked, [this]() {
        if (!hierarchicalSpendingHashEdit_ || hierarchicalSpendingHashEdit_->text().isEmpty()) {
            return;
        }
        QString hash = hierarchicalSpendingHashEdit_->text();
        GUIUtil::showQRCodeDialog(this,
            tr("QR-code Spending Hash"),
            hash,
            tr("Scan this QR-code for receiving spending hash"));
        logInfo(tr("Shown QR-code for spending hash"));
    });
    hashButtonLayout->addWidget(showHierarchicalHashQRButton_);
    hashButtonLayout->addStretch();
    layout->addLayout(hashButtonLayout);

    QLabel *hashInfoLabel = new QLabel(tr("Send the spending hash to the directors so they can sign."), box);
    hashInfoLabel->setWordWrap(true);
    hashInfoLabel->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    layout->addWidget(hashInfoLabel);

    // Разделитель
    QFrame *separator = new QFrame(box);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    // Секция для JSON черновика
    QLabel *draftSectionLabel = new QLabel(tr("JSON Draft:"), box);
    draftSectionLabel->setStyleSheet("QLabel { font-weight: bold; color: #95a5a6; }");
    layout->addWidget(draftSectionLabel);

    hierarchicalDraftEdit_ = new QTextEdit(box);
    hierarchicalDraftEdit_->setReadOnly(true);
    hierarchicalDraftEdit_->setMinimumHeight(120);
    hierarchicalDraftEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    hierarchicalDraftEdit_->setPlaceholderText(tr("JSON draft will appear here..."));
    layout->addWidget(hierarchicalDraftEdit_);

    hierarchicalDraftHashLabel_ = new QLabel(tr("Hash for signatures: —"), box);
    hierarchicalDraftHashLabel_->setStyleSheet("QLabel { color: #e67e22; font-weight: bold; }");
    hierarchicalDraftHashLabel_->setWordWrap(true);
    layout->addWidget(hierarchicalDraftHashLabel_);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    saveHierarchicalDraftButton_ = new QPushButton(tr("Save"), box);
    saveHierarchicalDraftButton_->setEnabled(false);
    connect(saveHierarchicalDraftButton_, &QPushButton::clicked, this, &CorporatePage::onSaveHierarchicalDraft);
    copyHierarchicalDraftButton_ = new QPushButton(tr("Copy"), box);
    copyHierarchicalDraftButton_->setEnabled(false);
    connect(copyHierarchicalDraftButton_, &QPushButton::clicked, this, &CorporatePage::onCopyHierarchicalDraft);
    buttonLayout->addWidget(saveHierarchicalDraftButton_);
    buttonLayout->addWidget(copyHierarchicalDraftButton_);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    return box;
}

QGroupBox* CorporatePage::buildDirectorCommitmentsBox()
{
    QGroupBox *box = new QGroupBox(tr("Collection signatures from directors"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    QLabel *infoLabel = new QLabel(tr("Required signatures: —"), box);
    infoLabel->setObjectName("directorCommitmentsInfo");
    layout->addWidget(infoLabel);

    // Создаем контейнер для динамических полей
    QWidget *commitmentsContainer = new QWidget(box);
    commitmentsContainer->setObjectName("directorCommitmentsContainer");
    QGridLayout *grid = new QGridLayout(commitmentsContainer);
    grid->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(commitmentsContainer);

    // Изначально создаем поля для текущего N
    updateDirectorCommitmentFields();

    // Разделитель
    QFrame *separator = new QFrame(box);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    // Секция автоматического сканирования
    QLabel *scanLabel = new QLabel(tr("<b>Automatic collection signatures:</b>"), box);
    layout->addWidget(scanLabel);

    autoScanHierarchicalCheck_ = new QCheckBox(tr("🔄 Auto-scanning blockchain"), box);
    autoScanHierarchicalCheck_->setToolTip(tr("Automatically search commitments in new blocks (interval: 30 sec)"));
    autoScanHierarchicalCheck_->setChecked(false);
    connect(autoScanHierarchicalCheck_, &QCheckBox::toggled, this, &CorporatePage::onAutoScanHierarchicalToggled);
    layout->addWidget(autoScanHierarchicalCheck_);

    QHBoxLayout *scanButtonLayout = new QHBoxLayout();
    scanNowHierarchicalButton_ = new QPushButton(tr("🔍 Check now"), box);
    scanNowHierarchicalButton_->setToolTip(tr("Check blockchain directly now (without auto-scanning)"));
    scanNowHierarchicalButton_->setEnabled(false);
    connect(scanNowHierarchicalButton_, &QPushButton::clicked, this, &CorporatePage::onScanNowHierarchical);
    scanButtonLayout->addWidget(scanNowHierarchicalButton_);
    scanButtonLayout->addStretch();
    layout->addLayout(scanButtonLayout);

    scanStatusHierarchicalLabel_ = new QLabel(tr("Status: ready to scan"), box);
    scanStatusHierarchicalLabel_->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    scanStatusHierarchicalLabel_->setWordWrap(true);
    layout->addWidget(scanStatusHierarchicalLabel_);

    QLabel *scanInfoLabel = new QLabel(tr("Scanning starts automatically after the draft is created."), box);
    scanInfoLabel->setWordWrap(true);
    scanInfoLabel->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 10px; }");
    layout->addWidget(scanInfoLabel);

    // Разделитель
    QFrame *separator2 = new QFrame(box);
    separator2->setFrameShape(QFrame::HLine);
    separator2->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator2);

    // Финализация
    finalizeHierarchicalBroadcastCheck_ = new QCheckBox(tr("Send in network immediately"), box);
    finalizeHierarchicalBroadcastCheck_->setChecked(true);
    layout->addWidget(finalizeHierarchicalBroadcastCheck_);

    finalizeHierarchicalButton_ = new QPushButton(tr("Finalize transaction"), box);
    finalizeHierarchicalButton_->setStyleSheet("QPushButton { background-color: #27ae60; color: white; font-weight: bold; padding: 10px; }");
    finalizeHierarchicalButton_->setEnabled(false);
    connect(finalizeHierarchicalButton_, &QPushButton::clicked, this, &CorporatePage::onFinalizeHierarchicalSpend);
    layout->addWidget(finalizeHierarchicalButton_);

    return box;
}

// ============================================================================
// Build UI - Signer Page
// ============================================================================

void CorporatePage::onLoadDelegationProof()
{
    QString txid = delegationProofTxidEdit_->text().trimmed();
    if (txid.isEmpty()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Enter TXID delegation proof"));
        return;
    }

    if (parseDelegationProofInfo(txid)) {
        logInfo(tr("Delegation proof loaded: %1").arg(txid));
        updateDirectorState();
    } else {
        logError(tr("Could not load the delegation"));
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Could not load the delegation"));
    }
}

void CorporatePage::onActiveDelegationProofSelected(int index)
{
    if (index <= 0 || !activeDelegationProofsCombo_) {
        return; // Первый элемент - placeholder
    }

    QString txid = activeDelegationProofsCombo_->itemData(index).toString();
    if (txid.isEmpty()) {
        return;
    }

    delegationProofTxidEdit_->setText(txid);

    if (parseDelegationProofInfo(txid)) {
        logInfo(tr("Delegation proof loaded from list: %1").arg(txid));
        updateDirectorState();
    } else {
        logError(tr("Could not load the delegation"));
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Could not load the delegation"));
    }
}

void CorporatePage::onCreateHierarchicalSpendDraft()
{
    QString error;
    qint64 amount = 0;
    qint64 fee = 0;
    if (!validateDirectorInputs(error, &amount, &fee)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Invalid data"), error);
        return;
    }

    // Создаем JSON конфигурацию из данных в памяти
    QJsonObject delegatedConfigObj;
    delegatedConfigObj["address"] = currentDelegatedAddress_;
    delegatedConfigObj["m"] = delegatedMSpin_->value();
    delegatedConfigObj["n"] = delegatedNSpin_->value();
    delegatedConfigObj["participants"] = delegatedConfigParticipants_;
    QJsonDocument delegatedConfigDoc(delegatedConfigObj);
    QString delegatedConfigJson = QString::fromUtf8(delegatedConfigDoc.toJson(QJsonDocument::Compact));

    logInfo(tr("Creation draft delegated spend (automatic mode)..."));
    setDirectorDraftInProgress(true);

    emit requestHierarchicalSpendDraft(
        currentDelegationProofTxid_,
        delegatedConfigJson,
        recipientEdit_->text(),
        amount,
        fee);
}

void CorporatePage::onHierarchicalCommitmentChanged()
{
    updateHierarchicalCommitmentState();
}

void CorporatePage::onFinalizeHierarchicalSpend()
{
    QString proofError;
    if (!isCurrentDelegationProofActive(proofError)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning,
            tr("Delegation proof is inactive"), proofError);
        return;
    }
    QStringList txids;
    QSet<QString> uniqueTxids;
    for (QLineEdit *edit : std::as_const(hierarchicalCommitmentEdits_)) {
        QString value = edit->text().trimmed();
        if (!value.isEmpty()) {
            if (uniqueTxids.contains(value)) {
                GUIUtil::showStyledMessageBox(this, QMessageBox::Warning,
                    tr("Duplicate TXID"),
                    tr("Each commitment TXID can be used only once."));
                return;
            }
            uniqueTxids.insert(value);
            txids.append(value);
        }
    }

    int required = delegatedMSpin_->value();
    if (txids.size() < required) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Not enough signatures"),
            tr("At least %1 commitment transaction ids are required.").arg(required));
        return;
    }

    if (currentHierarchicalDraftJson_.isEmpty()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("No draft"), tr("Create a draft first."));
        return;
    }

    if (currentHierarchicalSpendingHash_.isEmpty() || currentDelegatedAddress_.isEmpty() ||
        !walletModel_ || !walletModel_->getChainState()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning,
            tr("Signatures cannot be checked"),
            tr("Missing the spending hash, delegated address, or access to the active chain."));
        return;
    }
    QStringList inactiveTxids;
    QStringList allowedSigners;
    for (const QJsonValue& value : delegatedConfigParticipants_) {
        const QString address = value.toObject().value(QStringLiteral("address")).toString().trimmed();
        if (!address.isEmpty()) allowedSigners.append(address);
    }
    QSet<QString> activeSigners;
    for (const QString& txid : std::as_const(txids)) {
        QString signerAddress;
        if (!CommitmentScanner::isCommitmentActive(
                txid, currentHierarchicalSpendingHash_, currentDelegatedAddress_,
                walletModel_->getChainState(), allowedSigners, &signerAddress) ||
            activeSigners.contains(signerAddress)) {
            inactiveTxids.append(txid);
        } else {
            activeSigners.insert(signerAddress);
        }
    }
    if (!inactiveTxids.isEmpty()) {
        for (const QString& txid : std::as_const(inactiveTxids)) {
            if (scannerHierarchicalFields_.contains(txid)) {
                onHierarchicalCommitmentInvalidated(txid, tr("unknown signer"));
            }
        }
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning,
            tr("Signatures were not confirmed"),
            tr("Some commitments are not in the current chain, do not match the draft or participants, or repeat a signer:\n%1")
                .arg(inactiveTxids.join(QLatin1Char('\n'))));
        return;
    }

    logInfo(tr("Finalization delegated spend with %1 signatures...").arg(txids.size()));

    emit requestHierarchicalSpendFinalize(
        currentHierarchicalDraftJson_,
        txids,
        finalizeHierarchicalBroadcastCheck_->isChecked());
}

void CorporatePage::onSaveHierarchicalDraft()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save draft"),
        QString("hierarchical_draft_%1.json").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("JSON Files (*.json)"));
    if (fileName.isEmpty()) return;

    QString error;
    if (!GUIUtil::writeFileAtomically(fileName, currentHierarchicalDraftJson_.toUtf8(), error)) {
        GUIUtil::showStyledMessageBox(
            this, QMessageBox::Warning, tr("Error"),
            tr("Could not save the file: %1").arg(error));
        return;
    }
    logInfo(tr("Draft saved: %1").arg(fileName));
}

void CorporatePage::onCopyHierarchicalDraft()
{
    QApplication::clipboard()->setText(currentHierarchicalDraftJson_);
    logInfo(tr("Draft copied in buffer exchange"));
    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Copied"),
        tr("Draft delegated spend copied.\nSend its signers."));
}

void CorporatePage::onLoadHierarchicalCommitmentFile()
{
    QPushButton *btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;

    int index = btn->property("commitmentIndex").toInt();
    if (index < 0 || index >= hierarchicalCommitmentEdits_.size()) return;

    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Load commitment %1").arg(index + 1),
        QString(),
        tr("Transaction Files (*.tx *.tx.hex);;All Files (*)"));
    if (fileName.isEmpty()) return;

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Could not open the file"));
        return;
    }

    QString content = file.readAll().trimmed();
    file.close();

    // Пытаемся извлечь TXID из содержимого
    QString txid;

    // Если это JSON с полем txid или commitment_txid
    if (content.startsWith("{")) {
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8(), &error);
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();
            txid = obj.value("txid").toString();
            if (txid.isEmpty()) {
                txid = obj.value("commitment_txid").toString();
            }
            if (txid.isEmpty()) {
                txid = obj.value("hash").toString();
            }
        }
    }

    // Если не JSON или не нашли txid, считаем что весь файл - это txid
    if (txid.isEmpty()) {
        txid = content;
    }

    // Проверяем что это похоже на hex txid (64 символа)
    if (isHexTxid(txid)) {
        hierarchicalCommitmentEdits_[index]->setText(txid);
        logInfo(tr("Commitment %1 loaded from file: %2").arg(index + 1).arg(fileName));
    } else {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("File not contains valid TXID.\nPending 64 hex characters."));
    }
}

// ============================================================================
// Signer Slots
// ============================================================================

void CorporatePage::updateDelegatedBalance()
{
    if (currentDelegatedAddress_.isEmpty() || !walletModel_) {
        return;
    }

    // Для делегированного мультисига баланс не нужен (средства на основном)
    // Но можно показать для информации
}

void CorporatePage::updateDelegationAllowance()
{
    if (delegatedUtxoBalanceLabel_) {
        delegatedUtxoBalanceLabel_->setText(tr("—"));
    }
    if (delegationAllowanceLabel_) {
        delegationAllowanceLabel_->setText(tr("—"));
    }

    if (!walletModel_ || !walletModel_->getChainState()) {
        return;
    }

    auto chainState = walletModel_->getChainState();
    if (!delegationProofLoaded_ || currentDelegationProofTxid_.trimmed().isEmpty()) {
        return;
    }

    // 1) Баланс адреса директоров (UTXO)
    try {
        if (delegatedUtxoBalanceLabel_ && !currentDelegatedAddress_.isEmpty()) {
            geocoin::Address delegatedAddr(currentDelegatedAddress_.toStdString());
            geocoin::Amount bal = chainState->get_balance(delegatedAddr);
            delegatedUtxoBalanceLabel_->setText(tr("%1 GC").arg(GUIUtil::formatAmount(bal)));
        }
    } catch (...) {
        if (delegatedUtxoBalanceLabel_) {
            delegatedUtxoBalanceLabel_->setText(tr("Error"));
        }
    }

    // 2) Лимиты по delegation proof (это не UTXO, это allowance)
    try {
        // txid hex -> Hash256
        const QString cleanTxid = currentDelegationProofTxid_.trimmed();
        geocoin::bytes hashBytes;
        hashBytes.reserve(32);
        for (int i = 0; i < cleanTxid.length(); i += 2) {
            bool ok = false;
            hashBytes.push_back(static_cast<geocoin::byte>(cleanTxid.mid(i, 2).toUInt(&ok, 16)));
            if (!ok) return;
        }
        if (hashBytes.size() != 32) {
            return;
        }
        geocoin::Hash256 txidHash;
        std::copy(hashBytes.begin(), hashBytes.end(), txidHash.begin());

        auto txOpt = chainState->get_transaction(txidHash);
        if (!txOpt) {
            return;
        }
        const geocoin::core::Transaction proofTx = *txOpt;
        if (proofTx.type != geocoin::core::TransactionType::DELEGATION_PROOF) {
            return;
        }

        const geocoin::Amount spent = chainState->get_delegation_total_spent(txidHash);
        const geocoin::Amount primaryBalance = chainState->get_balance(proofTx.primary_multisig_address);

        auto fmt = [](geocoin::Amount a) -> QString { return GUIUtil::formatAmount(a); };
        auto lim = [&](geocoin::Amount a) -> QString {
            return (a > 0) ? (fmt(a) + " GC") : QStringLiteral("∞");
        };

        const geocoin::Amount maxTotal = proofTx.max_total_delegated;
        const geocoin::Amount maxPerTx = proofTx.max_per_transaction;
        const uint8_t maxPercent = proofTx.max_percent_of_balance;

        geocoin::Amount remainingTotal = 0;
        bool totalLimited = false;
        if (maxTotal > 0) {
            totalLimited = true;
            remainingTotal = maxTotal - spent;
            if (remainingTotal < 0) remainingTotal = 0;
        }

        geocoin::Amount percentCap = 0;
        bool percentLimited = false;
        if (maxPercent > 0) {
            percentLimited = true;
            const geocoin::Amount percent = static_cast<geocoin::Amount>(maxPercent);
            percentCap = (primaryBalance / 100) * percent +
                ((primaryBalance % 100) * percent) / 100;
            if (percentCap < 0) percentCap = 0;
        }

        geocoin::Amount effectivePerTx = 0;
        bool perTxLimited = false;
        if (maxPerTx > 0) {
            effectivePerTx = maxPerTx;
            perTxLimited = true;
        }
        if (percentLimited) {
            effectivePerTx = perTxLimited ? std::min(effectivePerTx, percentCap) : percentCap;
            perTxLimited = true;
        }

        QString text;
        text += tr("Spent: %1 GC").arg(fmt(spent));
        text += tr("\nRemaining (max_total): %1").arg(totalLimited ? (fmt(remainingTotal) + " GC") : QStringLiteral("∞"));
        text += tr("\nLimit on transaction: %1").arg(perTxLimited ? (fmt(effectivePerTx) + " GC") : QStringLiteral("∞"));
        if (maxPercent > 0) {
            text += tr("\nPercent limit now: %1 GC (%2% from primary)")
                        .arg(fmt(percentCap))
                        .arg(maxPercent);
        }

        if (delegationAllowanceLabel_) {
            delegationAllowanceLabel_->setText(text);
        }
    } catch (...) {
        if (delegationAllowanceLabel_) {
            delegationAllowanceLabel_->setText(tr("Error"));
        }
    }
}

void CorporatePage::handleHierarchicalSpendDraftReady(const QString& draftJson, const QString& txHash)
{
    setDirectorDraftInProgress(false);

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(draftJson.toUtf8(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        handleHierarchicalSpendDraftFailed(tr("Invalid JSON: %1").arg(parseError.errorString()));
        return;
    }

    resetHierarchicalOperation();

    currentHierarchicalDraftDoc_ = doc;
    currentHierarchicalDraftJson_ = draftJson;
    currentHierarchicalSpendingHash_ = txHash;  // Сохраняем spending hash для сканера

    // Заполняем поле spending hash
    if (hierarchicalSpendingHashEdit_) {
        hierarchicalSpendingHashEdit_->setText(txHash);
    }

    // Включаем кнопки для spending hash
    if (copyHierarchicalHashButton_) copyHierarchicalHashButton_->setEnabled(true);
    if (showHierarchicalHashQRButton_) showHierarchicalHashQRButton_->setEnabled(true);

    hierarchicalDraftEdit_->setPlainText(doc.toJson(QJsonDocument::Indented));
    hierarchicalDraftHashLabel_->setText(tr("Hash for signatures: %1").arg(txHash));

    saveHierarchicalDraftButton_->setEnabled(true);
    copyHierarchicalDraftButton_->setEnabled(true);

    // Автоматически включаем автосканирование
    if (autoScanHierarchicalCheck_) {
        if (!autoScanHierarchicalCheck_->isChecked()) {
            autoScanHierarchicalCheck_->setChecked(true);
        } else {
            onAutoScanHierarchicalToggled(true);
        }
    }

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Draft created"),
        tr("Delegated spend draft created.\n\nSpending hash for directors:\n%1\n\nSend the spending hash to the directors for signing.\n\n🔄 Autoscan started!\nSignatures will be collected from the blockchain automatically.").arg(txHash));

    logInfo(tr("Draft delegated spend created (hash %1).").arg(txHash));
    logInfo(tr("📋 Spending hash: %1").arg(txHash));
    logInfo(tr("🔄 Auto-scanning hierarchical commitments started"));
    addHistoryEntry("HierarchicalDraft", tr("Successfully"), tr("Hash: %1").arg(txHash), draftJson);
}

void CorporatePage::handleHierarchicalSpendFinalizeReady(const QString& finalHex, const QString& broadcastTxid)
{
    setDirectorDraftInProgress(false);

    // UX/безопасность: после успешной финализации очищаем все временные поля,
    // чтобы нельзя было повторно финализировать другой draft старыми TXID/хешем.
    currentHierarchicalSpendingHash_.clear();
    currentHierarchicalDraftDoc_ = QJsonDocument();

    currentHierarchicalDraftJson_.clear();
    hierarchicalDraftEdit_->clear();
    hierarchicalDraftHashLabel_->setText(tr("Hash for signatures: —"));
    saveHierarchicalDraftButton_->setEnabled(false);
    copyHierarchicalDraftButton_->setEnabled(false);
    if (hierarchicalSpendingHashEdit_) {
        hierarchicalSpendingHashEdit_->clear();
    }
    for (QLineEdit *edit : hierarchicalCommitmentEdits_) {
        if (edit) edit->clear();
    }

    // Останавливаем автоскан после успешной финализации
    if (autoScanHierarchicalCheck_ && autoScanHierarchicalCheck_->isChecked()) {
        autoScanHierarchicalCheck_->setChecked(false);
    }
    resetHierarchicalScanner(false);
    if (scanStatusHierarchicalLabel_) {
        scanStatusHierarchicalLabel_->setText(tr("Status: ready to scan"));
        scanStatusHierarchicalLabel_->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    }
    updateHierarchicalCommitmentState();
    updateDirectorState();

    const QString txidToShow = extractHexTxid(broadcastTxid);

    QString message;
    if (broadcastTxid.isEmpty()) {
        message = tr("Delegated spend formed.\nHEX:\n%1").arg(finalHex);
    } else if (!txidToShow.isEmpty()) {
        message = tr("Delegated spend sent!\n\nTXID: %1").arg(txidToShow);
    } else {
        message = tr("Delegated spend sent, but line TXID invalid.\n\nRAW:\n%1").arg(broadcastTxid);
    }

    GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Delegated spend ready"), message);

    logInfo(broadcastTxid.isEmpty()
        ? tr("Delegated spend formed.")
        : (txidToShow.isEmpty() ? tr("Delegated spend sent.") : tr("Delegated spend sent (txid %1).").arg(txidToShow)));
    addHistoryEntry("HierarchicalFinalize", broadcastTxid.isEmpty() ? tr("Prepared") : tr("Sent"),
                   broadcastTxid.isEmpty()
                       ? tr("HEX length: %1").arg(finalHex.length())
                       : (txidToShow.isEmpty() ? tr("RAW length: %1").arg(broadcastTxid.length()) : tr("TXID: %1").arg(txidToShow)),
                   finalHex);
}

void CorporatePage::onAutoScanHierarchicalToggled(bool checked)
{
    if (checked) {
        if (currentHierarchicalSpendingHash_.isEmpty() && hierarchicalSpendingHashEdit_ && !hierarchicalSpendingHashEdit_->text().trimmed().isEmpty()) {
            currentHierarchicalSpendingHash_ = hierarchicalSpendingHashEdit_->text().trimmed();
        }
        // Проверяем что есть spending hash для сканирования
        if (currentHierarchicalSpendingHash_.isEmpty()) {
            QInputDialog dialog(this);
            dialog.setWindowTitle(tr("Spending hash"));
            dialog.setLabelText(tr("Paste spending hash (64 hex characters) for scanning commitments:"));
            dialog.setTextEchoMode(QLineEdit::Normal);
            GUIUtil::applyDialogStyle(&dialog);
            if (dialog.exec() != QDialog::Accepted) {
                autoScanHierarchicalCheck_->setChecked(false);
                return;
            }
            const QString hash = dialog.textValue().trimmed();
            if (hash.isEmpty()) {
                autoScanHierarchicalCheck_->setChecked(false);
                return;
            }
            currentHierarchicalSpendingHash_ = hash;
            if (hierarchicalSpendingHashEdit_) {
                hierarchicalSpendingHashEdit_->setText(currentHierarchicalSpendingHash_);
            }
        }

        if (!ensureHierarchicalScanner()) {
            GUIUtil::showStyledMessageBox(
                this, QMessageBox::Warning, tr("Error"),
                tr("Chain state is unavailable. Cannot start scanning."));
            autoScanHierarchicalCheck_->setChecked(false);
            return;
        }

        hierarchicalScanner_->startAutoScan(30);  // 30 секунд интервал
        scanNowHierarchicalButton_->setEnabled(true);
        logInfo(tr("Auto-scanning hierarchical commitments started"));

    } else {
        // Останавливаем сканер
        if (hierarchicalScanner_) {
            hierarchicalScanner_->stopAutoScan();
        }
        scanNowHierarchicalButton_->setEnabled(false);
        if (scanStatusHierarchicalLabel_) {
            scanStatusHierarchicalLabel_->setText(tr("Status: auto-scanning stopped"));
        }
        logInfo(tr("Auto-scanning hierarchical commitments stopped"));
    }
}

void CorporatePage::onScanNowHierarchical()
{
    if (currentHierarchicalSpendingHash_.isEmpty() && hierarchicalSpendingHashEdit_ && !hierarchicalSpendingHashEdit_->text().trimmed().isEmpty()) {
        currentHierarchicalSpendingHash_ = hierarchicalSpendingHashEdit_->text().trimmed();
    }
    if (currentHierarchicalSpendingHash_.isEmpty()) {
            QInputDialog dialog(this);
            dialog.setWindowTitle(tr("Spending hash"));
            dialog.setLabelText(tr("Paste spending hash (64 hex characters) for scanning commitments:"));
            dialog.setTextEchoMode(QLineEdit::Normal);
            GUIUtil::applyDialogStyle(&dialog);
            if (dialog.exec() != QDialog::Accepted) {
                return;
            }
            const QString hash = dialog.textValue().trimmed();
            if (hash.isEmpty()) {
                return;
            }
            currentHierarchicalSpendingHash_ = hash;
            if (hierarchicalSpendingHashEdit_) {
                hierarchicalSpendingHashEdit_->setText(currentHierarchicalSpendingHash_);
            }
    }
    if (!ensureHierarchicalScanner()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Chain state is unavailable."));
        return;
    }

    logInfo(tr("Manual check hierarchical commitments..."));
    hierarchicalScanner_->scanNow();
}

void CorporatePage::onHierarchicalCommitmentFound(const QString& txid, const QString& signerAddress)
{
    // Сначала проверяем, нет ли уже такого TXID в полях (дубликат)
    for (auto* edit : hierarchicalCommitmentEdits_) {
        if (edit->text().trimmed() == txid) {
            scannerHierarchicalFields_.insert(txid, edit);
            logInfo(tr("Commitment %1 is already added.").arg(txid.left(16) + "..."));
            return;  // Уже есть, не добавляем повторно
        }
    }

    // Дополнительная проверка: если знаем адреса директоров, проверяем что подписант среди них
    if (!delegatedConfigParticipants_.isEmpty()) {
        bool isValidSigner = false;
        for (const auto& participant : delegatedConfigParticipants_) {
            if (!participant.isObject()) continue;
            const QJsonObject obj = participant.toObject();
            const QString addr = obj.value("address").toString().trimmed();
            if (!addr.isEmpty() && addr == signerAddress) {
                isValidSigner = true;
                break;
            }
        }

        if (!isValidSigner) {
            logError(tr("Ignored a commitment from %1 because that address is not a director.")
                .arg(signerAddress.left(15) + "..."));
            return;
        }
    }

    // Находим первое пустое поле и вставляем TXID
    bool inserted = false;
    for (auto* edit : hierarchicalCommitmentEdits_) {
        if (edit->text().trimmed().isEmpty()) {
            edit->setText(txid);
            edit->setStyleSheet(GUIUtil::themedSuccessEditableStyleSheet());
            scannerHierarchicalFields_.insert(txid, edit);
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

    logInfo(tr("✅ Received signature from director %1").arg(signerAddress.left(15) + "..."));
    logInfo(tr("   TXID: %1").arg(txid));

    updateHierarchicalCommitmentState();

    // Показываем уведомление
    if (scanStatusHierarchicalLabel_) {
        int found = 0;
        for (auto* edit : hierarchicalCommitmentEdits_) {
            if (!edit->text().trimmed().isEmpty()) found++;
        }
        int required = delegatedMSpin_ ? delegatedMSpin_->value() : 3;
        scanStatusHierarchicalLabel_->setText(
            tr("✅ Found signature! Collected: %1/%2").arg(found).arg(required));
    }
}

void CorporatePage::onHierarchicalCommitmentInvalidated(const QString& txid, const QString& signerAddress)
{
    const auto it = scannerHierarchicalFields_.find(txid);
    if (it == scannerHierarchicalFields_.end()) return;
    const QPointer<QLineEdit> edit = it.value();
    scannerHierarchicalFields_.erase(it);
    if (edit && edit->text().trimmed() == txid) {
        edit->clear();
        edit->setStyleSheet(GUIUtil::themedEditableStyleSheet());
        updateHierarchicalCommitmentState();
        logError(tr("Commitment %1 from %2 was removed after a reorg")
            .arg(txid.left(16) + QStringLiteral("..."), signerAddress.left(15) + QStringLiteral("...")));
        if (scanStatusHierarchicalLabel_) {
            scanStatusHierarchicalLabel_->setText(tr("Reorg: a signature was invalidated; scanning continues"));
        }
    }
}

void CorporatePage::onAllHierarchicalCommitmentsCollected()
{
    logInfo(tr("🎉 All signatures hierarchical spend collected! Can finalize."));

    if (scanStatusHierarchicalLabel_) {
        scanStatusHierarchicalLabel_->setText(tr("🎉 All signatures collected! Ready to finalization."));
        scanStatusHierarchicalLabel_->setStyleSheet("QLabel { color: #27ae60; font-weight: bold; font-size: 12px; }");
    }

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Signatures collected"),
        tr("🎉 All required director signatures have been collected!\n\nYou can now finalize the hierarchical spend.\nPress \"Finalize hierarchical spend\".\nThe scanner keeps watching the chain until finalization."));
}

void CorporatePage::onHierarchicalScanError(const QString& error)
{
    logError(tr("Error scanning hierarchical: %1").arg(error));

    if (scanStatusHierarchicalLabel_) {
        scanStatusHierarchicalLabel_->setText(tr("Error: %1").arg(error));
        scanStatusHierarchicalLabel_->setStyleSheet("QLabel { color: #e74c3c; font-size: 11px; }");
    }
}

void CorporatePage::onHierarchicalScanStatus(const QString& message)
{
    if (scanStatusHierarchicalLabel_) {
        scanStatusHierarchicalLabel_->setText(tr("Status: %1").arg(message));
        scanStatusHierarchicalLabel_->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    }
}
