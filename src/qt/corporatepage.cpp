#include "corporatepage_internal.h"

CorporatePage::CorporatePage(QWidget *parent)
    : QWidget(parent),
      clientModel_(nullptr),
      walletModel_(nullptr),
      roleButtons_(new QButtonGroup(this)),
      roleStack_(new QStackedWidget(this)),
      statusLabel_(nullptr),
      activityLog_(nullptr),
      primaryConfigLoaded_(false),
      delegatedConfigLoaded_(false),
      founderDraftInProgress_(false),
      delegationProofLoaded_(false),
      directorDraftInProgress_(false),
      signerDraftValid_(false),
      commitmentInProgress_(false),
      keySourceButtons_(new QButtonGroup(this)),
      delegationScanner_(nullptr),
      hierarchicalScanner_(nullptr)
{
    setupUi();
    updateFounderState();
    updateDirectorState();
    updateSignerState();
}

CorporatePage::~CorporatePage()
{
    // Останавливаем сканеры если они запущены
    if (delegationScanner_) {
        delegationScanner_->stopAutoScan();
        delete delegationScanner_;
        delegationScanner_ = nullptr;
    }

    if (hierarchicalScanner_) {
        hierarchicalScanner_->stopAutoScan();
        delete hierarchicalScanner_;
        hierarchicalScanner_ = nullptr;
    }
}

void CorporatePage::setClientModel(ClientModel *model)
{
    if (clientModel_ == model) {
        return;
    }
    if (clientModel_) {
        disconnect(clientModel_, nullptr, this, nullptr);
    }

    clientModel_ = model;

    // Обновляем "делегированные" балансы при росте цепочки.
    if (clientModel_) {
        connect(clientModel_, &ClientModel::numBlocksChanged, this, [this](int) {
            updatePrimaryBalance();
        }, Qt::UniqueConnection);
    }
}

void CorporatePage::setWalletModel(WalletModel *model)
{
    if (walletModel_ == model) {
        return;
    }

    if (autoScanDelegationCheck_ && autoScanDelegationCheck_->isChecked()) {
        autoScanDelegationCheck_->setChecked(false);
    }
    if (autoScanHierarchicalCheck_ && autoScanHierarchicalCheck_->isChecked()) {
        autoScanHierarchicalCheck_->setChecked(false);
    }
    resetDelegationOperation();
    resetHierarchicalOperation();

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
        historyFilePath_ = QDir(walletDataDir).filePath(QStringLiteral("corporate_history.json"));
        historyEntries_ = QJsonArray();
        refreshHistoryView();
    }

    if (walletModel_) {
        // Подключаем сигналы для delegation proof
        walletConnections_ << connect(walletModel_, &WalletModel::delegationProofDraftReady,
                                      this, &CorporatePage::handleDelegationProofDraftReady);
        walletConnections_ << connect(walletModel_, &WalletModel::delegationProofDraftFailed,
                                      this, &CorporatePage::handleDelegationProofDraftFailed);
        walletConnections_ << connect(walletModel_, &WalletModel::createAndSignDelegationProofReady,
                                      this, &CorporatePage::handleCreateAndSignReady);
        walletConnections_ << connect(walletModel_, &WalletModel::createAndSignDelegationProofFailed,
                                      this, &CorporatePage::handleCreateAndSignFailed);
        walletConnections_ << connect(walletModel_, &WalletModel::delegationProofFinalizeReady,
                                      this, &CorporatePage::handleDelegationProofFinalizeReady);
        walletConnections_ << connect(walletModel_, &WalletModel::delegationProofFinalizeFailed,
                                      this, &CorporatePage::handleDelegationProofFinalizeFailed);

        // Подключаем сигналы для hierarchical spend
        walletConnections_ << connect(walletModel_, &WalletModel::hierarchicalSpendDraftReady,
                                      this, &CorporatePage::handleHierarchicalSpendDraftReady);
        walletConnections_ << connect(walletModel_, &WalletModel::hierarchicalSpendDraftFailed,
                                      this, &CorporatePage::handleHierarchicalSpendDraftFailed);
        walletConnections_ << connect(walletModel_, &WalletModel::hierarchicalSpendFinalizeReady,
                                      this, &CorporatePage::handleHierarchicalSpendFinalizeReady);
        walletConnections_ << connect(walletModel_, &WalletModel::hierarchicalSpendFinalizeFailed,
                                      this, &CorporatePage::handleHierarchicalSpendFinalizeFailed);

        // Подключаем сигналы для commitment (универсальное подписание)
        walletConnections_ << connect(walletModel_, &WalletModel::multisigCommitmentReady,
                                      this, &CorporatePage::handleCommitmentReady);
        walletConnections_ << connect(walletModel_, &WalletModel::multisigCommitmentFailed,
                                      this, &CorporatePage::handleCommitmentFailed);

        // Подключаем сигналы от CorporatePage к WalletModel
        walletConnections_ << connect(this, &CorporatePage::requestDelegationProofDraft,
                                      walletModel_, &WalletModel::createDelegationProofDraft);
        walletConnections_ << connect(this, &CorporatePage::requestCreateAndSignDelegationProof,
                                      walletModel_, &WalletModel::createAndSignDelegationProof);
        walletConnections_ << connect(this, &CorporatePage::requestCreateAndSignDelegationProofWithWalletKey,
                                      walletModel_, &WalletModel::createAndSignDelegationProofWithWalletKey);
        walletConnections_ << connect(this, &CorporatePage::requestDelegationProofFinalize,
                                      walletModel_, &WalletModel::finalizeDelegationProof);
        walletConnections_ << connect(this, &CorporatePage::requestHierarchicalSpendDraft,
                                      walletModel_, &WalletModel::createHierarchicalSpendDraft);
        walletConnections_ << connect(this, &CorporatePage::requestHierarchicalSpendFinalize,
                                      walletModel_, &WalletModel::finalizeHierarchicalSpend);
        walletConnections_ << connect(this, &CorporatePage::requestCommitmentSign,
                                      walletModel_, &WalletModel::signCommitment);
        walletConnections_ << connect(walletModel_, &WalletModel::walletStateChanged,
                                      this, [this]() {
            if (!walletKeyCombo_) return;
            walletKeyCombo_->clear();
            if (walletModel_ && walletModel_->isWalletLoaded()) {
                const QString address = walletModel_->getAddress();
                if (!address.isEmpty()) walletKeyCombo_->addItem(address);
            }
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

    // ОТКЛЮЧЕНО: Автоматическая загрузка - теперь только по запросу пользователя
    // loadSavedMultisigAddresses();
    // loadActiveDelegationProofs();
}

bool CorporatePage::saveWalletChanges()
{
    if (!walletModel_ || !walletModel_->isWalletLoaded()) return false;
    walletModel_->markWalletDirty();
    QString password;
    GUIUtil::ScopedQStringCleanser passwordCleanser(password);
    if (walletModel_->isWalletEncrypted()) {
        if (!GUIUtil::getStyledText(this,
                                    tr("Save wallet"),
                                    tr("Enter the wallet password:"),
                                    password,
                                    QLineEdit::Password) || password.isEmpty()) return false;
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

void CorporatePage::setupUi()
{
    QVBoxLayout *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(15);

    statusLabel_ = new QLabel(tr("Ready"), this);
    statusLabel_->setStyleSheet("QLabel { font-weight: 600; color: #27ae60; padding: 8px; }");
    rootLayout->addWidget(statusLabel_);

    rootLayout->addWidget(buildRoleSelector());

    QWidget *founderPage = buildFounderPage();
    QWidget *directorPage = buildDirectorPage();
    QWidget *signerPage = buildSignerPage();

    roleStack_->addWidget(founderPage);
    roleStack_->addWidget(directorPage);
    roleStack_->addWidget(signerPage);
    roleStack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

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
    activityLog_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    logLayout->addWidget(activityLog_);
    rootLayout->addWidget(logBox);

    rootLayout->addWidget(buildHistoryBox());

    logInfo(tr("Corporate management interface is ready."));

    roleButtons_->button(0)->setChecked(true);
    roleStack_->setCurrentIndex(0);

    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir.isEmpty()) {
        dataDir = QDir::homePath() + "/.geocoin";
    }
    QDir().mkpath(dataDir);
    historyFilePath_ = QDir(dataDir).filePath("corporate_history.json");

    // ОТКЛЮЧЕНО: Автоматическая загрузка истории - теперь только по запросу пользователя
    // loadHistory();
    applyTheme();
}

void CorporatePage::applyTheme()
{
    setStyleSheet(QStringLiteral("CorporatePage { background-color: transparent; }"));

    const QString theme = GUIUtil::currentTheme();
    const QString panelStyle = GUIUtil::glassPanelStyleSheet(theme)
                             + GUIUtil::glassInputStyleSheet(theme)
                             + GUIUtil::glassListStyleSheet(theme)
                             + GUIUtil::spinBoxStyleSheet(theme);
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
            spin->setStyleSheet(GUIUtil::spinBoxStyleSheet(theme));
        }
    }
    const auto doubleSpinBoxes = findChildren<QDoubleSpinBox*>();
    for (QDoubleSpinBox* spin : doubleSpinBoxes) {
        if (spin) {
            spin->setStyleSheet(GUIUtil::spinBoxStyleSheet(theme));
        }
    }
    const auto tables = findChildren<QTableWidget*>();
    for (QTableWidget* table : tables) {
        if (table) {
            table->setStyleSheet(GUIUtil::glassListStyleSheet(theme));
            table->horizontalHeader()->setStretchLastSection(true);
        }
    }

    if (activityLog_) {
        activityLog_->setStyleSheet(GUIUtil::themedEditableStyleSheet());
    }
    if (delegationHashEdit_) {
        delegationHashEdit_->setStyleSheet(GUIUtil::themedMonoEditableStyleSheet());
    }
    if (hierarchicalSpendingHashEdit_) {
        hierarchicalSpendingHashEdit_->setStyleSheet(GUIUtil::themedMonoEditableStyleSheet());
    }
}

QWidget* CorporatePage::buildRoleSelector()
{
    QWidget *container = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(20);

    QLabel *title = new QLabel(tr("Corporate management"), container);
    title->setStyleSheet("QLabel { font-size: 20px; font-weight: bold; }");

    QRadioButton *founderButton = new QRadioButton(tr("Founder"), container);
    QRadioButton *directorButton = new QRadioButton(tr("Director"), container);
    QRadioButton *signerButton = new QRadioButton(tr("Signer"), container);

    roleButtons_->addButton(founderButton, 0);
    roleButtons_->addButton(directorButton, 1);
    roleButtons_->addButton(signerButton, 2);

    connect(roleButtons_, SIGNAL(idClicked(int)), this, SLOT(onRoleChanged(int)));

    layout->addWidget(title, 1);
    layout->addWidget(founderButton);
    layout->addWidget(directorButton);
    layout->addWidget(signerButton);
    layout->addStretch();

    return container;
}

void CorporatePage::onRoleChanged(int roleId)
{
    roleStack_->setCurrentIndex(roleId);
    logInfo(tr("Switched to role: %1").arg(
        roleId == 0 ? tr("Founder") :
        roleId == 1 ? tr("Director") : tr("Signer")
    ));
}

// ============================================================================
// Build UI - Founder Page
// ============================================================================

void CorporatePage::updateStatusBanner()
{
    if (!statusLabel_) return;

    if (founderDraftInProgress_ || directorDraftInProgress_ || commitmentInProgress_) {
        statusLabel_->setText(tr("⏳ Processing..."));
        statusLabel_->setStyleSheet("QLabel { font-weight: 600; color: #f39c12; padding: 8px; }");
    } else {
        statusLabel_->setText(tr("✅ Ready"));
        statusLabel_->setStyleSheet("QLabel { font-weight: 600; color: #27ae60; padding: 8px; }");
    }
}

// ============================================================================
// Helper Methods - Validation
// ============================================================================

bool CorporatePage::loadConfigFromFile(const QString& path, bool isPrimary)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QByteArray data = file.readAll();
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (doc.isNull() || !doc.isObject()) {
        return false;
    }

    QJsonObject obj = doc.object();
    QString address = obj.value("address").toString();
    int m = obj.value("m").toInt(1);
    int n = obj.value("n").toInt(1);
    QJsonArray participants = obj.value("participants").toArray();

    // Проверка: нельзя использовать один адрес для учредителей и директоров
    if (isPrimary) {
        if (!currentDelegatedAddress_.isEmpty() && address == currentDelegatedAddress_) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("Error"),
                tr("This address is already used for directors.\n\nFounders and directors must use different addresses."));
            return false;
        }
    } else {
        if (!currentPrimaryAddress_.isEmpty() && address == currentPrimaryAddress_) {
            GUIUtil::showStyledMessageBox(
                this,
                QMessageBox::Warning,
                tr("Error"),
                tr("This address already used as address founders!\n\nAddress founders and address directors must be different."));
            return false;
        }
    }

    if (isPrimary) {
        resetDelegationOperation();
        primaryMSpin_->setValue(m);
        primaryNSpin_->setValue(n);
        currentPrimaryAddress_ = address;
        primaryAddressLabel_->setText(tr("Address: %1").arg(address));
        primaryConfigParticipants_ = participants;
        primaryConfigLoaded_ = true;

        // Добавляем в кошелек если есть
        if (walletModel_ && walletModel_->getWallet() && !address.isEmpty()) {
            geocoin::wallet::MultisigConfig config;
            config.m = m;
            config.n = n;
            config.address = geocoin::Address(address.toStdString());

            for (const QJsonValue& val : participants) {
                QJsonObject p = val.toObject();
                QString addrStr = p.value("address").toString();

                geocoin::Address addr(addrStr.toStdString());
                config.addresses.push_back(addr);
                config.participants.emplace_back(addr);
            }

            if (walletModel_->rememberMultisigConfig(config)) {
                if (!saveWalletChanges()) return false;
                logInfo(tr("Address founders added in wallet"));

                // Обновляем список сохраненных адресов
                loadSavedMultisigAddresses();
            }
        }

        updatePrimaryBalance();
    } else {
        resetDelegationOperation();
        resetHierarchicalOperation();
        delegatedMSpin_->setValue(m);
        delegatedNSpin_->setValue(n);
        currentDelegatedAddress_ = address;
        delegatedAddressLabel_->setText(tr("Address: %1").arg(address));
        delegatedConfigParticipants_ = participants;
        delegatedConfigLoaded_ = true;

        // Добавляем в кошелек если есть
        if (walletModel_ && walletModel_->getWallet() && !address.isEmpty()) {
            geocoin::wallet::MultisigConfig config;
            config.m = m;
            config.n = n;
            config.address = geocoin::Address(address.toStdString());

            for (const QJsonValue& val : participants) {
                QJsonObject p = val.toObject();
                QString addrStr = p.value("address").toString();

                geocoin::Address addr(addrStr.toStdString());
                config.addresses.push_back(addr);
                config.participants.emplace_back(addr);
            }

            if (walletModel_->rememberMultisigConfig(config)) {
                if (!saveWalletChanges()) return false;
                logInfo(tr("Address directors added in wallet"));

                // Обновляем список сохраненных адресов
                loadSavedMultisigAddresses();
            }
        }
    }

    return true;
}

void CorporatePage::logError(const QString& message)
{
    if (activityLog_) {
        activityLog_->append(QString("<span style='color: #e74c3c;'>[%1] ❌ %2</span>")
            .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
            .arg(message));
    }
}

void CorporatePage::logInfo(const QString& message)
{
    if (activityLog_) {
        activityLog_->append(QString("<span style='color: #27ae60;'>[%1] ✅ %2</span>")
            .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
            .arg(message));
    }
}

void CorporatePage::setFounderDraftInProgress(bool inProgress)
{
    founderDraftInProgress_ = inProgress;
    updateFounderState();
    updateStatusBanner();
}

void CorporatePage::setDirectorDraftInProgress(bool inProgress)
{
    directorDraftInProgress_ = inProgress;
    updateDirectorState();
    updateStatusBanner();
}

void CorporatePage::setCommitmentInProgress(bool inProgress)
{
    commitmentInProgress_ = inProgress;
    updateSignerState();
    updateStatusBanner();
}


// ============================================================================
// Build UI - Director Page
// ============================================================================

void CorporatePage::handleDelegationProofDraftFailed(const QString& error)
{
    setFounderDraftInProgress(false);

    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not create the draft"), error);

    logError(tr("Could not create the delegation draft: %1").arg(error));
    addHistoryEntry("DelegationDraft", tr("Error"), error);
}

void CorporatePage::handleCreateAndSignFailed(const QString& error)
{
    setFounderDraftInProgress(false);

    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not create and sign"), error);
    logError(tr("Could not create and sign the delegation: %1").arg(error));
    addHistoryEntry("CreateAndSign", tr("Error"), error);
}

// ============================================================================
// Delegation Scanner Handlers
// ============================================================================

void CorporatePage::handleDelegationProofFinalizeFailed(const QString& error)
{
    setFounderDraftInProgress(false);

    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not finalize"), error);

    logError(tr("Could not finalize the delegation: %1").arg(error));
    addHistoryEntry("DelegationFinalize", tr("Error"), error);
}

void CorporatePage::handleHierarchicalSpendDraftFailed(const QString& error)
{
    setDirectorDraftInProgress(false);

    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not create the draft"), error);

    logError(tr("Could not create the delegated spend draft: %1").arg(error));
    addHistoryEntry("HierarchicalDraft", tr("Error"), error);
}

void CorporatePage::handleHierarchicalSpendFinalizeFailed(const QString& error)
{
    setDirectorDraftInProgress(false);

    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not finalize"), error);

    logError(tr("Could not finalize the delegated spend: %1").arg(error));
    addHistoryEntry("HierarchicalFinalize", tr("Error"), error);
}

void CorporatePage::handleCommitmentFailed(const QString& error)
{
    setCommitmentInProgress(false);
    commitmentResultLabel_->setText(tr("Error: %1").arg(error));

    GUIUtil::showStyledMessageBox(this, QMessageBox::Critical, tr("Could not create the commitment"), error);

    logError(tr("Could not sign the commitment: %1").arg(error));
    addHistoryEntry("Commitment", tr("Error"), error);
}

// ============================================================================
// Scanner Handlers для Hierarchical Spend
// ============================================================================
