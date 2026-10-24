#include "miningpage_internal.h"

MiningPage::MiningPage(QWidget *parent) :
    QWidget(parent),
    clientModel(nullptr),
    walletModel(nullptr),
    buttonStartStop(nullptr),
    spinThreads(nullptr),
    labelHashrate(nullptr),
    labelBlocksFound(nullptr),
    labelMiningAddress(nullptr),
    modeTabs(nullptr),
    checkGpuCuda(nullptr),
    checkGpuHip(nullptr),
    checkGpuOpenCl(nullptr),
    listGpuDevices(nullptr),
    spinGpuIntensity(nullptr),
    buttonGpuRefreshDevices(nullptr),
    buttonGpuStartStop(nullptr),
    labelGpuHashrate(nullptr),
    labelGpuAccepted(nullptr),
    labelGpuRejected(nullptr),
    labelGpuStatus(nullptr),
    gpuDeviceProbe(nullptr),
    gpuMinerProcess(nullptr),
    miningStopPollTimer(nullptr),
    gpuRpcServer(nullptr),
    gpuRpcPort(0),
    miningEnabled(false),
    miningActionInProgress(false),
    miningThreads(1),
    currentHashrate(0.0),
    blocksFound(0),
    gpuMiningEnabled(false),
    gpuAcceptedBlocks(0),
    gpuRejectedBlocks(0),
    gpuHashrate(0.0)
{
    setupUi();
}

MiningPage::~MiningPage()
{
    waitForPendingMiningStop();
    if (gpuDeviceProbe) {
        QObject::disconnect(gpuDeviceProbe, nullptr, this, nullptr);
        stopProcessQuietly(gpuDeviceProbe, 500, 1500);
    }
    stopGpuMining();
}

void MiningPage::waitForPendingMiningStop()
{
    if (miningStopPollTimer) {
        miningStopPollTimer->stop();
    }
    miningStopAction.wait();
    miningActionInProgress = false;
}

void MiningPage::setupUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setSpacing(20);

    // Title
    QLabel *titleLabel = new QLabel(tr("Mining"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    // Mining status group
    QGroupBox *statusGroup = new QGroupBox(tr("Mining Status"), this);
    QVBoxLayout *statusLayout = new QVBoxLayout(statusGroup);

    // Hashrate
    QHBoxLayout *hashrateLayout = new QHBoxLayout();
    hashrateLayout->addWidget(new QLabel(tr("Hashrate:"), this));
    labelHashrate = new QLabel(tr("0 H/s"), this);
    labelHashrate->setStyleSheet("QLabel { font-weight: bold; color: " + GUIUtil::Theme::ColorBlue + "; }");
    hashrateLayout->addWidget(labelHashrate);
    hashrateLayout->addStretch();
    statusLayout->addLayout(hashrateLayout);

    // Blocks found
    QHBoxLayout *blocksLayout = new QHBoxLayout();
    blocksLayout->addWidget(new QLabel(tr("Blocks Found:"), this));
    labelBlocksFound = new QLabel(tr("0"), this);
    labelBlocksFound->setStyleSheet("QLabel { font-weight: bold; color: " + GUIUtil::Theme::ColorGreen + "; }");
    blocksLayout->addWidget(labelBlocksFound);
    blocksLayout->addStretch();
    statusLayout->addLayout(blocksLayout);

    // Mining address
    QHBoxLayout *addressLayout = new QHBoxLayout();
    addressLayout->addWidget(new QLabel(tr("Mining Address:"), this));
    labelMiningAddress = new QLabel(tr("No wallet loaded"), this);
    labelMiningAddress->setFont(GUIUtil::geocoinAddressFont());
    addressLayout->addWidget(labelMiningAddress);
    addressLayout->addStretch();
    statusLayout->addLayout(addressLayout);

    layout->addWidget(statusGroup);

    modeTabs = new QTabWidget(this);
    modeTabs->setObjectName(QStringLiteral("miningModeTabs"));
    modeTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (modeTabs->tabBar()) {
        modeTabs->tabBar()->setExpanding(false);
        modeTabs->tabBar()->setMinimumHeight(48);
        modeTabs->tabBar()->setMinimumWidth(340);
    }
    applyModeTabsStyle();

    QWidget *cpuTab = new QWidget(this);
    cpuTab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QVBoxLayout *controlsLayout = new QVBoxLayout(cpuTab);

    // Threads control
    QHBoxLayout *threadsLayout = new QHBoxLayout();
    threadsLayout->addWidget(new QLabel(tr("Mining Threads:"), this));
    spinThreads = new QSpinBox(this);
    spinThreads->setMinimum(1);
    const int maxThreads = cpuMiningThreadLimit();
    spinThreads->setMaximum(maxThreads);
    const int defaultThreads = maxThreads;
    QSettings settings;
    int savedThreads = settings.value(QStringLiteral("mining/threads"), defaultThreads).toInt();
    if (savedThreads < 1 || savedThreads > maxThreads) {
        savedThreads = defaultThreads;
    }
    spinThreads->setValue(savedThreads);
    spinThreads->setMinimumWidth(120);
    spinThreads->setMinimumHeight(34);
    spinThreads->setAlignment(Qt::AlignCenter);
    spinThreads->setStyleSheet(GUIUtil::spinBoxStyleSheet(GUIUtil::currentTheme()));
    spinThreads->setToolTip(tr("Number of CPU threads to use for GUI mining. Maximum follows the CPU thread count detected by Qt. Mining workers yield between batches so the wallet UI stays responsive."));
    connect(spinThreads, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MiningPage::threadsChanged);
    threadsLayout->addWidget(spinThreads);
    threadsLayout->addWidget(new QLabel(tr("(CPU max: %1)").arg(maxThreads), this));
    threadsLayout->addStretch();
    controlsLayout->addLayout(threadsLayout);

    // Start/Stop button
    buttonStartStop = new QPushButton(tr("Start Mining"), this);
    buttonStartStop->setMinimumHeight(40);
    buttonStartStop->setStyleSheet(
        "QPushButton { "
        "  background-color: " + GUIUtil::Theme::ColorGreen + "; "
        "  color: white; "
        "  font-weight: bold; "
        "  border-radius: 5px; "
        "} "
        "QPushButton:hover { "
        "  background-color: #45a049; "
        "}"
    );
    connect(buttonStartStop, &QPushButton::clicked, this, &MiningPage::toggleMining);
    controlsLayout->addWidget(buttonStartStop);
    controlsLayout->addStretch(1);

    modeTabs->addTab(cpuTab, tr("CPU"));

    QWidget *gpuTab = new QWidget(this);
    gpuTab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QVBoxLayout *gpuLayout = new QVBoxLayout(gpuTab);

    QFormLayout *gpuForm = new QFormLayout();
    gpuForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    gpuForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    gpuForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    gpuForm->setHorizontalSpacing(18);
    gpuForm->setVerticalSpacing(14);
    QHBoxLayout *gpuBackendLayout = new QHBoxLayout();
    gpuBackendLayout->setContentsMargins(0, 0, 0, 0);
    gpuBackendLayout->setSpacing(10);
    checkGpuCuda = new QCheckBox(tr("NVIDIA CUDA"), this);
    checkGpuHip = new QCheckBox(tr("AMD HIP"), this);
    checkGpuOpenCl = new QCheckBox(tr("OpenCL"), this);
    checkGpuCuda->setCursor(Qt::PointingHandCursor);
    checkGpuHip->setCursor(Qt::PointingHandCursor);
    checkGpuOpenCl->setCursor(Qt::PointingHandCursor);
    const QString backendButtonStyle = gpuBackendButtonStyleSheet();
    checkGpuCuda->setStyleSheet(backendButtonStyle);
    checkGpuHip->setStyleSheet(backendButtonStyle);
    checkGpuOpenCl->setStyleSheet(backendButtonStyle);
    checkGpuCuda->setChecked(true);
    checkGpuCuda->setProperty("backendAvailable", false);
    checkGpuHip->setProperty("backendAvailable", false);
    checkGpuOpenCl->setProperty("backendAvailable", false);
    connect(checkGpuCuda, &QCheckBox::toggled, this, &MiningPage::gpuBackendToggled);
    connect(checkGpuHip, &QCheckBox::toggled, this, &MiningPage::gpuBackendToggled);
    connect(checkGpuOpenCl, &QCheckBox::toggled, this, &MiningPage::gpuBackendToggled);
    gpuBackendLayout->addWidget(checkGpuCuda);
    gpuBackendLayout->addWidget(checkGpuHip);
    gpuBackendLayout->addWidget(checkGpuOpenCl);
    gpuBackendLayout->addStretch();
    gpuForm->addRow(tr("Backend:"), gpuBackendLayout);

    spinGpuIntensity = new QSpinBox(this);
    spinGpuIntensity->setMinimum(1024);
    spinGpuIntensity->setMaximum(2147483647);
    spinGpuIntensity->setSingleStep(67108864);
    spinGpuIntensity->setValue(1073741824);
    spinGpuIntensity->setMinimumWidth(120);
    spinGpuIntensity->setMinimumHeight(34);
    spinGpuIntensity->setAlignment(Qt::AlignCenter);
    spinGpuIntensity->setStyleSheet(GUIUtil::spinBoxStyleSheet(GUIUtil::currentTheme()));
    spinGpuIntensity->setToolTip(tr("GPU nonce attempts per kernel launch. Higher values usually improve hashrate, but status updates become less frequent."));
    gpuForm->addRow(tr("Intensity:"), spinGpuIntensity);
    gpuLayout->addLayout(gpuForm);

    listGpuDevices = new QListWidget(this);
    listGpuDevices->setMinimumHeight(110);
    listGpuDevices->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    listGpuDevices->setAlternatingRowColors(false);
    listGpuDevices->setStyleSheet(gpuDeviceListStyleSheet());
    QFont gpuDeviceFont = listGpuDevices->font();
    gpuDeviceFont.setBold(true);
    gpuDeviceFont.setPointSize(std::max(10, gpuDeviceFont.pointSize() + 1));
    listGpuDevices->setFont(gpuDeviceFont);
    gpuLayout->addWidget(new QLabel(tr("GPU devices:"), this));
    gpuLayout->addWidget(listGpuDevices, 2);

    QHBoxLayout *gpuButtons = new QHBoxLayout();
    buttonGpuRefreshDevices = new QPushButton(tr("Refresh GPU Devices"), this);
    connect(buttonGpuRefreshDevices, &QPushButton::clicked, this, &MiningPage::probeGpuDevices);
    gpuButtons->addWidget(buttonGpuRefreshDevices);
    gpuButtons->addStretch();
    gpuLayout->addLayout(gpuButtons);

    QGroupBox *gpuStatusGroup = new QGroupBox(tr("GPU Status"), this);
    QFormLayout *gpuStatusLayout = new QFormLayout(gpuStatusGroup);
    gpuStatusLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    gpuStatusLayout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    gpuStatusLayout->setHorizontalSpacing(16);
    gpuStatusLayout->setVerticalSpacing(8);
    labelGpuHashrate = new QLabel(tr("0 H/s"), this);
    labelGpuAccepted = new QLabel(tr("0"), this);
    labelGpuRejected = new QLabel(tr("0"), this);
    labelGpuStatus = new QLabel(tr("Not running"), this);
    labelGpuStatus->setWordWrap(true);
    gpuStatusLayout->addRow(tr("GPU Hashrate:"), labelGpuHashrate);
    gpuStatusLayout->addRow(tr("Accepted Blocks:"), labelGpuAccepted);
    gpuStatusLayout->addRow(tr("Rejected Blocks:"), labelGpuRejected);
    gpuStatusLayout->addRow(tr("Status:"), labelGpuStatus);
    gpuLayout->addWidget(gpuStatusGroup, 1);

    buttonGpuStartStop = new QPushButton(tr("Start GPU Mining"), this);
    buttonGpuStartStop->setMinimumHeight(40);
    connect(buttonGpuStartStop, &QPushButton::clicked, this, &MiningPage::toggleGpuMining);
    gpuLayout->addWidget(buttonGpuStartStop);
    modeTabs->addTab(gpuTab, tr("GPU"));

    layout->addWidget(modeTabs, 1);

    // Info text
    QLabel *infoLabel = new QLabel(
        tr("<b>Note:</b> Mining uses CPU resources. "
           "GeoCoin uses a geographic proof-of-work (GeoPoW) "
           "that is designed for CPU mining. "
           "The wallet can use all detected CPU threads while workers yield between batches to keep the interface responsive. "
           "Typical hashrate: 1-10 KH/s per thread."),
        this
    );
    infoLabel->setWordWrap(true);
    infoLabel->setObjectName(QStringLiteral("miningNoteLabel"));
    infoLabel->setStyleSheet(GUIUtil::warningLabelStyleSheet(GUIUtil::currentTheme()));
    layout->addWidget(infoLabel);

    gpuDeviceProbe = new QProcess(this);
    miningStopPollTimer = new QTimer(this);
    miningStopPollTimer->setInterval(50);
    miningStopPollTimer->setTimerType(Qt::CoarseTimer);
    connect(miningStopPollTimer, &QTimer::timeout,
            this, &MiningPage::pollMiningStop);
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &MiningPage::stopGpuMining, Qt::DirectConnection);
    }
    connect(gpuDeviceProbe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MiningPage::gpuProbeFinished);
    createGpuMinerProcess();
    applyTheme();
    setGpuMiningUi(false);
    probeGpuDevices();
}

void MiningPage::applyTheme()
{
    const QString theme = GUIUtil::currentTheme();
    setStyleSheet(QStringLiteral("MiningPage { background-color: transparent; }")
                  + GUIUtil::glassPanelStyleSheet(theme)
                  + GUIUtil::glassInputStyleSheet(theme)
                  + GUIUtil::glassListStyleSheet(theme)
                  + GUIUtil::spinBoxStyleSheet(theme));
    if (spinThreads) {
        spinThreads->setStyleSheet(GUIUtil::spinBoxStyleSheet(theme));
    }
    if (spinGpuIntensity) {
        spinGpuIntensity->setStyleSheet(GUIUtil::spinBoxStyleSheet(theme));
    }
    const QString backendButtonStyle = gpuBackendButtonStyleSheet();
    if (checkGpuCuda) {
        checkGpuCuda->setStyleSheet(backendButtonStyle);
    }
    if (checkGpuHip) {
        checkGpuHip->setStyleSheet(backendButtonStyle);
    }
    if (checkGpuOpenCl) {
        checkGpuOpenCl->setStyleSheet(backendButtonStyle);
    }
    if (listGpuDevices) {
        listGpuDevices->setStyleSheet(gpuDeviceListStyleSheet());
    }
    applyModeTabsStyle();
    const auto notes = findChildren<QLabel*>(QStringLiteral("miningNoteLabel"));
    for (QLabel* note : notes) {
        if (note) {
            note->setStyleSheet(GUIUtil::warningLabelStyleSheet(theme));
        }
    }
    setGpuMiningUi(gpuMiningEnabled);
}

void MiningPage::applyModeTabsStyle()
{
    if (!modeTabs) {
        return;
    }
    const QString tabStyle = QStringLiteral(
        "QTabWidget#miningModeTabs::pane {"
        "  border: 1px solid rgba(214, 168, 79, 166);"
        "  border-radius: 10px;"
        "}"
        "QTabWidget#miningModeTabs QTabBar::tab {"
        "  min-width: 150px;"
        "  min-height: 38px;"
        "  padding-left: 12px;"
        "  padding-right: 12px;"
        "  font-weight: 800;"
        "}"
        "QTabBar::tab {"
        "  min-width: 150px;"
        "  min-height: 38px;"
        "  padding-left: 12px;"
        "  padding-right: 12px;"
        "  font-weight: 800;"
        "}"
    );
    modeTabs->setStyleSheet(tabStyle);
    if (modeTabs->tabBar()) {
        modeTabs->tabBar()->setMinimumHeight(48);
        modeTabs->tabBar()->setMinimumWidth(340);
        modeTabs->tabBar()->setStyleSheet(tabStyle);
    }
}

void MiningPage::setClientModel(ClientModel *model)
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
        connect(model, &ClientModel::miningChanged,
                this, &MiningPage::updateMiningInfo, Qt::UniqueConnection);

        // Initial update
        updateMiningInfo(
            model->getMiningEnabled(),
            model->getMiningThreads(),
            model->getHashrate(),
            model->getMinedBlocksAccepted()
        );
    }
}

void MiningPage::setWalletModel(WalletModel *model)
{
    if (walletModel && walletModel != model) {
        disconnect(walletModel, nullptr, this, nullptr);
    }
    this->walletModel = model;
    if (walletModel) {
        connect(walletModel, &WalletModel::walletStateChanged,
                this, &MiningPage::refreshWalletState,
                Qt::UniqueConnection);
    }
    refreshWalletState();
}

void MiningPage::refreshWalletState()
{
    if (walletModel && walletModel->isWalletLoaded()) {
        QString address = walletModel->getAddress();
        labelMiningAddress->setText(address);
        labelMiningAddress->setStyleSheet("");
        buttonStartStop->setEnabled(!miningActionInProgress);
        buttonGpuStartStop->setEnabled(true);
    } else {
        labelMiningAddress->setText(tr("No wallet loaded"));
        labelMiningAddress->setStyleSheet("color: " + GUIUtil::Theme::ColorRed);
        buttonStartStop->setEnabled(false);
        buttonGpuStartStop->setEnabled(false);
    }
}

void MiningPage::updateMiningInfo(bool enabled, int threads, double hashrate, quint64 minedBlocksAccepted)
{
    miningEnabled = enabled;
    blocksFound = static_cast<int>(minedBlocksAccepted);
    if (!enabled && !miningStopAction.isRunning()) {
        miningActionInProgress = false;
    }

    // Update threads only if changed
    if (threads > 0 && threads != miningThreads) {
        miningThreads = threads;
        if (!enabled) {
            spinThreads->setValue(threads);
        }
    }

    currentHashrate = hashrate;

    // Update UI
    if (enabled) {
        buttonStartStop->setText(tr("Stop Mining"));
        buttonStartStop->setStyleSheet(
            "QPushButton { "
            "  background-color: " + GUIUtil::Theme::ColorRed + "; "
            "  color: white; "
            "  font-weight: bold; "
            "  border-radius: 5px; "
            "} "
            "QPushButton:hover { "
            "  background-color: #d32f2f; "
            "}"
        );
        spinThreads->setEnabled(false);

    } else {
        buttonStartStop->setText(tr("Start Mining"));
        buttonStartStop->setStyleSheet(
            "QPushButton { "
            "  background-color: " + GUIUtil::Theme::ColorGreen + "; "
            "  color: white; "
            "  font-weight: bold; "
            "  border-radius: 5px; "
            "} "
            "QPushButton:hover { "
            "  background-color: #45a049; "
            "}"
        );
        spinThreads->setEnabled(true);
    }
    buttonStartStop->setEnabled(!miningActionInProgress);
    updateOverallMiningDisplay();
}

void MiningPage::updateOverallMiningDisplay()
{
    const bool gpuActive = isGpuMiningActive();
    const bool anyMiningActive = miningEnabled || gpuActive;
    const double totalHashrate = (miningEnabled ? currentHashrate : 0.0) +
                                 (gpuActive ? gpuHashrate : 0.0);

    if (labelHashrate) {
        if (anyMiningActive) {
            labelHashrate->setText(GUIUtil::formatHashrate(totalHashrate));
            labelHashrate->setStyleSheet("QLabel { font-weight: bold; color: " + GUIUtil::Theme::ColorGreen + "; }");
        } else {
            labelHashrate->setText(tr("0 H/s"));
            labelHashrate->setStyleSheet("QLabel { font-weight: bold; color: " + GUIUtil::Theme::ColorBlue + "; }");
        }
    }
    if (labelBlocksFound) {
        labelBlocksFound->setText(QString::number(blocksFound + gpuAcceptedBlocks));
    }
    emit gpuMiningChanged(gpuActive, gpuActive ? gpuHashrate : 0.0, gpuAcceptedBlocks, gpuRejectedBlocks);
}

void MiningPage::showStyledMessageBox(int icon, const QString &title, const QString &text)
{
    GUIUtil::showStyledMessageBox(this, static_cast<QMessageBox::Icon>(icon), title, text);
}

void MiningPage::toggleMining()
{
    if (miningActionInProgress) {
        return;
    }

    if (!walletModel || !walletModel->isWalletLoaded()) {
        showStyledMessageBox(2, tr("Mining"),
            tr("Please load or create a wallet before mining."));
        return;
    }

    if (!clientModel) {
        showStyledMessageBox(2, tr("Mining"), tr("Client model not initialized"));
        return;
    }

    auto mining = clientModel->getMiningControl();
    if (!mining) {
        showStyledMessageBox(3, tr("Mining"), tr("Node not initialized"));
        return;
    }

    if (miningEnabled) {
        // Joining CPU workers can block the UI, so use one owned background
        // action. The page and application shutdown both wait for it.
        miningActionInProgress = true;
        buttonStartStop->setEnabled(false);
        spinThreads->setEnabled(false);
        if (!miningStopAction.start([mining]() {
            mining->stop_mining();
        })) {
            miningActionInProgress = false;
            buttonStartStop->setEnabled(true);
            showStyledMessageBox(3, tr("Mining"), tr("Failed to schedule mining shutdown."));
            return;
        }
        miningStopPollTimer->start();
    } else {
        // Start mining
        int threads = spinThreads->value();
        geocoin::Address mining_address(walletModel->getAddress().toStdString());

        if (!mining->start_mining(threads, mining_address)) {
            showStyledMessageBox(3, tr("Mining"),
                tr("Failed to start mining. Check that wallet is loaded and node is running."));
        }
    }
}

void MiningPage::pollMiningStop()
{
    if (!miningStopAction.reapIfFinished()) {
        return;
    }
    miningStopPollTimer->stop();
    miningActionInProgress = false;

    if (clientModel) {
        updateMiningInfo(
            clientModel->getMiningEnabled(),
            clientModel->getMiningThreads(),
            clientModel->getHashrate(),
            clientModel->getMinedBlocksAccepted());
    } else {
        buttonStartStop->setEnabled(true);
        spinThreads->setEnabled(true);
    }
}

void MiningPage::threadsChanged(int threads)
{
    // Threads can only be changed when mining is stopped
    // When mining is active, spinThreads is disabled
    if (!miningEnabled) {
        miningThreads = threads;
        QSettings settings;
        settings.setValue(QStringLiteral("mining/threads"), threads);
    }
}
