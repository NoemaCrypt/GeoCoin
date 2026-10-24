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
    void setWalletPassword(const QString& password);

    // Get initialized objects (called from main thread after initialization).
    // The concrete Node remains owned by this core-thread composition root.
    geocoin::node::NodeComponentHandle getNodeComponents();
    std::shared_ptr<geocoin::wallet::Wallet> getWallet() const { return wallet; }

public slots:
    void initialize();
    void shutdown();

signals:
    void initializeResult(bool success, const QString &error);
    void shutdownResult();
    void runawayException(const QString &message);

private:
    QString dataDirectory;
    QStringList addNodes;
    QStringList connectPeers;
    QString miningSha256Backend = QStringLiteral("auto");
    QString walletPassword;
    bool noAutoConnect = false;
    std::shared_ptr<geocoin::node::Node> node;
    std::shared_ptr<geocoin::wallet::Wallet> wallet;
};

#endif // GEOCOIN_QT_GEOCOINCORE_H
