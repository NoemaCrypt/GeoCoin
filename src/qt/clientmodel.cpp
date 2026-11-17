// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "clientmodel.h"
#include "optionsmodel.h"

#include <cmath>
#include <utility>

ClientModel::ClientModel(QObject *parent) :
    QObject(parent),
    optionsModel(nullptr),
    cachedNumBlocks(0)
{
    optionsModel = new OptionsModel(this);
}

ClientModel::~ClientModel()
{
    try {
        // Destructor cleanup
    } catch (...) {
        // Suppress all exceptions in destructor
    }
}

OptionsModel *ClientModel::getOptionsModel()
{
    return optionsModel;
}

int ClientModel::getNumConnections() const
{
    return static_cast<int>(status_.authenticated_peers);
}

int ClientModel::getNumBlocks() const
{
    return static_cast<int>(status_.current_height);
}

double ClientModel::getVerificationProgress() const
{
    return status_.target_height == 0 ? 1.0 :
        static_cast<double>(status_.current_height) / status_.target_height;
}

bool ClientModel::inInitialBlockDownload() const
{
    return status_.syncing;
}

QString ClientModel::getStatusBarWarnings() const
{
    if (!lastMiningError_.isEmpty()) return miningDiagnostics_;
    if (!nodeComponents_.services.info) return tr("Node not initialized");
    if (!status_.running) return tr("Node not running");
    if (status_.syncing) return tr("Synchronizing with network...");
    if (status_.connected_peers == 0) return tr("No connections");
    return QString();
}

bool ClientModel::getMiningEnabled() const
{
    const auto& mining = nodeComponents_.services.mining;
    return mining && mining->is_mining();
}

int ClientModel::getMiningThreads() const
{
    return static_cast<int>(status_.mining_threads);
}

double ClientModel::getHashrate() const
{
    return status_.hashrate;
}

quint64 ClientModel::getMinedBlocksAccepted() const
{
    return status_.mined_blocks_accepted;
}

std::shared_ptr<geocoin::node::MiningControlService> ClientModel::getMiningControl() const
{
    return nodeComponents_.services.mining;
}

geocoin::node::NodeComponentHandle ClientModel::getNodeComponents() const
{
    return nodeComponents_;
}

void ClientModel::setNodeComponents(geocoin::node::NodeComponentHandle components)
{
    nodeComponents_ = std::move(components);
    updateStatus({});
}

void ClientModel::updateStatus(const geocoin::node::NodeStatus& status)
{
    status_ = status;
    updateNumBlocks(static_cast<int>(status.current_height));
    updateNumConnections(static_cast<int>(status.authenticated_peers));
    updateMining(status.mining, static_cast<int>(status.mining_threads), status.hashrate, status.mined_blocks_accepted);
    const QString error = QString::fromStdString(status.mining_error);
    const QString diagnostics = tr("CPU mining: requested %1, spawned %2, active %3%4")
        .arg(static_cast<qulonglong>(status.mining_threads_requested))
        .arg(static_cast<qulonglong>(status.mining_threads_spawned))
        .arg(static_cast<qulonglong>(status.mining_threads_active))
        .arg(error.isEmpty() ? QString() : QStringLiteral("\n") + error);
    if (diagnostics != miningDiagnostics_) {
        miningDiagnostics_ = diagnostics;
        emit miningDiagnosticsChanged(diagnostics);
    }
    if (!error.isEmpty() && error != lastMiningError_) emit message(tr("CPU Mining"), diagnostics, 2);
    lastMiningError_ = error;
}

void ClientModel::updateNumConnections(int numConnections)
{
    status_.authenticated_peers = static_cast<size_t>(std::max(0, numConnections));
    if (numConnections == cachedNumConnections) {
        return;
    }
    cachedNumConnections = numConnections;
    emit numConnectionsChanged(numConnections);
}

void ClientModel::updateNumBlocks(int numBlocks)
{
    status_.current_height = static_cast<geocoin::Height>(std::max(0, numBlocks));
    if (numBlocks == cachedNumBlocks) {
        return;
    }
    cachedNumBlocks = numBlocks;
    emit numBlocksChanged(numBlocks);
}

void ClientModel::updateMining(bool enabled, int threads, double hashrate, quint64 minedBlocksAccepted)
{
    status_.mining = enabled;
    status_.mining_threads = static_cast<size_t>(std::max(0, threads));
    status_.hashrate = hashrate;
    status_.mined_blocks_accepted = minedBlocksAccepted;
    if (enabled == cachedMiningEnabled &&
        threads == cachedMiningThreads &&
        minedBlocksAccepted == cachedMinedBlocksAccepted &&
        std::abs(hashrate - cachedHashrate) < 0.01) {
        return;
    }

    cachedMiningEnabled = enabled;
    cachedMiningThreads = threads;
    cachedHashrate = hashrate;
    cachedMinedBlocksAccepted = minedBlocksAccepted;
    emit miningChanged(enabled, threads, hashrate, minedBlocksAccepted);
}
