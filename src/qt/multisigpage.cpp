#include "multisigpage_internal.h"

MultisigPage::MultisigPage(QWidget *parent)
    : QWidget(parent),
      clientModel_(nullptr),
      walletModel_(nullptr),
      roleButtons_(new QButtonGroup(this)),
      roleStack_(new QStackedWidget(this)),
      configPathEdit_(nullptr),
      initiatorMSpin_(nullptr),
      initiatorNSpin_(nullptr),
      participantsEdit_(nullptr),
      multisigAddressLabel_(nullptr),
      receiverEdit_(nullptr),
      amountEdit_(nullptr),
      feeEdit_(nullptr),
      balanceLabel_(nullptr),
      multisigBalanceKnown_(false),
      multisigAvailableBalance_(0),
      createDraftButton_(nullptr),
      draftJsonEdit_(nullptr),
      draftHashLabel_(nullptr),
      saveDraftButton_(nullptr),
      copyDraftButton_(nullptr),
      commitmentEdits_(),
      finalizeButton_(nullptr),
      finalizeBroadcastCheck_(nullptr),
      currentMultisigAddress_(),
      currentDraftHash_(),
      currentDraftDoc_(),
      currentDraftJson_(),
      configLoaded_(false),
      configParticipants_(),
      walletConnections_(),
      clientBlocksConnection_(),
      statusLabel_(nullptr),
      activityLog_(nullptr),
      draftInProgress_(false),
      commitmentInProgress_(false),
      finalizeInProgress_(false),
      historyTable_(nullptr),
      historyLoadButton_(nullptr),
      historyFilePath_(),
      historyEntries_(),
      signerDraftEdit_(nullptr),
      signerSummaryLabel_(nullptr),
      signerHashLabel_(nullptr),
      keySourceButtons_(nullptr),
      walletKeyCombo_(nullptr),
      fileKeyPathEdit_(nullptr),
      manualKeyEdit_(nullptr),
      broadcastCheck_(nullptr),
      signCommitmentButton_(nullptr),
      signerDraftDoc_(),
      signerDraftHash_(),
      signerDraftValid_(false),
      multisigScanner_(nullptr),
      balanceUpdateTimer_(new QTimer(this))
{
    // Настраиваем таймер для throttling обновления баланса
    balanceUpdateTimer_->setSingleShot(true);
    balanceUpdateTimer_->setInterval(5000);  // 5 секунд
    connect(balanceUpdateTimer_, &QTimer::timeout, this, &MultisigPage::updateMultisigBalance);

    setupUi();
    updateInitiatorState();
    updateSignerState();

    // Подключаем сигналы от MultisigPage к WalletModel (напрямую, как в CorporatePage)
    connect(this, &MultisigPage::requestDraftCreation,
            this, [this](const QString& configPath,
                         const QString& recipient,
                         qint64 amount,
                         qint64 fee) {
        if (walletModel_) {
            walletModel_->createMultisigDraft(configPath, recipient, amount, fee);
        }
    });
    connect(this, &MultisigPage::requestCommitmentSign,
            this, [this](const QString& draftJson,
                         const QString& privateKeySource,
                         const QString& keyPath,
                         bool broadcastImmediately) {
        if (walletModel_) {
            walletModel_->signMultisigCommitment(draftJson, privateKeySource, keyPath, broadcastImmediately);
        }
    });
    connect(this, &MultisigPage::requestCreateAndSignMultisig,
            this, [this](const QString& configPath,
                         const QString& recipient,
                         qint64 amount,
                         qint64 fee,
                         const QString& privateKeyHex) {
        if (walletModel_) {
            walletModel_->createAndSignMultisig(configPath, recipient, amount, fee, privateKeyHex);
        }
    });
    connect(this, &MultisigPage::requestCreateAndSignMultisigWithWalletKey,
            this, [this](const QString& configPath,
                         const QString& recipient,
                         qint64 amount,
                         qint64 fee,
                         const QString& walletAddress,
                         const QString& walletPassword) {
        if (walletModel_) {
            walletModel_->createAndSignMultisigWithWalletKey(
                configPath, recipient, amount, fee, walletAddress, walletPassword);
        }
    });
    connect(this, &MultisigPage::requestDraftFinalize,
            this, [this](const QString& draftJson,
                         const QStringList& commitmentTxids,
                         bool broadcast) {
        if (walletModel_) {
            walletModel_->finalizeMultisigSpend(draftJson, commitmentTxids, broadcast);
        }
    });
}

MultisigPage::~MultisigPage()
{
    if (clientBlocksConnection_) {
        disconnect(clientBlocksConnection_);
    }

    // Останавливаем сканер если он запущен
    if (multisigScanner_) {
        multisigScanner_->stopAutoScan();
        delete multisigScanner_;
        multisigScanner_ = nullptr;
    }
}

void MultisigPage::setClientModel(ClientModel *model)
{
    if (clientModel_ == model) {
        return;
    }
    if (clientBlocksConnection_) {
        disconnect(clientBlocksConnection_);
        clientBlocksConnection_ = QMetaObject::Connection();
    }

    clientModel_ = model;

    // Подписываемся на обновления блоков для автообновления баланса
    // Используем таймер для throttling - обновляем не чаще раза в 5 секунд
    if (clientModel_) {
        clientBlocksConnection_ = connect(clientModel_, &ClientModel::numBlocksChanged, this, [this]() {
            // Запускаем таймер если он не активен
            if (!balanceUpdateTimer_->isActive()) {
                balanceUpdateTimer_->start();
            }
        });
    }
}

void MultisigPage::setWalletModel(WalletModel *model)
{
    if (walletModel_ == model) {
        return;
    }
    if (autoScanMultisigCheck_ && autoScanMultisigCheck_->isChecked()) {
        autoScanMultisigCheck_->setChecked(false);
    }
    resetMultisigScanner(true);
    for (const auto& connection : walletConnections_) {
        disconnect(connection);
    }
    walletConnections_.clear();
    walletModel_ = model;
    if (walletModel_) {
        QString walletDataDir = walletModel_->getDataDirectory();
        if (walletDataDir.isEmpty()) {
            walletDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        }
        if (walletDataDir.isEmpty()) walletDataDir = QDir::homePath() + QStringLiteral("/.geocoin");
        walletDataDir = QDir(walletDataDir).absolutePath();
        QDir().mkpath(walletDataDir);
        historyFilePath_ = QDir(walletDataDir).filePath(QStringLiteral("multisig_history.json"));
        pendingTransactionFilePath_ = QDir(walletDataDir).filePath(QStringLiteral("multisig_pending.json"));
        loadHistory();
        schedulePendingRestorePrompt();
    }
    if (walletModel_) {
        walletConnections_ << connect(walletModel_, &WalletModel::multisigDraftReady,
                                      this, &MultisigPage::handleDraftReady);
        walletConnections_ << connect(walletModel_, &WalletModel::multisigDraftFailed,
                                      this, &MultisigPage::handleDraftFailed);
        walletConnections_ << connect(walletModel_, &WalletModel::createAndSignMultisigReady,
                                      this, &MultisigPage::handleCreateAndSignReady);
        walletConnections_ << connect(walletModel_, &WalletModel::createAndSignMultisigFailed,
                                      this, &MultisigPage::handleCreateAndSignFailed);
        walletConnections_ << connect(walletModel_, &WalletModel::multisigCommitmentReady,
                                      this, &MultisigPage::handleCommitmentReady);
        walletConnections_ << connect(walletModel_, &WalletModel::multisigCommitmentFailed,
                                      this, &MultisigPage::handleCommitmentFailed);
        walletConnections_ << connect(walletModel_, &WalletModel::multisigFinalizeReady,
                                      this, &MultisigPage::handleFinalizeReady);
        walletConnections_ << connect(walletModel_, &WalletModel::multisigFinalizeFailed,
                                      this, &MultisigPage::handleFinalizeFailed);

        walletConnections_ << connect(walletModel_, &WalletModel::walletStateChanged,
                                      this, [this]() {
            if (walletKeyCombo_) {
                walletKeyCombo_->clear();
                QString primaryAddress = walletModel_->getAddress();
                if (!primaryAddress.isEmpty()) {
                    walletKeyCombo_->addItem(primaryAddress);
                }
                // Обновляем состояние кнопки если выбран вариант "Из кошелька"
                if (keySourceButtons_ && keySourceButtons_->checkedId() == 0) {
                    updateKeySourceState(0);
                }
            }
            loadSavedMultisigAddresses();
        });
    }

    if (walletKeyCombo_) {
        walletKeyCombo_->clear();
        if (walletModel_) {
            QString primaryAddress = walletModel_->getAddress();
            if (!primaryAddress.isEmpty()) {
                walletKeyCombo_->addItem(primaryAddress);
            }
        }
    }

    // Загружаем сохраненные мультиподпись адреса
    loadSavedMultisigAddresses();
}

bool MultisigPage::saveWalletChanges()
{
    if (!walletModel_ || !walletModel_->isWalletLoaded()) {
        GUIUtil::showStyledMessageBox(
            this,
            QMessageBox::Warning,
            tr("Wallet not loaded"),
            tr("Create or load a wallet before saving the multisig configuration."));
        return false;
    }
    walletModel_->markWalletDirty();
    QString password;
    GUIUtil::ScopedQStringCleanser passwordCleanser(password);
    if (walletModel_->isWalletEncrypted()) {
        if (!GUIUtil::getStyledText(this,
                                    tr("Save wallet"),
                                    tr("Enter the wallet password:"),
                                    password,
                                    QLineEdit::Password)) {
            return false;
        }
        if (password.isEmpty()) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("Password required"),
                tr("Enter the wallet password to save the multisig configuration."));
            return false;
        }
    }
    QString error;
    const bool saved = walletModel_->saveWallet(password, &error);
    GUIUtil::cleanseQString(password);
    if (!saved) {
        GUIUtil::showStyledMessageBox(this, QMessageBox::Critical,
                                      tr("Save failed"), error);
    }
    return saved;
}

void MultisigPage::handleDraftFailed(const QString& error)
{
    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not create the draft"), error);

    logError(tr("Could not create the draft: %1").arg(error));
    setDraftInProgress(false);
    addHistoryEntry("Draft", tr("Error"), error);
}

void MultisigPage::handleCreateAndSignFailed(const QString& error)
{
    setDraftInProgress(false);

    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not create and sign"), error);

    logError(tr("Could not create and sign the multisig transaction: %1").arg(error));
    addHistoryEntry("CreateAndSign", tr("Error"), error);
}

void MultisigPage::handleCommitmentFailed(const QString& error)
{
    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not create the commitment"), error);

    logError(tr("Could not sign the commitment: %1").arg(error));
    setCommitmentInProgress(false);
    addHistoryEntry("Commitment", tr("Error"), error);
}

void MultisigPage::handleFinalizeFailed(const QString& error)
{
    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not finalize"), error);

    logError(tr("Could not finalize the transaction: %1").arg(error));
    setFinalizeInProgress(false);
    addHistoryEntry("Finalize", tr("Error"), error);
}

void MultisigPage::setupUi()
{
    QVBoxLayout *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(15);

    statusLabel_ = new QLabel(tr("Ready"), this);
    statusLabel_->setObjectName("multisigStatusLabel");
    statusLabel_->setStyleSheet("QLabel#multisigStatusLabel { font-weight: 600; color: #27ae60; }");
    rootLayout->addWidget(statusLabel_);

    rootLayout->addWidget(buildRoleSelector());

    QWidget *initiatorPage = buildInitiatorPage();
    QWidget *signerPage = buildSignerPage();

    roleStack_->addWidget(initiatorPage);
    roleStack_->addWidget(signerPage);
    roleStack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Embed stack into scroll area to keep layout tidy on small screens
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(roleStack_);
    rootLayout->addWidget(scroll, 1);

    QGroupBox *logBox = new QGroupBox(tr("Activity log"), this);
    QVBoxLayout *logLayout = new QVBoxLayout(logBox);
    activityLog_ = new QTextEdit(logBox);
    activityLog_->setReadOnly(true);
    activityLog_->setMinimumHeight(100);
    activityLog_->setStyleSheet(GUIUtil::themedEditableStyleSheet());
    logLayout->addWidget(activityLog_);
    rootLayout->addWidget(logBox);

    QGroupBox *historyBox = new QGroupBox(tr("History operations"), this);
    QVBoxLayout *historyLayout = new QVBoxLayout(historyBox);
    historyTable_ = new QTableWidget(0, 4, historyBox);
    historyTable_->setHorizontalHeaderLabels({tr("Time"), tr("Type"), tr("Status"), tr("Details")});
    historyTable_->horizontalHeader()->setStretchLastSection(true);
    historyTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    historyTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    historyTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    historyTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyTable_->setMinimumHeight(180);
    historyLayout->addWidget(historyTable_);

    QHBoxLayout *historyButtonLayout = new QHBoxLayout();
    historyLoadButton_ = new QPushButton(tr("Load selected draft"), historyBox);
    historyLoadButton_->setEnabled(false);
    historyButtonLayout->addWidget(historyLoadButton_);

    QPushButton *clearHistoryButton = new QPushButton(tr("🗑️ Clear history"), historyBox);
    clearHistoryButton->setStyleSheet("QPushButton { background-color: #e74c3c; color: white; padding: 5px 10px; }");
    connect(clearHistoryButton, &QPushButton::clicked, this, &MultisigPage::clearHistory);
    historyButtonLayout->addWidget(clearHistoryButton);

    historyButtonLayout->addStretch();
    historyLayout->addLayout(historyButtonLayout);
    rootLayout->addWidget(historyBox);

    logInfo(tr("Interface multisig ready."));

    roleButtons_->button(0)->setChecked(true);
    roleStack_->setCurrentIndex(0);

    connect(historyTable_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &MultisigPage::onHistorySelectionChanged);
    connect(historyLoadButton_, &QPushButton::clicked,
            this, &MultisigPage::onHistoryLoadClicked);

    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir.isEmpty()) {
        dataDir = QDir::homePath() + "/.geocoin";
    }
    QDir().mkpath(dataDir);
    historyFilePath_ = QDir(dataDir).filePath("multisig_history.json");
    pendingTransactionFilePath_ = QDir(dataDir).filePath("multisig_pending.json");
    applyTheme();

}

void MultisigPage::schedulePendingRestorePrompt()
{
    if (!hasPendingTransaction()) return;
    const QString expectedPath = pendingTransactionFilePath_;
    QTimer::singleShot(500, this, [this, expectedPath]() {
        if (pendingTransactionFilePath_ != expectedPath || !hasPendingTransaction()) return;
        const auto answer = GUIUtil::showStyledMessageBox(
            this, QMessageBox::Question, tr("Incomplete transaction"),
            tr("Found incomplete multisig transaction.\n\nWant restore its and continue collection signatures?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) loadPendingTransaction();
        else clearPendingTransaction();
    });
}

QWidget* MultisigPage::buildRoleSelector()
{
    QWidget *container = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(20);

    QLabel *title = new QLabel(tr("Multisig (Multisig)"), container);
    title->setStyleSheet("QLabel { font-size: 20px; font-weight: bold; }");

    QRadioButton *initiatorButton = new QRadioButton(tr("Initiator (create)"), container);
    QRadioButton *signerButton = new QRadioButton(tr("Signer (sign)"), container);

    roleButtons_->addButton(initiatorButton, 0);
    roleButtons_->addButton(signerButton, 1);

    connect(roleButtons_, SIGNAL(idClicked(int)), this, SLOT(onRoleChanged(int)));

    layout->addWidget(title, 1);
    layout->addWidget(initiatorButton);
    layout->addWidget(signerButton);
    layout->addStretch();
    return container;
}

void MultisigPage::applyGeoTheme(QWidget *widget)
{
    widget->setStyleSheet(GUIUtil::glassPanelStyleSheet(GUIUtil::currentTheme())
                          + GUIUtil::glassInputStyleSheet(GUIUtil::currentTheme())
                          + GUIUtil::glassListStyleSheet(GUIUtil::currentTheme()));
}

void MultisigPage::applyTheme()
{
    setStyleSheet(QStringLiteral("MultisigPage { background-color: transparent; }"));

    const QString panelStyle = GUIUtil::glassPanelStyleSheet(GUIUtil::currentTheme())
                             + GUIUtil::glassInputStyleSheet(GUIUtil::currentTheme())
                             + GUIUtil::glassListStyleSheet(GUIUtil::currentTheme())
                             + GUIUtil::spinBoxStyleSheet(GUIUtil::currentTheme());
    const auto boxes = findChildren<QGroupBox*>();
    for (QGroupBox* box : boxes) {
        if (box) {
            box->setStyleSheet(panelStyle);
        }
    }

    const auto lineEdits = findChildren<QLineEdit*>();
    for (QLineEdit* edit : lineEdits) {
        if (edit) {
            edit->setStyleSheet(GUIUtil::themedEditableStyleSheet());
        }
    }
    const auto textEdits = findChildren<QTextEdit*>();
    for (QTextEdit* edit : textEdits) {
        if (edit) {
            edit->setStyleSheet(GUIUtil::themedEditableStyleSheet());
        }
    }
    const auto comboBoxes = findChildren<QComboBox*>();
    for (QComboBox* combo : comboBoxes) {
        if (combo) {
            combo->setStyleSheet(GUIUtil::themedEditableStyleSheet());
        }
    }
    const auto spinBoxes = findChildren<QSpinBox*>();
    for (QSpinBox* spin : spinBoxes) {
        if (spin) {
            spin->setStyleSheet(GUIUtil::spinBoxStyleSheet(GUIUtil::currentTheme()));
        }
    }
    const auto tables = findChildren<QTableWidget*>();
    for (QTableWidget* table : tables) {
        if (table) {
            table->setStyleSheet(GUIUtil::glassListStyleSheet(GUIUtil::currentTheme()));
            table->horizontalHeader()->setStretchLastSection(true);
        }
    }

    if (activityLog_) {
        activityLog_->setStyleSheet(GUIUtil::themedEditableStyleSheet());
    }
    if (spendingHashEdit_) {
        spendingHashEdit_->setStyleSheet(GUIUtil::themedMonoEditableStyleSheet());
    }
}

void MultisigPage::onRoleChanged(int roleId)
{
    roleStack_->setCurrentIndex(roleId);
    updateInitiatorState();
    updateSignerState();
}

void MultisigPage::logInfo(const QString& message)
{
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString("dd.MM HH:mm:ss"), message);
    if (activityLog_) {
        activityLog_->append(line);
        activityLog_->moveCursor(QTextCursor::End);
    }
    updateStatusBanner();
}

void MultisigPage::logError(const QString& message)
{
    const QString line = QStringLiteral("[%1] ⚠ %2")
                             .arg(QDateTime::currentDateTime().toString("dd.MM HH:mm:ss"), message);
    if (activityLog_) {
        activityLog_->append(line);
        activityLog_->moveCursor(QTextCursor::End);
    }
    if (statusLabel_) {
        statusLabel_->setText(message);
        statusLabel_->setStyleSheet("QLabel#multisigStatusLabel { font-weight: 600; color: #c0392b; }");
    }
}

void MultisigPage::setDraftInProgress(bool inProgress)
{
    if (draftInProgress_ == inProgress) {
        return;
    }
    draftInProgress_ = inProgress;
    updateInitiatorState();
    updateStatusBanner();
}

void MultisigPage::setCommitmentInProgress(bool inProgress)
{
    if (commitmentInProgress_ == inProgress) {
        return;
    }
    commitmentInProgress_ = inProgress;
    updateSignerState();
    updateStatusBanner();
}

void MultisigPage::setFinalizeInProgress(bool inProgress)
{
    if (finalizeInProgress_ == inProgress) {
        return;
    }
    finalizeInProgress_ = inProgress;
    updateCommitmentState();
    updateStatusBanner();
}

void MultisigPage::updateStatusBanner()
{
    if (!statusLabel_) {
        return;
    }
    QString text;
    QString color = "#27ae60";
    if (finalizeInProgress_) {
        text = tr("Finalization transactions...");
        color = "#e67e22";
    } else if (commitmentInProgress_) {
        text = tr("Signing commitment...");
        color = "#e67e22";
    } else if (draftInProgress_) {
        text = tr("Creation draft...");
        color = "#e67e22";
    } else {
        text = tr("Ready");
        color = "#27ae60";
    }
    statusLabel_->setText(text);
    statusLabel_->setStyleSheet(QStringLiteral("QLabel#multisigStatusLabel { font-weight: 600; color: %1; }")
                                    .arg(color));
}
