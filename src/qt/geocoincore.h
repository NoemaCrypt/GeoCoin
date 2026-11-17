// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_GEOCOINCORE_H
#define GEOCOIN_QT_GEOCOINCORE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

#include "geocoin/node/node_components.hpp"

namespace geocoin {
    namespace node {
        class Node;
    }
    namespace wallet {
        class Wallet;
    }
}

/** GeoCoin core initialization/shutdown thread */
class GeoCoinCore : public QObject
{
    Q_OBJECT

public:
    explicit GeoCoinCore(QObject *parent = nullptr);
    ~GeoCoinCore();

    void setDataDirectory(const QString &dataDir);
    void setAddNodes(const QStringList& nodes);
    void setConnectPeers(const QStringList& peers);
    void setNoAutoConnect(bool value);
    void setMiningSha256Backend(const QString& backend);
    void setRuntimeOptions(const QString& network, quint16 p2pPort, const QString& activationManifest);

    // Get initialized objects (called from main thread after initialization).
    // The concrete Node remains owned by this core-thread composition root.
    geocoin::node::NodeComponentHandle getNodeComponents();
    QString getNodeDataDirectory() const { return nodeDataDirectory_; }

public slots:
    void initialize();
    void shutdown();

signals:
    void initializeResult(bool success, const QString &error);
    void shutdownResult();
    void shutdownFailed(const QString& error);
    void runawayException(const QString &message);

private:
    QString dataDirectory;
    QString nodeDataDirectory_;
    QStringList addNodes;
    QStringList connectPeers;
    QString miningSha256Backend = QStringLiteral("auto");
    QString network_ = QStringLiteral("degree");
    quint16 p2pPort_ = 0;
    QString activationManifestPath_;
    bool noAutoConnect = false;
    std::shared_ptr<geocoin::node::Node> node;
    bool shutdownComplete_ = false;
};

#endif // GEOCOIN_QT_GEOCOINCORE_H
