#include "miningpage_internal.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>

struct MiningPage::RpcCleanupState {
    std::unique_ptr<geocoin::rpc::RPCServer> server;
};

bool MiningPage::isGpuMiningActive() const
{
    return gpuMiningEnabled || gpuStopRequested || gpuRpcStopAction.isRunning() ||
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
    connect(gpuMinerProcess, &QProcess::started, this, [this] {
        if (gpuStopRequested) {
            gpuMinerProcess->write("stop\n");
            return;
        }
        labelGpuStatus->setText(tr("GPU miner running"));
        recordGpuDiagnostic(QStringLiteral("Running"));
    });
    connect(gpuMinerProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!gpuMinerProcess) return;
        gpuLastError = gpuMinerProcess->errorString();
        recordGpuDiagnostic(QStringLiteral("Error"), gpuLastError);
        labelGpuStatus->setText(gpuLastError);
        if (error == QProcess::FailedToStart) finishGpuSession(-1, QProcess::NormalExit);
    });
}

void MiningPage::resetGpuMinerProcess(bool forceStop)
{
    QProcess* process = gpuMinerProcess;
    if (!process) {
        createGpuMinerProcess();
        return;
    }

    if (forceStop && process->state() != QProcess::NotRunning) {
        stopGpuMining();
        return;
    }
    QObject::disconnect(process, nullptr, this, nullptr);
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
    updateGpuBackendMinimumSizes();
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
    if (gpuRpcCleanup) {
        if (gpuRpcCleanup->server) gpuRpcCleanup->server->stop();
        gpuRpcCleanup.reset();
    }
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
    buttonGpuStartStop->setText(gpuStopRequested ? tr("Stopping...") :
                               (running ? tr("Stop GPU Mining") : tr("Start GPU Mining")));
    buttonGpuStartStop->setEnabled(!gpuStopRequested);
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
    updateGpuBackendMinimumSizes();
    updateOverallMiningDisplay();
}

void MiningPage::processGpuStatsLine(const QByteArray& line)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
    if (!doc.isObject()) {
        if (!line.trimmed().isEmpty()) {
            gpuLastError = QString::fromUtf8(line.trimmed().left(1024));
            labelGpuStatus->setText(gpuLastError);
        }
        return;
    }
    const QJsonObject obj = doc.object();
    const QString event = obj.value(QStringLiteral("event")).toString();
    if (event == QLatin1String("status")) {
        if (gpuStopRequested) return;
        gpuHashrate = obj.value(QStringLiteral("hashrate")).toDouble(gpuHashrate);
        labelGpuHashrate->setText(GUIUtil::formatHashrate(gpuHashrate));
        labelGpuStatus->setText(tr("Mining"));
        const QJsonArray devices = obj.value(QStringLiteral("devices")).toArray();
        for (const QJsonValue& value : devices) {
            const QJsonObject dev = value.toObject();
            const QString backend = dev.value(QStringLiteral("backend")).toString();
            if (backend != QLatin1String("cuda") && backend != QLatin1String("hip") && backend != QLatin1String("opencl")) continue;
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
        if (gpuStopRequested) return;
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
        gpuLastError = obj.value(QStringLiteral("message")).toString().left(1024);
        labelGpuStatus->setText(gpuLastError);
        recordGpuDiagnostic(QStringLiteral("Error"), gpuLastError);
    } else if (event == QLatin1String("state")) {
        const QString state = obj.value(QStringLiteral("name")).toString();
        const QString detail = obj.value(QStringLiteral("message")).toString().left(1024);
        if (!gpuStopRequested) labelGpuStatus->setText(detail.isEmpty() ? state : state + QStringLiteral(": ") + detail);
        if (state == QLatin1String("Retrying")) clearGpuRates();
        recordGpuDiagnostic(state, detail);
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
    if (isGpuMiningActive()) {
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
        return;
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
         << QStringLiteral("--continuous")
         << QStringLiteral("--control-stdin")
         << QStringLiteral("--stats-json")
         << QStringLiteral("--devices") << devices.join(QLatin1Char(','));

    gpuStopRequested = false;
    gpuLastError.clear();
    gpuOutputBuffer.clear();
    gpuErrorBuffer.clear();
    gpuAcceptedBlocks = 0;
    gpuRejectedBlocks = 0;
    labelGpuAccepted->setText(QStringLiteral("0"));
    labelGpuRejected->setText(QStringLiteral("0"));
    clearGpuRates();
    labelGpuHashrate->setText(tr("0 H/s"));
    labelGpuStatus->setText(tr("Starting GPU miner..."));
    createGpuMinerProcess();
    gpuMinerProcess->setProgram(miner);
    gpuMinerProcess->setArguments(args);
    setGpuMiningUi(true);
    recordGpuDiagnostic(QStringLiteral("Starting"), backend + QStringLiteral(" devices=") + devices.join(QLatin1Char(',')));
    gpuMinerProcess->start();
}

void MiningPage::stopGpuMining()
{
    if (gpuSessionFinalizing && gpuCleanupFailed) { requestGpuRpcStop(); return; }
    if (gpuStopRequested) return;
    if (!isGpuMiningActive()) return;
    gpuStopRequested = true;
    gpuStopElapsed.restart();
    gpuTerminateSent = false;
    gpuKillSent = false;
    labelGpuStatus->setText(tr("Stopping GPU miner..."));
    setGpuMiningUi(true);
    clearGpuRates();
    recordGpuDiagnostic(QStringLiteral("Stopping"), QStringLiteral("user/shutdown request"));
    if (gpuMinerProcess && gpuMinerProcess->state() != QProcess::NotRunning) {
        gpuMinerProcess->write("stop\n");
        gpuStopPollTimer->start();
    } else {
        finishGpuSession(0, QProcess::NormalExit);
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
    gpuOutputBuffer.append(process->readAllStandardOutput());
    int newline = -1;
    while ((newline = gpuOutputBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = gpuOutputBuffer.left(newline);
        gpuOutputBuffer.remove(0, newline + 1);
        processGpuStatsLine(line);
    }
    if (gpuOutputBuffer.size() > 65536) {
        gpuOutputBuffer.clear();
        gpuLastError = tr("GPU miner output exceeded the protocol limit");
        stopGpuMining();
    }
    gpuErrorBuffer.append(process->readAllStandardError());
    if (gpuErrorBuffer.size() > 4096) gpuErrorBuffer = gpuErrorBuffer.right(4096);
    if (!gpuErrorBuffer.trimmed().isEmpty()) {
        gpuLastError = QString::fromUtf8(gpuErrorBuffer.trimmed().right(1024));
        if (!gpuStopRequested) labelGpuStatus->setText(gpuLastError);
    }
}

void MiningPage::gpuMinerFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess* process = qobject_cast<QProcess*>(sender());
    if (process && process != gpuMinerProcess) {
        process->deleteLater();
        return;
    }
    gpuMinerReadyRead();
    if (!gpuOutputBuffer.trimmed().isEmpty()) processGpuStatsLine(gpuOutputBuffer);
    gpuOutputBuffer.clear();
    finishGpuSession(exitCode, exitStatus);
}

void MiningPage::finishGpuSession(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (gpuSessionFinalizing) return;
    gpuSessionFinalizing = true;
    const bool requested = gpuStopRequested;
    gpuStopRequested = true;
    clearGpuRates();
    const QString result = exitStatus == QProcess::CrashExit && !requested
        ? tr("GPU miner crashed")
        : (exitCode == 0 || requested ? tr("GPU miner stopped")
                                     : tr("GPU miner stopped with exit code %1").arg(exitCode));
    labelGpuStatus->setText(gpuLastError.isEmpty() ? result : result + QStringLiteral(": ") + gpuLastError);
    recordGpuDiagnostic(requested || exitCode == 0 ? QStringLiteral("Stopped") : QStringLiteral("Error"),
                        QStringLiteral("exit=%1 crash=%2 ").arg(exitCode).arg(exitStatus == QProcess::CrashExit) + gpuLastError);
    if (gpuMinerProcess) {
        QObject::disconnect(gpuMinerProcess, nullptr, this, nullptr);
        gpuMinerProcess->deleteLater();
        gpuMinerProcess = nullptr;
    }
    if (gpuRpcServer) {
        gpuRpcCleanup = std::make_shared<RpcCleanupState>();
        gpuRpcCleanup->server = std::move(gpuRpcServer);
    }
    requestGpuRpcStop();
}

void MiningPage::requestGpuRpcStop()
{
    if (gpuRpcStopAction.isRunning()) return;
    gpuCleanupFailed = false;
    if (gpuRpcCleanup && gpuRpcCleanup->server) {
        const bool diagnostic = qApp && !qApp->property("geocoinTestDataDirectory").toString().isEmpty();
        const int creationFailures = diagnostic ? property("geocoinTestRpcWorkerStartFailures").toInt() : 0;
        if (creationFailures > 0) {
            setProperty("geocoinTestRpcWorkerStartFailures", creationFailures - 1);
            reportGpuCleanupFailure(tr("GPU RPC cleanup worker could not start [rpc_cleanup_start_failed]. Listener ownership is retained; retry Stop."));
            return;
        }
        const int stopFailures = diagnostic ? property("geocoinTestRpcStopFailures").toInt() : 0;
        if (stopFailures > 0) setProperty("geocoinTestRpcStopFailures", stopFailures - 1);
        const auto cleanup = gpuRpcCleanup;
        if (!gpuRpcStopAction.start([cleanup, stopFailures] {
            if (stopFailures > 0) throw std::runtime_error("synthetic RPC stop failure");
            cleanup->server->stop();
            cleanup->server.reset();
        })) {
            reportGpuCleanupFailure(tr("GPU RPC cleanup worker could not start [rpc_cleanup_start_failed]. Listener ownership is retained; retry Stop."));
            return;
        }
    }
    gpuStopPollTimer->start();
}

void MiningPage::reportGpuCleanupFailure(const QString& error)
{
    gpuCleanupFailed = true;
    gpuStopPollTimer->stop();
    gpuLastError = error;
    labelGpuStatus->setText(error);
    recordGpuDiagnostic(QStringLiteral("CleanupError"), error);
    if (!gpuCleanupErrorDialog) {
        gpuCleanupErrorDialog = new QMessageBox(QMessageBox::Critical, tr("GPU Cleanup Failed"), error, QMessageBox::Retry);
        gpuCleanupErrorDialog->setTextFormat(Qt::PlainText);
        gpuCleanupErrorDialog->setWindowModality(Qt::NonModal);
        gpuCleanupErrorDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(gpuCleanupErrorDialog, &QMessageBox::finished, this, [this](int) {
            gpuCleanupErrorDialog = nullptr;
            if (gpuCleanupFailed) QTimer::singleShot(0, this, &MiningPage::requestGpuRpcStop);
        });
        gpuCleanupErrorDialog->show();
    } else {
        gpuCleanupErrorDialog->setText(error);
    }
    emit gpuCleanupError(error);
}

void MiningPage::pollGpuStop()
{
    if (gpuSessionFinalizing) {
        if (gpuCleanupFailed) return;
        if (!gpuRpcStopAction.reapIfFinished()) return;
        if (gpuRpcStopAction.failed()) {
            reportGpuCleanupFailure(tr("GPU RPC cleanup failed [rpc_cleanup_failed]. Listener ownership is retained; retry Stop."));
            return;
        }
        stopPrivateGpuRpc();
        gpuStopPollTimer->stop();
        gpuStopRequested = false;
        gpuSessionFinalizing = false;
        if (gpuCleanupErrorDialog) gpuCleanupErrorDialog->close();
        setGpuMiningUi(false);
        createGpuMinerProcess();
        emit gpuMiningStopped();
        return;
    }
    if (!gpuStopRequested) {
        gpuStopPollTimer->stop();
        return;
    }
    const qint64 elapsed = gpuStopElapsed.elapsed();
    if (!gpuMinerProcess || gpuMinerProcess->state() == QProcess::NotRunning) {
        finishGpuSession(0, QProcess::NormalExit);
    } else if (elapsed >= 4000 && !gpuKillSent) {
        gpuKillSent = true;
        recordGpuDiagnostic(QStringLiteral("Stopping"), QStringLiteral("termination timeout; killing owned child"));
        gpuMinerProcess->kill();
    } else if (elapsed >= 3000 && !gpuTerminateSent && !gpuKillSent) {
        gpuTerminateSent = true;
        recordGpuDiagnostic(QStringLiteral("Stopping"), QStringLiteral("cooperative timeout; terminating owned child"));
        gpuMinerProcess->terminate();
    }
}

void MiningPage::clearGpuRates()
{
    gpuHashrate = 0.0;
    if (labelGpuHashrate) labelGpuHashrate->setText(tr("0 H/s"));
    if (listGpuDevices) {
        for (int i = 0; i < listGpuDevices->count(); ++i) {
            auto* item = listGpuDevices->item(i);
            if (item->data(RoleBaseText).isValid()) item->setText(item->data(RoleBaseText).toString());
        }
    }
    updateOverallMiningDisplay();
}

void MiningPage::recordGpuDiagnostic(const QString& event, const QString& detail)
{
    const QString directory = QDir(GUIUtil::applicationDataPath())
        .filePath(QStringLiteral("diagnostics"));
    if (!QDir().mkpath(directory)) return;
    const QString path = QDir(directory).filePath(QStringLiteral("mining.log"));
    QFile file(path);
    if (file.size() > 128 * 1024) {
        QFile::remove(path + QStringLiteral(".1"));
        if (!file.rename(path + QStringLiteral(".1"))) return;
        file.setFileName(path);
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QString safe = detail.left(1024);
    safe.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]")), QStringLiteral(" "));
    safe.replace(QRegularExpression(QStringLiteral("https?://[^ /]*@")), QStringLiteral("http://[redacted]@"));
    if (safe.contains(QRegularExpression(QStringLiteral("password|passphrase|private.?key|secret|seed|mnemonic"),
                                        QRegularExpression::CaseInsensitiveOption))) safe = QStringLiteral("[sensitive diagnostic omitted]");
    file.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + QLatin1Char(' ') +
                selectedGpuBackend() + QLatin1Char(' ') + event.left(32) + QLatin1Char(' ') + safe + QLatin1Char('\n')).toUtf8());
}

bool MiningPage::runProcessLifecycleChecks(const QString& fixture, QString* error)
{
    if (!qApp || qApp->property("geocoinTestDataDirectory").toString().isEmpty() ||
        isGpuMiningActive() || miningEnabled || !QFile::exists(fixture)) {
        if (error) *error = QStringLiteral("GPU process checks require an isolated, idle diagnostic session");
        return false;
    }
    for (const QString& mode : {QStringLiteral("--stdin-fixture"), QStringLiteral("--stdin-fixture"),
                               QStringLiteral("--hang-fixture"), QStringLiteral("--crash-fixture"),
                               QStringLiteral("--failed-start"), QStringLiteral("--rpc-worker-failure"),
                               QStringLiteral("--rpc-stop-failure")}) {
        // Exercise the actual listener's owning lifetime, not just the child fixture.
        gpuRpcServer = std::make_unique<geocoin::rpc::RPCServer>(geocoin::rpc::RPCCommandProfile::MINING_ONLY);
        if (!gpuRpcServer->start("127.0.0.1", 0, "diagnostic-admin", "synthetic-diagnostic-password")) {
            gpuRpcServer.reset();
            if (error) *error = QStringLiteral("Cannot start isolated loopback RPC fixture");
            return false;
        }
        QEventLoop loop;
        QElapsedTimer elapsed;
        elapsed.start();
        int heartbeats = 0;
        bool timedOut = false;
        bool complete = false;
        int cleanupErrors = 0;
        bool retainedListener = false;
        const bool injectedCleanupFailure = mode.startsWith(QStringLiteral("--rpc-"));
        setProperty("geocoinTestRpcWorkerStartFailures", mode == QLatin1String("--rpc-worker-failure") ? 1 : 0);
        setProperty("geocoinTestRpcStopFailures", mode == QLatin1String("--rpc-stop-failure") ? 1 : 0);
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, &loop, [&] { ++heartbeats; });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &loop, [&] {
            timedOut = true;
            if (gpuMinerProcess && gpuMinerProcess->state() != QProcess::NotRunning) gpuMinerProcess->kill();
            loop.quit();
        });
        connect(this, &MiningPage::gpuMiningStopped, &loop, [&] { complete = true; loop.quit(); });
        connect(this, &MiningPage::gpuCleanupError, &loop, [&](const QString&) {
            ++cleanupErrors;
            retainedListener = isGpuMiningActive() && gpuRpcCleanup && gpuRpcCleanup->server &&
                gpuRpcCleanup->server->is_running();
            QTimer::singleShot(150, &loop, [this] { stopGpuMining(); });
        });
        gpuLastError.clear();
        gpuOutputBuffer.clear();
        gpuErrorBuffer.clear();
        gpuStopRequested = false;
        createGpuMinerProcess();
        setGpuMiningUi(true);
        gpuMinerProcess->setProgram(mode == QLatin1String("--failed-start") ? fixture + QStringLiteral(".missing") : fixture);
        gpuMinerProcess->setArguments({injectedCleanupFailure ? QStringLiteral("--stdin-fixture") : mode});
        gpuMinerProcess->start();
        if (mode == QLatin1String("--stdin-fixture") || mode == QLatin1String("--hang-fixture") || injectedCleanupFailure) {
            QTimer::singleShot(250, &loop, [this] { stopGpuMining(); stopGpuMining(); });
        }
        heartbeat.start();
        watchdog.start(6000);
        loop.exec();
        if (timedOut || !complete || isGpuMiningActive() || elapsed.elapsed() >= 5000 ||
            (injectedCleanupFailure && (cleanupErrors != 1 || !retainedListener || heartbeats < 25)) ||
            (mode == QLatin1String("--hang-fixture") && heartbeats < 200) ||
            (mode == QLatin1String("--crash-fixture") && !gpuLastError.contains(QStringLiteral("offline fixture crash")))) {
            if (gpuMinerProcess && gpuMinerProcess->state() != QProcess::NotRunning) {
                gpuMinerProcess->kill();
                gpuMinerProcess->waitForFinished(1000);
            }
            if (error) *error = QStringLiteral("GPU lifecycle case %1 failed (ms=%2 heartbeat=%3 complete=%4 error=%5)")
                .arg(mode).arg(elapsed.elapsed()).arg(heartbeats).arg(complete).arg(gpuLastError);
            return false;
        }
    }
    return true;
}
