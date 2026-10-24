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
    const auto& nodeInfo = nodeComponents_.services.info;
    if (!nodeInfo) return 0;

    auto status = nodeInfo->get_status();
    return status.authenticated_peers;
}

int ClientModel::getNumBlocks() const
{
    const auto& nodeInfo = nodeComponents_.services.info;
    if (!nodeInfo) return 0;

    auto status = nodeInfo->get_status();
    return status.current_height;
}

double ClientModel::getVerificationProgress() const
{
    const auto& nodeInfo = nodeComponents_.services.info;
    if (!nodeInfo) return 0.0;

    auto status = nodeInfo->get_status();
    if (status.target_height == 0) return 1.0;

    return (double)status.current_height / (double)status.target_height;
}

bool ClientModel::inInitialBlockDownload() const
{
    const auto& nodeInfo = nodeComponents_.services.info;
    if (!nodeInfo) return false;

    auto status = nodeInfo->get_status();
    return status.syncing;
}

QString ClientModel::getStatusBarWarnings() const
{
    const auto& nodeInfo = nodeComponents_.services.info;
    if (!nodeInfo) return tr("Node not initialized");

    auto status = nodeInfo->get_status();

    if (!status.running) {
        return tr("Node not running");
    }

    if (status.syncing) {
        return tr("Synchronizing with network...");
    }

    if (status.connected_peers == 0) {
        return tr("No connections");
    }

    return QString();
}

bool ClientModel::getMiningEnabled() const
{
    const auto& mining = nodeComponents_.services.mining;
    return mining && mining->is_mining();
}

int ClientModel::getMiningThreads() const
{
    const auto& nodeInfo = nodeComponents_.services.info;
    if (!nodeInfo) return 0;

    auto status = nodeInfo->get_status();
    return status.mining_threads;
}

double ClientModel::getHashrate() const
{
    const auto& nodeInfo = nodeComponents_.services.info;
    if (!nodeInfo) return 0.0;

    auto status = nodeInfo->get_status();
    return status.hashrate;
}

quint64 ClientModel::getMinedBlocksAccepted() const
{
    const auto& nodeInfo = nodeComponents_.services.info;
    if (!nodeInfo) return 0;

    auto status = nodeInfo->get_status();
    return status.mined_blocks_accepted;
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
    const auto& nodeInfo = nodeComponents_.services.info;
    if (!nodeInfo) {
        return;
    }

    const auto status = nodeInfo->get_status();
    cachedNumBlocks = status.current_height;
    cachedNumConnections = status.authenticated_peers;
    cachedMiningEnabled = status.mining;
    cachedMiningThreads = status.mining_threads;
    cachedHashrate = status.hashrate;
    cachedMinedBlocksAccepted = status.mined_blocks_accepted;
    emit numBlocksChanged(cachedNumBlocks);
    emit numConnectionsChanged(cachedNumConnections);
    emit miningChanged(
        cachedMiningEnabled,
        cachedMiningThreads,
        cachedHashrate,
        cachedMinedBlocksAccepted
    );
}

void ClientModel::updateNumConnections(int numConnections)
{
    if (numConnections == cachedNumConnections) {
        return;
    }
    cachedNumConnections = numConnections;
    emit numConnectionsChanged(numConnections);
}

void ClientModel::updateNumBlocks(int numBlocks)
{
    if (numBlocks == cachedNumBlocks) {
        return;
    }
    cachedNumBlocks = numBlocks;
    emit numBlocksChanged(numBlocks);
}

void ClientModel::updateMining(bool enabled, int threads, double hashrate, quint64 minedBlocksAccepted)
{
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
