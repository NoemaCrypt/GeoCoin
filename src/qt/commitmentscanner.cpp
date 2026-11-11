// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#include "commitmentscanner.h"
#include "geocoin/state/state.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/wallet/multisig_verifier.hpp"

#include <QDateTime>
#include <QHash>
#include <QMetaObject>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <map>

namespace {

constexpr geocoin::Height kDefaultInitialScanDepthBlocks =
    static_cast<geocoin::Height>((24 * 60 * 60) / geocoin::consensus::params::TARGET_BLOCK_TIME);

bool parseHash256(const QString& text, geocoin::Hash256& hash)
{
    if (text.size() != 64) return false;
    for (int i = 0; i < 32; ++i) {
        bool ok = false;
        const uint value = text.mid(i * 2, 2).toUInt(&ok, 16);
        if (!ok || value > 0xffU) return false;
        hash[static_cast<size_t>(i)] = static_cast<geocoin::byte>(value);
    }
    return true;
}

QString hashToString(const geocoin::Hash256& hash)
{
    QString result;
    result.reserve(64);
    for (const auto byte : hash) {
        result += QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0'));
    }
    return result;
}

bool isValidHistoricalCommitment(const geocoin::core::Transaction& tx)
{
    // Admission validation deliberately rejects a transaction that is already
    // indexed. Use the context-free commitment verifier for active-chain data.
    const geocoin::wallet::MultisigVerifier verifier(nullptr);
    return verifier.verify_angle_commitment(tx);
}

} // namespace

bool CommitmentScanner::anchorRequiresRescan(geocoin::Height lastScannedHeight,
                                              bool hasAnchor,
                                              const geocoin::Hash256& anchorHash,
                                              geocoin::Height currentHeight,
                                              bool activeAnchorAvailable,
                                              const geocoin::Hash256& activeAnchorHash)
{
    if (!hasAnchor) return false;
    if (currentHeight < lastScannedHeight) return true;
    return !activeAnchorAvailable || activeAnchorHash != anchorHash;
}

bool CommitmentScanner::signerCanBeCollected(const QString& signerAddress,
                                               const QSet<QString>& allowedSigners,
                                               const QSet<QString>& collectedSigners)
{
    return !signerAddress.isEmpty() &&
        !allowedSigners.isEmpty() &&
        allowedSigners.contains(signerAddress) &&
        !collectedSigners.contains(signerAddress);
}

bool CommitmentScanner::storedCommitmentRemainsActive(
    geocoin::Height commitmentHeight,
    const geocoin::Hash256& commitmentBlockHash,
    geocoin::Height currentHeight,
    bool activeBlockAvailable,
    const geocoin::Hash256& activeBlockHash)
{
    return commitmentHeight <= currentHeight && activeBlockAvailable &&
        commitmentBlockHash == activeBlockHash;
}

bool CommitmentScanner::isCommitmentActive(
    const QString& txid,
    const QString& targetHash,
    const QString& multisigAddress,
    const std::shared_ptr<geocoin::state::ChainState>& chainState,
    const QStringList& allowedSignerAddresses,
    QString* signerAddressOut)
{
    if (!chainState) return false;
    geocoin::Hash256 txHash{};
    geocoin::Hash256 target{};
    if (!parseHash256(txid, txHash) || !parseHash256(targetHash, target)) return false;
    const auto tx = chainState->get_transaction(txHash);
    if (!tx ||
        tx->type != geocoin::core::TransactionType::ANGLE_COMMITMENT ||
        tx->spending_tx_hash != target ||
        QString::fromStdString(tx->multisig_address.str()) != multisigAddress ||
        !isValidHistoricalCommitment(*tx)) {
        return false;
    }

    QSet<QString> allowedSigners;
    for (const QString& address : allowedSignerAddresses) {
        const QString normalized = address.trimmed();
        if (!normalized.isEmpty()) allowedSigners.insert(normalized);
    }
    const QString signerAddress = QString::fromStdString(tx->signer_address.str());
    if (allowedSigners.isEmpty() || !allowedSigners.contains(signerAddress)) return false;
    if (signerAddressOut) *signerAddressOut = signerAddress;
    return true;
}

class CommitmentScannerWorker : public QObject
{
    Q_OBJECT

public:
    CommitmentScannerWorker(QString targetHash,
                            QString multisigAddress,
                            int requiredM,
                            geocoin::Height initialScanDepthBlocks,
                            std::shared_ptr<geocoin::state::ChainState> chainState,
                            QStringList allowedSignerAddresses,
                            QObject* parent = nullptr)
        : QObject(parent),
          timer_(nullptr),
          targetHash_(std::move(targetHash)),
          multisigAddress_(std::move(multisigAddress)),
          requiredM_(requiredM),
          initialScanDepthBlocks_(initialScanDepthBlocks),
          chainState_(std::move(chainState)),
          lastScannedHeight_(0),
          scanCount_(0),
          scanning_(false),
          hasAnchor_(false),
          allCollectedEmitted_(false)
    {
        for (const QString& signer : allowedSignerAddresses) {
            const QString normalized = signer.trimmed();
            if (!normalized.isEmpty()) allowedSigners_.insert(normalized);
        }
        // The caller must provide the participants from its wallet/config.
        // ChainState is consensus data and must not be used as a mutable GUI
        // metadata registry.
    }

public slots:
    void startAutoScan(int intervalSeconds)
    {
        if (!chainState_) {
            emit scanError(tr("Chain state is not available"));
            return;
        }

        if (timer_) {
            timer_->stop();
            timer_->deleteLater();
            timer_ = nullptr;
        }

        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, &CommitmentScannerWorker::performScan);
        timer_->start(std::max(1, intervalSeconds) * 1000);

        scanning_ = true;
        emit scanningStateChanged(scanning_);
        emit statusUpdate(tr("🔄 Autoscan started (interval: %1 sec)").arg(std::max(1, intervalSeconds)));

        performScan();
    }

    void stopAutoScan()
    {
        if (timer_) {
            timer_->stop();
            timer_->deleteLater();
            timer_ = nullptr;
        }
        if (scanning_) {
            scanning_ = false;
            emit scanningStateChanged(scanning_);
            emit statusUpdate(tr("⏸️ Autoscan stopped"));
        }
    }

    void scanNow()
    {
        performScan();
    }

signals:
    void commitmentFound(const QString& txid, const QString& signerAddress);
    void commitmentInvalidated(const QString& txid, const QString& signerAddress);
    void allCommitmentsCollected();
    void scanError(const QString& error);
    void statusUpdate(const QString& message);
    void scanProgress(qulonglong lastScannedHeight, int scanCount);
    void scanningStateChanged(bool scanning);

private:
    struct FoundCommitment {
        geocoin::Height height = 0;
        geocoin::Hash256 blockHash{};
        QString signerAddress;
    };

    void invalidateOrphanedCommitments(geocoin::Height currentHeight)
    {
        for (auto it = foundCommitments_.begin(); it != foundCommitments_.end();) {
            geocoin::Hash256 activeBlockHash{};
            bool activeBlockAvailable = false;
            if (it->height <= currentHeight) {
                geocoin::core::Block block;
                activeBlockAvailable = chainState_->get_block_by_height(it->height, block);
                if (activeBlockAvailable) activeBlockHash = block.get_hash();
            }
            const bool active = CommitmentScanner::storedCommitmentRemainsActive(
                it->height, it->blockHash, currentHeight,
                activeBlockAvailable, activeBlockHash);
            if (!active) {
                const QString txid = it.key();
                const QString signer = it->signerAddress;
                collectedSigners_.remove(signer);
                it = foundCommitments_.erase(it);
                emit commitmentInvalidated(txid, signer);
            } else {
                ++it;
            }
        }
        if (foundCommitments_.size() < requiredM_) {
            allCollectedEmitted_ = false;
        }
    }

    geocoin::Height reorgRescanStart(geocoin::Height currentHeight)
    {
        const geocoin::Height fallback = currentHeight > initialScanDepthBlocks_
            ? currentHeight - initialScanDepthBlocks_
            : 0;

        for (auto it = recentBlockHashes_.rbegin(); it != recentBlockHashes_.rend(); ++it) {
            if (it->first > currentHeight) continue;
            geocoin::core::Block activeBlock;
            if (chainState_->get_block_by_height(it->first, activeBlock) &&
                activeBlock.get_hash() == it->second) {
                return it->first == currentHeight ? currentHeight : it->first + 1;
            }
        }
        return fallback;
    }

    void performScan()
    {
        if (!chainState_) {
            emit scanError(tr("Chain state is not available"));
            return;
        }

        try {
            const geocoin::Height currentHeight = chainState_->get_height();
            invalidateOrphanedCommitments(currentHeight);

            geocoin::Hash256 targetHashBytes{};
            if (!parseHash256(targetHash_, targetHashBytes)) {
                emit scanError(tr("Invalid target hash format"));
                return;
            }
            if (allowedSigners_.isEmpty()) {
                emit scanError(tr("No trusted multisig participant list; scan cancelled"));
                return;
            }

            geocoin::Hash256 activeAnchorHash{};
            bool activeAnchorAvailable = false;
            if (hasAnchor_ && currentHeight >= lastScannedHeight_) {
                geocoin::core::Block activeAnchor;
                activeAnchorAvailable = chainState_->get_block_by_height(lastScannedHeight_, activeAnchor);
                if (activeAnchorAvailable) {
                    activeAnchorHash = activeAnchor.get_hash();
                }
            }

            // This must happen before the same-height/no-new-blocks return: a
            // replacement tip has the same height but different active blocks.
            const bool reorg = CommitmentScanner::anchorRequiresRescan(
                lastScannedHeight_, hasAnchor_, anchorHash_, currentHeight,
                activeAnchorAvailable, activeAnchorHash);

            geocoin::Height startHeight = 0;
            if (reorg) {
                startHeight = reorgRescanStart(currentHeight);
                recentBlockHashes_.erase(recentBlockHashes_.lower_bound(startHeight),
                                         recentBlockHashes_.end());
                emit statusUpdate(tr("The active chain changed. Checking again from block %1")
                    .arg(startHeight));
            } else if (scanCount_ == 0) {
                // The first pass is bounded so a large chain cannot monopolize
                // the scanner thread.
                startHeight = currentHeight > initialScanDepthBlocks_
                    ? currentHeight - initialScanDepthBlocks_
                    : 0;
            } else {
                if (currentHeight <= lastScannedHeight_) {
                    emit statusUpdate(tr("No new blocks (height: %1)").arg(currentHeight));
                    return;
                }
                startHeight = lastScannedHeight_ + 1;
            }

            const qulonglong blocksToScan =
                static_cast<qulonglong>(currentHeight - startHeight) + 1ULL;
            emit statusUpdate(tr("🔍 Scanning %1 blocks...").arg(blocksToScan));

            int foundInThisScan = 0;
            for (geocoin::Height h = startHeight; h <= currentHeight; ++h) {
                geocoin::core::Block block;
                if (!chainState_->get_block_by_height(h, block)) {
                    emit scanError(tr("Could not read active block %1; the scan was not advanced")
                        .arg(h));
                    return;
                }
                const geocoin::Hash256 blockHash = block.get_hash();
                recentBlockHashes_[h] = blockHash;

                for (const auto& tx : block.transactions) {
                    if (tx.type != geocoin::core::TransactionType::ANGLE_COMMITMENT) {
                        continue;
                    }
                    if (tx.spending_tx_hash != targetHashBytes) {
                        continue;
                    }
                    if (QString::fromStdString(tx.multisig_address.str()) != multisigAddress_) {
                        continue;
                    }
                    if (!isValidHistoricalCommitment(tx)) {
                        continue;
                    }

                    const QString txidStr = hashToString(tx.get_hash());
                    const QString signer = QString::fromStdString(tx.signer_address.str());

                    if (!foundCommitments_.contains(txidStr)) {
                        if (!CommitmentScanner::signerCanBeCollected(
                                signer, allowedSigners_, collectedSigners_)) {
                            continue;
                        }
                        foundCommitments_.insert(txidStr, FoundCommitment{h, blockHash, signer});
                        collectedSigners_.insert(signer);
                        ++foundInThisScan;
                        emit commitmentFound(txidStr, signer);
                    }
                }

                if (h == currentHeight) break; // Avoid unsigned Height wraparound.
            }

            geocoin::core::Block finalAnchor;
            const geocoin::Height finalHeight = chainState_->get_height();
            if (finalHeight != currentHeight ||
                !chainState_->get_block_by_height(currentHeight, finalAnchor) ||
                finalAnchor.get_hash() != recentBlockHashes_.at(currentHeight)) {
                // The active chain changed while blocks were being read. Do not
                // anchor mixed-branch results or advance progress; immediately
                // retract anything that is no longer active and retry next tick.
                invalidateOrphanedCommitments(finalHeight);
                recentBlockHashes_.clear();
                emit statusUpdate(tr("The active chain changed during the scan; the check will be repeated"));
                return;
            }
            anchorHash_ = finalAnchor.get_hash();
            hasAnchor_ = true;
            lastScannedHeight_ = currentHeight;
            ++scanCount_;

            const geocoin::Height oldestAnchor = currentHeight > initialScanDepthBlocks_
                ? currentHeight - initialScanDepthBlocks_
                : 0;
            recentBlockHashes_.erase(recentBlockHashes_.begin(),
                                     recentBlockHashes_.lower_bound(oldestAnchor));
            emit scanProgress(static_cast<qulonglong>(lastScannedHeight_), scanCount_);

            if (foundInThisScan > 0) {
                emit statusUpdate(tr("✅ Found %1 new signatures (total: %2/%3)")
                    .arg(foundInThisScan)
                    .arg(foundCommitments_.size())
                    .arg(requiredM_));
            } else {
                emit statusUpdate(tr("Blocks checked: %1 (signatures: %2/%3)")
                    .arg(blocksToScan)
                    .arg(foundCommitments_.size())
                    .arg(requiredM_));
            }

            if (foundCommitments_.size() >= requiredM_ && !allCollectedEmitted_) {
                allCollectedEmitted_ = true;
                emit allCommitmentsCollected();
            } else if (foundCommitments_.size() < requiredM_) {
                allCollectedEmitted_ = false;
            }
        } catch (const std::exception& ex) {
            emit scanError(QString::fromStdString(ex.what()));
        }
    }

    QTimer* timer_;
    QString targetHash_;
    QString multisigAddress_;
    int requiredM_;
    geocoin::Height initialScanDepthBlocks_;
    std::shared_ptr<geocoin::state::ChainState> chainState_;
    QHash<QString, FoundCommitment> foundCommitments_;
    QSet<QString> allowedSigners_;
    QSet<QString> collectedSigners_;
    std::map<geocoin::Height, geocoin::Hash256> recentBlockHashes_;
    geocoin::Height lastScannedHeight_;
    int scanCount_;
    bool scanning_;
    geocoin::Hash256 anchorHash_{};
    bool hasAnchor_;
    bool allCollectedEmitted_;
};

CommitmentScanner::CommitmentScanner(const QString& targetHash,
                                   const QString& multisigAddress,
                                   int requiredM,
                                   std::shared_ptr<geocoin::state::ChainState> chainState,
                                   QObject* parent,
                                   const QStringList& allowedSignerAddresses)
    : QObject(parent),
      workerThread_(nullptr),
      worker_(nullptr),
      targetHash_(targetHash),
      multisigAddress_(multisigAddress),
      requiredM_(requiredM),
      chainState_(chainState),
      foundTxids_(),
      lastScannedHeight_(0),
      scanCount_(0),
      scanning_(false)
{
    QSettings settings;
    const qulonglong cfgDepth = settings.value(
        QStringLiteral("scanner/initial_depth_blocks"),
        static_cast<qulonglong>(kDefaultInitialScanDepthBlocks)).toULongLong();
    const geocoin::Height initialDepth = static_cast<geocoin::Height>(
        (cfgDepth == 0) ? kDefaultInitialScanDepthBlocks : cfgDepth);

    workerThread_ = new QThread(this);
    worker_ = new CommitmentScannerWorker(targetHash_, multisigAddress_, requiredM_, initialDepth,
                                          chainState_, allowedSignerAddresses);
    worker_->moveToThread(workerThread_);

    connect(workerThread_, &QThread::finished, worker_, &QObject::deleteLater);

    connect(worker_, &CommitmentScannerWorker::commitmentFound, this,
            [this](const QString& txid, const QString& signerAddress) {
        foundTxids_.insert(txid);
        emit commitmentFound(txid, signerAddress);
    });
    connect(worker_, &CommitmentScannerWorker::commitmentInvalidated, this,
            [this](const QString& txid, const QString& signerAddress) {
        foundTxids_.remove(txid);
        emit commitmentInvalidated(txid, signerAddress);
    });
    connect(worker_, &CommitmentScannerWorker::allCommitmentsCollected, this, &CommitmentScanner::allCommitmentsCollected);
    connect(worker_, &CommitmentScannerWorker::scanError, this, &CommitmentScanner::scanError);
    connect(worker_, &CommitmentScannerWorker::statusUpdate, this, &CommitmentScanner::statusUpdate);
    connect(worker_, &CommitmentScannerWorker::scanProgress, this,
            [this](qulonglong lastScannedHeight, int scanCount) {
        lastScannedHeight_ = static_cast<geocoin::Height>(lastScannedHeight);
        scanCount_ = scanCount;
    });
    connect(worker_, &CommitmentScannerWorker::scanningStateChanged, this,
            [this](bool scanning) { scanning_ = scanning; });

    workerThread_->start();
}

CommitmentScanner::~CommitmentScanner()
{
    stopAutoScan();
    if (workerThread_) {
        workerThread_->quit();
        workerThread_->wait();
    }
}

void CommitmentScanner::startAutoScan(int intervalSeconds)
{
    if (!worker_) {
        emit scanError(tr("Scanner worker is not available"));
        return;
    }
    QMetaObject::invokeMethod(worker_, "startAutoScan", Qt::QueuedConnection, Q_ARG(int, intervalSeconds));
}

void CommitmentScanner::stopAutoScan()
{
    if (!worker_) {
        return;
    }
    QMetaObject::invokeMethod(worker_, "stopAutoScan", Qt::QueuedConnection);
}

void CommitmentScanner::scanNow()
{
    if (!worker_) {
        return;
    }
    QMetaObject::invokeMethod(worker_, "scanNow", Qt::QueuedConnection);
}

#include "commitmentscanner.moc"
