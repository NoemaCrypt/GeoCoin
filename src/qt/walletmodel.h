// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_WALLETMODEL_H
#define GEOCOIN_QT_WALLETMODEL_H

#include "geocoin/types.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/node/node_components.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/utils/observer.hpp"
#include <QObject>
#include <QString>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <optional>

class OptionsModel;
class TransactionTableModel;
class AddressTableModel;

namespace geocoin {
    namespace wallet {
        class Wallet;
    }
    namespace state {
        class ChainState;
    }
    namespace network {
        class Mempool;
    }
    namespace core {
        class Transaction;
    }
}

struct CommitmentDraftInspection {
    QString operation;
    QString multisigAddress;
    QString signingHash;
    QString details;
};

/** Interface to GeoCoin wallet from Qt view code */
class WalletModel : public QObject
{
    Q_OBJECT

public:
    explicit WalletModel(QObject *parent = nullptr);
    ~WalletModel();

    enum StatusCode {
        OK,
        InvalidAmount,
        InvalidAddress,
        AmountExceedsBalance,
        AmountWithFeeExceedsBalance,
        DuplicateAddress,
        TransactionCreationFailed,
        TransactionCommitFailed,
        WalletSaveFailed,
        AbsurdFee,
        PaymentRequestExpired
    };

    TransactionTableModel *getTransactionTableModel();
    AddressTableModel *getAddressTableModel();
    OptionsModel *getOptionsModel();

    qint64 getBalance() const;
    qint64 getUnconfirmedBalance() const;
    qint64 getImmatureBalance() const;

    // Watch-only (например, чужие мультисиг адреса). Не тратится этим кошельком.
    qint64 getWatchOnlyBalance() const;
    qint64 getWatchOnlyUnconfirmedBalance() const;
    qint64 getWatchOnlyImmatureBalance() const;

    QString getAddress() const;
    double getArea() const;
    qint64 estimateFee(qint64 amount) const;
    static qint64 relayFeeForBytes(size_t serializedSize, const geocoin::network::Mempool* mempool);
    qint64 relayFeeForBytes(size_t serializedSize) const;
    static qint64 standardRelayFee(const geocoin::network::Mempool* mempool);
    qint64 standardRelayFee() const;
    static qint64 nftRelayFee(const geocoin::network::Mempool* mempool);
    qint64 nftRelayFee() const;
    static qint64 multisigRelayFee(int participants, const geocoin::network::Mempool* mempool);
    qint64 multisigRelayFee(int participants) const;

    bool isWalletLoaded() const;
    bool isWalletEncrypted() const;
    bool isWalletLocked() const;
    bool verifyPassword(const QString &password) const;

    // Send coins
    StatusCode sendCoins(const QString &address, qint64 amount, qint64 fee, const QString &password = QString());

    StatusCode createNft(const QString& name,
                         const QString& description,
                         const QString& imageUri,
                         const QString& locationName,
                         const QString& tagsCsv,
                         const geocoin::nft::NFTMetadata::Geometry& geometry,
                         const std::optional<geocoin::nft::NFTMetadata::CadastralData>& cadastral,
                         qint64 fee,
                         quint32 royaltyPercent = 0,
                         const QString& password = QString());

    StatusCode transferNft(const QString& nftIdHex,
                           const QString& toAddress,
                           qint64 fee,
                           const QString& password = QString());

    StatusCode burnNft(const QString& nftIdHex,
                       qint64 fee,
                       const QString& password = QString());

    // Wallet operations
    bool loadWallet(const QString &filename, const QString &password);
    bool createWallet(const QString &filename, const QString &password);
    void closeWallet();
    bool saveWallet(const QString& password = QString(), QString* error = nullptr);
    bool encryptWallet(const QString& password, QString* error = nullptr);
    bool changeWalletPassphrase(const QString& oldPassword,
                                const QString& newPassword,
                                QString* error = nullptr);
    bool backupWallet(const QString& filename,
                      const QString& password,
                      QString* error = nullptr) const;
    bool hasUnsavedChanges() const { return walletDirty_; }
    void markWalletDirty() { if (isWalletLoaded()) walletDirty_ = true; }

    // Import/Export private keys
    QString exportPrivateKey(const QString &password);
    bool importPrivateKey(const QString &privateKeyHex, const QString &password, bool addToExisting = false);

    /**
     * Bind the wallet and every node dependency as one coherent runtime context.
     * Partial setters are intentionally absent so the model cannot observe a
     * wallet from one runtime together with chain or mempool state from another.
     */
    bool setRuntimeContext(
        std::shared_ptr<geocoin::wallet::Wallet> wallet,
        geocoin::node::NodeComponentHandle components
    );
    void setDataDirectory(const QString& dataDir);  // Установить директорию данных

    /** Keep multisig watch/signing metadata in the wallet, not ChainState. */
    bool rememberMultisigConfig(
        const geocoin::wallet::MultisigConfig& config,
        QString* error = nullptr
    );

    QString getDataDirectory() const { return dataDirectory_; }
    QString getWalletFilePath() const { return walletFilePath_; }

    std::shared_ptr<geocoin::wallet::Wallet> getWallet() const { return wallet_; }
    std::shared_ptr<geocoin::state::ChainState> getChainState() const { return chain_state_; }
    std::shared_ptr<geocoin::network::Mempool> getMempool() const { return mempool_; }
    std::vector<geocoin::core::Transaction> pendingTransactions() const;
    bool isRescanInProgress() const;
    bool getRescanProgress(int& current, int& target) const;
    QString getHistoryScanError() const;

    /** Parse the canonical tx_hex and derive trusted signer-visible details. */
    bool inspectCommitmentDraft(
        const QString& draftJson,
        CommitmentDraftInspection& inspection,
        QString& error
    ) const;

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();

private:
    TransactionTableModel *transactionTableModel;
    AddressTableModel *addressTableModel;
    OptionsModel *optionsModel;

    std::shared_ptr<geocoin::wallet::Wallet> wallet_;
    std::shared_ptr<geocoin::state::ChainState> chain_state_;
    std::shared_ptr<geocoin::network::Mempool> mempool_;
    geocoin::node::NodeServiceHandle nodeServices_{};
    QString dataDirectory_;  // Директория данных для сохранения кошелька
    QString walletFilePath_; // Реальный путь к wallet.dat, который был загружен/создан

    qint64 cachedBalance;
    qint64 cachedUnconfirmedBalance;
    qint64 cachedImmatureBalance;

    qint64 cachedWatchOnlyBalance;
    qint64 cachedWatchOnlyUnconfirmedBalance;
    qint64 cachedWatchOnlyImmatureBalance;

    geocoin::Height cachedNumBlocks;  // Last known block height
    geocoin::Hash256 cachedBestBlockHash_; // Last observed active-chain tip hash
    bool haveCachedBestBlockHash_ = false;
    bool fForceCheckBalanceChanged;  // Flag to force balance/transaction update
    bool transactionRefreshPending_ = true;  // Coalesces expensive transaction model rebuilds
    bool transactionUiUpdateQueued_ = false; // Coalesces burst notifications into one UI refresh
    bool walletDirty_ = false;

    bool lastIbdState;  // Tracks node IBD state to trigger one-time rescan after IBD
    bool didPostIbdRescan;  // Ensures rescan runs once when IBD finishes

    // Background wallet rescan state
    mutable std::mutex rescanMutex_;
    std::thread rescanThread_;
    bool rescanInProgress_ = false;
    bool pendingRescan_ = false;
    geocoin::Height pendingRescanFrom_ = 0;
    geocoin::Height rescanTargetHeight_ = 0;
    geocoin::ScopedObserver walletTransactionChangedConnection_;
    geocoin::ScopedObserver walletScanFinishedConnection_;

    void startWalletRescanAsync(geocoin::Height fromHeight);
    void waitForRescanThread();
    void onWalletRescanFinished();
    void emitWalletStateChanged();
    void clearWalletInPlace();
    bool getAuthorizedWalletPrivateKey(const QString& address,
                                       const QString& password,
                                       geocoin::bytes& privateKey,
                                       QString& error) const;
    bool authorizeSigningPassword(const QString& password);
    StatusCode commitCreatedTransaction(
        const geocoin::core::Transaction& transaction,
        const QString& password
    );
    bool submitTransaction(const geocoin::core::Transaction& transaction);
signals:
    void balanceChanged(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance);
    void watchOnlyBalanceChanged(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance);
    void transactionCreated(const QString &txid);
    void message(const QString &title, const QString &message, unsigned int style);
    void encryptionStatusChanged(int status);
    void walletLoadedChanged(bool loaded);
    void primaryAddressChanged(const QString& address);
    void walletStateChanged();
    void walletSaveError(const QString& error);
    void walletHistoryError(const QString& error);
    void multisigDraftReady(const QString& draftJson, const QString& txHash);
    void multisigDraftFailed(const QString& error);
    void createAndSignMultisigReady(const QString& spendingHash, const QString& myCommitmentTxid, const QString& multisigSpendHex);
    void createAndSignMultisigFailed(const QString& error);
    void multisigCommitmentReady(const QString& commitmentHex, const QString& commitmentHash, const QString& broadcastTxid);
    void multisigCommitmentFailed(const QString& error);
    void multisigFinalizeReady(const QString& finalHex, const QString& broadcastTxid);
    void multisigFinalizeFailed(const QString& error);

    // Delegation proof signals
    void delegationProofDraftReady(const QString& draftJson, const QString& proofHash);
    void delegationProofDraftFailed(const QString& error);
    void createAndSignDelegationProofReady(const QString& delegationHash, const QString& myCommitmentTxid, const QString& draftJson);
    void createAndSignDelegationProofFailed(const QString& error);
    void delegationProofFinalizeReady(const QString& finalHex, const QString& broadcastTxid);
    void delegationProofFinalizeFailed(const QString& error);

    // Hierarchical spend signals
    void hierarchicalSpendDraftReady(const QString& draftJson, const QString& txHash);
    void hierarchicalSpendDraftFailed(const QString& error);
    void hierarchicalSpendFinalizeReady(const QString& finalHex, const QString& broadcastTxid);
    void hierarchicalSpendFinalizeFailed(const QString& error);

public slots:
    void updateBalance();
    void updateTransaction();  // Called when new transaction received (sets flag)
    void pollBalanceChanged();  // Вызывается таймером: проверяет необходимость обновления UI

    // Called from mining/network threads via QMetaObject::invokeMethod
    void notifyTransactionChanged(const QByteArray &txData, int height, quint64 timestamp);

    // Multisig workflow
    void createMultisigDraft(const QString& configJson,
                             const QString& toAddress,
                             qint64 amount,
                             qint64 fee = 0);
    void createAndSignMultisig(const QString& configJson,
                              const QString& toAddress,
                              qint64 amount,
                              qint64 fee,
                              const QString& privateKeyHex);
    void createAndSignMultisigWithWalletKey(const QString& configJson,
                                            const QString& toAddress,
                                            qint64 amount,
                                            qint64 fee,
                                            const QString& walletAddress,
                                            const QString& walletPassword);
    void signMultisigCommitment(const QString& draftJson,
                                const QString& keySource,
                                const QString& keyData,
                                bool broadcast);
    void finalizeMultisigSpend(const QString& draftJson,
                               const QStringList& commitmentTxids,
                               bool broadcast);

    // Delegation proof workflow
    void createDelegationProofDraft(const QString& primaryConfigJson,
                                    const QString& delegatedConfigJson,
                                    quint64 validFrom,
                                    quint64 validUntil,
                                    quint32 nonce,
                                    qint64 maxTotal,
                                    qint64 maxPerTx,
                                    int maxPercent);
    void createAndSignDelegationProof(const QString& primaryConfigJson,
                                     const QString& delegatedConfigJson,
                                     quint64 validFrom,
                                     quint64 validUntil,
                                     quint32 nonce,
                                     qint64 maxTotal,
                                     qint64 maxPerTx,
                                     int maxPercent,
                                     const QString& privateKeyHex);
    void createAndSignDelegationProofWithWalletKey(const QString& primaryConfigJson,
                                                   const QString& delegatedConfigJson,
                                                   quint64 validFrom,
                                                   quint64 validUntil,
                                                   quint32 nonce,
                                                   qint64 maxTotal,
                                                   qint64 maxPerTx,
                                                   int maxPercent,
                                                   const QString& walletAddress,
                                                   const QString& walletPassword);
    void finalizeDelegationProof(const QString& draftJson,
                                 const QStringList& commitmentTxids,
                                 bool broadcast);

    // Hierarchical spend workflow
    void createHierarchicalSpendDraft(const QString& delegationProofTxid,
                                      const QString& delegatedConfigJson,
                                      const QString& toAddress,
                                      qint64 amount,
                                      qint64 fee = 0);
    void finalizeHierarchicalSpend(const QString& draftJson,
                                   const QStringList& commitmentTxids,
                                   bool broadcast);

    // Universal commitment signing (works for all types)
    void signCommitment(const QString& draftJson,
                        const QString& keySource,
                        const QString& keyData,
                        bool broadcast);
};

#endif // GEOCOIN_QT_WALLETMODEL_H
