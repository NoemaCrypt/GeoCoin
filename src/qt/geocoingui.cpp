// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "geocoingui.h"
#include "geocoinapplication.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "transactiontablemodel.h"
#include "walletframe.h"
#include "guiutil.h"
#include "forktreedialog.h"
#include "commitmentscanner.h"
#include "networkstyle.h"
#include "optionsmodel.h"

#include <QApplication>
#include <QMenuBar>
#include <QMenu>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QToolButton>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QCloseEvent>
#include <QInputDialog>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QSettings>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDialog>
#include <QFormLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <limits>
#include <string>

using GUIUtil::cleanseQString;
using GUIUtil::ScopedQStringCleanser;

GeoCoinGUI::GeoCoinGUI(const NetworkStyle *networkStyle, QWidget *parent) :
    QMainWindow(parent),
    clientModel(nullptr),
    walletModel(nullptr),
    walletFrame(nullptr),
    lastVisitedPage_(QStringLiteral("overview")),
    titleAddText_(networkStyle->getTitleAddText())
{
    setWindowTitle(tr("GeoCoin Core") + " - " + titleAddText_);
    setWindowIcon(networkStyle->getTrayAndWindowIcon());
    setWindowFlags(windowFlags() |
                   Qt::WindowMinimizeButtonHint |
                   Qt::WindowMaximizeButtonHint |
                   Qt::WindowCloseButtonHint);

    setMinimumSize(860, 560);
    resize(960, 640);

    applyCurrentTheme();

    // Create actions
    createActions();

    // Create menu bar
    createMenuBar();

    // Create tool bars
    createToolBars();

    // Create wallet frame (central widget)
    walletFrame = new WalletFrame(this);
    walletFrame->setStyleSheet("background-color: transparent;");  // Прозрачный чтобы видеть фон главного окна
    connect(walletFrame, &WalletFrame::gpuMiningChanged,
            this, &GeoCoinGUI::updateGpuMiningStatus);
    setCentralWidget(walletFrame);

    // Create status bar
    createStatusBar();

    // Set initial state
    setNumConnections(0);
    setNumBlocks(0);

    restoreUiSettings();
    updateWalletActions();
}

GeoCoinGUI::~GeoCoinGUI()
{
}

void GeoCoinGUI::createActions()
{
    // File menu actions
    createWalletAction = new QAction(QIcon(":/icons/filesave"), tr("&Create Wallet..."), this);
    createWalletAction->setStatusTip(tr("Create a new wallet"));
    connect(createWalletAction, &QAction::triggered, this, &GeoCoinGUI::createWallet);

    loadWalletAction = new QAction(QIcon(":/icons/filesave"), tr("&Load Wallet..."), this);
    loadWalletAction->setStatusTip(tr("Load an existing wallet"));
    connect(loadWalletAction, &QAction::triggered, this, &GeoCoinGUI::loadWallet);

    closeWalletAction = new QAction(tr("&Close Wallet"), this);
    closeWalletAction->setStatusTip(tr("Close the currently loaded wallet"));
    connect(closeWalletAction, &QAction::triggered, this, &GeoCoinGUI::closeWallet);

    quitAction = new QAction(QIcon(":/icons/quit"), tr("E&xit"), this);
    quitAction->setStatusTip(tr("Quit application"));
    quitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));
    quitAction->setMenuRole(QAction::QuitRole);
    connect(quitAction, &QAction::triggered, this, &GeoCoinGUI::close);  // Use close() instead of quit()

    // Settings menu actions
    optionsAction = new QAction(QIcon(":/icons/options"), tr("&Options..."), this);
    optionsAction->setStatusTip(tr("Modify configuration options for GeoCoin"));
    optionsAction->setMenuRole(QAction::PreferencesRole);
    connect(optionsAction, &QAction::triggered, this, &GeoCoinGUI::optionsClicked);

    // Help menu actions
    aboutAction = new QAction(QIcon(":/icons/about"), tr("&About GeoCoin Core"), this);
    aboutAction->setStatusTip(tr("Show information about GeoCoin Core"));
    aboutAction->setMenuRole(QAction::AboutRole);
    connect(aboutAction, &QAction::triggered, this, &GeoCoinGUI::aboutClicked);

    // Wallet actions
    encryptWalletAction = new QAction(QIcon(":/icons/lock_closed"), tr("&Encrypt Wallet..."), this);
    encryptWalletAction->setStatusTip(tr("Encrypt the private keys that belong to your wallet"));
    connect(encryptWalletAction, &QAction::triggered, this, &GeoCoinGUI::encryptWallet);

    backupWalletAction = new QAction(QIcon(":/icons/filesave"), tr("&Backup Wallet..."), this);
    backupWalletAction->setStatusTip(tr("Backup wallet to another location"));
    connect(backupWalletAction, &QAction::triggered, this, &GeoCoinGUI::backupWallet);

    // Import/Export private key actions
    exportPrivateKeyAction = new QAction(QIcon(":/icons/key"), tr("&Export Private Key..."), this);
    exportPrivateKeyAction->setStatusTip(tr("Export private key for backup"));
    connect(exportPrivateKeyAction, &QAction::triggered, this, &GeoCoinGUI::exportPrivateKey);

    importPrivateKeyAction = new QAction(QIcon(":/icons/key"), tr("&Import Private Key..."), this);
    importPrivateKeyAction->setStatusTip(tr("Import private key from backup"));
    connect(importPrivateKeyAction, &QAction::triggered, this, &GeoCoinGUI::importPrivateKey);

    changePassphraseAction = new QAction(QIcon(":/icons/key"), tr("&Change Passphrase..."), this);
    changePassphraseAction->setStatusTip(tr("Change the passphrase used for wallet encryption"));
    connect(changePassphraseAction, &QAction::triggered, this, &GeoCoinGUI::changePassphrase);

    // Tab actions
    overviewAction = new QAction(QIcon(":/icons/overview"), tr("&Overview"), this);
    overviewAction->setStatusTip(tr("Show general overview of wallet"));
    overviewAction->setCheckable(true);
    connect(overviewAction, &QAction::triggered, this, &GeoCoinGUI::gotoOverviewPage);

    sendCoinsAction = new QAction(QIcon(":/icons/send"), tr("&Send"), this);
    sendCoinsAction->setStatusTip(tr("Send coins to a GeoCoin address"));
    sendCoinsAction->setCheckable(true);
    connect(sendCoinsAction, &QAction::triggered, this, &GeoCoinGUI::gotoSendCoinsPage);

    receiveCoinsAction = new QAction(QIcon(":/icons/receiving_addresses"), tr("&Receive"), this);
    receiveCoinsAction->setStatusTip(tr("Request payments (generates QR codes and geocoin: URIs)"));
    receiveCoinsAction->setCheckable(true);
    connect(receiveCoinsAction, &QAction::triggered, this, &GeoCoinGUI::gotoReceiveCoinsPage);

    historyAction = new QAction(QIcon(":/icons/history"), tr("&Transactions"), this);
    historyAction->setStatusTip(tr("Browse transaction history"));
    historyAction->setCheckable(true);
    connect(historyAction, &QAction::triggered, this, &GeoCoinGUI::gotoHistoryPage);

    miningAction = new QAction(QIcon(":/icons/mining"), tr("&Mining"), this);
    miningAction->setStatusTip(tr("Mine new blocks and earn rewards"));
    miningAction->setCheckable(true);
    connect(miningAction, &QAction::triggered, this, &GeoCoinGUI::gotoMiningPage);

    multisigAction = new QAction(QIcon(":/icons/multisig"), tr("&Multisig"), this);
    multisigAction->setStatusTip(tr("Manage multisig drafts and signatures"));
    multisigAction->setCheckable(true);
    connect(multisigAction, &QAction::triggered, this, &GeoCoinGUI::gotoMultisigPage);

    corporateAction = new QAction(QIcon(":/icons/corporate"), tr("&Corporate"), this);
    corporateAction->setStatusTip(tr("Corporate governance and delegation"));
    corporateAction->setCheckable(true);
    connect(corporateAction, &QAction::triggered, this, &GeoCoinGUI::gotoCorporatePage);

    nftAction = new QAction(QIcon(":/icons/corporate"), tr("&NFT / Cadastr"), this);
    nftAction->setStatusTip(tr("Browse your NFTs and cadastral certificates"));
    nftAction->setCheckable(true);
    connect(nftAction, &QAction::triggered, this, &GeoCoinGUI::gotoNftPage);

    QActionGroup* pageGroup = new QActionGroup(this);
    pageGroup->setExclusive(true);
    pageGroup->addAction(overviewAction);
    pageGroup->addAction(sendCoinsAction);
    pageGroup->addAction(receiveCoinsAction);
    pageGroup->addAction(historyAction);
    pageGroup->addAction(miningAction);
    pageGroup->addAction(multisigAction);
    pageGroup->addAction(corporateAction);
    pageGroup->addAction(nftAction);
}

bool GeoCoinGUI::runSmokeChecks(QString *error)
{
    auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (!walletFrame) {
        return fail(tr("Wallet frame was not created"));
    }
    if (!walletFrame->hasRequiredPages()) {
        return fail(tr("Required wallet pages are missing"));
    }
    if (walletFrame->pageCount() < 8) {
        return fail(tr("Wallet frame page count is incomplete"));
    }
    if (!overviewAction || !sendCoinsAction || !receiveCoinsAction || !historyAction ||
        !miningAction || !multisigAction || !corporateAction || !nftAction ||
        !closeWalletAction) {
        return fail(tr("Navigation actions were not created"));
    }
    if (!statusBar() || !progressBar || !labelConnectionsText || !labelBlocksIcon) {
        return fail(tr("Status bar widgets are missing"));
    }
    if ((windowFlags() & Qt::WindowMaximizeButtonHint) == 0) {
        return fail(tr("Main window is missing maximize button hint"));
    }
    if (TransactionTableModel::confirmationCount(0, 100, false) != 0 ||
        TransactionTableModel::confirmationCount(95, 100, false) != 6) {
        return fail(tr("Transaction confirmation accounting is incorrect"));
    }

    qint64 parsedAmount = -1;
    qint64 addedAmount = 0;
    const bool exactAmountChecks =
        GUIUtil::parseAmount(QStringLiteral("0"), parsedAmount) && parsedAmount == 0 &&
        GUIUtil::parseAmount(QStringLiteral("0.00000001"), parsedAmount) && parsedAmount == 1 &&
        GUIUtil::parseAmount(QStringLiteral("1.00000001"), parsedAmount) && parsedAmount == 100000001 &&
        GUIUtil::parseAmount(QStringLiteral("92233720368.54775807"), parsedAmount) &&
            parsedAmount == std::numeric_limits<qint64>::max() &&
        !GUIUtil::parseAmount(QString(), parsedAmount) &&
        !GUIUtil::parseAmount(QStringLiteral("-1"), parsedAmount) &&
        !GUIUtil::parseAmount(QStringLiteral("1."), parsedAmount) &&
        !GUIUtil::parseAmount(QStringLiteral(".1"), parsedAmount) &&
        !GUIUtil::parseAmount(QStringLiteral("1e-8"), parsedAmount) &&
        !GUIUtil::parseAmount(QStringLiteral("NaN"), parsedAmount) &&
        !GUIUtil::parseAmount(QStringLiteral("Inf"), parsedAmount) &&
        !GUIUtil::parseAmount(QStringLiteral("0.000000001"), parsedAmount) &&
        !GUIUtil::parseAmount(QStringLiteral("92233720368.54775808"), parsedAmount) &&
        GUIUtil::formatAmount(std::numeric_limits<qint64>::max()) ==
            QStringLiteral("92233720368.54775807") &&
        GUIUtil::formatAmount(std::numeric_limits<qint64>::min()) ==
            QStringLiteral("-92233720368.54775808") &&
        GUIUtil::addAmounts(1, 2, addedAmount) && addedAmount == 3 &&
        !GUIUtil::addAmounts(std::numeric_limits<qint64>::max(), 1, addedAmount) &&
        !GUIUtil::addAmounts(std::numeric_limits<qint64>::min(), -1, addedAmount);
    if (!exactAmountChecks) {
        return fail(tr("Exact amount parser boundary checks failed"));
    }

    QString qtSecret = QStringLiteral("secret");
    {
        ScopedQStringCleanser cleanser(qtSecret);
    }
    std::string stdSecret = "secret";
    {
        GUIUtil::ScopedStringCleanser cleanser(stdSecret);
    }
    if (!qtSecret.isEmpty() || !stdSecret.empty()) {
        return fail(tr("Sensitive value cleanup checks failed"));
    }

    geocoin::Hash256 blockA{};
    geocoin::Hash256 blockB{};
    blockA[0] = 0x11;
    blockB[0] = 0x22;
    const bool scannerReconciliationChecks =
        !CommitmentScanner::anchorRequiresRescan(100, false, blockA, 100, true, blockB) &&
        !CommitmentScanner::anchorRequiresRescan(100, true, blockA, 100, true, blockA) &&
        CommitmentScanner::anchorRequiresRescan(100, true, blockA, 100, true, blockB) &&
        CommitmentScanner::anchorRequiresRescan(100, true, blockA, 120, true, blockB) &&
        CommitmentScanner::anchorRequiresRescan(100, true, blockA, 99, false, blockB) &&
        CommitmentScanner::storedCommitmentRemainsActive(90, blockA, 100, true, blockA) &&
        !CommitmentScanner::storedCommitmentRemainsActive(90, blockA, 100, true, blockB) &&
        !CommitmentScanner::storedCommitmentRemainsActive(101, blockA, 100, false, blockA);
    QSet<QString> allowedSigners{QStringLiteral("alice"), QStringLiteral("bob")};
    QSet<QString> collectedSigners{QStringLiteral("alice")};
    if (!scannerReconciliationChecks ||
        CommitmentScanner::signerCanBeCollected(QStringLiteral("mallory"), allowedSigners, {}) ||
        CommitmentScanner::signerCanBeCollected(QStringLiteral("alice"), allowedSigners, collectedSigners) ||
        !CommitmentScanner::signerCanBeCollected(QStringLiteral("bob"), allowedSigners, collectedSigners)) {
        return fail(tr("Commitment scanner reorg reconciliation checks failed"));
    }

    geocoin::Amount checkedAmount = 0;
    if (geocoin::checked_add_amount((std::numeric_limits<geocoin::Amount>::max)(), 1, checkedAmount) ||
        geocoin::checked_sub_amount((std::numeric_limits<geocoin::Amount>::min)(), 1, checkedAmount)) {
        return fail(tr("Wallet balance overflow checks failed"));
    }

    const QString lifecycleDir = QDir(dataDirectory).filePath(QStringLiteral("wallet-smoke"));
    QDir(lifecycleDir).removeRecursively();
    if (!QDir().mkpath(lifecycleDir)) {
        return fail(tr("Could not create wallet smoke directory"));
    }
    {
        WalletModel lifecycleModel;
        lifecycleModel.setDataDirectory(lifecycleDir);
        const QString expectedPath = QDir(lifecycleDir).filePath(QStringLiteral("wallet.dat"));
        if (QFileInfo(lifecycleModel.getWalletFilePath()).absoluteFilePath() !=
            QFileInfo(expectedPath).absoluteFilePath()) {
            return fail(tr("Wallet model did not use the configured data directory"));
        }

        int stateChanges = 0;
        connect(&lifecycleModel, &WalletModel::walletStateChanged,
                this, [&stateChanges]() { ++stateChanges; });
        const QString smokePassword = QStringLiteral("smoke-password");
        if (!lifecycleModel.createWallet(expectedPath, smokePassword) ||
            !lifecycleModel.isWalletLoaded() || !lifecycleModel.isWalletEncrypted()) {
            return fail(tr("Encrypted wallet creation smoke check failed"));
        }
        QString saveError;
        if (lifecycleModel.saveWallet(QString(), &saveError) ||
            !lifecycleModel.hasUnsavedChanges() ||
            lifecycleModel.saveWallet(smokePassword, &saveError) == false) {
            return fail(tr("Encrypted wallet save result smoke check failed"));
        }
        lifecycleModel.closeWallet();
        if (lifecycleModel.isWalletLoaded() ||
            !lifecycleModel.loadWallet(expectedPath, smokePassword) ||
            stateChanges < 3) {
            return fail(tr("Wallet close/load lifecycle smoke check failed"));
        }
        const QString replacementPassword = QStringLiteral("smoke-password-2");
        if (!lifecycleModel.changeWalletPassphrase(
                smokePassword, replacementPassword, &saveError) ||
            QFileInfo::exists(expectedPath + QStringLiteral(".bak"))) {
            return fail(tr("Wallet passphrase replacement smoke check failed"));
        }
        lifecycleModel.closeWallet();
        if (lifecycleModel.loadWallet(expectedPath, smokePassword) ||
            !lifecycleModel.loadWallet(expectedPath, replacementPassword)) {
            return fail(tr("Old wallet passphrase remained usable after replacement"));
        }
        lifecycleModel.closeWallet();
    }
    QDir(lifecycleDir).removeRecursively();

    gotoOverviewPage();
    QCoreApplication::processEvents();
    gotoSendCoinsPage();
    QCoreApplication::processEvents();
    gotoReceiveCoinsPage();
    QCoreApplication::processEvents();
    gotoHistoryPage();
    QCoreApplication::processEvents();
    gotoMiningPage();
    QCoreApplication::processEvents();
    gotoMultisigPage();
    QCoreApplication::processEvents();
    gotoCorporatePage();
    QCoreApplication::processEvents();
    gotoNftPage();
    QCoreApplication::processEvents();
    gotoOverviewPage();
    QCoreApplication::processEvents();

    QSettings settings;
    settings.setValue(QStringLiteral("smoke/lastDataDir"), dataDirectory);
    settings.setValue(QStringLiteral("smoke/pageCount"), walletFrame->pageCount());
    settings.sync();

    return true;
}

void GeoCoinGUI::createMenuBar()
{
    // File menu
    fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(createWalletAction);
    fileMenu->addAction(loadWalletAction);
    fileMenu->addAction(closeWalletAction);
    fileMenu->addSeparator();
    fileMenu->addAction(backupWalletAction);
    fileMenu->addAction(exportPrivateKeyAction);
    fileMenu->addAction(importPrivateKeyAction);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction);

    // Settings menu
    settingsMenu = menuBar()->addMenu(tr("&Settings"));
    settingsMenu->addAction(encryptWalletAction);
    settingsMenu->addAction(changePassphraseAction);
    settingsMenu->addSeparator();
    settingsMenu->addAction(optionsAction);

    languageMenu = settingsMenu->addMenu(tr("&Language"));
    langEnglishAction = new QAction(tr("&English"), this);
    langRussianAction = new QAction(tr("&Russian"), this);
    langEnglishAction->setCheckable(true);
    langRussianAction->setCheckable(true);

    QActionGroup* langGroup = new QActionGroup(this);
    langGroup->addAction(langEnglishAction);
    langGroup->addAction(langRussianAction);
    langEnglishAction->setChecked(true);

    QSettings settings;
    const QString code = settings.value(QStringLiteral("ui/language"), QStringLiteral("en")).toString();
    if (code == "ru") {
        langRussianAction->setChecked(true);
    } else {
        langEnglishAction->setChecked(true);
    }

    languageMenu->addAction(langEnglishAction);
    languageMenu->addAction(langRussianAction);

    auto onLangSelected = [this](const QString& langCode) {
        OptionsModel options;
        const QString oldLang = options.getLanguage();
        options.setLanguage(langCode);
        if (oldLang == langCode) {
            return;
        }
        if (auto* app = qobject_cast<GeoCoinApplication*>(qApp)) {
            app->setLanguage(langCode);
        }
        retranslateUi();
        if (walletFrame) {
            walletFrame->rebuildForLanguageChange();
        }
        showStyledMessageBox(1, tr("Language"), tr("Language applied."));
    };
    connect(langEnglishAction, &QAction::triggered, this, [onLangSelected]() { onLangSelected("en"); });
    connect(langRussianAction, &QAction::triggered, this, [onLangSelected]() { onLangSelected("ru"); });

    // Help menu
    helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(aboutAction);
}

void GeoCoinGUI::applyCurrentTheme()
{
    const QString theme = GUIUtil::currentTheme();
    GUIUtil::applyApplicationTheme(qApp, theme);
    setStyleSheet(GUIUtil::windowStyleSheet(theme));
    GUIUtil::applyNativeWindowTheme(this, theme);
    if (walletFrame) {
        walletFrame->applyTheme();
    }
}

void GeoCoinGUI::retranslateUi()
{
    setWindowTitle(tr("GeoCoin Core") + " - " + titleAddText_);

    if (fileMenu) fileMenu->setTitle(tr("&File"));
    if (settingsMenu) settingsMenu->setTitle(tr("&Settings"));
    if (helpMenu) helpMenu->setTitle(tr("&Help"));
    if (languageMenu) languageMenu->setTitle(tr("&Language"));

    if (createWalletAction) {
        createWalletAction->setText(tr("&Create Wallet..."));
        createWalletAction->setStatusTip(tr("Create a new wallet"));
    }
    if (loadWalletAction) {
        loadWalletAction->setText(tr("&Load Wallet..."));
        loadWalletAction->setStatusTip(tr("Load an existing wallet"));
    }
    if (closeWalletAction) {
        closeWalletAction->setText(tr("&Close Wallet"));
        closeWalletAction->setStatusTip(tr("Close the currently loaded wallet"));
    }
    if (quitAction) {
        quitAction->setText(tr("E&xit"));
        quitAction->setStatusTip(tr("Quit application"));
    }
    if (optionsAction) {
        optionsAction->setText(tr("&Options..."));
        optionsAction->setStatusTip(tr("Modify configuration options for GeoCoin"));
    }
    if (aboutAction) {
        aboutAction->setText(tr("&About GeoCoin Core"));
        aboutAction->setStatusTip(tr("Show information about GeoCoin Core"));
    }
    if (encryptWalletAction) encryptWalletAction->setText(tr("&Encrypt Wallet..."));
    if (backupWalletAction) backupWalletAction->setText(tr("&Backup Wallet..."));
    if (exportPrivateKeyAction) exportPrivateKeyAction->setText(tr("&Export Private Key..."));
    if (importPrivateKeyAction) importPrivateKeyAction->setText(tr("&Import Private Key..."));
    if (changePassphraseAction) changePassphraseAction->setText(tr("&Change Passphrase..."));

    if (overviewAction) overviewAction->setText(tr("&Overview"));
    if (sendCoinsAction) sendCoinsAction->setText(tr("&Send"));
    if (receiveCoinsAction) receiveCoinsAction->setText(tr("&Receive"));
    if (historyAction) historyAction->setText(tr("&Transactions"));
    if (miningAction) miningAction->setText(tr("&Mining"));
    if (multisigAction) multisigAction->setText(tr("&Multisig"));
    if (corporateAction) corporateAction->setText(tr("&Corporate"));
    if (nftAction) nftAction->setText(tr("&NFT / Cadastr"));
    if (langEnglishAction) langEnglishAction->setText(tr("&English"));
    if (langRussianAction) langRussianAction->setText(tr("&Russian"));
}

void GeoCoinGUI::createToolBars()
{
    QToolBar *toolbar = addToolBar(tr("Tabs toolbar"));
    toolbar->setObjectName(QStringLiteral("tabsToolbar"));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setStyleSheet(QStringLiteral(
        "QToolBar#tabsToolbar { spacing: 3px; }"
        "QToolBar#tabsToolbar QToolButton { padding: 7px 10px; min-width: 0px; }"
    ));

    toolbar->addAction(overviewAction);
    toolbar->addAction(sendCoinsAction);
    toolbar->addAction(receiveCoinsAction);
    toolbar->addAction(historyAction);
    toolbar->addAction(miningAction);
    toolbar->addAction(multisigAction);
    toolbar->addAction(corporateAction);
    toolbar->addAction(nftAction);

    overviewAction->setChecked(true);

    // Disable tooltips on toolbar buttons to avoid hover popups/lag
    const QList<QAction*> tabActions = {
        overviewAction,
        sendCoinsAction,
        receiveCoinsAction,
        historyAction,
        miningAction,
        multisigAction,
        corporateAction,
        nftAction
    };
    for (QAction* action : tabActions) {
        if (!action) {
            continue;
        }
        action->setToolTip(QString());
        action->setStatusTip(QString());
        action->setWhatsThis(QString());
    }
    const auto toolButtons = toolbar->findChildren<QToolButton*>();
    for (QToolButton* btn : toolButtons) {
        if (!btn) {
            continue;
        }
        btn->setToolTip(QString());
        btn->setWhatsThis(QString());
    }
}

void GeoCoinGUI::createStatusBar()
{
    // Connections icon
    labelConnectionsIcon = new QLabel();
    labelConnectionsIcon->setToolTip(tr("Number of connections to the GeoCoin network"));
    statusBar()->addPermanentWidget(labelConnectionsIcon);

    labelConnectionsText = new QLabel(tr("0 peers"));
    labelConnectionsText->setStyleSheet("QLabel { color: " + GUIUtil::Theme::ColorBlue + "; }");
    statusBar()->addPermanentWidget(labelConnectionsText);

    // Blocks icon
    labelBlocksIcon = new QLabel();
    labelBlocksIcon->setToolTip(tr("Number of blocks in the blockchain"));
    statusBar()->addPermanentWidget(labelBlocksIcon);

    labelBlocksText = new QLabel(tr("0 blocks"));
    labelBlocksText->setStyleSheet("QLabel { color: " + GUIUtil::Theme::ColorBlue + "; }");
    statusBar()->addPermanentWidget(labelBlocksText);

    // Progress bar
    progressBarLabel = new QLabel();
    progressBarLabel->setVisible(false);
    statusBar()->addWidget(progressBarLabel);

    progressBar = new QProgressBar();
    progressBar->setAlignment(Qt::AlignCenter);
    progressBar->setVisible(false);
    statusBar()->addWidget(progressBar);

    labelHashrateStatus = new QLabel(tr("Mining: idle (0 H/s)"));
    labelHashrateStatus->setStyleSheet("QLabel { color: " + GUIUtil::Theme::ColorGray + "; font-weight: bold; }");
    statusBar()->addPermanentWidget(labelHashrateStatus, 1);
}

void GeoCoinGUI::setClientModel(ClientModel *model)
{
    if (clientModel == model) {
        return;
    }
    if (clientModel) {
        disconnect(clientModel, nullptr, this, nullptr);
    }

    this->clientModel = model;
    if (model) {
        // Connect signals
        connect(model, &ClientModel::numConnectionsChanged, this, &GeoCoinGUI::setNumConnections, Qt::UniqueConnection);
        connect(model, &ClientModel::numBlocksChanged, this, &GeoCoinGUI::setNumBlocks, Qt::UniqueConnection);
        connect(model, &ClientModel::miningChanged, this, &GeoCoinGUI::updateMiningStatus, Qt::UniqueConnection);

        // Pass to wallet frame
        if (walletFrame) {
            walletFrame->setClientModel(model);
        }

        // Initial update
        setNumConnections(model->getNumConnections());
        setNumBlocks(model->getNumBlocks());
        updateMiningStatus(model->getMiningEnabled(), model->getMiningThreads(), model->getHashrate(), model->getMinedBlocksAccepted());
    }
}

void GeoCoinGUI::setWalletModel(WalletModel *model)
{
    if (walletModel == model) {
        updateWalletActions();
        return;
    }
    if (walletModel) {
        disconnect(walletModel, nullptr, this, nullptr);
    }
    this->walletModel = model;
    if (walletFrame) {
        walletFrame->setWalletModel(model);
    }
    if (walletModel) {
        connect(walletModel, &WalletModel::walletStateChanged,
                this, &GeoCoinGUI::updateWalletActions);
        auto showHistoryError = [this](const QString& error) {
            if (error == lastWalletHistoryError_) return;
            lastWalletHistoryError_ = error;
            if (error.isEmpty()) return;
            auto* message = new QMessageBox(QMessageBox::Warning, tr("Wallet history is incomplete"),
                                           error, QMessageBox::Ok, this);
            message->setAttribute(Qt::WA_DeleteOnClose);
            message->open();
        };
        connect(walletModel, &WalletModel::walletHistoryError, this, showHistoryError);
        showHistoryError(walletModel->getHistoryScanError());
    }
    updateWalletActions();
}

void GeoCoinGUI::waitForPendingMiningStop()
{
    if (walletFrame) {
        walletFrame->waitForPendingMiningStop();
    }
}

void GeoCoinGUI::setDataDirectory(const QString &dataDir)
{
    this->dataDirectory = dataDir;
}

void GeoCoinGUI::persistLastPage(const QString& pageId)
{
    lastVisitedPage_ = pageId;
}

void GeoCoinGUI::restoreUiSettings()
{
    QSettings settings;

    const QByteArray geometry = settings.value(QStringLiteral("ui/main_window_geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }

    const QByteArray state = settings.value(QStringLiteral("ui/main_window_state")).toByteArray();
    if (!state.isEmpty()) {
        restoreState(state);
    }

    const bool restoreLastPage = settings.value(QStringLiteral("ui/restore_last_page"), true).toBool();
    const QString startupPage = settings.value(QStringLiteral("ui/startup_page"), QStringLiteral("overview")).toString();
    const QString page = restoreLastPage
        ? settings.value(QStringLiteral("ui/last_page"), startupPage).toString()
        : startupPage;
    gotoPageById(page);
}

void GeoCoinGUI::gotoPageById(const QString& pageId)
{
    if (pageId == QLatin1String("send")) {
        gotoSendCoinsPage();
    } else if (pageId == QLatin1String("receive")) {
        gotoReceiveCoinsPage();
    } else if (pageId == QLatin1String("history")) {
        gotoHistoryPage();
    } else if (pageId == QLatin1String("mining")) {
        gotoMiningPage();
    } else if (pageId == QLatin1String("multisig")) {
        gotoMultisigPage();
    } else if (pageId == QLatin1String("corporate")) {
        gotoCorporatePage();
    } else if (pageId == QLatin1String("nft")) {
        gotoNftPage();
    } else {
        gotoOverviewPage();
    }
}

void GeoCoinGUI::setNumConnections(int count)
{
    labelConnectionsIcon->setToolTip(tr("%n active connection(s) to GeoCoin network", "", count));

    if (labelConnectionsText) {
        labelConnectionsText->setText(tr("%n peer(s)", "", count));
    }
}

void GeoCoinGUI::setNumBlocks(int count)
{
    labelBlocksIcon->setToolTip(tr("Processed %n block(s) of transaction history.", "", count));
    if (labelBlocksText) {
        labelBlocksText->setText(tr("%n block(s)", "", count));
    }
}

void GeoCoinGUI::updateMiningStatus(bool enabled, int threads, double hashrate, quint64)
{
    statusCpuMiningEnabled = enabled;
    statusCpuMiningThreads = threads;
    statusCpuHashrate = enabled ? hashrate : 0.0;
    refreshMiningStatusBar();
}

void GeoCoinGUI::updateGpuMiningStatus(bool enabled, double hashrate, int, int)
{
    statusGpuMiningEnabled = enabled;
    statusGpuHashrate = enabled ? hashrate : 0.0;
    refreshMiningStatusBar();
}

void GeoCoinGUI::refreshMiningStatusBar()
{
    if (!labelHashrateStatus) return;

    const bool cpuActive = statusCpuMiningEnabled;
    const bool gpuActive = statusGpuMiningEnabled;
    const double totalHashrate = (cpuActive ? statusCpuHashrate : 0.0) +
                                 (gpuActive ? statusGpuHashrate : 0.0);

    QString text;
    if (cpuActive && gpuActive) {
        text = tr("Mining: %1 • CPU %2 thread(s) + GPU")
            .arg(GUIUtil::formatHashrate(totalHashrate))
            .arg(statusCpuMiningThreads);
        labelHashrateStatus->setStyleSheet("QLabel { color: " + GUIUtil::Theme::ColorGreen + "; font-weight: bold; }");
    } else if (cpuActive) {
        text = tr("Mining: %1 • CPU %2 thread(s)")
            .arg(GUIUtil::formatHashrate(statusCpuHashrate))
            .arg(statusCpuMiningThreads);
        labelHashrateStatus->setStyleSheet("QLabel { color: " + GUIUtil::Theme::ColorGreen + "; font-weight: bold; }");
    } else if (gpuActive) {
        text = tr("Mining: %1 • GPU").arg(GUIUtil::formatHashrate(statusGpuHashrate));
        labelHashrateStatus->setStyleSheet("QLabel { color: " + GUIUtil::Theme::ColorGreen + "; font-weight: bold; }");
    } else {
        text = tr("Mining: idle (0 H/s)");
        labelHashrateStatus->setStyleSheet("QLabel { color: " + GUIUtil::Theme::ColorGray + "; font-weight: bold; }");
    }
    labelHashrateStatus->setText(text);
}

void GeoCoinGUI::setSyncProgress(double progress, int current, int target)
{
    if (progress < 1.0 && target > 0) {
        // Show progress bar
        progressBarLabel->setVisible(true);
        progressBar->setVisible(true);

        progressBarLabel->setText(tr("Synchronizing..."));
        progressBar->setFormat(tr("%1 / %2 blocks").arg(current).arg(target));
        progressBar->setMaximum(target);
        progressBar->setValue(current);

        labelBlocksIcon->setToolTip(tr("Synchronizing with the network"));
    } else {
        // Hide progress bar
        progressBarLabel->setVisible(false);
        progressBar->setVisible(false);
        labelBlocksIcon->setToolTip(tr("Synchronized with the network"));
    }
}

void GeoCoinGUI::aboutClicked()
{
    QMessageBox::about(this, tr("About GeoCoin Core"),
        tr("<b>GeoCoin Core</b> is a peer-to-peer digital currency system."
           "<br><br>"
           "This is experimental software."
           "<br><br>"
           "GeoCoin uses the Falcon-512 cryptographic protocol for "
           "post-quantum secure signatures, "
           "and GeoPoW for proof-of-work mining."));
}

void GeoCoinGUI::optionsClicked()
{
    OptionsModel options;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Options"));
    dialog.setModal(true);
    dialog.setStyleSheet(GUIUtil::dialogStyleSheet(GUIUtil::currentTheme()));

    QFormLayout* form = new QFormLayout(&dialog);

    QComboBox* languageCombo = new QComboBox(&dialog);
    languageCombo->addItem(tr("English"), QStringLiteral("en"));
    languageCombo->addItem(tr("Russian"), QStringLiteral("ru"));
    const QString currentLanguage = options.getLanguage();
    const int langIndex = languageCombo->findData(currentLanguage);
    languageCombo->setCurrentIndex(langIndex >= 0 ? langIndex : 0);
    form->addRow(tr("Interface language:"), languageCombo);

    QComboBox* themeCombo = new QComboBox(&dialog);
    themeCombo->addItem(tr("Light"), QStringLiteral("light"));
    themeCombo->addItem(tr("Dark"), QStringLiteral("dark"));
    const QString currentTheme = options.getTheme();
    const int themeIndex = themeCombo->findData(currentTheme);
    themeCombo->setCurrentIndex(themeIndex >= 0 ? themeIndex : 0);
    form->addRow(tr("Theme:"), themeCombo);

    QCheckBox* restoreLastPage = new QCheckBox(tr("Restore last open page on startup"), &dialog);
    restoreLastPage->setChecked(options.getRestoreLastPage());
    form->addRow(QString(), restoreLastPage);

    QComboBox* startupPageCombo = new QComboBox(&dialog);
    startupPageCombo->addItem(tr("Overview"), QStringLiteral("overview"));
    startupPageCombo->addItem(tr("Send"), QStringLiteral("send"));
    startupPageCombo->addItem(tr("Receive"), QStringLiteral("receive"));
    startupPageCombo->addItem(tr("Transactions"), QStringLiteral("history"));
    startupPageCombo->addItem(tr("Mining"), QStringLiteral("mining"));
    startupPageCombo->addItem(tr("Multisig"), QStringLiteral("multisig"));
    startupPageCombo->addItem(tr("Corporate"), QStringLiteral("corporate"));
    startupPageCombo->addItem(tr("NFT / Cadastr"), QStringLiteral("nft"));
    const QString startupPage = options.getStartupPage();
    const int startupIndex = startupPageCombo->findData(startupPage);
    startupPageCombo->setCurrentIndex(startupIndex >= 0 ? startupIndex : 0);
    startupPageCombo->setEnabled(!restoreLastPage->isChecked());
    form->addRow(tr("Startup page:"), startupPageCombo);

    connect(restoreLastPage, &QCheckBox::toggled, startupPageCombo, [startupPageCombo](bool checked) {
        startupPageCombo->setEnabled(!checked);
    });

    QPushButton* forkTreeButton = new QPushButton(tr("Open Fork Tree"), &dialog);
    connect(forkTreeButton, &QPushButton::clicked, this, [this, &dialog]() {
        showForkTreeDialog(clientModel, &dialog);
    });
    form->addRow(tr("Engineering:"), forkTreeButton);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString newLanguage = languageCombo->currentData().toString();
    const QString newTheme = themeCombo->currentData().toString();
    const bool newRestore = restoreLastPage->isChecked();
    const QString newStartupPage = startupPageCombo->currentData().toString();
    const QString oldLanguage = options.getLanguage();
    const QString oldTheme = options.getTheme();

    options.setLanguage(newLanguage);
    options.setTheme(newTheme);
    options.setRestoreLastPage(newRestore);
    options.setStartupPage(newStartupPage);

    if (langEnglishAction && langRussianAction) {
        langEnglishAction->setChecked(newLanguage != QStringLiteral("ru"));
        langRussianAction->setChecked(newLanguage == QStringLiteral("ru"));
    }

    if (!newRestore) {
        gotoPageById(newStartupPage);
    }

    if (newTheme != oldTheme) {
        applyCurrentTheme();
    }
    if (newLanguage != oldLanguage) {
        if (auto* app = qobject_cast<GeoCoinApplication*>(qApp)) {
            app->setLanguage(newLanguage);
        }
        retranslateUi();
        if (walletFrame) {
            walletFrame->rebuildForLanguageChange();
        }
    }
    if (newLanguage != oldLanguage || newTheme != oldTheme) {
        showStyledMessageBox(1, tr("Options"), tr("Interface settings applied."));
    }
}

void GeoCoinGUI::gotoOverviewPage()
{
    persistLastPage(QStringLiteral("overview"));
    overviewAction->setChecked(true);
    if (walletFrame) walletFrame->gotoOverviewPage();
}

void GeoCoinGUI::gotoSendCoinsPage()
{
    persistLastPage(QStringLiteral("send"));
    sendCoinsAction->setChecked(true);
    if (walletFrame) walletFrame->gotoSendCoinsPage();
}

void GeoCoinGUI::gotoReceiveCoinsPage()
{
    persistLastPage(QStringLiteral("receive"));
    receiveCoinsAction->setChecked(true);
    if (walletFrame) walletFrame->gotoReceiveCoinsPage();
}

void GeoCoinGUI::gotoHistoryPage()
{
    persistLastPage(QStringLiteral("history"));
    historyAction->setChecked(true);
    if (walletFrame) walletFrame->gotoHistoryPage();
}

void GeoCoinGUI::gotoMiningPage()
{
    persistLastPage(QStringLiteral("mining"));
    miningAction->setChecked(true);
    if (walletFrame) walletFrame->gotoMiningPage();
}

void GeoCoinGUI::gotoMultisigPage()
{
    persistLastPage(QStringLiteral("multisig"));
    multisigAction->setChecked(true);
    if (walletFrame) walletFrame->gotoMultisigPage();
}

void GeoCoinGUI::gotoCorporatePage()
{
    persistLastPage(QStringLiteral("corporate"));
    corporateAction->setChecked(true);
    if (walletFrame) walletFrame->gotoCorporatePage();
}

void GeoCoinGUI::gotoNftPage()
{
    persistLastPage(QStringLiteral("nft"));
    nftAction->setChecked(true);
    if (walletFrame) walletFrame->gotoNftPage();
}

void GeoCoinGUI::closeEvent(QCloseEvent *event)
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/main_window_geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/main_window_state"), saveState());
    settings.setValue(QStringLiteral("ui/last_page"), lastVisitedPage_);
    settings.sync();

    const bool cpuMiningActive = clientModel && clientModel->getMiningEnabled();
    const bool gpuMiningActive = walletFrame && walletFrame->isGpuMiningActive();
    if (cpuMiningActive || gpuMiningActive) {
        int reply = showStyledQuestionBox(tr("Confirm Exit"),
            tr("Mining is active. Are you sure you want to exit?"));

        if (reply != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }

    if (!ensureWalletSaved()) {
        event->ignore();
        return;
    }

    if (walletFrame) {
        walletFrame->stopGpuMining();
    }

    // Accept event and hide window immediately
    event->accept();
    hide();

    // Request shutdown asynchronously (will call quit() when done)
    GeoCoinApplication* app = qobject_cast<GeoCoinApplication*>(qApp);
    if (app) {
        QMetaObject::invokeMethod(app, [app]() {
            app->requestShutdown();
        }, Qt::QueuedConnection);
    }
}

void GeoCoinGUI::showStyledMessageBox(int icon, const QString &title, const QString &text)
{
    GUIUtil::showStyledMessageBox(this, static_cast<QMessageBox::Icon>(icon), title, text);
}

int GeoCoinGUI::showStyledQuestionBox(const QString &title, const QString &text)
{
    return GUIUtil::showStyledMessageBox(
        this,
        QMessageBox::Question,
        title,
        text,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
}

QString GeoCoinGUI::showStyledInputDialog(const QString &title, const QString &label, int echoMode)
{
    QInputDialog inputDialog(this);
    inputDialog.setWindowTitle(title);
    inputDialog.setLabelText(label);
    inputDialog.setTextEchoMode(static_cast<QLineEdit::EchoMode>(echoMode));
    inputDialog.setStyleSheet(GUIUtil::dialogStyleSheet(GUIUtil::currentTheme()));

    if (inputDialog.exec() == QDialog::Accepted) {
        return inputDialog.textValue();
    }
    return QString();
}

void GeoCoinGUI::updateWalletActions()
{
    const bool loaded = walletModel && walletModel->isWalletLoaded();
    const bool encrypted = loaded && walletModel->isWalletEncrypted();
    if (createWalletAction) createWalletAction->setEnabled(walletModel && !loaded);
    if (loadWalletAction) loadWalletAction->setEnabled(walletModel && !loaded);
    if (closeWalletAction) closeWalletAction->setEnabled(loaded);
    if (encryptWalletAction) encryptWalletAction->setEnabled(loaded && !encrypted);
    if (changePassphraseAction) changePassphraseAction->setEnabled(encrypted);
    if (backupWalletAction) backupWalletAction->setEnabled(loaded);
    if (exportPrivateKeyAction) exportPrivateKeyAction->setEnabled(loaded);
    if (importPrivateKeyAction) importPrivateKeyAction->setEnabled(walletModel != nullptr);
    if (sendCoinsAction) sendCoinsAction->setEnabled(loaded);
    if (receiveCoinsAction) receiveCoinsAction->setEnabled(loaded);
    if (nftAction) nftAction->setEnabled(loaded);
}

bool GeoCoinGUI::ensureWalletSaved()
{
    if (!walletModel || !walletModel->isWalletLoaded() || !walletModel->hasUnsavedChanges()) {
        return true;
    }

    QString password;
    ScopedQStringCleanser passwordCleanser(password);
    if (walletModel->isWalletEncrypted()) {
        password = showStyledInputDialog(
            tr("Save Wallet"),
            tr("Unsaved wallet changes exist. Enter the wallet password to save them:"),
            QLineEdit::Password);
        if (password.isEmpty()) {
            return false;
        }
    }

    QString error;
    const bool saved = walletModel->saveWallet(password, &error);
    cleanseQString(password);
    if (!saved) {
        showStyledMessageBox(3, tr("Save Wallet"), error);
    }
    return saved;
}

void GeoCoinGUI::closeWallet()
{
    if (!walletModel || !walletModel->isWalletLoaded()) {
        return;
    }
    const bool cpuMiningActive = clientModel && clientModel->getMiningEnabled();
    const bool gpuMiningActive = walletFrame && walletFrame->isGpuMiningActive();
    if (cpuMiningActive || gpuMiningActive) {
        showStyledMessageBox(2, tr("Close Wallet"),
            tr("Stop CPU and GPU mining before closing the wallet."));
        return;
    }
    if (!ensureWalletSaved()) {
        return;
    }
    walletModel->closeWallet();
}

void GeoCoinGUI::encryptWallet()
{
    if (!walletModel) {
        return;
    }

    if (!walletModel->isWalletLoaded()) {
        showStyledMessageBox(2, tr("Encrypt Wallet"),
            tr("Please load or create a wallet first."));
        return;
    }

    // Check if already encrypted by trying to verify empty password
    if (walletModel->isWalletEncrypted()) {
        showStyledMessageBox(1, tr("Encrypt Wallet"),
            tr("Wallet is already encrypted. Use 'Change Passphrase' to change the password."));
        return;
    }

    showStyledMessageBox(1, tr("Encrypt Wallet"),
        tr("<b>IMPORTANT:</b> You are about to encrypt your wallet.<br><br>"
           "Please make sure you remember your passphrase. "
           "If you lose it, you will lose access to your funds!<br><br>"
           "It is recommended to write down your passphrase and store it in a safe place."));

    // Request new password
    QString password = showStyledInputDialog(tr("Encrypt Wallet"),
        tr("Enter new passphrase to encrypt wallet:"), 2);
    ScopedQStringCleanser passwordCleanser(password);

    if (password.isEmpty()) {
        return;
    }

    if (password.length() < 8) {
        showStyledMessageBox(2, tr("Encrypt Wallet"),
            tr("Passphrase must be at least 8 characters long."));
        return;
    }

    // Confirm password
    QString password2 = showStyledInputDialog(tr("Encrypt Wallet"),
        tr("Confirm passphrase:"), 2);
    ScopedQStringCleanser password2Cleanser(password2);

    if (password2.isEmpty()) {
        return;
    }

    if (password != password2) {
        showStyledMessageBox(3, tr("Encrypt Wallet"),
            tr("Passphrases do not match."));
        return;
    }

    QString error;
    if (walletModel->encryptWallet(password, &error)) {
        showStyledMessageBox(1, tr("Encrypt Wallet"),
            tr("Wallet encrypted successfully!<br><br>"
               "<b>REMEMBER YOUR PASSPHRASE!</b><br>"
               "You will need it to send transactions and backup your wallet."));
    } else {
        showStyledMessageBox(3, tr("Encrypt Wallet"),
            error.isEmpty() ? tr("Failed to encrypt wallet.") : error);
    }
    cleanseQString(password);
    cleanseQString(password2);
}

void GeoCoinGUI::backupWallet()
{
    if (!walletModel || !walletModel->isWalletLoaded()) {
        showStyledMessageBox(2, tr("Backup Wallet"),
            tr("Please load or create a wallet first."));
        return;
    }

    QString filename = QFileDialog::getSaveFileName(this,
        tr("Backup Wallet"), QString(),
        tr("Wallet files (*.dat);;All files (*.*)"));

    if (filename.isEmpty()) {
        return;
    }

    QString password;
    ScopedQStringCleanser passwordCleanser(password);
    if (walletModel->isWalletEncrypted()) {
        password = showStyledInputDialog(tr("Backup Wallet"),
            tr("Enter wallet password:"), QLineEdit::Password);
        if (password.isEmpty()) {
            return;
        }
    }

    QString error;
    if (walletModel->backupWallet(filename, password, &error)) {
        showStyledMessageBox(1, tr("Backup Wallet"),
            tr("Wallet backed up successfully to:\n%1").arg(filename));
    } else {
        showStyledMessageBox(3, tr("Backup Wallet"),
            error.isEmpty() ? tr("Failed to backup wallet.") : error);
    }
    cleanseQString(password);
}

void GeoCoinGUI::changePassphrase()
{
    if (!walletModel || !walletModel->isWalletLoaded()) {
        showStyledMessageBox(2, tr("Change Passphrase"),
            tr("Please load or create a wallet first."));
        return;
    }

    if (!walletModel->isWalletEncrypted()) {
        showStyledMessageBox(2, tr("Change Passphrase"),
            tr("Wallet is not encrypted. Use 'Encrypt Wallet' first."));
        return;
    }

    // Request old password
    QString oldPassword = showStyledInputDialog(tr("Change Passphrase"),
        tr("Enter current passphrase:"), 2);
    ScopedQStringCleanser oldPasswordCleanser(oldPassword);

    if (oldPassword.isEmpty()) {
        return;
    }

    if (!walletModel->verifyPassword(oldPassword)) {
        showStyledMessageBox(3, tr("Change Passphrase"),
            tr("Incorrect password."));
        return;
    }

    // Request new password
    QString newPassword = showStyledInputDialog(tr("Change Passphrase"),
        tr("Enter new passphrase:"), 2);
    ScopedQStringCleanser newPasswordCleanser(newPassword);

    if (newPassword.isEmpty()) {
        return;
    }

    if (newPassword.length() < 8) {
        showStyledMessageBox(2, tr("Change Passphrase"),
            tr("Passphrase must be at least 8 characters long."));
        return;
    }

    // Confirm new password
    QString newPassword2 = showStyledInputDialog(tr("Change Passphrase"),
        tr("Confirm new passphrase:"), 2);
    ScopedQStringCleanser newPassword2Cleanser(newPassword2);

    if (newPassword2.isEmpty()) {
        return;
    }

    if (newPassword != newPassword2) {
        showStyledMessageBox(3, tr("Change Passphrase"),
            tr("Passphrases do not match."));
        return;
    }

    QString error;
    if (walletModel->changeWalletPassphrase(oldPassword, newPassword, &error)) {
        showStyledMessageBox(1, tr("Change Passphrase"),
            tr("Passphrase changed successfully!"));
    } else {
        showStyledMessageBox(3, tr("Change Passphrase"),
            error.isEmpty() ? tr("Failed to change passphrase.") : error);
    }
    cleanseQString(oldPassword);
    cleanseQString(newPassword);
    cleanseQString(newPassword2);
}

void GeoCoinGUI::createWallet()
{
    if (!walletModel) {
        return;
    }

    if (walletModel->isWalletLoaded()) {
        showStyledMessageBox(1, tr("Create Wallet"),
            tr("A wallet is already loaded. Close it first."));
        return;
    }

    const QString filename = walletModel->getWalletFilePath();

    // Check if wallet already exists
    if (QFile::exists(filename)) {
        showStyledMessageBox(2, tr("Create Wallet"),
            tr("Wallet file already exists at:\n%1\n\nUse 'Load Wallet' instead.").arg(filename));
        return;
    }

    // Ask for password
    QString password = showStyledInputDialog(tr("Create Wallet"),
        tr("Enter password for new wallet (min 8 characters):"), 2);
    ScopedQStringCleanser passwordCleanser(password);

    if (password.isEmpty()) {
        return;
    }

    if (password.length() < 8) {
        showStyledMessageBox(2, tr("Create Wallet"),
            tr("Password must be at least 8 characters long."));
        return;
    }

    // Confirm password
    QString password2 = showStyledInputDialog(tr("Create Wallet"),
        tr("Confirm password:"), 2);
    ScopedQStringCleanser password2Cleanser(password2);

    if (password2.isEmpty()) {
        return;
    }

    if (password != password2) {
        showStyledMessageBox(3, tr("Create Wallet"),
            tr("Passwords do not match."));
        return;
    }

    // Create wallet
    if (walletModel->createWallet(filename, password)) {
        showStyledMessageBox(1, tr("Create Wallet"),
            tr("Wallet created successfully!\n\nAddress: %1\n\nWallet saved to: %2")
                .arg(walletModel->getAddress())
                .arg(filename));
    } else {
        showStyledMessageBox(3, tr("Create Wallet"),
            tr("Failed to create wallet."));
    }
    cleanseQString(password);
    cleanseQString(password2);
}

void GeoCoinGUI::loadWallet()
{
    if (!walletModel) {
        return;
    }

    if (walletModel->isWalletLoaded()) {
        showStyledMessageBox(1, tr("Load Wallet"),
            tr("A wallet is already loaded. Close it first."));
        return;
    }

    const QString filename = walletModel->getWalletFilePath();

    // Check if wallet exists
    if (!QFile::exists(filename)) {
        showStyledMessageBox(2, tr("Load Wallet"),
            tr("No wallet file found at:\n%1\n\nCreate a new wallet first.").arg(filename));
        return;
    }

    // An empty password is valid for legacy/unprotected wallets. Use a dialog
    // whose Accepted result is distinct from Cancel.
    QString password;
    if (!GUIUtil::getStyledText(this,
                                tr("Load Wallet"),
                                tr("Enter wallet password (leave empty if unencrypted):"),
                                password,
                                QLineEdit::Password)) {
        return;
    }
    ScopedQStringCleanser passwordCleanser(password);

    // Load wallet
    if (walletModel->loadWallet(filename, password)) {
        showStyledMessageBox(1, tr("Load Wallet"),
            tr("Wallet loaded successfully!\n\nAddress: %1\nBalance: %2")
                .arg(walletModel->getAddress())
                .arg(GUIUtil::formatAmountWithUnit(walletModel->getBalance())));
    } else {
        showStyledMessageBox(3, tr("Load Wallet"),
            tr("Failed to load wallet. Incorrect password?"));
    }
    cleanseQString(password);
}

void GeoCoinGUI::exportPrivateKey()
{
    if (!walletModel || !walletModel->isWalletLoaded()) {
        showStyledMessageBox(2, tr("Export Private Key"),
            tr("Please load or create a wallet first."));
        return;
    }

    showStyledMessageBox(1, tr("Export Private Key"),
        tr("<b>WARNING:</b> Your private key controls access to your funds!<br><br>"
           "Anyone with this key can spend your coins.<br>"
           "Keep it safe and never share it with anyone."));

    QString password;
    ScopedQStringCleanser passwordCleanser(password);
    if (walletModel->isWalletEncrypted()) {
        password = showStyledInputDialog(tr("Export Private Key"),
            tr("Enter wallet password:"), QLineEdit::Password);
        if (password.isEmpty()) {
            return;
        }
    }

    // Export private key
    QString privateKey = walletModel->exportPrivateKey(password);
    ScopedQStringCleanser privateKeyCleanser(privateKey);
    cleanseQString(password);

    if (privateKey.isEmpty()) {
        showStyledMessageBox(3, tr("Export Private Key"),
            tr("Failed to export private key. Incorrect password?"));
        return;
    }

    // Show private key
    showStyledMessageBox(1, tr("Export Private Key"),
        tr("Your private key (keep it safe!):<br><br>"
           "<b>%1</b><br><br>"
           "Write this down and store it in a secure location.").arg(privateKey));
    cleanseQString(privateKey);
}

void GeoCoinGUI::importPrivateKey()
{
    if (!walletModel) {
        return;
    }

    enum class ImportMode { Add, Replace };
    ImportMode mode = ImportMode::Replace;
    if (walletModel->isWalletLoaded()) {
        QMessageBox modeDialog(QMessageBox::Question,
                               tr("Import Private Key"),
                               tr("Choose whether to add the key or replace the current wallet."),
                               QMessageBox::NoButton,
                               this);
        QPushButton* addButton = modeDialog.addButton(tr("Add to Wallet"), QMessageBox::AcceptRole);
        QPushButton* replaceButton = modeDialog.addButton(tr("Replace Wallet"), QMessageBox::DestructiveRole);
        modeDialog.addButton(QMessageBox::Cancel);
        modeDialog.setStyleSheet(GUIUtil::dialogStyleSheet(GUIUtil::currentTheme()));
        modeDialog.exec();
        if (modeDialog.clickedButton() == addButton) {
            mode = ImportMode::Add;
        } else if (modeDialog.clickedButton() == replaceButton) {
            mode = ImportMode::Replace;
        } else {
            return;
        }
    } else if (QFileInfo::exists(walletModel->getWalletFilePath()) &&
               showStyledQuestionBox(tr("Import Private Key"),
                   tr("wallet.dat already exists. Replace it with a wallet created from this key?"))
                   != QMessageBox::Yes) {
        return;
    }

    const bool addToExisting = mode == ImportMode::Add;

    showStyledMessageBox(1, tr("Import Private Key"),
        tr("<b>IMPORTANT:</b> You are about to import a private key.<br><br>"
           "%1<br>"
           "The blockchain will be rescanned for transactions.")
            .arg(addToExisting ?
                tr("This key will be added to your existing wallet.") :
                tr("This will create a new wallet with this key.")));

    // Request private key
    QString privateKey = showStyledInputDialog(tr("Import Private Key"),
        tr("Enter private key (64 hex characters):"), QLineEdit::Password);
    ScopedQStringCleanser privateKeyCleanser(privateKey);

    if (privateKey.isEmpty()) {
        return;
    }

    QString normalizedKey;
    ScopedQStringCleanser normalizedKeyCleanser(normalizedKey);
    normalizedKey.reserve(privateKey.size());
    for (const QChar ch : privateKey) {
        if (!ch.isSpace()) normalizedKey.append(ch.toLower());
    }
    cleanseQString(privateKey);
    privateKey = normalizedKey;
    cleanseQString(normalizedKey);
    bool keyValid = privateKey.length() == 64;
    for (const QChar ch : privateKey) {
        if (!ch.isDigit() && (ch < QLatin1Char('a') || ch > QLatin1Char('f'))) {
            keyValid = false;
            break;
        }
    }
    if (!keyValid) {
        showStyledMessageBox(3, tr("Import Private Key"),
            tr("Invalid private key. Must be 64 hexadecimal characters."));
        cleanseQString(privateKey);
        return;
    }

    QString password;
    QString password2;
    ScopedQStringCleanser passwordCleanser(password);
    ScopedQStringCleanser password2Cleanser(password2);
    if (addToExisting && walletModel->isWalletEncrypted()) {
        password = showStyledInputDialog(tr("Import Private Key"),
            tr("Enter the current wallet password:"), QLineEdit::Password);
        if (password.isEmpty()) {
            cleanseQString(privateKey);
            return;
        }
    } else if (!addToExisting) {
        password = showStyledInputDialog(tr("Import Private Key"),
            tr("Enter a password for the replacement wallet:"), QLineEdit::Password);
        if (password.length() < 8) {
            showStyledMessageBox(2, tr("Import Private Key"),
                tr("Password must be at least 8 characters long."));
            cleanseQString(privateKey);
            cleanseQString(password);
            return;
        }

        password2 = showStyledInputDialog(tr("Import Private Key"),
            tr("Confirm password:"), QLineEdit::Password);

        if (password2.isEmpty()) {
            cleanseQString(privateKey);
            cleanseQString(password);
            return;
        }

        if (password != password2) {
            showStyledMessageBox(3, tr("Import Private Key"),
                tr("Passwords do not match."));
            cleanseQString(privateKey);
            cleanseQString(password);
            cleanseQString(password2);
            return;
        }
    }

    QString preImportBackup;
    if (!addToExisting) {
        if (walletModel->isWalletLoaded() && !ensureWalletSaved()) {
            cleanseQString(privateKey);
            cleanseQString(password);
            cleanseQString(password2);
            return;
        }
        const QString walletPath = walletModel->getWalletFilePath();
        if (QFileInfo::exists(walletPath)) {
            preImportBackup = walletPath + QStringLiteral(".pre-import-") +
                QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")) +
                QStringLiteral(".bak");
            if (!QFile::copy(walletPath, preImportBackup)) {
                showStyledMessageBox(3, tr("Import Private Key"),
                    tr("Could not create the safety backup:\n%1").arg(preImportBackup));
                cleanseQString(privateKey);
                cleanseQString(password);
                cleanseQString(password2);
                return;
            }
        }
        if (walletModel->isWalletLoaded()) {
            walletModel->closeWallet();
        }
    }

    // Import private key
    if (walletModel->importPrivateKey(privateKey, password, addToExisting)) {
        if (addToExisting) {
            showStyledMessageBox(1, tr("Import Private Key"),
                tr("Private key added to wallet successfully!<br><br>"
                   "Background rescan started. Transactions will appear as scan progresses."));
        } else {
            showStyledMessageBox(1, tr("Import Private Key"),
                tr("Private key imported successfully!<br><br>"
                   "Address: %1<br>"
                   "Wallet saved to: %2<br><br>"
                   "Background rescan started.")
                    .arg(walletModel->getAddress())
                    .arg(walletModel->getWalletFilePath()));
        }
    } else {
        showStyledMessageBox(3, tr("Import Private Key"),
            preImportBackup.isEmpty()
                ? tr("Failed to import private key. Incorrect password?")
                : tr("Failed to import the private key. The previous wallet backup is at:\n%1")
                    .arg(preImportBackup));
    }
    cleanseQString(privateKey);
    cleanseQString(password);
    cleanseQString(password2);
}
