#include "corporatepage_internal.h"

void CorporatePage::resetDelegationScanner(bool clearCommitments)
{
    if (delegationScanner_) {
        delegationScanner_->stopAutoScan();
        delete delegationScanner_;
        delegationScanner_ = nullptr;
    }
    delegationScannerTarget_.clear();
    delegationScannerAddress_.clear();
    delegationScannerRequired_ = 0;
    scannerDelegationFields_.clear();
    if (clearCommitments) {
        for (QLineEdit* edit : std::as_const(delegationCommitmentEdits_)) {
            if (!edit) continue;
            edit->clear();
            edit->setStyleSheet(GUIUtil::themedEditableStyleSheet());
        }
        updateDelegationCommitmentState();
    }
}

void CorporatePage::resetDelegationOperation()
{
    if (autoScanDelegationCheck_ && autoScanDelegationCheck_->isChecked()) {
        autoScanDelegationCheck_->setChecked(false);
    }
    resetDelegationScanner(true);
    currentDelegationHash_.clear();
    currentDelegationDraftDoc_ = QJsonDocument();
    currentDelegationDraftJson_.clear();
    if (delegationHashEdit_) delegationHashEdit_->clear();
    if (delegationDraftEdit_) delegationDraftEdit_->clear();
    if (delegationDraftHashLabel_) delegationDraftHashLabel_->setText(tr("Hash for signatures: —"));
    if (saveDelegationDraftButton_) saveDelegationDraftButton_->setEnabled(false);
    if (copyDelegationDraftButton_) copyDelegationDraftButton_->setEnabled(false);
    updateDelegationCommitmentState();
}

bool CorporatePage::ensureDelegationScanner()
{
    if (!walletModel_ || !walletModel_->getChainState()) return false;
    const int required = primaryMSpin_ ? primaryMSpin_->value() : 2;
    if (delegationScanner_ &&
        (delegationScannerTarget_ != currentDelegationHash_ ||
         delegationScannerAddress_ != currentPrimaryAddress_ ||
         delegationScannerRequired_ != required)) {
        resetDelegationScanner(true);
    }
    if (!delegationScanner_) {
        QStringList allowedSigners;
        for (const QJsonValue& value : primaryConfigParticipants_) {
            const QString address = value.toObject().value(QStringLiteral("address")).toString().trimmed();
            if (!address.isEmpty()) allowedSigners.append(address);
        }
        delegationScanner_ = new CommitmentScanner(
            currentDelegationHash_, currentPrimaryAddress_, required,
            walletModel_->getChainState(), this, allowedSigners);
        delegationScannerTarget_ = currentDelegationHash_;
        delegationScannerAddress_ = currentPrimaryAddress_;
        delegationScannerRequired_ = required;
        connect(delegationScanner_, &CommitmentScanner::commitmentFound,
                this, &CorporatePage::onDelegationCommitmentFound);
        connect(delegationScanner_, &CommitmentScanner::commitmentInvalidated,
                this, &CorporatePage::onDelegationCommitmentInvalidated);
        connect(delegationScanner_, &CommitmentScanner::allCommitmentsCollected,
                this, &CorporatePage::onAllDelegationCommitmentsCollected);
        connect(delegationScanner_, &CommitmentScanner::scanError,
                this, &CorporatePage::onDelegationScanError);
        connect(delegationScanner_, &CommitmentScanner::statusUpdate,
                this, &CorporatePage::onDelegationScanStatus);
    }
    return true;
}

QWidget* CorporatePage::buildFounderPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setSpacing(15);

    layout->addWidget(buildFounderPrimaryConfigBox());
    layout->addWidget(buildFounderDelegatedConfigBox());
    layout->addWidget(buildFounderParametersBox());
    layout->addWidget(buildFounderDraftBox());
    layout->addWidget(buildFounderCommitmentsBox());
    layout->addStretch();

    return page;
}

QGroupBox* CorporatePage::buildFounderPrimaryConfigBox()
{
    QGroupBox *box = new QGroupBox(tr("Primary multisig (founders)"), this);
    QGridLayout *grid = new QGridLayout(box);
    grid->setColumnStretch(1, 1);

    primaryMultisigCombo_ = new QComboBox(box);
    primaryMultisigCombo_->addItem(tr("-- Select saved address --"), "");
    connect(primaryMultisigCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CorporatePage::onSavedPrimaryMultisigSelected);

    QPushButton *refreshSavedBtn = new QPushButton(tr("🔄"), box);
    refreshSavedBtn->setToolTip(tr("Refresh list saved addresses"));
    refreshSavedBtn->setMaximumWidth(40);
    connect(refreshSavedBtn, &QPushButton::clicked, this, &CorporatePage::loadSavedMultisigAddresses);

    grid->addWidget(new QLabel(tr("Saved addresses:")), 0, 0);
    grid->addWidget(primaryMultisigCombo_, 0, 1);
    grid->addWidget(refreshSavedBtn, 0, 2);

    QFrame *line = new QFrame(box);
    line->setFrameShape(QFrame::HLine);
    grid->addWidget(line, 1, 0, 1, 3);

    primaryConfigEdit_ = new QLineEdit(box);
    QPushButton *browseBtn = new QPushButton(tr("Load"), box);
    connect(browseBtn, &QPushButton::clicked, this, &CorporatePage::onLoadPrimaryConfig);

    QPushButton *addToWalletBtn = new QPushButton(tr("➕ Add to wallet"), box);
    addToWalletBtn->setToolTip(tr("Add current address founders in wallet for tracking"));
    connect(addToWalletBtn, &QPushButton::clicked, this, &CorporatePage::onAddPrimaryToWallet);

    grid->addWidget(new QLabel(tr("OR file:")), 2, 0);
    grid->addWidget(primaryConfigEdit_, 2, 1);

    QHBoxLayout *configButtonsLayout = new QHBoxLayout();
    configButtonsLayout->addWidget(browseBtn);
    configButtonsLayout->addWidget(addToWalletBtn);
    configButtonsLayout->setSpacing(5);
    QWidget *configButtonsWidget = new QWidget(box);
    configButtonsWidget->setLayout(configButtonsLayout);
    grid->addWidget(configButtonsWidget, 2, 2);

    primaryMSpin_ = new QSpinBox(box);
    primaryMSpin_->setRange(1, 15);
    primaryMSpin_->setValue(2);
    primaryMSpin_->setEnabled(true);  // Включено по умолчанию
    primaryNSpin_ = new QSpinBox(box);
    primaryNSpin_->setRange(1, 15);
    primaryNSpin_->setValue(3);
    primaryNSpin_->setEnabled(true);  // Включено по умолчанию

    // Валидация: M <= N
    connect(primaryMSpin_, QOverload<int>::of(&QSpinBox::valueChanged), [this](int m) {
        resetDelegationOperation();
        if (m > primaryNSpin_->value()) {
            primaryNSpin_->setValue(m);
        }
    });
    connect(primaryNSpin_, QOverload<int>::of(&QSpinBox::valueChanged), [this](int n) {
        resetDelegationOperation();
        if (primaryMSpin_->value() > n) {
            primaryMSpin_->setValue(n);
        }
        // Обновляем количество полей для подписей
        updateFounderCommitmentFields();
    });

    QHBoxLayout *mnLayout = new QHBoxLayout();
    mnLayout->addWidget(new QLabel(tr("M:")));
    mnLayout->addWidget(primaryMSpin_);
    mnLayout->addWidget(new QLabel(tr("of N:")));
    mnLayout->addWidget(primaryNSpin_);
    mnLayout->addWidget(new QLabel(tr("(can be edited manually or loaded from file)")));
    mnLayout->addStretch();
    QWidget *mnWidget = new QWidget(box);
    mnWidget->setLayout(mnLayout);
    grid->addWidget(mnWidget, 3, 0, 1, 3);

    // Секция создания нового адреса
    QLabel *createLabel = new QLabel(tr("<b>OR create new founders address:</b>"), box);
    grid->addWidget(createLabel, 4, 0, 1, 3);

    primaryParticipantsEdit_ = new QTextEdit(box);
    primaryParticipantsEdit_->setPlaceholderText(tr("Enter founders addresses (one per line):\nGEO1abc...xyz\nGEO2def...uvw\n..."));
    primaryParticipantsEdit_->setMinimumHeight(80);
    primaryParticipantsEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    grid->addWidget(new QLabel(tr("Founders addresses:")), 5, 0);
    grid->addWidget(primaryParticipantsEdit_, 5, 1, 1, 2);
    grid->setRowStretch(5, 1);

    QPushButton *createPrimaryBtn = new QPushButton(tr("Create address founders"), box);
    createPrimaryBtn->setStyleSheet("QPushButton { background-color: #3498db; color: white; font-weight: bold; padding: 8px; }");
    connect(createPrimaryBtn, &QPushButton::clicked, this, &CorporatePage::onCreatePrimaryAddress);
    grid->addWidget(createPrimaryBtn, 6, 0, 1, 3);

    primaryAddressLabel_ = new QLabel(tr("Address: —"), box);
    primaryAddressLabel_->setStyleSheet("QLabel { color: #27ae60; font-weight: bold; }");
    primaryAddressLabel_->setWordWrap(true);
    grid->addWidget(primaryAddressLabel_, 7, 0, 1, 3);

    primaryBalanceLabel_ = new QLabel(tr("Balance: —"), box);
    primaryBalanceLabel_->setStyleSheet("QLabel { color: #2980b9; font-weight: bold; }");
    grid->addWidget(primaryBalanceLabel_, 8, 0, 1, 3);

    return box;
}

QGroupBox* CorporatePage::buildFounderDelegatedConfigBox()
{
    QGroupBox *box = new QGroupBox(tr("Delegated multisig (directors)"), this);
    QGridLayout *grid = new QGridLayout(box);
    grid->setColumnStretch(1, 1);

    delegatedMultisigCombo_ = new QComboBox(box);
    delegatedMultisigCombo_->addItem(tr("-- Select saved address --"), "");
    connect(delegatedMultisigCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CorporatePage::onSavedDelegatedMultisigSelected);

    QPushButton *refreshSavedBtn = new QPushButton(tr("🔄"), box);
    refreshSavedBtn->setToolTip(tr("Refresh list saved addresses"));
    refreshSavedBtn->setMaximumWidth(40);
    connect(refreshSavedBtn, &QPushButton::clicked, this, &CorporatePage::loadSavedMultisigAddresses);

    grid->addWidget(new QLabel(tr("Saved addresses:")), 0, 0);
    grid->addWidget(delegatedMultisigCombo_, 0, 1);
    grid->addWidget(refreshSavedBtn, 0, 2);

    QFrame *line = new QFrame(box);
    line->setFrameShape(QFrame::HLine);
    grid->addWidget(line, 1, 0, 1, 3);

    delegatedConfigEdit_ = new QLineEdit(box);
    QPushButton *browseBtn = new QPushButton(tr("Load"), box);
    connect(browseBtn, &QPushButton::clicked, this, &CorporatePage::onLoadDelegatedConfig);

    QPushButton *addToWalletBtn = new QPushButton(tr("➕ Add to wallet"), box);
    addToWalletBtn->setToolTip(tr("Add current address directors in wallet for tracking"));
    connect(addToWalletBtn, &QPushButton::clicked, this, &CorporatePage::onAddDelegatedToWallet);

    grid->addWidget(new QLabel(tr("OR file:")), 2, 0);
    grid->addWidget(delegatedConfigEdit_, 2, 1);

    QHBoxLayout *configButtonsLayout = new QHBoxLayout();
    configButtonsLayout->addWidget(browseBtn);
    configButtonsLayout->addWidget(addToWalletBtn);
    configButtonsLayout->setSpacing(5);
    QWidget *configButtonsWidget = new QWidget(box);
    configButtonsWidget->setLayout(configButtonsLayout);
    grid->addWidget(configButtonsWidget, 2, 2);

    delegatedMSpin_ = new QSpinBox(box);
    delegatedMSpin_->setRange(1, 20);
    delegatedMSpin_->setValue(3);
    delegatedMSpin_->setEnabled(true);  // Включено по умолчанию
    delegatedNSpin_ = new QSpinBox(box);
    delegatedNSpin_->setRange(1, 20);
    delegatedNSpin_->setValue(5);
    delegatedNSpin_->setEnabled(true);  // Включено по умолчанию

    // Валидация: M <= N
    connect(delegatedMSpin_, QOverload<int>::of(&QSpinBox::valueChanged), [this](int m) {
        resetDelegationOperation();
        resetHierarchicalOperation();
        if (m > delegatedNSpin_->value()) {
            delegatedNSpin_->setValue(m);
        }
    });
    connect(delegatedNSpin_, QOverload<int>::of(&QSpinBox::valueChanged), [this](int n) {
        resetDelegationOperation();
        resetHierarchicalOperation();
        if (delegatedMSpin_->value() > n) {
            delegatedMSpin_->setValue(n);
        }
        // Обновляем количество полей для подписей
        updateDirectorCommitmentFields();
    });

    QHBoxLayout *mnLayout = new QHBoxLayout();
    mnLayout->addWidget(new QLabel(tr("M:")));
    mnLayout->addWidget(delegatedMSpin_);
    mnLayout->addWidget(new QLabel(tr("of N:")));
    mnLayout->addWidget(delegatedNSpin_);
    mnLayout->addWidget(new QLabel(tr("(can be edited manually or loaded from file)")));
    mnLayout->addStretch();
    QWidget *mnWidget = new QWidget(box);
    mnWidget->setLayout(mnLayout);
    grid->addWidget(mnWidget, 3, 0, 1, 3);

    // Секция создания нового адреса
    QLabel *createLabel = new QLabel(tr("<b>OR create new directors address:</b>"), box);
    grid->addWidget(createLabel, 4, 0, 1, 3);

    delegatedParticipantsEdit_ = new QTextEdit(box);
    delegatedParticipantsEdit_->setPlaceholderText(tr("Enter directors addresses (one per line):\nGEO1abc...xyz\nGEO2def...uvw\n..."));
    delegatedParticipantsEdit_->setMinimumHeight(80);
    delegatedParticipantsEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    grid->addWidget(new QLabel(tr("Directors addresses:")), 5, 0);
    grid->addWidget(delegatedParticipantsEdit_, 5, 1, 1, 2);
    grid->setRowStretch(5, 1);

    QPushButton *createDelegatedBtn = new QPushButton(tr("Create address directors"), box);
    createDelegatedBtn->setStyleSheet("QPushButton { background-color: #3498db; color: white; font-weight: bold; padding: 8px; }");
    connect(createDelegatedBtn, &QPushButton::clicked, this, &CorporatePage::onCreateDelegatedAddress);
    grid->addWidget(createDelegatedBtn, 6, 0, 1, 3);

    delegatedAddressLabel_ = new QLabel(tr("Address: —"), box);
    delegatedAddressLabel_->setStyleSheet("QLabel { color: #27ae60; font-weight: bold; }");
    delegatedAddressLabel_->setWordWrap(true);
    grid->addWidget(delegatedAddressLabel_, 7, 0, 1, 3);

    return box;
}

QGroupBox* CorporatePage::buildFounderParametersBox()
{
    QGroupBox *box = new QGroupBox(tr("Settings delegation"), this);
    QFormLayout *form = new QFormLayout(box);

    validFromSpin_ = new QSpinBox(box);
    validFromSpin_->setRange(0, 3650);
    validFromSpin_->setValue(0);
    validFromSpin_->setSpecialValueText(tr("Immediately"));
    form->addRow(tr("Start after (days):"), validFromSpin_);

    validUntilSpin_ = new QSpinBox(box);
    validUntilSpin_->setRange(0, 3650);
    validUntilSpin_->setValue(0);
    validUntilSpin_->setSpecialValueText(tr("No expiration"));
    form->addRow(tr("Valid for (days from today):"), validUntilSpin_);

    nonceSpin_ = new QSpinBox(box);
    nonceSpin_->setRange(1, 999999);
    nonceSpin_->setValue(1);
    form->addRow(tr("Number version (nonce):"), nonceSpin_);

    QLabel *infoLabel = new QLabel(tr("To revoke a delegation, create a new proof with a higher nonce."), box);
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; }");
    form->addRow(infoLabel);

    // Лимиты делегирования
    QFrame *line = new QFrame(box);
    line->setFrameShape(QFrame::HLine);
    form->addRow(line);

    QLabel *limitsLabel = new QLabel(tr("Limits delegation:"), box);
    limitsLabel->setStyleSheet("QLabel { font-weight: bold; color: #e74c3c; }");
    form->addRow(limitsLabel);

    maxTotalSpin_ = new QLineEdit(QStringLiteral("500.00000000"), box);
    maxTotalSpin_->setPlaceholderText(tr("Positive amount in GC"));
    form->addRow(tr("Max. total amount:"), maxTotalSpin_);

    maxPerTxSpin_ = new QLineEdit(QStringLiteral("100.00000000"), box);
    maxPerTxSpin_->setPlaceholderText(tr("0 = no per-transaction limit"));
    form->addRow(tr("Max. on transaction:"), maxPerTxSpin_);

    maxPercentSpin_ = new QSpinBox(box);
    maxPercentSpin_->setRange(0, 100);
    maxPercentSpin_->setValue(0);
    maxPercentSpin_->setSuffix("%");
    maxPercentSpin_->setSpecialValueText(tr("Without limit"));
    form->addRow(tr("Max. % from balance:"), maxPercentSpin_);

    const auto semanticPolicyChanged = [this]() {
        if (!currentDelegationHash_.isEmpty() || !currentDelegationDraftJson_.isEmpty()) {
            resetDelegationOperation();
        }
        updateFounderState();
    };
    connect(validFromSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, semanticPolicyChanged);
    connect(validUntilSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, semanticPolicyChanged);
    connect(nonceSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, semanticPolicyChanged);
    connect(maxTotalSpin_, &QLineEdit::textChanged, this, semanticPolicyChanged);
    connect(maxPerTxSpin_, &QLineEdit::textChanged, this, semanticPolicyChanged);
    connect(maxPercentSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, semanticPolicyChanged);

    QLabel *limitsInfo = new QLabel(tr("Directors can spend only within these limits."), box);
    limitsInfo->setWordWrap(true);
    limitsInfo->setStyleSheet("QLabel { color: #e67e22; font-style: italic; }");
    form->addRow(limitsInfo);

    return box;
}

QGroupBox* CorporatePage::buildFounderDraftBox()
{
    QGroupBox *box = new QGroupBox(tr("Creation proofs delegation"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    // Две кнопки: старый способ (черновик) и новый (создать и подписать)
    QHBoxLayout *buttonRow = new QHBoxLayout();

    createDelegationDraftButton_ = new QPushButton(tr("Create draft"), box);
    createDelegationDraftButton_->setStyleSheet("QPushButton { background-color: #95a5a6; color: white; font-weight: bold; padding: 10px; }");
    createDelegationDraftButton_->setEnabled(false);
    createDelegationDraftButton_->setToolTip(tr("Old method: create JSON draft for manual transfer"));
    connect(createDelegationDraftButton_, &QPushButton::clicked, this, &CorporatePage::onCreateDelegationProofDraft);
    buttonRow->addWidget(createDelegationDraftButton_);

    createAndSignDelegationButton_ = new QPushButton(tr("Create and sign"), box);
    createAndSignDelegationButton_->setStyleSheet("QPushButton { background-color: #3498db; color: white; font-weight: bold; padding: 10px; }");
    createAndSignDelegationButton_->setEnabled(false);
    createAndSignDelegationButton_->setToolTip(tr("New method: create, sign and publish commitment in blockchain"));
    connect(createAndSignDelegationButton_, &QPushButton::clicked, this, &CorporatePage::onCreateAndSignDelegationProof);
    buttonRow->addWidget(createAndSignDelegationButton_);

    layout->addLayout(buttonRow);

    // Разделитель
    QFrame *separator1 = new QFrame(box);
    separator1->setFrameShape(QFrame::HLine);
    separator1->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator1);

    // Секция для delegation hash (новый способ)
    QLabel *hashSectionLabel = new QLabel(tr("Delegation Hash (for automatic collection):"), box);
    hashSectionLabel->setStyleSheet("QLabel { font-weight: bold; color: #2980b9; }");
    layout->addWidget(hashSectionLabel);

    delegationHashEdit_ = new QLineEdit(box);
    delegationHashEdit_->setReadOnly(true);
    delegationHashEdit_->setPlaceholderText(tr("Delegation hash will appear here after creation and signing..."));
    delegationHashEdit_->setStyleSheet(GUIUtil::themedMonoEditableStyleSheet());
    layout->addWidget(delegationHashEdit_);

    QHBoxLayout *hashButtonLayout = new QHBoxLayout();
    copyDelegationHashButton_ = new QPushButton(tr("📋 Copy hash"), box);
    copyDelegationHashButton_->setEnabled(false);
    copyDelegationHashButton_->setToolTip(tr("Copy delegation hash for sending other signers"));
    connect(copyDelegationHashButton_, &QPushButton::clicked, this, &CorporatePage::onCopyDelegationHash);
    hashButtonLayout->addWidget(copyDelegationHashButton_);

    showDelegationHashQRButton_ = new QPushButton(tr("📱 QR-code"), box);
    showDelegationHashQRButton_->setEnabled(false);
    showDelegationHashQRButton_->setToolTip(tr("Show QR-code for scanning"));
    connect(showDelegationHashQRButton_, &QPushButton::clicked, this, &CorporatePage::onShowDelegationHashQR);
    hashButtonLayout->addWidget(showDelegationHashQRButton_);

    hashButtonLayout->addStretch();
    layout->addLayout(hashButtonLayout);

    QLabel *hashInfoLabel = new QLabel(tr("Send the delegation hash to the other founders so they can sign."), box);
    hashInfoLabel->setWordWrap(true);
    hashInfoLabel->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    layout->addWidget(hashInfoLabel);

    // Разделитель
    QFrame *separator2 = new QFrame(box);
    separator2->setFrameShape(QFrame::HLine);
    separator2->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator2);

    // Секция для черновика (старый способ)
    QLabel *draftSectionLabel = new QLabel(tr("JSON Draft (old method):"), box);
    draftSectionLabel->setStyleSheet("QLabel { font-weight: bold; color: #95a5a6; }");
    layout->addWidget(draftSectionLabel);

    delegationDraftEdit_ = new QTextEdit(box);
    delegationDraftEdit_->setReadOnly(true);
    delegationDraftEdit_->setMinimumHeight(120);
    delegationDraftEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    delegationDraftEdit_->setPlaceholderText(tr("JSON draft will appear here (if use old method)..."));
    layout->addWidget(delegationDraftEdit_);

    delegationDraftHashLabel_ = new QLabel(tr("Hash for signatures: —"), box);
    delegationDraftHashLabel_->setStyleSheet("QLabel { color: #e67e22; font-weight: bold; }");
    delegationDraftHashLabel_->setWordWrap(true);
    layout->addWidget(delegationDraftHashLabel_);

    QHBoxLayout *draftButtonLayout = new QHBoxLayout();
    saveDelegationDraftButton_ = new QPushButton(tr("Save"), box);
    saveDelegationDraftButton_->setEnabled(false);
    connect(saveDelegationDraftButton_, &QPushButton::clicked, this, &CorporatePage::onSaveDelegationDraft);
    copyDelegationDraftButton_ = new QPushButton(tr("Copy"), box);
    copyDelegationDraftButton_->setEnabled(false);
    connect(copyDelegationDraftButton_, &QPushButton::clicked, this, &CorporatePage::onCopyDelegationDraft);
    draftButtonLayout->addWidget(saveDelegationDraftButton_);
    draftButtonLayout->addWidget(copyDelegationDraftButton_);
    draftButtonLayout->addStretch();
    layout->addLayout(draftButtonLayout);

    return box;
}

QGroupBox* CorporatePage::buildFounderCommitmentsBox()
{
    QGroupBox *box = new QGroupBox(tr("Collection signatures from founders"), this);
    QVBoxLayout *layout = new QVBoxLayout(box);

    QLabel *infoLabel = new QLabel(tr("Required signatures: —"), box);
    infoLabel->setObjectName("founderCommitmentsInfo");
    layout->addWidget(infoLabel);

    // Создаем контейнер для динамических полей
    QWidget *commitmentsContainer = new QWidget(box);
    commitmentsContainer->setObjectName("founderCommitmentsContainer");
    QGridLayout *grid = new QGridLayout(commitmentsContainer);
    grid->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(commitmentsContainer);

    // Изначально создаем поля для текущего N
    updateFounderCommitmentFields();

    // Разделитель
    QFrame *separator = new QFrame(box);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    // Секция автоматического сканирования
    QLabel *scanLabel = new QLabel(tr("<b>Automatic collection signatures:</b>"), box);
    layout->addWidget(scanLabel);

    autoScanDelegationCheck_ = new QCheckBox(tr("🔄 Auto-scanning blockchain"), box);
    autoScanDelegationCheck_->setToolTip(tr("Automatically search commitments in new blocks (interval: 30 sec)"));
    autoScanDelegationCheck_->setChecked(false);
    connect(autoScanDelegationCheck_, &QCheckBox::toggled, this, &CorporatePage::onAutoScanDelegationToggled);
    layout->addWidget(autoScanDelegationCheck_);

    QHBoxLayout *scanButtonLayout = new QHBoxLayout();
    scanNowDelegationButton_ = new QPushButton(tr("🔍 Check now"), box);
    scanNowDelegationButton_->setToolTip(tr("Check blockchain directly now (without auto-scanning)"));
    scanNowDelegationButton_->setEnabled(false);
    connect(scanNowDelegationButton_, &QPushButton::clicked, this, &CorporatePage::onScanNowDelegation);
    scanButtonLayout->addWidget(scanNowDelegationButton_);
    scanButtonLayout->addStretch();
    layout->addLayout(scanButtonLayout);

    scanStatusDelegationLabel_ = new QLabel(tr("Status: ready to scan"), box);
    scanStatusDelegationLabel_->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    scanStatusDelegationLabel_->setWordWrap(true);
    layout->addWidget(scanStatusDelegationLabel_);

    QLabel *scanInfoLabel = new QLabel(tr("Scanning starts automatically after Create and sign."), box);
    scanInfoLabel->setWordWrap(true);
    scanInfoLabel->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 10px; }");
    layout->addWidget(scanInfoLabel);

    // Разделитель
    QFrame *separator2 = new QFrame(box);
    separator2->setFrameShape(QFrame::HLine);
    separator2->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator2);

    finalizeDelegationBroadcastCheck_ = new QCheckBox(tr("Send in network immediately"), box);
    finalizeDelegationBroadcastCheck_->setChecked(true);
    layout->addWidget(finalizeDelegationBroadcastCheck_);

    finalizeDelegationButton_ = new QPushButton(tr("Finalize delegation proof"), box);
    finalizeDelegationButton_->setStyleSheet("QPushButton { background-color: #27ae60; color: white; font-weight: bold; padding: 10px; }");
    finalizeDelegationButton_->setEnabled(false);
    connect(finalizeDelegationButton_, &QPushButton::clicked, this, &CorporatePage::onFinalizeDelegationProof);
    layout->addWidget(finalizeDelegationButton_);

    return box;
}

void CorporatePage::updateFounderCommitmentFields()
{
    // Получаем контейнер
    QWidget *container = findChild<QWidget*>("founderCommitmentsContainer");
    if (!container) return;

    QGridLayout *grid = qobject_cast<QGridLayout*>(container->layout());
    if (!grid) return;

    // Очищаем старые поля
    QLayoutItem *item;
    while ((item = grid->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    delegationCommitmentEdits_.clear();
    scannerDelegationFields_.clear();

    // Создаем новые поля в зависимости от N
    int n = primaryNSpin_ ? primaryNSpin_->value() : 3;
    for (int i = 0; i < n; ++i) {
        QLabel *label = new QLabel(tr("TXID %1:").arg(i + 1), container);
        QLineEdit *edit = new QLineEdit(container);
        edit->setPlaceholderText(tr("Paste TXID signatures %1...").arg(i + 1));
        connect(edit, &QLineEdit::textChanged, this, &CorporatePage::onDelegationCommitmentChanged);

        QPushButton *loadBtn = new QPushButton(tr("📁"), container);
        loadBtn->setToolTip(tr("Load commitment from file"));
        loadBtn->setMaximumWidth(40);
        loadBtn->setProperty("commitmentIndex", i);
        connect(loadBtn, &QPushButton::clicked, this, &CorporatePage::onLoadDelegationCommitmentFile);

        delegationCommitmentEdits_.append(edit);
        grid->addWidget(label, i, 0);
        grid->addWidget(edit, i, 1);
        grid->addWidget(loadBtn, i, 2);
    }
}

void CorporatePage::updateFounderState()
{
    bool hasConfigs = primaryConfigLoaded_ && delegatedConfigLoaded_;
    bool inputsReady = hasConfigs && validFromSpin_ && validUntilSpin_ && nonceSpin_;

    // Обе кнопки доступны при наличии конфигураций
    if (createDelegationDraftButton_) {
        createDelegationDraftButton_->setEnabled(inputsReady && !founderDraftInProgress_);
    }
    if (createAndSignDelegationButton_) {
        createAndSignDelegationButton_->setEnabled(inputsReady && !founderDraftInProgress_);
    }

    // Кнопки для delegation hash
    const bool hasHash = delegationHashEdit_ && !delegationHashEdit_->text().isEmpty();
    if (copyDelegationHashButton_) copyDelegationHashButton_->setEnabled(hasHash);
    if (showDelegationHashQRButton_) showDelegationHashQRButton_->setEnabled(hasHash);

    // Кнопки для черновика (старый способ)
    const bool hasDraft = delegationDraftEdit_ && !delegationDraftEdit_->toPlainText().isEmpty();
    if (saveDelegationDraftButton_) saveDelegationDraftButton_->setEnabled(hasDraft);
    if (copyDelegationDraftButton_) copyDelegationDraftButton_->setEnabled(hasDraft);

    updateDelegationCommitmentState();
}

void CorporatePage::updateDelegationCommitmentState()
{
    int filled = 0;
    for (QLineEdit *edit : std::as_const(delegationCommitmentEdits_)) {
        if (!edit->text().trimmed().isEmpty()) filled++;
    }

    int required = primaryMSpin_ ? primaryMSpin_->value() : 2;
    const bool ready = filled >= required;

    if (finalizeDelegationButton_) {
        finalizeDelegationButton_->setEnabled(ready && !founderDraftInProgress_ && !currentDelegationDraftJson_.isEmpty());
    }
    if (finalizeDelegationBroadcastCheck_) {
        finalizeDelegationBroadcastCheck_->setEnabled(!founderDraftInProgress_);
    }

    QLabel *infoLabel = findChild<QLabel*>("founderCommitmentsInfo");
    if (infoLabel) {
        infoLabel->setText(tr("Required signatures: %1 from %2 (collected: %3)")
                          .arg(required).arg(primaryNSpin_ ? primaryNSpin_->value() : 3).arg(filled));
    }
}

bool CorporatePage::validateFounderInputs(QString& error,
                                          qint64* parsedMaxTotal,
                                          qint64* parsedMaxPerTx) const
{
    if (!primaryConfigLoaded_) {
        error = tr("The founders configuration is not loaded.");
        return false;
    }
    if (!delegatedConfigLoaded_) {
        error = tr("The directors configuration is not loaded.");
        return false;
    }
    if (currentPrimaryAddress_.isEmpty()) {
        error = tr("The founders address is missing.");
        return false;
    }
    if (currentDelegatedAddress_.isEmpty()) {
        error = tr("The directors address is missing.");
        return false;
    }
    qint64 maxTotal = 0;
    qint64 maxPerTx = 0;
    if (!maxTotalSpin_ || !GUIUtil::parseAmount(maxTotalSpin_->text(), maxTotal) || maxTotal <= 0) {
        error = tr("The maximum total must be a positive number with up to 8 decimal places.");
        return false;
    }
    if (!maxPerTxSpin_ || !GUIUtil::parseAmount(maxPerTxSpin_->text(), maxPerTx)) {
        error = tr("The per-transaction maximum must have at most 8 decimal places.");
        return false;
    }
    if (maxPerTx > 0 && maxPerTx > maxTotal) {
        error = tr("The per-transaction limit cannot exceed the total limit.");
        return false;
    }
    if (parsedMaxTotal) *parsedMaxTotal = maxTotal;
    if (parsedMaxPerTx) *parsedMaxPerTx = maxPerTx;
    return true;
}

bool CorporatePage::delegationValidityTimestamps(quint64& validFrom,
                                                  quint64& validUntil,
                                                  QString& error) const
{
    if (!validFromSpin_ || !validUntilSpin_) {
        error = tr("Delegation period fields are unavailable.");
        return false;
    }
    constexpr quint64 kSecondsPerDay = 24ULL * 60ULL * 60ULL;
    const quint64 now = static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
    const quint64 startDelay = static_cast<quint64>(validFromSpin_->value()) * kSecondsPerDay;
    const quint64 lifetime = static_cast<quint64>(validUntilSpin_->value()) * kSecondsPerDay;
    validFrom = startDelay == 0 ? 0 : now + startDelay;
    validUntil = lifetime == 0 ? 0 : now + lifetime;
    if (validFrom > 0 && validUntil > 0 && validUntil <= validFrom) {
        error = tr("The expiry must be later than the start delay.");
        return false;
    }
    return true;
}

void CorporatePage::onLoadPrimaryConfig()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Select configuration file"),
        QString(), tr("JSON Files (*.json);;All Files (*)"));
    if (fileName.isEmpty()) return;

    primaryConfigEdit_->setText(fileName);
    if (loadConfigFromFile(fileName, true)) {
        logInfo(tr("Founders configuration loaded: %1").arg(currentPrimaryAddress_));
        updateFounderState();
    } else {
        logError(tr("Could not load the configuration"));
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Could not load the configuration file"));
    }
}

void CorporatePage::onLoadDelegatedConfig()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Select configuration file"),
        QString(), tr("JSON Files (*.json);;All Files (*)"));
    if (fileName.isEmpty()) return;

    delegatedConfigEdit_->setText(fileName);
    if (loadConfigFromFile(fileName, false)) {
        logInfo(tr("Directors configuration loaded: %1").arg(currentDelegatedAddress_));
        updateFounderState();
    } else {
        logError(tr("Could not load the configuration"));
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Could not load the configuration file"));
    }
}

void CorporatePage::onSavedPrimaryMultisigSelected(int index)
{
    if (index <= 0 || !primaryMultisigCombo_ || !walletModel_ || !walletModel_->getWallet()) {
        return; // Первый элемент - placeholder
    }

    QString address = primaryMultisigCombo_->itemData(index).toString();
    if (address.isEmpty()) {
        return;
    }

    // Проверка: нельзя использовать один адрес для учредителей и директоров
    if (!currentDelegatedAddress_.isEmpty() && address == currentDelegatedAddress_) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("This address is already used for directors.\n\nFounders and directors must use different addresses."));

        // Сбрасываем выбор
        primaryMultisigCombo_->setCurrentIndex(0);
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
        logError(tr("Error: configuration not found for addresses %1").arg(address));
        return;
    }

    resetDelegationOperation();

    currentPrimaryAddress_ = address;
    primaryAddressLabel_->setText(tr("✅ Address founders: %1").arg(currentPrimaryAddress_));
    primaryConfigLoaded_ = true;

    // Устанавливаем M и N
    if (foundConfig->m > 0 && foundConfig->n > 0) {
        primaryMSpin_->setValue(foundConfig->m);
        primaryNSpin_->setValue(foundConfig->n);

        // Сохраняем участников в primaryConfigParticipants_
        QJsonArray participantsArray;
        for (const auto& participant : foundConfig->participants) {
            QJsonObject p;
            p["address"] = QString::fromStdString(participant.address.str());
            participantsArray.append(p);
        }
        primaryConfigParticipants_ = participantsArray;

        logInfo(tr("Loaded address founders: %1 (%2-of-%3)").arg(address).arg(foundConfig->m).arg(foundConfig->n));
    } else {
        logInfo(tr("Loaded address founders: %1 (M/N unknown)").arg(address));
    }

    updateFounderState();
    updatePrimaryBalance();
}

void CorporatePage::onSavedDelegatedMultisigSelected(int index)
{
    if (index <= 0 || !delegatedMultisigCombo_ || !walletModel_ || !walletModel_->getWallet()) {
        return; // Первый элемент - placeholder
    }

    QString address = delegatedMultisigCombo_->itemData(index).toString();
    if (address.isEmpty()) {
        return;
    }

    // Проверка: нельзя использовать один адрес для учредителей и директоров
    if (!currentPrimaryAddress_.isEmpty() && address == currentPrimaryAddress_) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("This address already used as address founders!\n\nAddress founders and address directors must be different."));

        // Сбрасываем выбор
        delegatedMultisigCombo_->setCurrentIndex(0);
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
        logError(tr("Error: configuration not found for addresses %1").arg(address));
        return;
    }

    resetDelegationOperation();
    resetHierarchicalOperation();

    currentDelegatedAddress_ = address;
    delegatedAddressLabel_->setText(tr("✅ Address directors: %1").arg(currentDelegatedAddress_));
    delegatedConfigLoaded_ = true;

    // Устанавливаем M и N
    if (foundConfig->m > 0 && foundConfig->n > 0) {
        delegatedMSpin_->setValue(foundConfig->m);
        delegatedNSpin_->setValue(foundConfig->n);

        // Сохраняем участников в delegatedConfigParticipants_
        QJsonArray participantsArray;
        for (const auto& participant : foundConfig->participants) {
            QJsonObject p;
            p["address"] = QString::fromStdString(participant.address.str());
            participantsArray.append(p);
        }
        delegatedConfigParticipants_ = participantsArray;

        logInfo(tr("Loaded address directors: %1 (%2-of-%3)").arg(address).arg(foundConfig->m).arg(foundConfig->n));
    } else {
        logInfo(tr("Loaded address directors: %1 (M/N unknown)").arg(address));
    }

    updateFounderState();
    updateDelegatedBalance();
}

void CorporatePage::onCreatePrimaryAddress()
{
    // Получаем параметры
    uint8_t m = static_cast<uint8_t>(primaryMSpin_->value());
    uint8_t n = static_cast<uint8_t>(primaryNSpin_->value());

    // Парсим адреса из текстового поля
    QString participantsText = primaryParticipantsEdit_->toPlainText();
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
            tr("Pending %1 addresses founders, found %2.\n\nEnter by one address on line.")
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

        resetDelegationOperation();
        currentPrimaryAddress_ = QString::fromStdString(multisig.address);
        primaryAddressLabel_->setText(tr("✅ Address founders: %1").arg(currentPrimaryAddress_));

        // Сохраняем конфигурацию в JSON
        QJsonObject configObj;
        configObj["address"] = currentPrimaryAddress_;
        configObj["m"] = m;
        configObj["n"] = n;

        QJsonArray participantsArray;
        for (const QString& addr : addresses) {
            QJsonObject p;
            p["address"] = addr;
            participantsArray.append(p);
        }
        configObj["participants"] = participantsArray;

        primaryConfigParticipants_ = participantsArray;

        // Предлагаем сохранить конфигурацию
        QString suggestedName = QString("primary_founders_%1.json")
            .arg(currentPrimaryAddress_.left(10));

        QString saveFile = QFileDialog::getSaveFileName(
            this,
            tr("Save configuration founders"),
            suggestedName,
            tr("JSON files (*.json);;All files (*)")
        );

        if (!saveFile.isEmpty()) {
            const QJsonDocument doc(configObj);
            QString writeError;
            if (GUIUtil::writeFileAtomically(
                    saveFile, doc.toJson(QJsonDocument::Indented), writeError)) {

                primaryConfigEdit_->setText(saveFile);
                primaryConfigLoaded_ = true;

                // Добавляем мультиподпись адрес в кошелек для отслеживания
                if (walletModel_ && walletModel_->getWallet()) {
                    geocoin::wallet::MultisigConfig config;
                    config.m = m;
                    config.n = n;
                    config.address = geocoin::Address(currentPrimaryAddress_.toStdString());

                    for (const auto& addr : addrVec) {
                        config.addresses.push_back(addr);
                        config.participants.emplace_back(addr);
                    }

                    walletModel_->rememberMultisigConfig(config);
                    if (!saveWalletChanges()) return;
                }

                GUIUtil::showStyledMessageBox(
                    this,
                    QMessageBox::Information,
                    tr("Success"),
                    tr("Address founders created!\n\nAddress: %1\nType: %2-of-%3\n\nConfiguration saved in:\n%4\n\nSend this file all founders!")
                        .arg(currentPrimaryAddress_)
                        .arg(m).arg(n)
                        .arg(saveFile));

                logInfo(tr("Created address founders %1 (%2-of-%3)")
                    .arg(currentPrimaryAddress_).arg(m).arg(n));

                loadSavedMultisigAddresses();
            } else {
                GUIUtil::showStyledMessageBox(
                    this,
                    QMessageBox::Warning,
                    tr("Error"),
                    tr("Could not save the configuration file: %1").arg(writeError));
            }
        } else {
            primaryConfigLoaded_ = true;
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Information,
                tr("Address created"),
                tr("Founders address created:\n%1\n\nSave the configuration file before leaving this page.")
                    .arg(currentPrimaryAddress_));
        }

        updateFounderState();
        updatePrimaryBalance();

    } catch (const std::exception& e) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Critical,
            tr("Could not create the address"),
            tr("Could not create the founders address:\n%1")
                .arg(QString::fromStdString(e.what())));

        logError(tr("Could not create the founders address: %1")
            .arg(QString::fromStdString(e.what())));
    }
}

void CorporatePage::onCreateDelegatedAddress()
{
    // Получаем параметры
    uint8_t m = static_cast<uint8_t>(delegatedMSpin_->value());
    uint8_t n = static_cast<uint8_t>(delegatedNSpin_->value());

    // Парсим адреса из текстового поля
    QString participantsText = delegatedParticipantsEdit_->toPlainText();
    QStringList lines = participantsText.split('\n', Qt::SkipEmptyParts);

    QStringList addresses;
    for (const QString& line : lines) {
        QString addr = line.trimmed();
        if (addr.startsWith('#')) continue;
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
            tr("Pending %1 addresses directors, found %2.\n\nEnter by one address on line.")
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

        resetDelegationOperation();
        resetHierarchicalOperation();
        currentDelegatedAddress_ = QString::fromStdString(multisig.address);
        delegatedAddressLabel_->setText(tr("✅ Address directors: %1").arg(currentDelegatedAddress_));

        // Сохраняем конфигурацию в JSON
        QJsonObject configObj;
        configObj["address"] = currentDelegatedAddress_;
        configObj["m"] = m;
        configObj["n"] = n;

        QJsonArray participantsArray;
        for (const QString& addr : addresses) {
            QJsonObject p;
            p["address"] = addr;
            participantsArray.append(p);
        }
        configObj["participants"] = participantsArray;

        delegatedConfigParticipants_ = participantsArray;

        // Предлагаем сохранить конфигурацию
        QString suggestedName = QString("delegated_directors_%1.json")
            .arg(currentDelegatedAddress_.left(10));

        QString saveFile = QFileDialog::getSaveFileName(
            this,
            tr("Save configuration directors"),
            suggestedName,
            tr("JSON files (*.json);;All files (*)")
        );

        if (!saveFile.isEmpty()) {
            const QJsonDocument doc(configObj);
            QString writeError;
            if (GUIUtil::writeFileAtomically(
                    saveFile, doc.toJson(QJsonDocument::Indented), writeError)) {

                delegatedConfigEdit_->setText(saveFile);
                delegatedConfigLoaded_ = true;

                // Добавляем мультиподпись адрес в кошелек для отслеживания
                if (walletModel_ && walletModel_->getWallet()) {
                    geocoin::wallet::MultisigConfig config;
                    config.m = m;
                    config.n = n;
                    config.address = geocoin::Address(currentDelegatedAddress_.toStdString());

                    for (const auto& addr : addrVec) {
                        config.addresses.push_back(addr);
                        config.participants.emplace_back(addr);
                    }

                    walletModel_->rememberMultisigConfig(config);
                    if (!saveWalletChanges()) return;
                }

                GUIUtil::showStyledMessageBox(
                    this,
                    QMessageBox::Information,
                    tr("Success"),
                    tr("Address directors created!\n\nAddress: %1\nType: %2-of-%3\n\nConfiguration saved in:\n%4\n\nSend this file all directors!")
                        .arg(currentDelegatedAddress_)
                        .arg(m).arg(n)
                        .arg(saveFile));

                logInfo(tr("Created address directors %1 (%2-of-%3)")
                    .arg(currentDelegatedAddress_).arg(m).arg(n));

                loadSavedMultisigAddresses();
            } else {
                GUIUtil::showStyledMessageBox(
                    this,
                    QMessageBox::Warning,
                    tr("Error"),
                    tr("Could not save the configuration file: %1").arg(writeError));
            }
        } else {
            delegatedConfigLoaded_ = true;
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Information,
                tr("Address created"),
                tr("Directors address created:\n%1\n\nSave the configuration file before leaving this page.")
                    .arg(currentDelegatedAddress_));
        }

        updateFounderState();
        updateDelegatedBalance();

    } catch (const std::exception& e) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Critical,
            tr("Could not create the address"),
            tr("Could not create the directors address:\n%1")
                .arg(QString::fromStdString(e.what())));

        logError(tr("Could not create the directors address: %1")
            .arg(QString::fromStdString(e.what())));
    }
}

void CorporatePage::onAddPrimaryToWallet()
{
    if (currentPrimaryAddress_.isEmpty()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("No addresses"),
            tr("Load a configuration or create a founders address first."));
        return;
    }

    if (!walletModel_ || !walletModel_->getWallet()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Wallet not loaded."));
        return;
    }

    // Запрашиваем метку у пользователя
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Add address to wallet"));
    dialog.setLabelText(tr("Label for the founders address %1:").arg(currentPrimaryAddress_));
    dialog.setTextValue(tr("%1-of-%2 Founders").arg(primaryMSpin_->value()).arg(primaryNSpin_->value()));
    GUIUtil::applyDialogStyle(&dialog);

    if (dialog.exec() != QDialog::Accepted || dialog.textValue().isEmpty()) {
        return;
    }

    QString label = dialog.textValue();

    // Создаем конфигурацию
    geocoin::wallet::MultisigConfig config;
    config.m = primaryMSpin_->value();
    config.n = primaryNSpin_->value();
    config.address = geocoin::Address(currentPrimaryAddress_.toStdString());

    // Добавляем участников из сохраненного JSON
    for (const QJsonValue& val : primaryConfigParticipants_) {
        QJsonObject p = val.toObject();
        QString addrStr = p.value("address").toString();

        geocoin::Address addr(addrStr.toStdString());
        config.addresses.push_back(addr);
        config.participants.emplace_back(addr);
    }

    // Добавляем в кошелек
    if (walletModel_->rememberMultisigConfig(config)) {
        if (!saveWalletChanges()) return;

        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Information,
            tr("Success"),
            tr("Address founders added in wallet!\n\nAddress: %1\nLabel: %2\n\nNow you can track balance and create transactions with this addresses.")
                .arg(currentPrimaryAddress_)
                .arg(label));

        logInfo(tr("Address founders %1 added in wallet").arg(currentPrimaryAddress_));

        loadSavedMultisigAddresses();
    } else {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("Could not add the address to the wallet. It may already be there."));
    }
}

void CorporatePage::onAddDelegatedToWallet()
{
    if (currentDelegatedAddress_.isEmpty()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("No addresses"),
            tr("Load a configuration or create a directors address first."));
        return;
    }

    if (!walletModel_ || !walletModel_->getWallet()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Wallet not loaded."));
        return;
    }

    // Запрашиваем метку у пользователя
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Add address to wallet"));
    dialog.setLabelText(tr("Label for the directors address %1:").arg(currentDelegatedAddress_));
    dialog.setTextValue(tr("%1-of-%2 Directors").arg(delegatedMSpin_->value()).arg(delegatedNSpin_->value()));
    GUIUtil::applyDialogStyle(&dialog);

    if (dialog.exec() != QDialog::Accepted || dialog.textValue().isEmpty()) {
        return;
    }

    QString label = dialog.textValue();

    // Создаем конфигурацию
    geocoin::wallet::MultisigConfig config;
    config.m = delegatedMSpin_->value();
    config.n = delegatedNSpin_->value();
    config.address = geocoin::Address(currentDelegatedAddress_.toStdString());

    // Добавляем участников из сохраненного JSON
    for (const QJsonValue& val : delegatedConfigParticipants_) {
        QJsonObject p = val.toObject();
        QString addrStr = p.value("address").toString();

        geocoin::Address addr(addrStr.toStdString());
        config.addresses.push_back(addr);
        config.participants.emplace_back(addr);
    }

    // Добавляем в кошелек
    if (walletModel_->rememberMultisigConfig(config)) {
        if (!saveWalletChanges()) return;

        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Information,
            tr("Success"),
            tr("Address directors added in wallet!\n\nAddress: %1\nLabel: %2\n\nNow you can track balance and create transactions with this addresses.")
                .arg(currentDelegatedAddress_)
                .arg(label));

        logInfo(tr("Address directors %1 added in wallet").arg(currentDelegatedAddress_));

        loadSavedMultisigAddresses();
    } else {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Error"),
            tr("Could not add the address to the wallet. It may already be there."));
    }
}

void CorporatePage::onCreateDelegationProofDraft()
{
    QString error;
    qint64 maxTotal = 0;
    qint64 maxPerTx = 0;
    if (!validateFounderInputs(error, &maxTotal, &maxPerTx)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Invalid data"), error);
        return;
    }
    quint64 validFrom = 0;
    quint64 validUntil = 0;
    if (!delegationValidityTimestamps(validFrom, validUntil, error)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Invalid data"), error);
        return;
    }

    // Создаем JSON конфигурации из текущих данных
    QJsonObject primaryConfigObj;
    primaryConfigObj["m"] = primaryMSpin_->value();
    primaryConfigObj["n"] = primaryNSpin_->value();
    primaryConfigObj["address"] = currentPrimaryAddress_;
    primaryConfigObj["participants"] = primaryConfigParticipants_;
    QJsonDocument primaryConfigDoc(primaryConfigObj);
    QString primaryConfigJson = QString::fromUtf8(primaryConfigDoc.toJson(QJsonDocument::Compact));

    QJsonObject delegatedConfigObj;
    delegatedConfigObj["m"] = delegatedMSpin_->value();
    delegatedConfigObj["n"] = delegatedNSpin_->value();
    delegatedConfigObj["address"] = currentDelegatedAddress_;
    delegatedConfigObj["participants"] = delegatedConfigParticipants_;
    QJsonDocument delegatedConfigDoc(delegatedConfigObj);
    QString delegatedConfigJson = QString::fromUtf8(delegatedConfigDoc.toJson(QJsonDocument::Compact));

    logInfo(tr("Creation draft delegation proof (automatic mode)..."));
    setFounderDraftInProgress(true);

    emit requestDelegationProofDraft(
        primaryConfigJson,
        delegatedConfigJson,
        validFrom,
        validUntil,
        nonceSpin_->value(),
        maxTotal,
        maxPerTx,
        maxPercentSpin_->value());
}

void CorporatePage::onCreateAndSignDelegationProof()
{
    QString error;
    qint64 maxTotal = 0;
    qint64 maxPerTx = 0;
    if (!validateFounderInputs(error, &maxTotal, &maxPerTx)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Invalid data"), error);
        return;
    }
    quint64 validFrom = 0;
    quint64 validUntil = 0;
    if (!delegationValidityTimestamps(validFrom, validUntil, error)) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Invalid data"), error);
        return;
    }

    // Создаем диалог для выбора источника ключа
    QDialog keyDialog(this);
    keyDialog.setWindowTitle(tr("Selection source private key"));
    keyDialog.setMinimumWidth(400);

    QVBoxLayout *layout = new QVBoxLayout(&keyDialog);

    QLabel *infoLabel = new QLabel(tr("Select source private key for signing:"), &keyDialog);
    layout->addWidget(infoLabel);

    QButtonGroup *sourceButtons = new QButtonGroup(&keyDialog);

    QRadioButton *walletKeyBtn = new QRadioButton(tr("From wallet"), &keyDialog);
    QRadioButton *fileKeyBtn = new QRadioButton(tr("From file"), &keyDialog);
    QRadioButton *manualKeyBtn = new QRadioButton(tr("Enter manually (HEX)"), &keyDialog);

    sourceButtons->addButton(walletKeyBtn, 0);
    sourceButtons->addButton(fileKeyBtn, 1);
    sourceButtons->addButton(manualKeyBtn, 2);

    // По умолчанию выбираем кошелек если есть адреса
    if (walletModel_ && !walletModel_->getAddress().isEmpty()) {
        walletKeyBtn->setChecked(true);
    } else {
        manualKeyBtn->setChecked(true);
    }

    layout->addWidget(walletKeyBtn);

    QComboBox *walletKeyCombo = new QComboBox(&keyDialog);
    walletKeyCombo->setEnabled(walletKeyBtn->isChecked());
    if (walletModel_) {
        QString primaryAddress = walletModel_->getAddress();
        if (!primaryAddress.isEmpty()) {
            walletKeyCombo->addItem(primaryAddress);
        }
    }
    layout->addWidget(walletKeyCombo);

    layout->addWidget(fileKeyBtn);

    QHBoxLayout *fileLayout = new QHBoxLayout();
    QLineEdit *fileKeyPathEdit = new QLineEdit(&keyDialog);
    fileKeyPathEdit->setEnabled(fileKeyBtn->isChecked());
    fileKeyPathEdit->setPlaceholderText(tr("Path to file with key"));
    QPushButton *browseBtn = new QPushButton(tr("Overview..."), &keyDialog);
    browseBtn->setEnabled(fileKeyBtn->isChecked());
    fileLayout->addWidget(fileKeyPathEdit);
    fileLayout->addWidget(browseBtn);
    QWidget *fileWidget = new QWidget(&keyDialog);
    fileWidget->setLayout(fileLayout);
    layout->addWidget(fileWidget);

    layout->addWidget(manualKeyBtn);

    QLineEdit *manualKeyEdit = new QLineEdit(&keyDialog);
    manualKeyEdit->setEnabled(manualKeyBtn->isChecked());
    manualKeyEdit->setPlaceholderText(tr("Enter private key (64 hex characters)"));
    manualKeyEdit->setEchoMode(QLineEdit::Password);
    layout->addWidget(manualKeyEdit);

    // Обновляем состояние полей при изменении выбора
    connect(sourceButtons, QOverload<int>::of(&QButtonGroup::idClicked), [&](int id) {
        walletKeyCombo->setEnabled(id == 0);
        fileKeyPathEdit->setEnabled(id == 1);
        browseBtn->setEnabled(id == 1);
        manualKeyEdit->setEnabled(id == 2);
    });

    // Кнопка обзора для выбора файла
    connect(browseBtn, &QPushButton::clicked, [&]() {
        QString fileName = QFileDialog::getOpenFileName(&keyDialog, tr("Select file with key"),
            QString(), tr("Text Files (*.txt);;All Files (*)"));
        if (!fileName.isEmpty()) {
            fileKeyPathEdit->setText(fileName);
        }
    });

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    QPushButton *okBtn = new QPushButton(tr("OK"), &keyDialog);
    QPushButton *cancelBtn = new QPushButton(tr("Cancel"), &keyDialog);
    buttonLayout->addStretch();
    buttonLayout->addWidget(okBtn);
    buttonLayout->addWidget(cancelBtn);
    layout->addLayout(buttonLayout);

    connect(okBtn, &QPushButton::clicked, &keyDialog, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &keyDialog, &QDialog::reject);

    GUIUtil::applyDialogStyle(&keyDialog);

    if (keyDialog.exec() != QDialog::Accepted) {
        logInfo(tr("Creation cancelled user"));
        return;
    }

    int sourceId = sourceButtons->checkedId();
    QString privateKeyHex;
    GUIUtil::ScopedQStringCleanser privateKeyHexCleanser(privateKeyHex);
    QString walletAddress;
    QString walletPassword;
    GUIUtil::ScopedQStringCleanser walletPasswordCleanser(walletPassword);

    if (sourceId == 0) {
        // Из кошелька
        if (!walletModel_ || !walletModel_->isWalletLoaded() || walletKeyCombo->count() == 0) {
            GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Wallet not loaded."));
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
        logInfo(tr("Used private key from wallet for addresses %1")
                    .arg(walletAddress));
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
        logInfo(tr("Using the private key from the protected file"));
    } else {
        // Вручную
        privateKeyHex = manualKeyEdit->text().trimmed();
        logInfo(tr("Used key entered manually"));
    }

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

    logInfo(tr("Creation and signing delegation proof..."));
    setFounderDraftInProgress(true);

    // Создаем JSON конфигурации из данных в памяти
    QJsonObject primaryConfigObj;
    primaryConfigObj["address"] = currentPrimaryAddress_;
    primaryConfigObj["m"] = primaryMSpin_->value();
    primaryConfigObj["n"] = primaryNSpin_->value();
    primaryConfigObj["participants"] = primaryConfigParticipants_;
    QJsonDocument primaryConfigDoc(primaryConfigObj);
    QString primaryConfigJson = QString::fromUtf8(primaryConfigDoc.toJson(QJsonDocument::Compact));

    QJsonObject delegatedConfigObj;
    delegatedConfigObj["address"] = currentDelegatedAddress_;
    delegatedConfigObj["m"] = delegatedMSpin_->value();
    delegatedConfigObj["n"] = delegatedNSpin_->value();
    delegatedConfigObj["participants"] = delegatedConfigParticipants_;
    QJsonDocument delegatedConfigDoc(delegatedConfigObj);
    QString delegatedConfigJson = QString::fromUtf8(delegatedConfigDoc.toJson(QJsonDocument::Compact));

    if (sourceId == 0) {
        emit requestCreateAndSignDelegationProofWithWalletKey(
            primaryConfigJson,
            delegatedConfigJson,
            validFrom,
            validUntil,
            nonceSpin_->value(),
            maxTotal,
            maxPerTx,
            maxPercentSpin_->value(),
            walletAddress,
            walletPassword);
    } else {
        emit requestCreateAndSignDelegationProof(
            primaryConfigJson,
            delegatedConfigJson,
            validFrom,
            validUntil,
            nonceSpin_->value(),
            maxTotal,
            maxPerTx,
            maxPercentSpin_->value(),
            privateKeyHex);
    }
}

void CorporatePage::onDelegationCommitmentChanged()
{
    updateDelegationCommitmentState();
}

void CorporatePage::onFinalizeDelegationProof()
{
    QStringList txids;
    QSet<QString> uniqueTxids;
    for (QLineEdit *edit : std::as_const(delegationCommitmentEdits_)) {
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

    int required = primaryMSpin_->value();
    if (txids.size() < required) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Not enough signatures"),
            tr("At least %1 commitment transaction ids are required.").arg(required));
        return;
    }

    if (currentDelegationDraftJson_.isEmpty()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("No draft"), tr("Create a draft first."));
        return;
    }

    if (currentDelegationHash_.isEmpty() || currentPrimaryAddress_.isEmpty() ||
        !walletModel_ || !walletModel_->getChainState()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning,
            tr("Signatures cannot be checked"),
            tr("Missing the delegation hash, primary address, or access to the active chain."));
        return;
    }
    QStringList inactiveTxids;
    QStringList allowedSigners;
    for (const QJsonValue& value : primaryConfigParticipants_) {
        const QString address = value.toObject().value(QStringLiteral("address")).toString().trimmed();
        if (!address.isEmpty()) allowedSigners.append(address);
    }
    QSet<QString> activeSigners;
    for (const QString& txid : std::as_const(txids)) {
        QString signerAddress;
        if (!CommitmentScanner::isCommitmentActive(
                txid, currentDelegationHash_, currentPrimaryAddress_,
                walletModel_->getChainState(), allowedSigners, &signerAddress) ||
            activeSigners.contains(signerAddress)) {
            inactiveTxids.append(txid);
        } else {
            activeSigners.insert(signerAddress);
        }
    }
    if (!inactiveTxids.isEmpty()) {
        for (const QString& txid : std::as_const(inactiveTxids)) {
            if (scannerDelegationFields_.contains(txid)) {
                onDelegationCommitmentInvalidated(txid, tr("unknown signer"));
            }
        }
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning,
            tr("Signatures were not confirmed"),
            tr("Some commitments are not in the current chain, do not match the draft or participants, or repeat a signer:\n%1")
                .arg(inactiveTxids.join(QLatin1Char('\n'))));
        return;
    }

    logInfo(tr("Finalization delegation proof with %1 signatures...").arg(txids.size()));

    emit requestDelegationProofFinalize(
        currentDelegationDraftJson_,
        txids,
        finalizeDelegationBroadcastCheck_->isChecked());
}

void CorporatePage::onSaveDelegationDraft()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save draft"),
        QString("delegation_draft_%1.json").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("JSON Files (*.json)"));
    if (fileName.isEmpty()) return;

    QString error;
    if (!GUIUtil::writeFileAtomically(fileName, currentDelegationDraftJson_.toUtf8(), error)) {
        GUIUtil::showStyledMessageBox(
            this, QMessageBox::Warning, tr("Error"),
            tr("Could not save the file: %1").arg(error));
        return;
    }
    logInfo(tr("Draft saved: %1").arg(fileName));
}

void CorporatePage::onCopyDelegationDraft()
{
    QApplication::clipboard()->setText(currentDelegationDraftJson_);
    logInfo(tr("Draft copied in buffer exchange"));
    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Copied"),
        tr("Draft delegation proof copied.\nSend its signers."));
}

void CorporatePage::onCopyDelegationHash()
{
    if (!delegationHashEdit_ || delegationHashEdit_->text().isEmpty()) {
        return;
    }

    QString hash = delegationHashEdit_->text();
    QApplication::clipboard()->setText(hash);
    logInfo(tr("Delegation hash copied in buffer exchange"));

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Hash copied"),
        tr("Delegation hash copied in buffer exchange:\n\n%1\n\nSend its other founders for signing.\nThey can use its on tab \"Signer\".").arg(hash));
}

void CorporatePage::onShowDelegationHashQR()
{
    if (!delegationHashEdit_ || delegationHashEdit_->text().isEmpty()) {
        return;
    }

    QString hash = delegationHashEdit_->text();
    GUIUtil::showQRCodeDialog(this,
        tr("QR-code Delegation Hash"),
        hash,
        tr("Scan this QR-code for receiving delegation hash"));
    logInfo(tr("Shown QR-code for delegation hash"));
}

void CorporatePage::onLoadDelegationCommitmentFile()
{
    QPushButton *btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;

    int index = btn->property("commitmentIndex").toInt();
    if (index < 0 || index >= delegationCommitmentEdits_.size()) return;

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
        delegationCommitmentEdits_[index]->setText(txid);
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
// Director Slots
// ============================================================================

void CorporatePage::handleDelegationProofDraftReady(const QString& draftJson, const QString& txHash)
{
    setFounderDraftInProgress(false);

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(draftJson.toUtf8(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        handleDelegationProofDraftFailed(tr("Invalid JSON: %1").arg(parseError.errorString()));
        return;
    }

    resetDelegationOperation();

    currentDelegationDraftDoc_ = doc;
    currentDelegationDraftJson_ = draftJson;
    currentDelegationHash_ = doc.object().value(QStringLiteral("delegation_hash")).toString(txHash);
    if (delegationHashEdit_) delegationHashEdit_->setText(currentDelegationHash_);
    delegationDraftEdit_->setPlainText(doc.toJson(QJsonDocument::Indented));
    delegationDraftHashLabel_->setText(tr("Hash for signatures: %1").arg(txHash));

    saveDelegationDraftButton_->setEnabled(true);
    copyDelegationDraftButton_->setEnabled(true);

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Draft created"),
        tr("Draft delegation proof successfully created.\nSend JSON signers."));
    logInfo(tr("Draft delegation proof created (hash %1).").arg(txHash));
    addHistoryEntry("DelegationDraft", tr("Successfully"), tr("Hash: %1").arg(txHash), draftJson);
}

void CorporatePage::handleCreateAndSignReady(const QString& delegationHash, const QString& myCommitmentTxid, const QString& draftJson)
{
    setFounderDraftInProgress(false);

    resetDelegationOperation();

    // Сохраняем черновик
    currentDelegationDraftJson_ = draftJson;

    // Парсим и сохраняем документ
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(draftJson.toUtf8(), &parseError);
    if (!doc.isNull() && doc.isObject()) {
        currentDelegationDraftDoc_ = doc;
        delegationDraftEdit_->setPlainText(doc.toJson(QJsonDocument::Indented));
    }

    // Сохраняем delegation hash
    currentDelegationHash_ = delegationHash;
    if (delegationHashEdit_) {
        delegationHashEdit_->setText(delegationHash);
    }

    // Вставляем свою подпись в первое поле commitments
    if (!delegationCommitmentEdits_.isEmpty()) {
        delegationCommitmentEdits_[0]->setText(myCommitmentTxid);
        delegationCommitmentEdits_[0]->setStyleSheet(GUIUtil::themedSuccessEditableStyleSheet());
    }

    // Обновляем состояние кнопок
    updateFounderState();

    // Автоматически запускаем сканер
    if (autoScanDelegationCheck_) {
        if (!autoScanDelegationCheck_->isChecked()) {
            autoScanDelegationCheck_->setChecked(true);
        } else {
            onAutoScanDelegationToggled(true);
        }
    }

    // Показываем информационное сообщение
    QString message = tr("✅ Your commitment sent in blockchain!\n\nTXID: %1\n\nDelegation hash for other founders:\n%2\n\n📋 Send delegation hash other founders.\nThey can sign its on tab \"Signer\".\n\n🔄 Auto-scanning started!\nSystem automatically find signatures in blockchain.")
        .arg(myCommitmentTxid, delegationHash);

    GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Commitment created"), message);

    logInfo(tr("✅ Your commitment sent! TXID: %1").arg(myCommitmentTxid));
    logInfo(tr("📋 Delegation hash: %1").arg(delegationHash));
    logInfo(tr("🔄 Auto-scanning started. Waiting signatures..."));

    // Сохраняем в историю
    addHistoryEntry("CreateAndSign", tr("Successfully"),
                   tr("Delegation hash: %1\nCommitment TXID: %2").arg(delegationHash, myCommitmentTxid),
                   delegationHash);
}

void CorporatePage::onAutoScanDelegationToggled(bool checked)
{
    if (checked) {
        if (currentDelegationHash_.isEmpty() && delegationHashEdit_ && !delegationHashEdit_->text().trimmed().isEmpty()) {
            currentDelegationHash_ = delegationHashEdit_->text().trimmed();
        }
        // Проверяем что есть delegation hash для сканирования
        if (currentDelegationHash_.isEmpty()) {
            QInputDialog dialog(this);
            dialog.setWindowTitle(tr("Delegation hash"));
            dialog.setLabelText(tr("Paste delegation hash (64 hex characters) for scanning commitments:"));
            dialog.setTextEchoMode(QLineEdit::Normal);
            GUIUtil::applyDialogStyle(&dialog);
            if (dialog.exec() != QDialog::Accepted) {
                autoScanDelegationCheck_->setChecked(false);
                return;
            }
            const QString hash = dialog.textValue().trimmed();
            if (hash.isEmpty()) {
                autoScanDelegationCheck_->setChecked(false);
                return;
            }
            currentDelegationHash_ = hash;
            if (delegationHashEdit_) {
                delegationHashEdit_->setText(currentDelegationHash_);
            }
        }

        if (!ensureDelegationScanner()) {
            GUIUtil::showStyledMessageBox(
                this, QMessageBox::Warning, tr("Error"),
                tr("Chain state is unavailable. Cannot start scanning."));
            autoScanDelegationCheck_->setChecked(false);
            return;
        }

        delegationScanner_->startAutoScan(30);  // 30 секунд интервал
        scanNowDelegationButton_->setEnabled(true);
        logInfo(tr("Auto-scanning delegation commitments started"));

    } else {
        // Останавливаем сканер
        if (delegationScanner_) {
            delegationScanner_->stopAutoScan();
        }
        scanNowDelegationButton_->setEnabled(false);
        if (scanStatusDelegationLabel_) {
            scanStatusDelegationLabel_->setText(tr("Status: auto-scanning stopped"));
        }
        logInfo(tr("Auto-scanning delegation commitments stopped"));
    }
}

void CorporatePage::onScanNowDelegation()
{
    if (currentDelegationHash_.isEmpty() && delegationHashEdit_ && !delegationHashEdit_->text().trimmed().isEmpty()) {
        currentDelegationHash_ = delegationHashEdit_->text().trimmed();
    }
    if (currentDelegationHash_.isEmpty()) {
            QInputDialog dialog(this);
            dialog.setWindowTitle(tr("Delegation hash"));
            dialog.setLabelText(tr("Paste delegation hash (64 hex characters) for scanning commitments:"));
            dialog.setTextEchoMode(QLineEdit::Normal);
            GUIUtil::applyDialogStyle(&dialog);
            if (dialog.exec() != QDialog::Accepted) {
                return;
            }
            const QString hash = dialog.textValue().trimmed();
            if (hash.isEmpty()) {
                return;
            }
            currentDelegationHash_ = hash;
            if (delegationHashEdit_) {
                delegationHashEdit_->setText(currentDelegationHash_);
            }
    }
    if (!ensureDelegationScanner()) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Warning, tr("Error"), tr("Chain state is unavailable."));
        return;
    }

    logInfo(tr("Manual check delegation commitments..."));
    delegationScanner_->scanNow();
}

void CorporatePage::onDelegationCommitmentFound(const QString& txid, const QString& signerAddress)
{
    if (::geocoin::qt::debugEnabled()) {
        GEO_QT_LOG_DEBUG << "[CorporatePage] onDelegationCommitmentFound:";
        GEO_QT_LOG_DEBUG << "  TXID:" << txid.left(16) << "...";
        GEO_QT_LOG_DEBUG << "  signerAddress:" << signerAddress;
        GEO_QT_LOG_DEBUG << "  currentPrimaryAddress_:" << currentPrimaryAddress_;
    }

    // Сначала проверяем, нет ли уже такого TXID в полях (дубликат)
    for (auto* edit : delegationCommitmentEdits_) {
        if (edit->text().trimmed() == txid) {
            scannerDelegationFields_.insert(txid, edit);
            if (::geocoin::qt::debugEnabled()) {
                GEO_QT_LOG_DEBUG << "  -> Дубликат TXID, пропускаем";
            }
            logInfo(tr("Commitment %1 is already added.").arg(txid.left(16) + "..."));
            return;  // Уже есть, не добавляем повторно
        }
    }

    // Проверяем, что подписант - это один из учредителей (если знаем их адреса)
    if (!primaryConfigParticipants_.isEmpty()) {
        bool isValidSigner = false;
        for (const auto& participant : primaryConfigParticipants_) {
            if (!participant.isObject()) continue;
            const QJsonObject obj = participant.toObject();
            const QString addr = obj.value("address").toString().trimmed();
            if (!addr.isEmpty() && addr == signerAddress) {
                isValidSigner = true;
                break;
            }
        }

        if (!isValidSigner) {
            if (::geocoin::qt::debugEnabled()) {
                GEO_QT_LOG_DEBUG << "  -> Пропускаем: не в списке учредителей";
            }
            logError(tr("Ignored a commitment from %1 because that address is not a founder.")
                .arg(signerAddress.left(15) + "..."));
            return;
        }
    }

    if (::geocoin::qt::debugEnabled()) {
        GEO_QT_LOG_DEBUG << "  -> Принимаем commitment";
    }

    // Находим первое пустое поле и вставляем TXID
    bool inserted = false;
    for (auto* edit : delegationCommitmentEdits_) {
        if (edit->text().trimmed().isEmpty()) {
            edit->setText(txid);
            edit->setStyleSheet(GUIUtil::themedSuccessEditableStyleSheet());
            scannerDelegationFields_.insert(txid, edit);
            inserted = true;
            if (::geocoin::qt::debugEnabled()) {
                GEO_QT_LOG_DEBUG << "  -> Вставлен в поле";
            }
            break;
        }
    }

    if (!inserted) {
        // Все поля заняты
        if (::geocoin::qt::debugEnabled()) {
            GEO_QT_LOG_DEBUG << "  -> Все поля заняты";
        }
        logError(tr("All fields used! Found commitment from %1, but nowhere paste")
            .arg(signerAddress.left(15) + "..."));
        return;
    }

    logInfo(tr("✅ Received signature from founder %1").arg(signerAddress.left(15) + "..."));
    logInfo(tr("   TXID: %1").arg(txid));

    updateDelegationCommitmentState();

    // Показываем уведомление
    if (scanStatusDelegationLabel_) {
        int found = 0;
        for (auto* edit : delegationCommitmentEdits_) {
            if (!edit->text().trimmed().isEmpty()) found++;
        }
        int required = primaryMSpin_ ? primaryMSpin_->value() : 2;
        scanStatusDelegationLabel_->setText(
            tr("✅ Found signature! Collected: %1/%2").arg(found).arg(required));
    }
}

void CorporatePage::onDelegationCommitmentInvalidated(const QString& txid, const QString& signerAddress)
{
    const auto it = scannerDelegationFields_.find(txid);
    if (it == scannerDelegationFields_.end()) return;
    const QPointer<QLineEdit> edit = it.value();
    scannerDelegationFields_.erase(it);
    if (edit && edit->text().trimmed() == txid) {
        edit->clear();
        edit->setStyleSheet(GUIUtil::themedEditableStyleSheet());
        updateDelegationCommitmentState();
        logError(tr("Commitment %1 from %2 was removed after a reorg")
            .arg(txid.left(16) + QStringLiteral("..."), signerAddress.left(15) + QStringLiteral("...")));
        if (scanStatusDelegationLabel_) {
            scanStatusDelegationLabel_->setText(tr("Reorg: a signature was invalidated; scanning continues"));
        }
    }
}

void CorporatePage::onAllDelegationCommitmentsCollected()
{
    logInfo(tr("🎉 All signatures delegation proof collected! Can finalize."));

    if (scanStatusDelegationLabel_) {
        scanStatusDelegationLabel_->setText(tr("🎉 All signatures collected! Ready to finalization."));
        scanStatusDelegationLabel_->setStyleSheet("QLabel { color: #27ae60; font-weight: bold; font-size: 12px; }");
    }

    GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Information,
        tr("Signatures collected"),
        tr("🎉 All required signatures have been collected!\n\nYou can now finalize the delegation proof.\nPress \"Finalize delegation proof\".\nThe scanner keeps watching the chain until finalization."));
}

void CorporatePage::onDelegationScanError(const QString& error)
{
    logError(tr("Error scanning: %1").arg(error));

    if (scanStatusDelegationLabel_) {
        scanStatusDelegationLabel_->setText(tr("Error: %1").arg(error));
        scanStatusDelegationLabel_->setStyleSheet("QLabel { color: #e74c3c; font-size: 11px; }");
    }
}

void CorporatePage::onDelegationScanStatus(const QString& message)
{
    if (scanStatusDelegationLabel_) {
        scanStatusDelegationLabel_->setText(tr("Status: %1").arg(message));
        scanStatusDelegationLabel_->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    }
}

void CorporatePage::handleDelegationProofFinalizeReady(const QString& finalHex, const QString& broadcastTxid)
{
    setFounderDraftInProgress(false);

    // UX/безопасность: после успешной финализации очищаем все временные поля,
    // чтобы нельзя было повторно финализировать другой draft старыми TXID/хешем.
    currentDelegationHash_.clear();
    currentDelegationDraftDoc_ = QJsonDocument();

    currentDelegationDraftJson_.clear();
    delegationDraftEdit_->clear();
    delegationDraftHashLabel_->setText(tr("Hash for signatures: —"));
    saveDelegationDraftButton_->setEnabled(false);
    copyDelegationDraftButton_->setEnabled(false);
    if (delegationHashEdit_) {
        delegationHashEdit_->clear();
    }
    for (QLineEdit *edit : delegationCommitmentEdits_) {
        if (edit) edit->clear();
    }

    // Останавливаем автоскан после успешной финализации
    if (autoScanDelegationCheck_ && autoScanDelegationCheck_->isChecked()) {
        autoScanDelegationCheck_->setChecked(false);
    }
    resetDelegationScanner(false);
    if (scanStatusDelegationLabel_) {
        scanStatusDelegationLabel_->setText(tr("Status: ready to scan"));
        scanStatusDelegationLabel_->setStyleSheet("QLabel { color: #7f8c8d; font-style: italic; font-size: 11px; }");
    }
    updateDelegationCommitmentState();
    updateFounderState();

    const QString txidToShow = extractHexTxid(broadcastTxid);

    QString message;
    if (broadcastTxid.isEmpty()) {
        message = tr("Delegation proof formed.\nHEX:\n%1").arg(finalHex);
    } else if (!txidToShow.isEmpty()) {
        message = tr("Delegation proof sent!\n\nTXID: %1\n\nNow director can use this TXID.").arg(txidToShow);
    } else {
        message = tr("Delegation proof sent, but line TXID invalid.\n\nRAW:\n%1").arg(broadcastTxid);
    }

    GUIUtil::showStyledMessageBox(this, QMessageBox::Information, tr("Delegation proof ready"), message);

    logInfo(broadcastTxid.isEmpty()
        ? tr("Delegation proof formed.")
        : (txidToShow.isEmpty() ? tr("Delegation proof sent.") : tr("Delegation proof sent (txid %1).").arg(txidToShow)));
    addHistoryEntry("DelegationFinalize", broadcastTxid.isEmpty() ? tr("Prepared") : tr("Sent"),
                   broadcastTxid.isEmpty()
                       ? tr("HEX length: %1").arg(finalHex.length())
                       : (txidToShow.isEmpty() ? tr("RAW length: %1").arg(broadcastTxid.length()) : tr("TXID: %1").arg(txidToShow)),
                   finalHex);

    if (!broadcastTxid.isEmpty()) loadActiveDelegationProofs();
}
