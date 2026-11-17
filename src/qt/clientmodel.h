// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_CLIENTMODEL_H
#define GEOCOIN_QT_CLIENTMODEL_H

#include "geocoin/node/node_components.hpp"

#include <QObject>
#include <memory>
#include <cstdint>

class OptionsModel;

/** Model for GeoCoin network client */
class ClientModel : public QObject
{
    Q_OBJECT

public:
    explicit ClientModel(QObject *parent = nullptr);
    ~ClientModel();

    OptionsModel *getOptionsModel();

    int getNumConnections() const;
    int getNumBlocks() const;
    double getVerificationProgress() const;

    bool inInitialBlockDownload() const;
    QString getStatusBarWarnings() const;

    // Mining
    bool getMiningEnabled() const;
    int getMiningThreads() const;
    double getHashrate() const;
    quint64 getMinedBlocksAccepted() const;
    geocoin::node::NodeStatus getCachedStatus() const { return status_; }
    QString getMiningDiagnostics() const { return miningDiagnostics_; }
    void updateStatus(const geocoin::node::NodeStatus& status);

    std::shared_ptr<geocoin::node::MiningControlService> getMiningControl() const;
    geocoin::node::NodeComponentHandle getNodeComponents() const;
    /** Install one coherent node-runtime snapshot and derive all service ports from it. */
    void setNodeComponents(geocoin::node::NodeComponentHandle components);

private:
    OptionsModel *optionsModel;
    geocoin::node::NodeComponentHandle nodeComponents_;
    geocoin::node::NodeStatus status_{};
    QString miningDiagnostics_;
    QString lastMiningError_;

    int cachedNumBlocks;
    int cachedNumConnections = -1;
    bool cachedMiningEnabled = false;
    int cachedMiningThreads = -1;
    double cachedHashrate = -1.0;
    uint64_t cachedMinedBlocksAccepted = 0;

signals:
    void numConnectionsChanged(int count);
    void numBlocksChanged(int count);
    void miningChanged(bool enabled, int threads, double hashrate, quint64 minedBlocksAccepted);
    void message(const QString &title, const QString &message, unsigned int style);
    void miningDiagnosticsChanged(const QString& diagnostics);

public slots:
    void updateNumConnections(int numConnections);
    void updateNumBlocks(int numBlocks);
    void updateMining(bool enabled, int threads, double hashrate, quint64 minedBlocksAccepted);
};

#endif // GEOCOIN_QT_CLIENTMODEL_H
