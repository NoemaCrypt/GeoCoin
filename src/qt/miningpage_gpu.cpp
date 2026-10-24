#include "miningpage_internal.h"

bool MiningPage::isGpuMiningActive() const
{
    return gpuMiningEnabled ||
           (gpuMinerProcess && gpuMinerProcess->state() != QProcess::NotRunning);
}

void MiningPage::createGpuMinerProcess()
{
    if (gpuMinerProcess) {
        return;
    }
    gpuMinerProcess = new QProcess(this);
    connect(gpuMinerProcess, &QProcess::readyReadStandardOutput,
            this, &MiningPage::gpuMinerReadyRead);
    connect(gpuMinerProcess, &QProcess::readyReadStandardError,
            this, &MiningPage::gpuMinerReadyRead);
    connect(gpuMinerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MiningPage::gpuMinerFinished);
}

void MiningPage::resetGpuMinerProcess(bool forceStop)
{
    QProcess* process = gpuMinerProcess;
    if (!process) {
        createGpuMinerProcess();
        return;
    }

    QObject::disconnect(process, nullptr, this, nullptr);
    if (forceStop && process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(800)) {
            process->kill();
            process->waitForFinished(1800);
        }
    }
    process->deleteLater();
    gpuMinerProcess = nullptr;
    createGpuMinerProcess();
}

QString MiningPage::gpuMinerBinaryPath() const
{
#ifdef _WIN32
    const QString exe = QStringLiteral("geocoin-gpu-miner.exe");
#else
    const QString exe = QStringLiteral("geocoin-gpu-miner");
#endif
    const QString sameDir = QDir(QCoreApplication::applicationDirPath()).filePath(exe);
    if (QFile::exists(sameDir)) {
        return sameDir;
    }
    const QString binDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("bin/%1").arg(exe));
    if (QFile::exists(binDir)) {
        return binDir;
    }
    return sameDir;
}

QString MiningPage::selectedGpuBackend() const
{
    if (checkGpuHip && checkGpuHip->isChecked()) {
        return QStringLiteral("hip");
    }
    if (checkGpuOpenCl && checkGpuOpenCl->isChecked()) {
        return QStringLiteral("opencl");
    }
    return QStringLiteral("cuda");
}

QStringList MiningPage::selectedGpuDeviceIds() const
{
    QStringList ids;
    if (!listGpuDevices) {
        return ids;
    }
    const QString backend = selectedGpuBackend();
    const int role = backend == QLatin1String("cuda")
        ? RoleCudaId
        : (backend == QLatin1String("hip") ? RoleHipId : RoleOpenClId);
    for (int i = 0; i < listGpuDevices->count(); ++i) {
        QListWidgetItem *item = listGpuDevices->item(i);
        if (!item || item->checkState() != Qt::Checked) {
            continue;
        }
        const int id = itemIntRole(item, role);
        if (id < 0) {
            continue;
        }
        ids << QString::number(id);
    }
    return ids;
}

void MiningPage::setGpuBackendAvailability(bool hasCuda, bool hasHip, bool hasOpenCl)
{
    if (!checkGpuCuda || !checkGpuHip || !checkGpuOpenCl) {
        return;
    }

    checkGpuCuda->setProperty("backendAvailable", hasCuda);
    checkGpuHip->setProperty("backendAvailable", hasHip);
    checkGpuOpenCl->setProperty("backendAvailable", hasOpenCl);

    {
        QSignalBlocker blockCuda(checkGpuCuda);
        QSignalBlocker blockHip(checkGpuHip);
        QSignalBlocker blockOpenCl(checkGpuOpenCl);
        if (!hasCuda && checkGpuCuda->isChecked()) {
            checkGpuCuda->setChecked(false);
            checkGpuHip->setChecked(hasHip);
            checkGpuOpenCl->setChecked(!hasHip && hasOpenCl);
        }
        if (!hasHip && checkGpuHip->isChecked()) {
            checkGpuHip->setChecked(false);
            checkGpuCuda->setChecked(hasCuda);
            checkGpuOpenCl->setChecked(!hasCuda && hasOpenCl);
        }
        if (!hasOpenCl && checkGpuOpenCl->isChecked()) {
            checkGpuOpenCl->setChecked(false);
            checkGpuCuda->setChecked(hasCuda);
            checkGpuHip->setChecked(!hasCuda && hasHip);
        }
        if (!checkGpuCuda->isChecked() && !checkGpuHip->isChecked() && !checkGpuOpenCl->isChecked()) {
            if (hasCuda) {
                checkGpuCuda->setChecked(true);
            } else if (hasHip) {
                checkGpuHip->setChecked(true);
            } else if (hasOpenCl) {
                checkGpuOpenCl->setChecked(true);
            }
        }
    }

    checkGpuCuda->setEnabled(!gpuMiningEnabled && hasCuda);
    checkGpuHip->setEnabled(!gpuMiningEnabled && hasHip);
    checkGpuOpenCl->setEnabled(!gpuMiningEnabled && hasOpenCl);
    updateGpuDeviceItemsForBackend(true);
}

void MiningPage::updateGpuDeviceItemsForBackend(bool checkAvailable)
{
    if (!listGpuDevices) {
        return;
    }

    const QString backend = selectedGpuBackend();
    const int role = backend == QLatin1String("cuda")
        ? RoleCudaId
        : (backend == QLatin1String("hip") ? RoleHipId : RoleOpenClId);
    for (int i = 0; i < listGpuDevices->count(); ++i) {
        QListWidgetItem *item = listGpuDevices->item(i);
        if (!item || !item->data(RoleBaseText).isValid()) {
            continue;
        }
        const bool availableForBackend = itemIntRole(item, role) >= 0;
        Qt::ItemFlags flags = item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
        if (availableForBackend) {
            flags |= Qt::ItemIsEnabled;
            if (checkAvailable) {
                item->setCheckState(Qt::Checked);
            }
        } else {
            flags &= ~Qt::ItemIsEnabled;
            item->setCheckState(Qt::Unchecked);
        }
        item->setFlags(flags);
    }
}

bool MiningPage::startPrivateGpuRpc(QString *error)
{
    stopPrivateGpuRpc();
    if (!clientModel) {
        if (error) *error = tr("Node not initialized");
        return false;
    }

    const auto nodeComponents = clientModel->getNodeComponents();
    if (!nodeComponents.complete_for_rpc()) {
        if (error) *error = tr("Node services are not initialized");
        return false;
    }
    gpuRpcUser = QStringLiteral("geocoin-gpu");
    gpuRpcPassword = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString adminRpcUser = QStringLiteral("geocoin-gpu-admin-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString adminRpcPassword = QUuid::createUuid().toString(QUuid::WithoutBraces);
    GUIUtil::ScopedQStringCleanser adminPasswordCleanser(adminRpcPassword);
    const QString passwordTemplate = QDir(
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
    ).filePath(QStringLiteral("geocoin-gpu-rpc-XXXXXX.pass"));
    QTemporaryFile passFile(passwordTemplate);
    passFile.setAutoRemove(true);
    if (!passFile.open()) {
        if (error) *error = tr("Failed to create temporary GPU RPC password file");
        stopPrivateGpuRpc();
        return false;
    }
    if (!passFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        passFile.close();
        if (error) *error = tr("Failed to restrict temporary GPU RPC password file permissions");
        stopPrivateGpuRpc();
        return false;
    }
    QByteArray passwordBytes = gpuRpcPassword.toUtf8();
    passwordBytes.append('\n');
    const bool passwordWritten =
        passFile.write(passwordBytes) == passwordBytes.size() && passFile.flush();
    passwordBytes.fill('\0');
    passwordBytes.clear();
    if (!passwordWritten) {
        if (error) *error = tr("Failed to write temporary GPU RPC password file");
        stopPrivateGpuRpc();
        return false;
    }
    gpuRpcPasswordFile = passFile.fileName();
    passFile.setAutoRemove(false);
    passFile.close();

    const std::string adminUserBytes = adminRpcUser.toStdString();
    std::string adminPasswordBytes = adminRpcPassword.toStdString();
    const std::string minerUserBytes = gpuRpcUser.toStdString();
    std::string minerPasswordBytes = gpuRpcPassword.toStdString();
    GUIUtil::ScopedStringCleanser adminPasswordBytesCleanser(adminPasswordBytes);
    GUIUtil::ScopedStringCleanser minerPasswordBytesCleanser(minerPasswordBytes);

    for (int port = 19445; port < 19545; ++port) {
        auto server = std::make_unique<geocoin::rpc::RPCServer>(
            geocoin::rpc::RPCCommandProfile::MINING_ONLY
        );
        geocoin::rpc::RPCServerContext rpcContext;
        rpcContext.blockchain = nodeComponents.blockchain;
        rpcContext.chain_state = nodeComponents.chain_state;
        rpcContext.mempool = nodeComponents.mempool;
        rpcContext.peer_manager = nodeComponents.peer_manager;
        rpcContext.node_services = nodeComponents.services;
        rpcContext.chain_mutations = rpcContext.node_services.mutations;
        if (!server->set_context(std::move(rpcContext))) {
            stopPrivateGpuRpc();
            if (error) *error = tr("Failed to configure private GPU RPC services");
            return false;
        }
        if (server->start("127.0.0.1", static_cast<uint16_t>(port),
                          adminUserBytes, adminPasswordBytes,
                          minerUserBytes, minerPasswordBytes)) {
            gpuRpcPort = port;
            gpuRpcServer = std::move(server);
            return true;
        }
    }

    stopPrivateGpuRpc();
    if (error) *error = tr("Failed to start private GPU RPC listener on 127.0.0.1");
    return false;
}

void MiningPage::stopPrivateGpuRpc()
{
    if (gpuRpcServer) {
        gpuRpcServer->stop();
        gpuRpcServer.reset();
    }
    if (!gpuRpcPasswordFile.isEmpty()) {
        QFile::remove(gpuRpcPasswordFile);
        gpuRpcPasswordFile.clear();
    }
    GUIUtil::cleanseQString(gpuRpcPassword);
    gpuRpcUser.clear();
    gpuRpcPort = 0;
}

void MiningPage::setGpuMiningUi(bool running)
{
    gpuMiningEnabled = running;
    buttonGpuStartStop->setText(running ? tr("Stop GPU Mining") : tr("Start GPU Mining"));
    buttonGpuStartStop->setEnabled(true);
    const bool hasCuda = checkGpuCuda && checkGpuCuda->property("backendAvailable").toBool();
    const bool hasHip = checkGpuHip && checkGpuHip->property("backendAvailable").toBool();
    const bool hasOpenCl = checkGpuOpenCl && checkGpuOpenCl->property("backendAvailable").toBool();
    if (checkGpuCuda) {
        checkGpuCuda->setEnabled(!running && hasCuda);
    }
    if (checkGpuHip) {
        checkGpuHip->setEnabled(!running && hasHip);
    }
    if (checkGpuOpenCl) {
        checkGpuOpenCl->setEnabled(!running && hasOpenCl);
    }
    listGpuDevices->setEnabled(!running);
    spinGpuIntensity->setEnabled(!running);
    buttonGpuRefreshDevices->setEnabled(!running);
    buttonGpuStartStop->setStyleSheet(
        running
            ? "QPushButton { background-color: " + GUIUtil::Theme::ColorRed + "; color: white; font-weight: bold; border-radius: 8px; min-height: 36px; }"
            : "QPushButton { background-color: " + GUIUtil::Theme::ColorGreen + "; color: white; font-weight: bold; border-radius: 8px; min-height: 36px; }"
    );
    updateOverallMiningDisplay();
}

void MiningPage::processGpuStatsLine(const QByteArray& line)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
    if (!doc.isObject()) {
        if (!line.trimmed().isEmpty()) {
            labelGpuStatus->setText(QString::fromUtf8(line.trimmed()));
        }
        return;
    }
    const QJsonObject obj = doc.object();
    const QString event = obj.value(QStringLiteral("event")).toString();
    if (event == QLatin1String("status")) {
        gpuHashrate = obj.value(QStringLiteral("hashrate")).toDouble(gpuHashrate);
        labelGpuHashrate->setText(GUIUtil::formatHashrate(gpuHashrate));
        labelGpuStatus->setText(tr("Mining"));
        const QJsonArray devices = obj.value(QStringLiteral("devices")).toArray();
        for (const QJsonValue& value : devices) {
            const QJsonObject dev = value.toObject();
            const QString backend = dev.value(QStringLiteral("backend")).toString();
            const int id = dev.value(QStringLiteral("id")).toInt(-1);
            const double hashrate = dev.value(QStringLiteral("hashrate")).toDouble(0.0);
            const int role = backend == QLatin1String("cuda")
                ? RoleCudaId
                : (backend == QLatin1String("hip") ? RoleHipId : RoleOpenClId);
            for (int i = 0; i < listGpuDevices->count(); ++i) {
                QListWidgetItem *item = listGpuDevices->item(i);
                if (!item || itemIntRole(item, role) != id) {
                    continue;
                }
                const QString baseText = item->data(RoleBaseText).toString();
                item->setText(QStringLiteral("%1  |  %2").arg(baseText, GUIUtil::formatHashrate(hashrate)));
                break;
            }
        }
        updateOverallMiningDisplay();
    } else if (event == QLatin1String("block_found")) {
        gpuHashrate = obj.value(QStringLiteral("hashrate")).toDouble(gpuHashrate);
        labelGpuHashrate->setText(GUIUtil::formatHashrate(gpuHashrate));
        labelGpuStatus->setText(tr("Block found, submitting..."));
        updateOverallMiningDisplay();
    } else if (event == QLatin1String("block_accepted")) {
        gpuAcceptedBlocks = obj.value(QStringLiteral("accepted")).toInt(gpuAcceptedBlocks + 1);
        labelGpuAccepted->setText(QString::number(gpuAcceptedBlocks));
        labelGpuStatus->setText(tr("Block accepted at height %1").arg(obj.value(QStringLiteral("height")).toVariant().toString()));
        updateOverallMiningDisplay();
    } else if (event == QLatin1String("block_rejected")) {
        gpuRejectedBlocks = obj.value(QStringLiteral("rejected")).toInt(gpuRejectedBlocks + 1);
        labelGpuRejected->setText(QString::number(gpuRejectedBlocks));
        labelGpuStatus->setText(tr("Block rejected"));
        updateOverallMiningDisplay();
    } else if (event == QLatin1String("error")) {
        labelGpuStatus->setText(obj.value(QStringLiteral("message")).toString());
    } else if (event == QLatin1String("start")) {
        labelGpuStatus->setText(tr("GPU miner started: %1").arg(
            obj.value(QStringLiteral("backend")).toString().toUpper()));
    } else if (event == QLatin1String("template")) {
        labelGpuStatus->setText(tr("Mining block template at height %1").arg(
            obj.value(QStringLiteral("height")).toVariant().toString()));
    }
}

void MiningPage::gpuBackendToggled(bool checked)
{
    QCheckBox *source = qobject_cast<QCheckBox*>(sender());
    if (!checkGpuCuda || !checkGpuHip || !checkGpuOpenCl || !source) {
        return;
    }

    if (checked) {
        QSignalBlocker blockCuda(checkGpuCuda);
        QSignalBlocker blockHip(checkGpuHip);
        QSignalBlocker blockOpenCl(checkGpuOpenCl);
        if (source == checkGpuCuda) {
            checkGpuCuda->setChecked(true);
            checkGpuHip->setChecked(false);
            checkGpuOpenCl->setChecked(false);
        } else if (source == checkGpuHip) {
            checkGpuCuda->setChecked(false);
            checkGpuHip->setChecked(true);
            checkGpuOpenCl->setChecked(false);
        } else if (source == checkGpuOpenCl) {
            checkGpuCuda->setChecked(false);
            checkGpuHip->setChecked(false);
            checkGpuOpenCl->setChecked(true);
        }
        updateGpuDeviceItemsForBackend(true);
        return;
    }

    if (!checkGpuCuda->isChecked() && !checkGpuHip->isChecked() && !checkGpuOpenCl->isChecked() && source->isEnabled()) {
        QSignalBlocker blockSource(source);
        source->setChecked(true);
    }
    updateGpuDeviceItemsForBackend(false);
}

void MiningPage::probeGpuDevices()
{
    const QString miner = gpuMinerBinaryPath();
    listGpuDevices->clear();
    QListWidgetItem *probing = new QListWidgetItem(tr("Detecting GPU devices..."), listGpuDevices);
    probing->setFlags(Qt::NoItemFlags);

    if (!QFile::exists(miner)) {
        listGpuDevices->clear();
        QListWidgetItem *missing = new QListWidgetItem(tr("geocoin-gpu-miner not found next to geocoin-core"), listGpuDevices);
        missing->setFlags(Qt::NoItemFlags);
        setGpuBackendAvailability(false, false, false);
        return;
    }

    if (gpuDeviceProbe->state() != QProcess::NotRunning) {
        stopProcessQuietly(gpuDeviceProbe, 500, 1500);
    }
    gpuDeviceProbe->setProgram(miner);
    gpuDeviceProbe->setArguments(QStringList() << QStringLiteral("--list-devices") << QStringLiteral("--stats-json"));
    gpuDeviceProbe->start();
}

void MiningPage::gpuProbeFinished(int, QProcess::ExitStatus)
{
    const QByteArray output = gpuDeviceProbe->readAllStandardOutput();
    listGpuDevices->clear();
    bool hasCuda = false;
    bool hasHip = false;
    bool hasOpenCl = false;
    QList<QJsonObject> cudaDevices;
    QList<QJsonObject> hipDevices;
    QList<QJsonObject> openClDevices;

    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray& raw : lines) {
        if (raw.trimmed().isEmpty()) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw.trimmed());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        if (obj.value(QStringLiteral("event")).toString() != QLatin1String("devices")) {
            continue;
        }
        const QJsonArray devices = obj.value(QStringLiteral("devices")).toArray();
        for (const QJsonValue& value : devices) {
            const QJsonObject dev = value.toObject();
            const QString backend = dev.value(QStringLiteral("backend")).toString();
            if (backend == QLatin1String("cuda")) {
                cudaDevices.append(dev);
                hasCuda = true;
            } else if (backend == QLatin1String("hip")) {
                hipDevices.append(dev);
                hasHip = true;
            } else if (backend == QLatin1String("opencl")) {
                openClDevices.append(dev);
                hasOpenCl = true;
            }
        }
    }

    QVector<bool> openClUsed(openClDevices.size(), false);
    for (const QJsonObject& cudaDevice : cudaDevices) {
        int matchedOpenCl = -1;
        for (int i = 0; i < openClDevices.size(); ++i) {
            if (!openClUsed[i] && samePhysicalGpuModel(cudaDevice, openClDevices[i])) {
                matchedOpenCl = i;
                openClUsed[i] = true;
                break;
            }
        }

        const int cudaId = cudaDevice.value(QStringLiteral("id")).toInt(-1);
        const int openClId = matchedOpenCl >= 0
            ? openClDevices[matchedOpenCl].value(QStringLiteral("id")).toInt(-1)
            : -1;
        const QString name = deviceDisplayName(cudaDevice, matchedOpenCl >= 0 ? openClDevices[matchedOpenCl] : QJsonObject());
        const QString vendor = vendorFamily(cudaDevice, matchedOpenCl >= 0 ? openClDevices[matchedOpenCl] : QJsonObject());
        const QString detail = cudaDevice.value(QStringLiteral("detail")).toString();
        const QString text = unifiedGpuText(name, cudaId, -1, openClId, vendor, detail);
        QListWidgetItem *item = new QListWidgetItem(text, listGpuDevices);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(RoleCudaId, cudaId);
        item->setData(RoleHipId, -1);
        item->setData(RoleOpenClId, openClId);
        item->setData(RoleBaseText, text);
    }

    for (const QJsonObject& hipDevice : hipDevices) {
        int matchedOpenCl = -1;
        for (int i = 0; i < openClDevices.size(); ++i) {
            if (!openClUsed[i] && samePhysicalGpuModel(hipDevice, openClDevices[i])) {
                matchedOpenCl = i;
                openClUsed[i] = true;
                break;
            }
        }

        const int hipId = hipDevice.value(QStringLiteral("id")).toInt(-1);
        const int openClId = matchedOpenCl >= 0
            ? openClDevices[matchedOpenCl].value(QStringLiteral("id")).toInt(-1)
            : -1;
        const QString name = deviceDisplayName(hipDevice, matchedOpenCl >= 0 ? openClDevices[matchedOpenCl] : QJsonObject());
        const QString vendor = vendorFamily(hipDevice, matchedOpenCl >= 0 ? openClDevices[matchedOpenCl] : QJsonObject());
        const QString detail = hipDevice.value(QStringLiteral("detail")).toString();
        const QString text = unifiedGpuText(name, -1, hipId, openClId, vendor, detail);
        QListWidgetItem *item = new QListWidgetItem(text, listGpuDevices);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(RoleCudaId, -1);
        item->setData(RoleHipId, hipId);
        item->setData(RoleOpenClId, openClId);
        item->setData(RoleBaseText, text);
    }

    for (int i = 0; i < openClDevices.size(); ++i) {
        if (openClUsed[i]) {
            continue;
        }
        const QJsonObject& openClDevice = openClDevices[i];
        const int openClId = openClDevice.value(QStringLiteral("id")).toInt(-1);
        const QString name = deviceDisplayName(openClDevice, QJsonObject());
        const QString vendor = vendorFamily(openClDevice, QJsonObject());
        const QString detail = openClDevice.value(QStringLiteral("detail")).toString();
        const QString text = unifiedGpuText(name, -1, -1, openClId, vendor, detail);
        QListWidgetItem *item = new QListWidgetItem(text, listGpuDevices);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(RoleCudaId, -1);
        item->setData(RoleHipId, -1);
        item->setData(RoleOpenClId, openClId);
        item->setData(RoleBaseText, text);
    }

    setGpuBackendAvailability(hasCuda, hasHip, hasOpenCl);

    if (listGpuDevices->count() == 0) {
        QListWidgetItem *empty = new QListWidgetItem(tr("No CUDA/HIP/OpenCL GPU devices found"), listGpuDevices);
        empty->setFlags(Qt::NoItemFlags);
    }
}

void MiningPage::toggleGpuMining()
{
    if (gpuMiningEnabled) {
        stopGpuMining();
        return;
    }

    if (!walletModel || !walletModel->isWalletLoaded()) {
        showStyledMessageBox(2, tr("GPU Mining"),
            tr("Please load or create a wallet before GPU mining."));
        return;
    }
    if (!clientModel || !clientModel->getNodeComponents().complete_for_rpc()) {
        showStyledMessageBox(3, tr("GPU Mining"), tr("Node not initialized"));
        return;
    }
    if (!gpuMinerProcess) {
        createGpuMinerProcess();
    }
    if (gpuMinerProcess && gpuMinerProcess->state() != QProcess::NotRunning) {
        labelGpuStatus->setText(tr("Stopping previous GPU miner..."));
        resetGpuMinerProcess(true);
    }

    QString error;
    if (!startPrivateGpuRpc(&error)) {
        showStyledMessageBox(3, tr("GPU Mining"), error);
        return;
    }

    const QString miner = gpuMinerBinaryPath();
    if (!QFile::exists(miner)) {
        stopPrivateGpuRpc();
        showStyledMessageBox(3, tr("GPU Mining"), tr("geocoin-gpu-miner was not found in the bundle."));
        return;
    }

    const QString backend = selectedGpuBackend();
    const QStringList devices = selectedGpuDeviceIds();
    if (devices.isEmpty()) {
        stopPrivateGpuRpc();
        showStyledMessageBox(3, tr("GPU Mining"),
            tr("No selected GPU devices support the %1 backend.").arg(backend.toUpper()));
        return;
    }

    QStringList args;
    args << QStringLiteral("--url") << QStringLiteral("http://127.0.0.1:%1/").arg(gpuRpcPort)
         << QStringLiteral("--rpc-user") << gpuRpcUser
         << QStringLiteral("--rpc-pass-file") << gpuRpcPasswordFile
         << QStringLiteral("--address") << walletModel->getAddress()
         << QStringLiteral("--backend") << backend
         << QStringLiteral("--intensity") << QString::number(spinGpuIntensity->value())
         << QStringLiteral("--blocks") << QStringLiteral("1000000")
         << QStringLiteral("--timeout") << QStringLiteral("3600")
         << QStringLiteral("--stats-json")
         << QStringLiteral("--devices") << devices.join(QLatin1Char(','));

    gpuHashrate = 0.0;
    labelGpuHashrate->setText(tr("0 H/s"));
    labelGpuStatus->setText(tr("Starting GPU miner..."));
    createGpuMinerProcess();
    gpuMinerProcess->setProgram(miner);
    gpuMinerProcess->setArguments(args);
    gpuMinerProcess->start();
    if (!gpuMinerProcess->waitForStarted(1500)) {
        const QString reason = gpuMinerProcess->errorString();
        stopPrivateGpuRpc();
        setGpuMiningUi(false);
        showStyledMessageBox(3, tr("GPU Mining"), tr("Failed to start GPU miner: %1").arg(reason));
        return;
    }
    setGpuMiningUi(true);
}

void MiningPage::stopGpuMining()
{
    const bool wasActive = isGpuMiningActive();
    if (labelGpuStatus && wasActive) {
        labelGpuStatus->setText(tr("Stopping GPU miner..."));
    }
    if (buttonGpuStartStop && wasActive) {
        buttonGpuStartStop->setEnabled(false);
    }
    stopPrivateGpuRpc();
    resetGpuMinerProcess(true);
    gpuHashrate = 0.0;
    if (labelGpuHashrate) {
        labelGpuHashrate->setText(tr("0 H/s"));
    }
    setGpuMiningUi(false);
    if (labelGpuStatus) {
        labelGpuStatus->setText(tr("Not running"));
    }
}

void MiningPage::gpuMinerReadyRead()
{
    QProcess* process = qobject_cast<QProcess*>(sender());
    if (!process) {
        process = gpuMinerProcess;
    }
    if (!process) {
        return;
    }
    while (process->canReadLine()) {
        processGpuStatsLine(process->readLine());
    }
    const QByteArray err = process->readAllStandardError();
    if (!err.trimmed().isEmpty()) {
        labelGpuStatus->setText(QString::fromUtf8(err.trimmed()));
    }
}

void MiningPage::gpuMinerFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess* process = qobject_cast<QProcess*>(sender());
    if (process) {
        while (process->canReadLine()) {
            processGpuStatsLine(process->readLine());
        }
        const QByteArray err = process->readAllStandardError();
        if (!err.trimmed().isEmpty()) {
            labelGpuStatus->setText(QString::fromUtf8(err.trimmed()));
        }
    }
    if (process && process != gpuMinerProcess) {
        process->deleteLater();
        return;
    }
    stopPrivateGpuRpc();
    gpuHashrate = 0.0;
    if (labelGpuHashrate) {
        labelGpuHashrate->setText(tr("0 H/s"));
    }
    setGpuMiningUi(false);
    if (exitStatus == QProcess::CrashExit) {
        labelGpuStatus->setText(tr("GPU miner crashed"));
    } else if (exitCode == 0) {
        labelGpuStatus->setText(tr("GPU miner stopped"));
    } else {
        labelGpuStatus->setText(tr("GPU miner stopped with exit code %1").arg(exitCode));
    }
    if (gpuMinerProcess) {
        QObject::disconnect(gpuMinerProcess, nullptr, this, nullptr);
        gpuMinerProcess->deleteLater();
        gpuMinerProcess = nullptr;
    }
    createGpuMinerProcess();
}
