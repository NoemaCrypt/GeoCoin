
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "walletmodel.h"
#include "optionsmodel.h"
#include "transactiontablemodel.h"
#include "addresstablemodel.h"
#include "guiutil.h"
#include "qtlogging.h"
#include "walletsyncworker.h"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/wallet/address_funds.hpp"
#include "geocoin/wallet/multisig_service.hpp"
#include "geocoin/wallet/wallet_application_service.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/network/mempool_policy.hpp"
#include "geocoin/utils/u64_math.hpp"
#include "geocoin/types.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/crypto/addresses.hpp"

#include <QTimer>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QInputDialog>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QMetaObject>
#include <QRegularExpression>
#include <QThread>
#include <QLockFile>
#include <QSettings>
#include <QCryptographicHash>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <unordered_set>
#include <utility>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

// Helper function to generate random private key
static geocoin::bytes generate_random_private_key() {
    geocoin::bytes private_key;
    if (!geocoin::crypto::generate_private_key(private_key)) {
        throw std::runtime_error("Failed to generate random private key");
    }
    return private_key;
}

namespace {

using namespace geocoin;
using namespace geocoin::wallet;

bool computeContentHashHexFromImageUri(
    const QString& imageUri,
    const QString& dataDir,
    std::string& outHex
) {
    QString path;
    if (imageUri.startsWith("local://")) {
        // local://nft_images/<file> -> <dataDir>/nft_images/<file>
        const QString rel = imageUri.mid(QString("local://").size());
        path = QDir(dataDir).filePath(rel);
    } else if (imageUri.startsWith("file://")) {
        const QUrl url(imageUri);
        path = url.toLocalFile();
    } else {
        return false;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytesArr = f.readAll();
    f.close();

    geocoin::bytes b(bytesArr.begin(), bytesArr.end());
    const geocoin::Hash256 h = geocoin::crypto::sha256(b);
    outHex = geocoin::crypto::bytes_to_hex(geocoin::bytes(h.begin(), h.end()));
    return true;
}

QString bytesToHexString(const bytes& data) {
    return QString::fromStdString(crypto::bytes_to_hex(data));
}

QString hashToHexString(const Hash256& hash) {
    return bytesToHexString(bytes(hash.begin(), hash.end()));
}

bool hexStringToBytes(const QString& input, bytes& out) {
    QString filtered;
    filtered.reserve(input.size());
    for (QChar ch : input) {
        if (!ch.isSpace()) {
            filtered.append(ch);
        }
    }
    if (filtered.length() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(filtered.length() / 2);
    auto hexValue = [](QChar c) -> int {
        if (c >= '0' && c <= '9') return c.unicode() - '0';
        if (c >= 'a' && c <= 'f') return c.unicode() - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c.unicode() - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < filtered.length(); i += 2) {
        int hi = hexValue(filtered[i]);
        int lo = hexValue(filtered[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<byte>((hi << 4) | lo));
    }
    return true;
}

bool hexStringToHash(const QString& hex, Hash256& hash) {
    bytes data;
    if (!hexStringToBytes(hex, data) || data.size() != hash.size()) {
        return false;
    }
    std::copy(data.begin(), data.end(), hash.begin());
    return true;
}

bool jsonAmountToAngles(const QJsonObject& object,
                        const char* anglesField,
                        const char* gcField,
                        Amount& amount)
{
    const QJsonValue anglesValue = object.value(QLatin1String(anglesField));
    if (anglesValue.isString()) {
        const QString text = anglesValue.toString();
        if (text.isEmpty() || text.size() > 19) return false;
        for (const QChar ch : text) {
            if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
        }
        bool ok = false;
        const qint64 parsed = text.toLongLong(&ok, 10);
        if (ok && parsed >= 0) {
            amount = static_cast<Amount>(parsed);
            return true;
        }
        return false;
    }

    const QJsonValue gcValue = object.value(QLatin1String(gcField));
    qint64 parsed = 0;
    if (!gcValue.isString() || !GUIUtil::parseAmount(gcValue.toString(), parsed)) {
        return false;
    }
    amount = static_cast<Amount>(parsed);
    return true;
}

bool readKeyFile(const QString& path, bytes& private_key, QString& error) {
    QString keyHex;
    GUIUtil::ScopedQStringCleanser keyHexCleanser(keyHex);
    if (!GUIUtil::readPrivateKeyFileHex(path, keyHex, error)) {
        return false;
    }

    if (!hexStringToBytes(keyHex, private_key) || private_key.size() != 32) {
        error = QObject::tr("Invalid private key format in file");
        return false;
    }
    return true;
}

bool deserializeTransaction(const QString& hex, core::Transaction& tx) {
    bytes data;
    if (!hexStringToBytes(hex, data)) {
        return false;
    }
    size_t offset = 0;
    try {
        (void)core::Transaction::canonical_hash_from_serialized(data);
        tx = core::Transaction::deserialize(data, offset);
        return offset == data.size() && tx.serialize() == data;
    } catch (...) {
        return false;
    }
}

struct ParsedCommitmentDraft {
    QJsonObject json;
    core::Transaction transaction;
    CommitmentSigningIntent intent;
};

QString firstStringField(
    const QJsonObject& object,
    std::initializer_list<const char*> names
) {
    for (const char* name : names) {
        const QJsonValue value = object.value(QLatin1String(name));
        if (value.isString() && !value.toString().trimmed().isEmpty()) {
            return value.toString().trimmed();
        }
    }
    return {};
}

bool parseCommitmentDraft(
    const QString& draftJson,
    const std::shared_ptr<state::ChainState>& chainState,
    ParsedCommitmentDraft& parsed,
    QString& error
) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        draftJson.toUtf8(), &parseError);
    if (document.isNull() || !document.isObject()) {
        error = QObject::tr("Invalid draft JSON: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("mode")).toString() ==
        QStringLiteral("spending_hash_only")) {
        error = QObject::tr(
            "Hash-only signing is disabled. Load the complete JSON draft with canonical tx_hex.");
        return false;
    }
    const QString txHex = firstStringField(
        object, {"tx_hex", "multisig_spend_hex", "delegation_proof_hex"});
    if (txHex.isEmpty()) {
        error = QObject::tr(
            "Draft must contain the complete canonical transaction in tx_hex.");
        return false;
    }

    core::Transaction transaction;
    if (!deserializeTransaction(txHex, transaction)) {
        error = QObject::tr("Draft contains an invalid or non-canonical transaction");
        return false;
    }

    std::shared_ptr<core::Transaction> proof;
    Hash256 proofTxid{};
    if (transaction.type == core::TransactionType::MULTISIG_SPEND) {
        proofTxid = transaction.delegation_proof_txid;
    } else if (transaction.type == core::TransactionType::REVOKE_DELEGATION) {
        proofTxid = transaction.revoked_delegation_proof_txid;
    }
    const bool needsProof = std::any_of(
        proofTxid.begin(), proofTxid.end(), [](byte value) { return value != 0; });
    if (needsProof) {
        if (!chainState) {
            error = QObject::tr(
                "Chain state is required to verify the referenced delegation proof");
            return false;
        }
        proof = chainState->get_transaction(proofTxid);
        if (!proof || proof->type != core::TransactionType::DELEGATION_PROOF) {
            error = QObject::tr("Referenced delegation proof is not confirmed");
            return false;
        }
        const uint64_t now = static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch());
        if (chainState->is_delegation_revoked(proofTxid) ||
            (proof->valid_from > 0 && now < proof->valid_from) ||
            (proof->valid_until > 0 && now > proof->valid_until)) {
            error = QObject::tr("Referenced delegation proof is not active");
            return false;
        }
    }

    CommitmentSigningIntent intent;
    std::string serviceError;
    if (!derive_commitment_signing_intent(
            transaction, proof.get(), intent, serviceError)) {
        error = QString::fromStdString(serviceError);
        return false;
    }

    QString expectedType;
    QString declaredHash;
    QString declaredAddress;
    switch (intent.kind) {
        case CommitmentIntentKind::DIRECT_SPEND:
            expectedType = QStringLiteral("multisig_spend");
            declaredHash = firstStringField(
                object, {"spending_tx_hash", "spending_hash"});
            declaredAddress = firstStringField(object, {"multisig_address"});
            break;
        case CommitmentIntentKind::DELEGATION_PROOF:
            expectedType = QStringLiteral("delegation_proof");
            declaredHash = firstStringField(object, {"delegation_hash"});
            declaredAddress = firstStringField(
                object, {"primary_address", "primary_multisig_address"});
            break;
        case CommitmentIntentKind::HIERARCHICAL_SPEND:
            expectedType = QStringLiteral("hierarchical_spend");
            declaredHash = firstStringField(
                object, {"spending_hash", "spending_tx_hash"});
            declaredAddress = firstStringField(
                object, {"delegated_address", "delegated_multisig_address"});
            break;
        case CommitmentIntentKind::REVOKE_DELEGATION:
            expectedType = QStringLiteral("revoke_delegation");
            declaredHash = firstStringField(object, {"revoke_hash"});
            declaredAddress = firstStringField(
                object, {"primary_address", "primary_multisig_address"});
            break;
    }

    const QString declaredType = object.value(QStringLiteral("type")).toString().trimmed();
    if ((!declaredType.isEmpty() && declaredType != expectedType) ||
        declaredHash != hashToHexString(intent.signing_hash) ||
        declaredAddress != QString::fromStdString(intent.signer_config.address.str())) {
        error = QObject::tr(
            "Draft summary does not match the canonical transaction or signer policy");
        return false;
    }

    parsed.json = object;
    parsed.transaction = std::move(transaction);
    parsed.intent = std::move(intent);
    return true;
}

} // namespace

WalletModel::StatusCode WalletModel::createNft(const QString& name,
                                               const QString& description,
                                               const QString& imageUri,
                                               const QString& locationName,
                                               const QString& tagsCsv,
                                               const geocoin::nft::NFTMetadata::Geometry& geometry,
                                               const std::optional<geocoin::nft::NFTMetadata::CadastralData>& cadastral,
                                               qint64 fee,
                                               quint32 royaltyPercent,
                                               const QString& password)
{
    if (!wallet_) {
        return TransactionCreationFailed;
    }
    if (!chain_state_) {
        return TransactionCreationFailed;
    }
    if (fee < nftRelayFee()) {
        return InvalidAmount;
    }
    if (fee > cachedBalance) {
        return AmountWithFeeExceedsBalance;
    }

    if (!authorizeSigningPassword(password)) {
        return TransactionCreationFailed;
    }

    try {
        geocoin::nft::NFTMetadata meta;
        meta.name = name.toStdString();
        meta.description = description.toStdString();
        meta.image_uri = imageUri.toStdString();

        // Consensus commitment to content bytes (SHA-256 hex)
        std::string contentHashHex;
        if (!computeContentHashHexFromImageUri(imageUri, dataDirectory_, contentHashHex)) {
            return TransactionCreationFailed;
        }
        meta.content_hash = contentHashHex;

        meta.geometry = geocoin::nft::coordinates::canonicalize_geometry(geometry);
        if (!geocoin::nft::coordinates::validate_geometry(meta.geometry)) {
            return TransactionCreationFailed;
        }
        meta.area_bits = 0;

        meta.location_name = locationName.toStdString();
        meta.tags.clear();
        if (!tagsCsv.trimmed().isEmpty()) {
            const QStringList parts = tagsCsv.split(",", Qt::SkipEmptyParts);
            for (const QString& p : parts) {
                const QString t = p.trimmed();
                if (!t.isEmpty()) {
                    meta.tags.push_back(t.toStdString());
                }
            }
        }

        if (cadastral.has_value()) {
            meta.cadastral = cadastral;
        }

        geocoin::wallet::WalletApplicationService useCases(
            wallet_, chain_state_, nodeServices_.mutations
        );
        const auto created = useCases.create_nft(meta, royaltyPercent, fee, pendingTransactions());
        if (!created.succeeded()) {
            return TransactionCreationFailed;
        }

        return commitCreatedTransaction(created.transaction, password);
    } catch (...) {
        return TransactionCreationFailed;
    }
}

WalletModel::StatusCode WalletModel::transferNft(const QString& nftIdHex,
                                                 const QString& toAddress,
                                                 qint64 fee,
                                                 const QString& password)
{
    if (!wallet_) {
        return TransactionCreationFailed;
    }
    if (!chain_state_) {
        return TransactionCreationFailed;
    }
    if (fee < nftRelayFee()) {
        return InvalidAmount;
    }
    if (fee > cachedBalance) {
        return AmountWithFeeExceedsBalance;
    }

    if (!authorizeSigningPassword(password)) {
        return TransactionCreationFailed;
    }

    Hash256 nft_id;
    if (!hexStringToHash(nftIdHex, nft_id)) {
        return TransactionCreationFailed;
    }

    geocoin::Address to(toAddress.toStdString());
    if (to.str().empty() || !geocoin::crypto::verify_address(to)) {
        return InvalidAddress;
    }

    try {
        geocoin::wallet::WalletApplicationService useCases(
            wallet_, chain_state_, nodeServices_.mutations
        );
        const auto created = useCases.transfer_nft(nft_id, to, fee, pendingTransactions());
        if (!created.succeeded()) {
            return TransactionCreationFailed;
        }

        return commitCreatedTransaction(created.transaction, password);
    } catch (...) {
        return TransactionCreationFailed;
    }
}

WalletModel::StatusCode WalletModel::burnNft(const QString& nftIdHex,
                                             qint64 fee,
                                             const QString& password)
{
    if (!wallet_) {
        return TransactionCreationFailed;
    }
    if (!chain_state_) {
        return TransactionCreationFailed;
    }
    if (fee < nftRelayFee()) {
        return InvalidAmount;
    }
    if (fee > cachedBalance) {
        return AmountWithFeeExceedsBalance;
    }

    if (!authorizeSigningPassword(password)) {
        return TransactionCreationFailed;
    }

    Hash256 nft_id;
    if (!hexStringToHash(nftIdHex, nft_id)) {
        return TransactionCreationFailed;
    }

    try {
        geocoin::wallet::WalletApplicationService useCases(
            wallet_, chain_state_, nodeServices_.mutations
        );
        const auto created = useCases.burn_nft(nft_id, fee, pendingTransactions());
        if (!created.succeeded()) {
            return TransactionCreationFailed;
        }

        return commitCreatedTransaction(created.transaction, password);
    } catch (...) {
        return TransactionCreationFailed;
    }
}

namespace {

bool parseMultisigConfigObject(
    const QJsonObject& obj,
    MultisigConfig& config,
    QString& error
) {
    if (!obj.contains("m") || !obj.contains("n") || !obj.contains("address")) {
        error = QObject::tr("Config missing required fields (m, n, address)");
        return false;
    }
    config.m = static_cast<uint8_t>(obj.value("m").toInt());
    config.n = static_cast<uint8_t>(obj.value("n").toInt());
    QString addressStr = obj.value("address").toString();
    if (addressStr.isEmpty()) {
        error = QObject::tr("Config address is empty");
        return false;
    }
    config.address = geocoin::Address(addressStr.toStdString());
    config.addresses.clear();
    config.participants.clear();
    QJsonArray participants = obj.value("participants").toArray();
    if (participants.isEmpty()) {
        error = QObject::tr("Config must contain participants array");
        return false;
    }
    for (const auto& value : participants) {
        if (!value.isObject()) continue;
        QJsonObject participantObj = value.toObject();
        QString participantAddr = participantObj.value("address").toString();
        if (participantAddr.isEmpty()) continue;
        geocoin::Address addr(participantAddr.toStdString());
        config.addresses.push_back(addr);
        config.participants.emplace_back(addr);
    }
    if (!config.is_valid()) {
        error = QObject::tr("Config is not valid (check m/n and participants)");
        return false;
    }
    return true;
}

bool parseMultisigConfigDocument(
    const QByteArray& json,
    MultisigConfig& config,
    QString& error
) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (document.isNull() || !document.isObject()) {
        error = QObject::tr("Invalid config JSON: %1").arg(parseError.errorString());
        return false;
    }
    return parseMultisigConfigObject(document.object(), config, error);
}

bool loadMultisigConfig(const QString& path, MultisigConfig& config, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QObject::tr("Cannot open config file: %1").arg(path);
        return false;
    }
    return parseMultisigConfigDocument(file.readAll(), config, error);
}

// Загрузка конфигурации из JSON строки (вместо файла)
bool loadMultisigConfigFromJson(const QString& jsonStr, MultisigConfig& config, QString& error) {
    return parseMultisigConfigDocument(jsonStr.toUtf8(), config, error);
}

} // namespace

struct WalletModel::RefreshSnapshot {
    qint64 balances[6]{};
    geocoin::Height height = 0;
    geocoin::Hash256 tip{};
    bool haveTip = false;
    bool ibd = false;
    bool reorg = false;
    bool rescan = false;
    geocoin::Height rescanFrom = 0;
    TransactionTableModel::Snapshot transactions;
};

WalletModel::WalletModel(QObject *parent) :
    QObject(parent),
    transactionTableModel(nullptr),
    addressTableModel(nullptr),
    optionsModel(nullptr),
    wallet_(nullptr),
    chain_state_(nullptr),
    mempool_(nullptr),
    cachedBalance(0),
    cachedUnconfirmedBalance(0),
    cachedImmatureBalance(0),
    cachedWatchOnlyBalance(0),
    cachedWatchOnlyUnconfirmedBalance(0),
    cachedWatchOnlyImmatureBalance(0),
    cachedNumBlocks(0),
    haveCachedBestBlockHash_(false),
    fForceCheckBalanceChanged(false),
    lastIbdState(false),
    didPostIbdRescan(false)
{
    optionsModel = new OptionsModel(this);
    transactionTableModel = new TransactionTableModel(this);
    addressTableModel = new AddressTableModel(this);
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(25);
    connect(refreshTimer_, &QTimer::timeout, this, &WalletModel::finishWalletRefresh);
}

WalletModel::~WalletModel()
{
    try {
        beginQuiesce();
        quiesceAction_.wait();
        waitForRescanThread();
        syncWorker_.reset();
        stagedWallet_.reset();
        wallet_.reset();
        stagedLock_.reset();
        walletLock_.reset();
    } catch (...) {
        // Suppress all exceptions in destructor
    }
}

void WalletModel::waitForRescanThread()
{
    if (rescanThread_.joinable()) {
        rescanThread_.join();
    }
}

bool WalletModel::isRescanInProgress() const
{
    std::lock_guard<std::mutex> lock(rescanMutex_);
    return rescanInProgress_;
}

bool WalletModel::getRescanProgress(int& current, int& target) const
{
    std::shared_ptr<geocoin::wallet::Wallet> wallet_snapshot;
    geocoin::Height target_height = 0;
    {
        std::lock_guard<std::mutex> lock(rescanMutex_);
        if (!rescanInProgress_) {
            return false;
        }
        target_height = rescanTargetHeight_;
        wallet_snapshot = wallet_;
    }

    geocoin::Height current_height = 0;
    if (wallet_snapshot) {
        current_height = wallet_snapshot->get_last_scanned_height();
    }
    if (current_height > target_height) {
        target_height = current_height;
    }

    const geocoin::Height int_max_h = static_cast<geocoin::Height>(std::numeric_limits<int>::max());
    current = static_cast<int>(std::min(current_height, int_max_h));
    target = static_cast<int>(std::min(target_height, int_max_h));
    return target > 0;
}

void WalletModel::startWalletRescanAsync(geocoin::Height fromHeight,
    std::optional<geocoin::Height> knownTarget)
{
    if (quiescing_ || !wallet_ || !chain_state_) {
        return;
    }

    geocoin::Height current_height = knownTarget.value_or(cachedNumBlocks);
    geocoin::Height start_height = fromHeight;

    {
        std::lock_guard<std::mutex> lock(rescanMutex_);
        if (rescanInProgress_) {
            if (!pendingRescan_) {
                pendingRescan_ = true;
                pendingRescanFrom_ = start_height;
            } else {
                pendingRescanFrom_ = std::min(pendingRescanFrom_, start_height);
            }
            return;
        }
        rescanInProgress_ = true;
        rescanTargetHeight_ = current_height;
    }

    waitForRescanThread();

    auto wallet_snapshot = wallet_;
    auto chain_snapshot = chain_state_;
    try {
        const auto generation = walletGeneration_;
        rescanThread_ = std::thread([this, wallet_snapshot, chain_snapshot, fromHeight, generation]() {
            try {
                const auto height = chain_snapshot->get_height();
                { std::lock_guard<std::mutex> lock(rescanMutex_); rescanTargetHeight_ = height; }
                const auto start_height = std::min(fromHeight, height);
                wallet_snapshot->scan_blockchain(chain_snapshot, start_height);
            } catch (const std::exception& exception) {
                qWarning() << "Wallet rescan failed:" << exception.what();
            } catch (...) {
                qWarning() << "Wallet rescan failed with an unknown exception";
            }
            QMetaObject::invokeMethod(
                this,
                [this, generation]() {
                    if (!quiescing_ && generation == walletGeneration_) onWalletRescanFinished();
                },
                Qt::QueuedConnection
            );
        });
    } catch (const std::exception& exception) {
        qWarning() << "Failed to start wallet rescan thread:" << exception.what();
        std::lock_guard<std::mutex> lock(rescanMutex_);
        rescanInProgress_ = false;
        rescanTargetHeight_ = 0;
        pendingRescan_ = false;
        pendingRescanFrom_ = 0;
    }
}

void WalletModel::onWalletRescanFinished()
{
    bool run_pending = false;
    geocoin::Height pending_from = 0;
    {
        std::lock_guard<std::mutex> lock(rescanMutex_);
        rescanInProgress_ = false;
        run_pending = pendingRescan_;
        pending_from = pendingRescanFrom_;
        pendingRescan_ = false;
        pendingRescanFrom_ = 0;
    }

    updateBalance();
    if (transactionTableModel) {
        transactionTableModel->updateTransactions();
        transactionRefreshPending_ = false;
    }

    if (run_pending) {
        startWalletRescanAsync(pending_from);
    }
}

TransactionTableModel *WalletModel::getTransactionTableModel()
{
    return transactionTableModel;
}

AddressTableModel *WalletModel::getAddressTableModel()
{
    return addressTableModel;
}

OptionsModel *WalletModel::getOptionsModel()
{
    return optionsModel;
}

qint64 WalletModel::getBalance() const
{
    return cachedBalance;
}

qint64 WalletModel::getUnconfirmedBalance() const
{
    return cachedUnconfirmedBalance;
}

qint64 WalletModel::getImmatureBalance() const
{
    return cachedImmatureBalance;
}

qint64 WalletModel::getWatchOnlyBalance() const
{
    return cachedWatchOnlyBalance;
}

qint64 WalletModel::getWatchOnlyUnconfirmedBalance() const
{
    return cachedWatchOnlyUnconfirmedBalance;
}

qint64 WalletModel::getWatchOnlyImmatureBalance() const
{
    return cachedWatchOnlyImmatureBalance;
}

QString WalletModel::getAddress() const
{
    if (!wallet_ || wallet_->get_key_count() == 0) return QString();

    auto address = wallet_->get_address();
    return QString::fromStdString(address.str());
}

double WalletModel::getArea() const
{
    if (!wallet_ || wallet_->get_key_count() == 0) return 0.0;
    return 0.0;
}

qint64 WalletModel::relayFeeForBytes(
    size_t serializedSize,
    const geocoin::network::Mempool* mempool)
{
    geocoin::network::MempoolPolicyConfig config;
    config.min_relay_fee_rate_per_kb =
        geocoin::network::fee_policy::recommended_rate_per_kb(mempool);
    const uint64_t required = geocoin::network::MempoolPolicy::required_fee_for_size(
        serializedSize, config);
    if (required > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
        return std::numeric_limits<qint64>::max();
    }
    return static_cast<qint64>(required);
}

qint64 WalletModel::relayFeeForBytes(size_t serializedSize) const
{
    return relayFeeForBytes(serializedSize, mempool_.get());
}

qint64 WalletModel::standardRelayFee(const geocoin::network::Mempool* mempool)
{
    const size_t bytes = geocoin::consensus::params::TX_BASE_SIZE +
        geocoin::consensus::params::TX_INPUT_SIZE +
        geocoin::consensus::params::TX_OUTPUT_SIZE;
    return relayFeeForBytes(bytes, mempool);
}

qint64 WalletModel::standardRelayFee() const
{
    return standardRelayFee(mempool_.get());
}

qint64 WalletModel::nftRelayFee(const geocoin::network::Mempool* mempool)
{
    return relayFeeForBytes(
        geocoin::consensus::params::TX_BASE_SIZE +
        geocoin::consensus::params::TX_INPUT_SIZE +
        geocoin::consensus::params::TX_OUTPUT_SIZE + 8192,
        mempool);
}

qint64 WalletModel::nftRelayFee() const
{
    return nftRelayFee(mempool_.get());
}

qint64 WalletModel::multisigRelayFee(int participants, const geocoin::network::Mempool* mempool)
{
    const int count = std::clamp(participants, 1, 20);
    const size_t bytes = geocoin::consensus::params::TX_BASE_SIZE +
        geocoin::consensus::params::TX_INPUT_SIZE +
        geocoin::consensus::params::TX_OUTPUT_SIZE +
        256 + static_cast<size_t>(count) * 2048;
    return relayFeeForBytes(bytes, mempool);
}

qint64 WalletModel::multisigRelayFee(int participants) const
{
    return multisigRelayFee(participants, mempool_.get());
}

std::vector<geocoin::core::Transaction> WalletModel::pendingTransactions() const
{
    if (!mempool_) {
        return {};
    }
    return mempool_->get_all_transactions();
}

qint64 WalletModel::estimateFee(qint64 amount) const
{
    const qint64 floor = standardRelayFee();
    if (!wallet_ || !chain_state_ || amount <= 0) {
        return floor;
    }

    const auto pending = pendingTransactions();
    geocoin::Amount fee = wallet_->estimate_required_fee(amount, chain_state_, pending);

    const uint64_t recommended_rate =
        geocoin::network::fee_policy::recommended_rate_per_kb(mempool_.get());
    const qint64 scaled = geocoin::network::fee_policy::scale_fee_to_rate(fee, recommended_rate);
    return std::max(scaled, floor);
}

bool WalletModel::isWalletLoaded() const
{
    return wallet_ != nullptr && (wallet_->get_key_count() > 0 || !wallet_->get_watch_only_addresses().empty());
}

bool WalletModel::isWalletEncrypted() const
{
    return wallet_ && (wallet_->has_password_protection() || wallet_->last_load_result().encrypted);
}

bool WalletModel::isWalletLocked() const
{
    return wallet_ && wallet_->is_locked();
}

bool WalletModel::verifyPassword(const QString &password) const
{
    if (!wallet_) {
        return false;
    }

    std::string pwd = password.toStdString();
    GUIUtil::ScopedStringCleanser passwordCleanser(pwd);

    return wallet_->is_vault() ? wallet_->unlock(pwd, 60) : wallet_->verify_password(pwd);
}

void WalletModel::emitWalletStateChanged()
{
    const bool loaded = isWalletLoaded();
    emit walletLoadedChanged(loaded);
    emit primaryAddressChanged(loaded ? getAddress() : QString());
    emit walletStateChanged();
}

void WalletModel::clearWalletInPlace()
{
    if (!wallet_) return;
    // Wallet::clear() intentionally focuses on keys/transactions. A GUI-level
    // close or replacement must also forget watch-only multisig state so the
    // next wallet cannot inherit addresses from the previous one.
    const auto multisigConfigs = wallet_->get_multisig_addresses();
    for (const auto& config : multisigConfigs) {
        wallet_->remove_multisig_address(config.address.str());
    }
    wallet_->clear();
}

bool WalletModel::getAuthorizedWalletPrivateKey(const QString& address,
                                                const QString& password,
                                                geocoin::bytes& privateKey,
                                                QString& error) const
{
    privateKey.clear();
    error.clear();
    if (!isWalletLoaded()) {
        error = tr("Wallet is not loaded");
        return false;
    }

    std::string pwd;
    GUIUtil::ScopedStringCleanser passwordCleanser(pwd);
    if (wallet_->has_password_protection()) {
        if (password.isEmpty()) {
            error = tr("Wallet password is required");
            return false;
        }
        pwd = password.toStdString();
        if (!(wallet_->is_vault() ? wallet_->unlock(pwd, 60) : wallet_->verify_password(pwd))) {
            error = tr("Incorrect wallet password");
            return false;
        }
    }

    try {
        const geocoin::Address walletAddress(address.toStdString());
        if (!wallet_->get_private_key(walletAddress, privateKey)) {
            error = tr("Private key for %1 was not found in the wallet").arg(address);
        }
    } catch (const std::exception& ex) {
        error = tr("Invalid wallet address: %1").arg(ex.what());
    }

    return !privateKey.empty();
}

bool WalletModel::authorizeSigningPassword(const QString& password)
{
    if (!wallet_) {
        return false;
    }
    if (password.isEmpty()) {
        // Preserve support for callers that already authorized the wallet and
        // intentionally rely on its short-lived remembered signing password.
        return true;
    }

    std::string passwordBytes = password.toStdString();
    GUIUtil::ScopedStringCleanser passwordCleanser(passwordBytes);
    if (!(wallet_->is_vault() ? wallet_->unlock(passwordBytes, 60) : wallet_->verify_password(passwordBytes))) {
        return false;
    }
    wallet_->remember_password(passwordBytes);
    return true;
}

WalletModel::StatusCode WalletModel::commitCreatedTransaction(
    const geocoin::core::Transaction& transaction,
    const QString& password
) {
    if (!submitTransaction(transaction)) {
        // The wallet recorded this transaction while creating it; a rejected
        // one never reaches the chain and must not stay as an unconfirmed send.
        wallet_->discard_unconfirmed_transaction(transaction.get_hash());
        return TransactionCommitFailed;
    }

    emit transactionCreated(hashToHexString(transaction.get_hash()));
    updateBalance();
    if (transactionTableModel) {
        transactionTableModel->updateTransactions();
    }

    QString saveError;
    if (!saveWallet(password, &saveError)) {
        emit walletSaveError(saveError);
        return WalletSaveFailed;
    }
    return OK;
}

bool WalletModel::submitTransaction(const geocoin::core::Transaction& transaction)
{
    geocoin::wallet::WalletApplicationService useCases(
        wallet_, chain_state_, nodeServices_.mutations
    );
    return useCases.submit(transaction).succeeded();
}

WalletModel::StatusCode WalletModel::sendCoins(const QString &address, qint64 amount, qint64 fee, const QString &password)
{
    if (!wallet_) {
        return TransactionCreationFailed;
    }

    if (!chain_state_) {
        return TransactionCreationFailed;
    }

    if (amount <= 0 || fee < 0) {
        return InvalidAmount;
    }

    const qint64 requiredFee = estimateFee(amount);
    if (requiredFee <= 0) {
        return TransactionCreationFailed;
    }
    fee = qMax(fee, requiredFee);

    qint64 total = 0;
    if (!GUIUtil::addAmounts(amount, fee, total)) {
        return InvalidAmount;
    }
    if (total > cachedBalance) {
        return AmountWithFeeExceedsBalance;
    }

    if (!authorizeSigningPassword(password)) {
        return TransactionCreationFailed;
    }

    geocoin::Address to_address(address.toStdString());
    if (to_address.str().empty() || !geocoin::crypto::verify_address(to_address)) {
        return InvalidAddress;
    }

    try {
        geocoin::wallet::WalletApplicationService useCases(
            wallet_, chain_state_, nodeServices_.mutations
        );
        const auto created = useCases.create_payment(to_address, amount, fee, pendingTransactions());
        if (!created.succeeded()) {
            return TransactionCreationFailed;
        }

        return commitCreatedTransaction(created.transaction, password);
    } catch (const std::exception&) {
        return TransactionCreationFailed;
    }
}

bool WalletModel::loadWallet(const QString &filename, const QString &password)
{
    if (!stageWallet(filename, password)) return false;
    if (stagedMigrationNeeded_) return walletFailure(QStringLiteral("migration_required"), filename,
        tr("Legacy wallet requires explicit migration confirmation."));
    return activateStagedWallet();
}

bool WalletModel::createWallet(const QString &filename, const QString &password)
{
    return stageWallet(filename, password, true) && activateStagedWallet();
}

namespace {
QString canonicalWalletPath(const QString& filename)
{
    const QFileInfo info(filename);
    if (info.exists()) return info.canonicalFilePath();
    const QString parent = info.dir().canonicalPath();
    return parent.isEmpty() ? QString() : QDir(parent).filePath(info.fileName());
}

QString walletSettingsKey(const QString& dataDir)
{
    const QString canonical = QDir(dataDir).canonicalPath();
    const QByteArray identity = (canonical.isEmpty() ? QDir(dataDir).absolutePath() : canonical).toUtf8();
    return QStringLiteral("wallet/last_successful/%1").arg(QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex()));
}

QString walletLockPath(const QString& path)
{
    // A canonical, process-independent identity also permits migration from a
    // read-only source directory without dropping the source lock.
    QString directoryName = QStringLiteral("geocoin-wallet-locks");
#ifndef Q_OS_WIN
    directoryName += QStringLiteral("-%1").arg(static_cast<qulonglong>(::geteuid()));
#endif
    const QString directory = QDir::temp().filePath(directoryName);
    if (!QDir().mkpath(directory)) return QString();
#ifndef Q_OS_WIN
    const QFileInfo lockDirectory(directory);
    if (lockDirectory.isSymLink() || lockDirectory.ownerId() != ::geteuid() ||
        !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return QString();
#endif
    QString identity = path;
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    return QDir(directory).filePath(QString::fromLatin1(QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex()) + QStringLiteral(".lock"));
}

std::unique_ptr<QLockFile> lockBackupDestination(const QString& path, QString* error)
{
    const QString backup = canonicalWalletPath(path + QStringLiteral(".bak"));
    auto lock = std::make_unique<QLockFile>(walletLockPath(backup));
    lock->setStaleLockTime(0);
    if (backup.isEmpty() || !lock->tryLock(0)) {
        if (error) *error = WalletModel::tr("Wallet backup destination is locked or unavailable [backup_lock]: %1").arg(backup);
        return {};
    }
    return lock;
}
}

QString WalletModel::initialWalletPath(const QString& dataDir, const QString& explicitPath)
{
    if (!explicitPath.isEmpty()) return QFileInfo(explicitPath).isAbsolute()
        ? explicitPath : QDir(dataDir).filePath(explicitPath);
    return QSettings().value(walletSettingsKey(dataDir)).toString();
}

bool WalletModel::walletFailure(const QString& code, const QString& path, const QString& detail)
{
    walletOperationCode_ = code.toLower();
    walletOperationError_ = tr("Wallet operation failed [%1]\nPath: %2\n%3").arg(code, path, detail);
    return false;
}

void WalletModel::discardStagedWallet()
{
    stagedWallet_.reset();
    stagedLock_.reset();
    stagedPath_.clear();
    stagedMigrationNeeded_ = false;
}

bool WalletModel::stageWallet(const QString& filename, const QString& password, bool create)
{
    Q_ASSERT(QThread::currentThread() == thread());
    discardStagedWallet();
    walletOperationError_.clear();
    walletOperationCode_.clear();
    const QString path = canonicalWalletPath(filename);
    if (filename.isEmpty() || path.isEmpty()) return walletFailure(QStringLiteral("invalid_path"), filename,
        tr("The wallet parent directory does not exist."));
#ifdef Q_OS_WIN
    const bool samePath = path.compare(walletFilePath_, Qt::CaseInsensitive) == 0;
#else
    const bool samePath = path == walletFilePath_;
#endif
    if (samePath && walletLock_)
        return walletFailure(QStringLiteral("already_active"), path, tr("This wallet is already active."));
    if (create && QFileInfo::exists(path)) return walletFailure(QStringLiteral("already_exists"), path,
        tr("Choose a new filename. Existing wallets are never overwritten by Create."));
    if (!create && !QFileInfo(path).isFile()) return walletFailure(QStringLiteral("not_found"), path,
        tr("Select an existing wallet file."));
    const QString lockPath = walletLockPath(path);
    if (lockPath.isEmpty()) return walletFailure(QStringLiteral("wallet_lock_directory"), path, tr("Cannot create wallet lock directory."));
    auto lock = std::make_unique<QLockFile>(lockPath);
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) return walletFailure(QStringLiteral("wallet_lock"), path,
        lock->error() == QLockFile::LockFailedError ? tr("Another process holds this wallet lock.")
            : tr("Cannot create the wallet lock. Check directory permissions."));
    auto candidate = std::make_shared<geocoin::wallet::Wallet>();
    std::string pwd = password.toStdString();
    GUIUtil::ScopedStringCleanser cleanser(pwd);
    try {
        if (create) {
            QString error;
            auto backupLock = lockBackupDestination(path, &error);
            if (!backupLock) return walletFailure(QStringLiteral("backup_lock"), path, error);
            auto key = generate_random_private_key();
            geocoin::utils::ScopedContainerCleanser<geocoin::bytes> keyCleanser(key);
            if (!candidate->initialize(key) || !candidate->save_to_file(path.toStdString(), pwd))
                return walletFailure(QStringLiteral("create_failed"), path, tr("Could not durably save the new wallet."));
        } else {
            const auto result = candidate->load_from_file_result(path.toStdString(), pwd, false);
            if (!result.ok()) {
                return walletFailure(QString::fromLatin1(geocoin::wallet::wallet_load_error_name(result.error)),
                    path, tr("The selected file was not activated. Check its format, passphrase and permissions."));
            }
            stagedMigrationNeeded_ = result.migration_needed;
            if (result.encrypted && !candidate->has_password_protection()) candidate->remember_password(pwd);
        }
        stagedWallet_ = std::move(candidate);
        stagedLock_ = std::move(lock);
        stagedPath_ = path;
        return true;
    } catch (const std::exception& exception) {
        return walletFailure(QStringLiteral("exception"), path, QString::fromUtf8(exception.what()));
    }
}

bool WalletModel::migrateStagedWallet(const QString& password, const QString& outputPath)
{
    if (!stagedWallet_ || !stagedMigrationNeeded_) return false;
    const QString output = canonicalWalletPath(outputPath);
    if (output.isEmpty()) return walletFailure(QStringLiteral("invalid_output"), outputPath, tr("Choose an existing output directory."));
    std::unique_ptr<QLockFile> outputLock;
    if (output != stagedPath_) {
        if (QFileInfo::exists(output)) return walletFailure(QStringLiteral("output_exists"), output, tr("Choose a new output filename."));
        outputLock = std::make_unique<QLockFile>(walletLockPath(output));
        outputLock->setStaleLockTime(0);
        if (!outputLock->tryLock(0)) return walletFailure(QStringLiteral("output_lock"), output, tr("Cannot lock migration output."));
    }
    std::string pwd = password.toStdString();
    GUIUtil::ScopedStringCleanser cleanser(pwd);
    geocoin::wallet::WalletMigrationOptions options;
    options.confirmed = true;
    options.output_path = output.toStdString();
    if (output != stagedPath_) options.backup_path = (output + QStringLiteral(".legacy.bak")).toStdString();
    const auto result = geocoin::wallet::Wallet::migrate_file(stagedPath_.toStdString(), pwd, options);
    if (!result.ok() || !result.durable) return walletFailure(result.committed ? QStringLiteral("migration_committed_not_durable")
        : QString::fromLatin1(geocoin::wallet::wallet_migration_error_name(result.error)),
        stagedPath_, tr("Migration was not confirmed durable. Source: %1\nOutput: %2\nBackup: %3")
            .arg(stagedPath_, QString::fromStdString(result.output_path), QString::fromStdString(result.backup_path)));
    auto candidate = std::make_shared<geocoin::wallet::Wallet>();
    const auto loaded = candidate->load_from_file_result(output.toStdString(), pwd, false);
    if (!loaded.ok())
        return walletFailure(QStringLiteral("migration_reload"), output, tr("Migration output could not be reopened."));
    if (loaded.encrypted && !candidate->has_password_protection()) candidate->remember_password(pwd);
    stagedWallet_ = std::move(candidate);
    if (outputLock) stagedLock_ = std::move(outputLock);
    stagedPath_ = output;
    stagedMigrationNeeded_ = false;
    return true;
}

void WalletModel::beginQuiesce()
{
    if (quiescing_ && (!quiesceAction_.failed() || !quiesceAction_.reapIfFinished())) return;
    if (walletOperationCode_ == QStringLiteral("wallet_sync_stop_failed")) {
        walletOperationCode_.clear();
        walletOperationError_.clear();
    }
    quiescing_ = true;
    if (refreshTimer_) refreshTimer_->stop();
    ++walletGeneration_;
    unsubscribeFromCoreSignals();
    if (nodeServices_.events) nodeServices_.events->clear_chain_mutation_event_callback();
    { std::lock_guard<std::mutex> lock(rescanMutex_); pendingRescan_ = false; }
    if (wallet_) wallet_->abort_scan();
    quiesceAction_.start([this]() {
        refreshAction_.wait();
        if (syncWorker_) syncWorker_->stopAndDrain();
        waitForRescanThread();
    });
}

bool WalletModel::isQuiesced()
{
    if (!quiescing_ || !quiesceAction_.reapIfFinished()) return false;
    if (quiesceAction_.failed()) return walletFailure(QStringLiteral("wallet_sync_stop_failed"), walletFilePath_,
        tr("The wallet synchronization stop failed. Wallet and database ownership are retained; retry cleanup."));
    return true;
}

void WalletModel::resumeWalletActivity()
{
    quiescing_ = false;
    refreshPending_ = false;
    refreshRequested_ = false;
    refreshSnapshot_.reset();
    { std::lock_guard<std::mutex> lock(rescanMutex_); rescanInProgress_ = false; pendingRescan_ = false; }
    syncWorker_.reset();
    if (!isWalletLoaded() || !chain_state_) return;
    subscribeToCoreSignals();
    const std::weak_ptr<geocoin::node::ChainQueryService> query = nodeServices_.chain_query;
    syncWorker_ = std::make_shared<WalletSyncWorker>(wallet_, chain_state_, [query]() {
        if (const auto service = query.lock()) return service->get_mempool_transactions();
        return std::vector<geocoin::core::Transaction>{};
    });
    const std::weak_ptr<WalletSyncWorker> worker = syncWorker_;
    nodeServices_.events->set_chain_mutation_event_callback([worker](const auto& event) {
        if (auto live = worker.lock()) (void)live->enqueue(event);
    });
    // Reconcile the gap between detaching the old subscriber and binding this one.
    geocoin::services::ChainMutationEvent gap;
    gap.kind = geocoin::services::ChainMutationEventKind::RESCAN_REQUIRED;
    gap.gap.chain_transition_lost = true;
    (void)syncWorker_->enqueue(gap);
}

bool WalletModel::activateStagedWallet()
{
    if (!stagedWallet_ || stagedMigrationNeeded_) return false;
    if (walletDirty_) return walletFailure(QStringLiteral("unsaved_changes"), walletFilePath_, tr("Save the active wallet first."));
    if (isWalletLoaded() && !isQuiesced()) return walletFailure(QStringLiteral("wallet_busy"), walletFilePath_, tr("Stop mining and wallet synchronization before switching."));
    wallet_.reset();
    walletLock_.reset();
    wallet_ = std::move(stagedWallet_);
    walletLock_ = std::move(stagedLock_);
    walletFilePath_ = std::move(stagedPath_);
    walletDirty_ = false;
    haveCachedBestBlockHash_ = false;
    applyWalletRefresh(RefreshSnapshot{});
    QSettings settings;
    settings.setValue(walletSettingsKey(dataDirectory_), walletFilePath_);
    settings.sync();
    resumeWalletActivity();
    updateBalance();
    transactionTableModel->updateTransactions();
    addressTableModel->updateAddresses();
    emitWalletStateChanged();
    return true;
}

void WalletModel::closeWallet()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (isWalletLoaded() && (!isQuiesced() || walletDirty_)) return;

    {
        std::lock_guard<std::mutex> lock(rescanMutex_);
        pendingRescan_ = false;
        pendingRescanFrom_ = 0;
    }
    waitForRescanThread();
    {
        std::lock_guard<std::mutex> lock(rescanMutex_);
        rescanInProgress_ = false;
        rescanTargetHeight_ = 0;
    }

    unsubscribeFromCoreSignals();
    syncWorker_.reset();
    wallet_.reset();
    walletLock_.reset();
    walletFilePath_.clear();
    wallet_ = std::make_shared<geocoin::wallet::Wallet>();
    applyWalletRefresh(RefreshSnapshot{});

    walletDirty_ = false;

    cachedBalance = 0;
    cachedUnconfirmedBalance = 0;
    cachedImmatureBalance = 0;
    cachedWatchOnlyBalance = 0;
    cachedWatchOnlyUnconfirmedBalance = 0;
    cachedWatchOnlyImmatureBalance = 0;
    transactionUiUpdateQueued_ = false;

    emit balanceChanged(0, 0, 0);
    emit watchOnlyBalanceChanged(0, 0, 0);

    transactionTableModel->updateTransactions();
    addressTableModel->updateAddresses();
    emitWalletStateChanged();
}

bool WalletModel::saveWallet(const QString& password, QString* error)
{
    if (error) {
        error->clear();
    }
    if (!isWalletLoaded()) {
        if (error) *error = tr("No wallet is loaded");
        return false;
    }
    if (walletFilePath_.isEmpty()) {
        if (error) *error = tr("Wallet path is not configured");
        return false;
    }

    walletDirty_ = true;
    std::string pwd;
    GUIUtil::ScopedStringCleanser passwordCleanser(pwd);
    if (wallet_->has_password_protection()) {
        if (password.isEmpty()) {
            if (error) *error = tr("Wallet password is required to save changes");
            return false;
        }
        pwd = password.toStdString();
        if (!(wallet_->is_vault() ? wallet_->unlock(pwd, 60) : wallet_->verify_password(pwd))) {
            if (error) *error = tr("Incorrect wallet password");
            return false;
        }
    }

    auto backupLock = lockBackupDestination(walletFilePath_, error);
    if (!backupLock) return false;
    const bool success = wallet_->save_to_file(walletFilePath_.toStdString(), pwd);
    if (!success) {
        if (error) *error = tr("Failed to save wallet to %1").arg(walletFilePath_);
        return false;
    }

    walletDirty_ = false;
    if (::geocoin::qt::debugEnabled()) {
        GEO_QT_LOG_DEBUG << "[WalletModel] Wallet saved to" << walletFilePath_;
    }
    return true;
}

bool WalletModel::encryptWallet(const QString& password, QString* error)
{
    if (error) error->clear();
    if (!isWalletLoaded() || walletFilePath_.isEmpty()) {
        if (error) *error = tr("No wallet is loaded");
        return false;
    }
    if (password.isEmpty() || wallet_->has_password_protection()) {
        if (error) *error = password.isEmpty()
            ? tr("Wallet password cannot be empty")
            : tr("Wallet is already encrypted");
        return false;
    }

    auto backupLock = lockBackupDestination(walletFilePath_, error);
    if (!backupLock) return false;
    walletDirty_ = true;
    std::string pwd = password.toStdString();
    GUIUtil::ScopedStringCleanser passwordCleanser(pwd);
    const bool success = wallet_->save_to_file(walletFilePath_.toStdString(), pwd);
    if (!success) {
        if (error) *error = tr("Failed to encrypt and save wallet");
        return false;
    }

    walletDirty_ = false;
    emit encryptionStatusChanged(1);
    emitWalletStateChanged();
    return true;
}

bool WalletModel::changeWalletPassphrase(const QString& oldPassword,
                                         const QString& newPassword,
                                         QString* error)
{
    if (error) error->clear();
    if (!isWalletLoaded() || walletFilePath_.isEmpty()) {
        if (error) *error = tr("No wallet is loaded");
        return false;
    }
    if (!wallet_->has_password_protection() || newPassword.isEmpty()) {
        if (error) *error = newPassword.isEmpty()
            ? tr("New wallet password cannot be empty")
            : tr("Wallet is not encrypted");
        return false;
    }

    std::string oldPwd = oldPassword.toStdString();
    GUIUtil::ScopedStringCleanser oldPasswordCleanser(oldPwd);
    if (!wallet_->verify_password(oldPwd)) {
        if (error) *error = tr("Incorrect current wallet password");
        return false;
    }
    std::string newPwd = newPassword.toStdString();
    GUIUtil::ScopedStringCleanser newPasswordCleanser(newPwd);
    auto backupLock = lockBackupDestination(walletFilePath_, error);
    if (!backupLock) return false;
    walletDirty_ = true;

    const bool success = wallet_->change_password(
        walletFilePath_.toStdString(),
        oldPwd,
        newPwd
    );
    if (!success) {
        if (wallet_->last_save_status() == geocoin::wallet::Wallet::SaveStatus::COMMITTED_NOT_DURABLE) {
            if (error) *error = tr("The new password is active, but saving could not be confirmed. Save the wallet again using the new password.");
            emit encryptionStatusChanged(1);
            emitWalletStateChanged();
        } else if (error) {
            *error = tr("Failed to save wallet with the new password");
        }
    } else {
        walletDirty_ = false;
        emit encryptionStatusChanged(1);
        emitWalletStateChanged();
    }

    return success;
}

bool WalletModel::backupWallet(const QString& filename,
                               const QString& password,
                               QString* error) const
{
    if (error) error->clear();
    if (!isWalletLoaded() || filename.isEmpty()) {
        if (error) *error = tr("No wallet is loaded or backup path is empty");
        return false;
    }
    const QString destination = canonicalWalletPath(filename);
    if (destination.isEmpty() || destination == walletFilePath_) {
        if (error) *error = tr("Choose a different, valid backup filename.");
        return false;
    }
    QLockFile destinationLock(walletLockPath(destination));
    destinationLock.setStaleLockTime(0);
    if (!destinationLock.tryLock(0)) {
        if (error) *error = tr("Backup destination is locked or cannot be locked: %1").arg(destination);
        return false;
    }

    std::string pwd;
    GUIUtil::ScopedStringCleanser passwordCleanser(pwd);
    if (wallet_->has_password_protection()) {
        pwd = password.toStdString();
        if (pwd.empty() || !wallet_->verify_password(pwd)) {
            if (error) *error = tr("Incorrect wallet password");
            return false;
        }
    }
    auto backupLock = lockBackupDestination(destination, error);
    if (!backupLock) return false;
    const bool success = wallet_->save_to_file(destination.toStdString(), pwd);
    if (!success && error) {
        *error = tr("Failed to write wallet backup to %1").arg(filename);
    }
    return success;
}

bool WalletModel::setRuntimeContext(
    std::shared_ptr<geocoin::wallet::Wallet> wallet,
    geocoin::node::NodeComponentHandle components
)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!wallet || !components.complete()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(rescanMutex_);
        pendingRescan_ = false;
        pendingRescanFrom_ = 0;
    }
    waitForRescanThread();
    {
        std::lock_guard<std::mutex> lock(rescanMutex_);
        rescanInProgress_ = false;
        rescanTargetHeight_ = 0;
    }

    wallet_ = std::move(wallet);
    chain_state_ = std::move(components.chain_state);
    mempool_ = std::move(components.mempool);
    nodeServices_ = std::move(components.services);

    haveCachedBestBlockHash_ = false;
    cachedBestBlockHash_ = geocoin::Hash256{};
    transactionUiUpdateQueued_ = false;
    transactionRefreshPending_ = true;
    walletDirty_ = false;

    cachedNumBlocks = 0;
    updateBalance();
    if (transactionTableModel) {
        transactionTableModel->updateTransactions();
        transactionRefreshPending_ = false;
    }
    if (addressTableModel) {
        addressTableModel->updateAddresses();
    }
    emitWalletStateChanged();
    return true;
}

void WalletModel::setDataDirectory(const QString& dataDir)
{
    dataDirectory_ = QDir(dataDir).absolutePath();
}

bool WalletModel::rememberMultisigConfig(
    const geocoin::wallet::MultisigConfig& config,
    QString* error
) {
    geocoin::wallet::WalletApplicationService useCases(
        wallet_, chain_state_, nodeServices_.mutations
    );
    std::string serviceError;
    if (!useCases.remember_multisig_config(config, serviceError)) {
        if (error) {
            *error = QString::fromStdString(serviceError);
        }
        return false;
    }
    walletDirty_ = true;
    if (addressTableModel) {
        addressTableModel->updateAddresses();
    }
    return true;
}

void WalletModel::updateBalance()
{
    if (quiescing_) return;
    refreshRequested_ = true;
    pollBalanceChanged();
}

void WalletModel::pollBalanceChanged()
{
    if (quiescing_) return;
    if (refreshPending_) {
        finishWalletRefresh();
        if (refreshPending_ || quiescing_) return;
    }
    if (!refreshAction_.reapIfFinished()) return;
    if (!wallet_) { applyWalletRefresh(RefreshSnapshot{}); return; }

    const auto wallet = wallet_;
    const auto chain = chain_state_;
    const auto query = nodeServices_.chain_query;
    const auto info = nodeServices_.info;
    const auto previous = transactionTableModel->snapshot();
    const auto oldHeight = cachedNumBlocks;
    const auto oldTip = cachedBestBlockHash_;
    const bool hadTip = haveCachedBestBlockHash_;
    const bool postIbd = lastIbdState && !didPostIbdRescan;
    refreshSnapshot_ = std::make_shared<RefreshSnapshot>();
    const auto result = refreshSnapshot_;
    refreshGeneration_ = walletGeneration_;
    refreshRequested_ = false;
    refreshPending_ = refreshAction_.start([wallet, chain, query, info, previous,
        oldHeight, oldTip, hadTip, postIbd, result]() {
        if (chain) {
            result->height = chain->get_height();
            geocoin::core::Block tip;
            if (query && query->get_block_by_height(result->height, tip)) {
                result->tip = tip.get_hash();
                result->haveTip = true;
            }
            if (hadTip && query) {
                geocoin::core::Block prior;
                result->reorg = result->height < oldHeight ||
                    (query->get_block_by_height(oldHeight, prior) && prior.get_hash() != oldTip);
            }
            result->ibd = info && info->is_initial_block_download();
            if ((!hadTip && (wallet->get_key_count() > 0 || !wallet->get_watch_only_addresses().empty())) ||
                (postIbd && !result->ibd) || result->reorg) {
                const auto scanned = wallet->get_last_scanned_height();
                geocoin::Hash256 anchor{};
                const bool matches = scanned <= result->height &&
                    chain->get_active_block_hash(scanned, anchor) &&
                    anchor == wallet->get_last_scanned_block_hash();
                result->rescan = !matches || scanned < result->height;
                result->rescanFrom = matches ? scanned + 1 : 0;
            }
            const auto pending = query ? query->get_mempool_transactions() :
                std::vector<geocoin::core::Transaction>{};
            bool valid = true;
            auto sum = [&](const auto& addresses, size_t offset) {
                for (const auto& address : addresses) {
                    const auto funds = geocoin::wallet::classify_address_funds(*chain, address, pending);
                    const geocoin::Amount amounts[] = {funds.available, funds.unconfirmed, funds.immature};
                    for (size_t i = 0; i < 3; ++i) {
                        geocoin::Amount next = 0;
                        if (!funds.valid || amounts[i] < 0 ||
                            !geocoin::checked_add_amount(result->balances[offset + i], amounts[i], next)) {
                            valid = false;
                            return;
                        }
                        result->balances[offset + i] = next;
                    }
                }
            };
            sum(wallet->get_spendable_addresses(), 0);
            if (valid) sum(wallet->get_watch_only_addresses(), 3);
            if (!valid) {
                std::fill(std::begin(result->balances), std::end(result->balances), 0);
                qWarning() << "Wallet balance snapshot rejected invalid or overflowing amounts";
            }
        }
        result->transactions = TransactionTableModel::readSnapshot(wallet, chain, previous);
    });
    if (!refreshPending_) {
        emit walletHistoryError(tr("Wallet refresh could not start [wallet_refresh_start_failed]. The previous snapshot is retained."));
        return;
    }
    refreshTimer_->start();
}

void WalletModel::applyWalletRefresh(const RefreshSnapshot& snapshot)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const bool spendableChanged = cachedBalance != snapshot.balances[0] ||
        cachedUnconfirmedBalance != snapshot.balances[1] || cachedImmatureBalance != snapshot.balances[2];
    const bool watchedChanged = cachedWatchOnlyBalance != snapshot.balances[3] ||
        cachedWatchOnlyUnconfirmedBalance != snapshot.balances[4] ||
        cachedWatchOnlyImmatureBalance != snapshot.balances[5];
    cachedBalance = snapshot.balances[0];
    cachedUnconfirmedBalance = snapshot.balances[1];
    cachedImmatureBalance = snapshot.balances[2];
    cachedWatchOnlyBalance = snapshot.balances[3];
    cachedWatchOnlyUnconfirmedBalance = snapshot.balances[4];
    cachedWatchOnlyImmatureBalance = snapshot.balances[5];
    if (spendableChanged) emit balanceChanged(cachedBalance, cachedUnconfirmedBalance, cachedImmatureBalance);
    if (watchedChanged) emit watchOnlyBalanceChanged(cachedWatchOnlyBalance,
        cachedWatchOnlyUnconfirmedBalance, cachedWatchOnlyImmatureBalance);
    transactionTableModel->applySnapshot(snapshot.transactions);
}

void WalletModel::finishWalletRefresh()
{
    if (quiescing_ || !refreshPending_ || !refreshAction_.reapIfFinished()) return;
    refreshTimer_->stop();
    refreshPending_ = false;
    if (refreshGeneration_ != walletGeneration_) return;
    if (refreshAction_.failed()) {
        emit walletHistoryError(tr("Wallet refresh failed [wallet_refresh_failed]. The previous snapshot is retained."));
    } else {
        const auto snapshot = refreshSnapshot_;
        cachedNumBlocks = snapshot->height;
        cachedBestBlockHash_ = snapshot->tip;
        haveCachedBestBlockHash_ = snapshot->haveTip;
        if (lastIbdState && !snapshot->ibd) didPostIbdRescan = true;
        lastIbdState = snapshot->ibd;
        applyWalletRefresh(*snapshot);
        if (snapshot->rescan) startWalletRescanAsync(snapshot->rescanFrom, snapshot->height);
    }
    if (refreshRequested_) pollBalanceChanged();
}

void WalletModel::updateTransaction()
{
    if (quiescing_) return;
    // Balance and number of transactions might have changed
    // Ставим флаг: обновить данные на следующем тике таймера.
    fForceCheckBalanceChanged = true;
    transactionRefreshPending_ = true;
    updateBalance();
}

void WalletModel::notifyTransactionChanged(const QByteArray &txData, int height, quint64 timestamp)
{
    // This is called from mining/network thread via QMetaObject::invokeMethod
    // It's safe to access wallet here because we're in the main thread now

    if (!wallet_ || !chain_state_) {
        return;
    }

    fForceCheckBalanceChanged = true;
    transactionRefreshPending_ = true;
    if (transactionUiUpdateQueued_) {
        return;
    }
    transactionUiUpdateQueued_ = true;

    // Троттлим всплески сигналов: вместо немедленного тяжелого обновления
    // на каждую транзакцию, объединяем их в один refresh.
    QTimer::singleShot(100, this, [this]() {
        transactionUiUpdateQueued_ = false;
        pollBalanceChanged();
    });
}

bool WalletModel::inspectCommitmentDraft(
    const QString& draftJson,
    CommitmentDraftInspection& inspection,
    QString& error
) const {
    ParsedCommitmentDraft parsed;
    if (!parseCommitmentDraft(draftJson, chain_state_, parsed, error)) {
        return false;
    }

    CommitmentDraftInspection trusted;
    switch (parsed.intent.kind) {
        case CommitmentIntentKind::DIRECT_SPEND:
            trusted.operation = tr("Multisig spend");
            break;
        case CommitmentIntentKind::DELEGATION_PROOF:
            trusted.operation = tr("Delegation proof");
            break;
        case CommitmentIntentKind::HIERARCHICAL_SPEND:
            trusted.operation = tr("Hierarchical spend");
            break;
        case CommitmentIntentKind::REVOKE_DELEGATION:
            trusted.operation = tr("Delegation revocation");
            break;
    }
    trusted.multisigAddress = QString::fromStdString(
        parsed.intent.signer_config.address.str());
    trusted.signingHash = hashToHexString(parsed.intent.signing_hash);

    QStringList lines;
    lines << tr("Inputs: %1").arg(parsed.transaction.inputs.size());
    for (size_t i = 0; i < parsed.transaction.outputs.size(); ++i) {
        const auto& output = parsed.transaction.outputs[i];
        lines << tr("Output %1: %2 GC -> %3")
                     .arg(i)
                     .arg(GUIUtil::formatAmount(output.value))
                     .arg(QString::fromStdString(output.address.str()));
    }
    if (parsed.intent.kind == CommitmentIntentKind::DELEGATION_PROOF) {
        lines << tr("Delegated address: %1")
                     .arg(QString::fromStdString(
                         parsed.transaction.delegated_multisig_address.str()));
        lines << tr("Maximum total: %1 GC")
                     .arg(GUIUtil::formatAmount(
                         parsed.transaction.max_total_delegated));
        lines << tr("Maximum per transaction: %1 GC")
                     .arg(GUIUtil::formatAmount(
                         parsed.transaction.max_per_transaction));
        lines << tr("Maximum balance share: %1%")
                     .arg(parsed.transaction.max_percent_of_balance);
    }
    trusted.details = lines.join(QLatin1Char('\n'));
    inspection = std::move(trusted);
    return true;
}

void WalletModel::createMultisigDraft(const QString& configJson,
                                      const QString& toAddress,
                                      qint64 amount,
                                      qint64 fee)
{
    if (!chain_state_) {
        emit multisigDraftFailed(tr("Chain state is unavailable"));
        return;
    }
    MultisigConfig config;
    QString error;
    if (!loadMultisigConfigFromJson(configJson, config, error)) {
        emit multisigDraftFailed(error);
        return;
    }

    // Добавляем мультисиг адрес в кошелек для отслеживания
    if (!rememberMultisigConfig(config, &error)) {
        emit multisigDraftFailed(error);
        return;
    }

    if (amount <= 0 || fee < 0) {
        emit multisigDraftFailed(tr("Invalid amount or fee"));
        return;
    }
    const qint64 effectiveFee = qMax(fee, multisigRelayFee(config.n));
    qint64 total = 0;
    if (!GUIUtil::addAmounts(amount, effectiveFee, total)) {
        emit multisigDraftFailed(tr("Amount plus fee is too large"));
        return;
    }
    const Amount amountAngles = static_cast<Amount>(amount);
    const Amount feeAngles = static_cast<Amount>(effectiveFee);
    Address recipient;
    try {
        recipient = Address(toAddress.toStdString());
    } catch (const std::exception& ex) {
        emit multisigDraftFailed(tr("Invalid recipient address: %1").arg(ex.what()));
        return;
    }
    MultisigDraftResult result;
    std::string serviceError;
    if (!create_multisig_draft(config,
                               recipient,
                               amountAngles,
                               feeAngles,
                               chain_state_,
                               result,
                               serviceError,
                               pendingTransactions())) {
        emit multisigDraftFailed(QString::fromStdString(serviceError));
        return;
    }
    QJsonObject draftJson;
    draftJson["amount_schema"] = 2;
    draftJson["type"] = "multisig_spend";
    draftJson["config"] = configJson;
    draftJson["multisig_address"] = QString::fromStdString(config.address.str());
    draftJson["to_address"] = toAddress;
    draftJson["amount_angles"] = QString::number(amountAngles);
    draftJson["fee_angles"] = QString::number(result.fee_used);
    draftJson["amount"] = GUIUtil::formatAmount(amountAngles);
    draftJson["fee"] = GUIUtil::formatAmount(result.fee_used);
    draftJson["m"] = config.m;
    draftJson["n"] = config.n;
    draftJson["spending_tx_hash"] = hashToHexString(result.spending_tx_hash);
    draftJson["tx_hex"] = bytesToHexString(result.transaction.serialize());
    const Amount change = result.total_input - amountAngles - result.fee_used;
    draftJson["total_input_angles"] = QString::number(result.total_input);
    draftJson["change_angles"] = QString::number(change);
    draftJson["total_input"] = GUIUtil::formatAmount(result.total_input);
    draftJson["change"] = GUIUtil::formatAmount(change);
    QJsonDocument doc(draftJson);
    emit multisigDraftReady(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)),
                            hashToHexString(result.spending_tx_hash));
}

void WalletModel::createAndSignMultisig(const QString& configJson,
                                        const QString& toAddress,
                                        qint64 amount,
                                        qint64 fee,
                                        const QString& privateKeyHex)
{
    try {
        // 1. Проверяем доступность chain_state
        if (!chain_state_) {
            emit createAndSignMultisigFailed(tr("Chain state is unavailable"));
            return;
        }

        // 2. Загружаем конфигурацию
        MultisigConfig config;
        QString error;
        if (!loadMultisigConfigFromJson(configJson, config, error)) {
            emit createAndSignMultisigFailed(error);
            return;
        }

        // Keep watch/signing metadata in the wallet. Consensus validation uses
        // the participant preimage carried by the spend itself.
        if (!rememberMultisigConfig(config, &error)) {
            emit createAndSignMultisigFailed(error);
            return;
        }

        // 3. Парсим приватный ключ
        bytes privateKey;
        geocoin::utils::ScopedContainerCleanser<bytes> privateKeyCleanser(privateKey);
        if (!hexStringToBytes(privateKeyHex, privateKey) || privateKey.size() != 32) {
            emit createAndSignMultisigFailed(tr("Invalid private key format (expected 32 bytes hex)"));
            return;
        }

        // 4. Создаем черновик транзакции (локально)
        if (amount <= 0 || fee < 0) {
            emit createAndSignMultisigFailed(tr("Invalid amount or fee"));
            return;
        }
        const qint64 effectiveFee = qMax(fee, multisigRelayFee(config.n));
        qint64 total = 0;
        if (!GUIUtil::addAmounts(amount, effectiveFee, total)) {
            emit createAndSignMultisigFailed(tr("Amount plus fee is too large"));
            return;
        }
        const Amount amountAngles = static_cast<Amount>(amount);
        const Amount feeAngles = static_cast<Amount>(effectiveFee);
        Address recipient;
        try {
            recipient = Address(toAddress.toStdString());
        } catch (const std::exception& ex) {
            emit createAndSignMultisigFailed(tr("Invalid recipient address: %1").arg(ex.what()));
            return;
        }

        MultisigDraftResult draftResult;
        std::string serviceError;
        if (!create_multisig_draft(config,
                                   recipient,
                                   amountAngles,
                                   feeAngles,
                                   chain_state_,
                                   draftResult,
                                   serviceError,
                                   pendingTransactions())) {
            emit createAndSignMultisigFailed(QString::fromStdString(serviceError));
            return;
        }

        Hash256 spendingHash = draftResult.spending_tx_hash;

        CommitmentResult commitmentResult;
        if (!sign_configured_angle_commitment(
                config,
                spendingHash,
                privateKey,
                commitmentResult,
                serviceError)) {
            emit createAndSignMultisigFailed(QString::fromStdString(serviceError));
            return;
        }

        // 8. Отправляем МОЙ commitment в блокчейн
        if (!submitTransaction(commitmentResult.commitment_tx)) {
            emit createAndSignMultisigFailed(tr("Failed to broadcast commitment to network"));
            return;
        }

        Hash256 myCommitmentTxid = commitmentResult.commitment_hash;

        // 9. Сохраняем draft для финализации позже
        QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dataDir.isEmpty()) {
            dataDir = QDir::homePath() + "/.geocoin";
        }
        QDir().mkpath(dataDir);

        QJsonObject draftJson;
        draftJson["amount_schema"] = 2;
        draftJson["type"] = "multisig_spend";
        draftJson["spending_hash"] = hashToHexString(spendingHash);
        draftJson["multisig_spend_hex"] = bytesToHexString(draftResult.transaction.serialize());
        draftJson["my_commitment_txid"] = hashToHexString(myCommitmentTxid);
        draftJson["multisig_address"] = QString::fromStdString(config.address.str());
        draftJson["m"] = config.m;
        draftJson["n"] = config.n;
        draftJson["to_address"] = toAddress;
        draftJson["amount_angles"] = QString::number(amountAngles);
        draftJson["fee_angles"] = QString::number(draftResult.fee_used);
        draftJson["amount"] = GUIUtil::formatAmount(amountAngles);
        draftJson["fee"] = GUIUtil::formatAmount(draftResult.fee_used);

        QString draftPath = QDir(dataDir).filePath(
            QString("multisig_draft_%1.json").arg(hashToHexString(spendingHash).left(16))
        );

        QString writeError;
        if (!GUIUtil::writeFileAtomically(
                draftPath, QJsonDocument(draftJson).toJson(), writeError)) {
            qWarning() << "Unable to persist multisig draft safely:" << writeError;
        }

        // 10. Уведомляем UI
        QString spendingHashStr = hashToHexString(spendingHash);
        QString commitmentTxidStr = hashToHexString(myCommitmentTxid);
        QString multisigSpendHex = bytesToHexString(draftResult.transaction.serialize());

        emit createAndSignMultisigReady(spendingHashStr, commitmentTxidStr, multisigSpendHex);

    } catch (const std::exception& ex) {
        emit createAndSignMultisigFailed(QString::fromStdString(ex.what()));
    }
}

void WalletModel::createAndSignMultisigWithWalletKey(const QString& configJson,
                                                     const QString& toAddress,
                                                     qint64 amount,
                                                     qint64 fee,
                                                     const QString& walletAddress,
                                                     const QString& walletPassword)
{
    geocoin::bytes privateKey;
    geocoin::utils::ScopedContainerCleanser<geocoin::bytes> privateKeyCleanser(privateKey);
    QString error;
    if (!getAuthorizedWalletPrivateKey(walletAddress, walletPassword, privateKey, error)) {
        emit createAndSignMultisigFailed(error);
        return;
    }

    QString privateKeyHex = bytesToHexString(privateKey);
    GUIUtil::ScopedQStringCleanser privateKeyHexCleanser(privateKeyHex);
    createAndSignMultisig(configJson, toAddress, amount, fee, privateKeyHex);
    GUIUtil::cleanseQString(privateKeyHex);
}

void WalletModel::signMultisigCommitment(const QString& draftJson,
                                         const QString& keySource,
                                         const QString& keyData,
                                         bool broadcast)
{
    signCommitment(draftJson, keySource, keyData, broadcast);
}

void WalletModel::finalizeMultisigSpend(const QString& draftJson,
                                        const QStringList& commitmentTxids,
                                        bool broadcast)
{
    if (::geocoin::qt::debugEnabled()) {
        GEO_QT_LOG_DEBUG << "[WalletModel::finalizeMultisigSpend] START";
        GEO_QT_LOG_DEBUG << "[WalletModel] chain_state_ pointer:" << chain_state_.get();
    }

    ParsedCommitmentDraft parsed;
    QString parseError;
    if (!parseCommitmentDraft(draftJson, chain_state_, parsed, parseError) ||
        parsed.intent.kind != CommitmentIntentKind::DIRECT_SPEND) {
        emit multisigFinalizeFailed(parseError.isEmpty()
            ? tr("Draft is not a direct multisig spend")
            : parseError);
        return;
    }
    core::Transaction draftTx = std::move(parsed.transaction);
    const uint8_t m = draftTx.m;

    // Собираем commitment hashes
    std::vector<Hash256> hashes;
    hashes.reserve(commitmentTxids.size());
    for (const QString& txid : commitmentTxids) {
        Hash256 hash;
        if (!hexStringToHash(txid.trimmed(), hash)) {
            emit multisigFinalizeFailed(tr("Invalid commitment txid: %1").arg(txid));
            return;
        }
        hashes.push_back(hash);
    }

    // Обычный MULTISIG_SPEND должен нести spend_participants (ончейн preimage конфига).
    bool isHierarchical = false;
    for (auto b : draftTx.delegation_proof_txid) {
        if (b != 0) { isHierarchical = true; break; }
    }
    if (!isHierarchical) {
        if (draftTx.spend_participants.size() != draftTx.n) {
            emit multisigFinalizeFailed(tr("Invalid multisig draft format.\n\n"
                                           "Please recreate the multisig draft with spend_participants."));
            return;
        }
    }

    // Финализируем транзакцию
    core::Transaction finalTx;
    std::string serviceError;
    if (!finalize_multisig_spend(draftTx,
                                 hashes,
                                 m,
                                 finalTx,
                                 serviceError)) {
        emit multisigFinalizeFailed(QString::fromStdString(serviceError));
        return;
    }

    // Логируем детали финальной транзакции для отладки
    if (::geocoin::qt::debugEnabled()) {
        GEO_QT_LOG_DEBUG << "[WalletModel] Final transaction details:";
        GEO_QT_LOG_DEBUG << "  Type:" << (int)finalTx.type;
        GEO_QT_LOG_DEBUG << "  Inputs:" << finalTx.inputs.size();
        GEO_QT_LOG_DEBUG << "  Outputs:" << finalTx.outputs.size();
        GEO_QT_LOG_DEBUG << "  M:" << (int)finalTx.m << "N:" << (int)finalTx.n;
        GEO_QT_LOG_DEBUG << "  Commitments:" << finalTx.angle_commitment_hashes.size();
    }

    // В прод-версии конфиг для обычного MULTISIG_SPEND проверяется trustless по spend_participants,
    // поэтому регистрация конфига на ноде не требуется.

    QString finalHex = bytesToHexString(finalTx.serialize());
    QString txid;

    if (broadcast) {
        if (::geocoin::qt::debugEnabled()) {
            GEO_QT_LOG_DEBUG << "[WalletModel] Attempting to broadcast final transaction...";
            GEO_QT_LOG_DEBUG << "  TX hex length:" << finalHex.length();
        }

        if (!submitTransaction(finalTx)) {
            // Получаем более детальную информацию об ошибке
            QString errorDetails = tr("Failed to broadcast final transaction.\n\nPossible causes:\n1. Multisig configuration is missing from the wallet\n2. The node rejected the transaction\n3. The network is unavailable");

            if (::geocoin::qt::debugEnabled()) {
                GEO_QT_LOG_DEBUG << "[WalletModel] ERROR: Broadcast failed!";
                GEO_QT_LOG_DEBUG << "  Multisig address:" << QString::fromStdString(draftTx.inputs.empty() ? "unknown" : "check logs");
            }

            emit multisigFinalizeFailed(errorDetails);
            return;
        }
        txid = hashToHexString(finalTx.get_hash());
        if (::geocoin::qt::debugEnabled()) {
            GEO_QT_LOG_DEBUG << "[WalletModel] Broadcast successful! TXID:" << txid;
        }
    }

    emit multisigFinalizeReady(finalHex, txid);
}

// Статические callback-функции для сигналов кошелька.
static void NotifyTransactionChanged(WalletModel *walletmodel, geocoin::wallet::Wallet *wallet, const geocoin::Hash256 &hash)
{
    Q_UNUSED(wallet);
    Q_UNUSED(hash);
    // Вызывается из потока кошелька, поэтому обновление UI делаем через QueuedConnection.
    QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection);
}

void WalletModel::subscribeToCoreSignals()
{
    if (!wallet_) {
        return;
    }

    walletTransactionChangedConnection_.disconnect();
    walletScanFinishedConnection_.disconnect();

    // Keep an explicit connection handle so unsubscribe is deterministic across toolchains.
    const auto generation = walletGeneration_;
    walletTransactionChangedConnection_ = wallet_->NotifyTransactionChanged.connect(
        [this, generation](geocoin::wallet::Wallet*, const geocoin::Hash256&) {
            QMetaObject::invokeMethod(this, [this, generation]() {
                if (!quiescing_ && generation == walletGeneration_) updateTransaction();
            }, Qt::QueuedConnection);
        }
    );
    walletScanFinishedConnection_ = wallet_->NotifyScanFinished.connect(
        [this, generation](geocoin::wallet::Wallet*, const geocoin::wallet::WalletScanResult& result) {
            const QString error = QString::fromStdString(result.error);
            QMetaObject::invokeMethod(this, [this, error, generation]() {
                if (quiescing_ || generation != walletGeneration_) return;
                emit walletHistoryError(error);
                updateTransaction();
            }, Qt::QueuedConnection);
        });
}

QString WalletModel::getHistoryScanError() const
{
    return wallet_ ? QString::fromStdString(wallet_->get_last_scan_result().error) : QString();
}

void WalletModel::unsubscribeFromCoreSignals()
{
    if (!wallet_) {
        return;
    }

    walletTransactionChangedConnection_.disconnect();
    walletScanFinishedConnection_.disconnect();
}

// Delegation proof workflow
void WalletModel::createDelegationProofDraft(const QString& primaryConfigJson,
                                             const QString& delegatedConfigJson,
                                             quint64 validFrom,
                                             quint64 validUntil,
                                             quint32 nonce,
                                             qint64 maxTotal,
                                             qint64 maxPerTx,
                                             int maxPercent)
{
    // Load primary config
    MultisigConfig primaryConfig;
    QString error;
    if (!loadMultisigConfigFromJson(primaryConfigJson, primaryConfig, error)) {
        emit delegationProofDraftFailed(error);
        return;
    }

    // Load delegated config
    MultisigConfig delegatedConfig;
    if (!loadMultisigConfigFromJson(delegatedConfigJson, delegatedConfig, error)) {
        emit delegationProofDraftFailed(error);
        return;
    }

    if (maxPercent < 0 || maxPercent > 100) {
        emit delegationProofDraftFailed(tr("Invalid delegation limits"));
        return;
    }
    DelegationProofRequest request;
    request.primary_config = primaryConfig;
    request.delegated_config = delegatedConfig;
    request.valid_from = validFrom;
    request.valid_until = validUntil;
    request.nonce = nonce;
    request.max_total = static_cast<Amount>(maxTotal);
    request.max_per_transaction = static_cast<Amount>(maxPerTx);
    request.max_percent_of_balance = static_cast<uint8_t>(maxPercent);

    DelegationProofResult proofResult;
    std::string serviceError;
    if (!create_delegation_proof(request, chain_state_, proofResult, serviceError, pendingTransactions())) {
        emit delegationProofDraftFailed(QString::fromStdString(serviceError));
        return;
    }
    const core::Transaction& delegationProof = proofResult.transaction;
    const Hash256& delegationHash = proofResult.delegation_hash;

    // Create draft JSON
    QJsonObject draftJson;
    draftJson["amount_schema"] = 2;
    draftJson["type"] = "delegation_proof";
    draftJson["primary_config"] = primaryConfigJson;
    draftJson["delegated_config"] = delegatedConfigJson;
    draftJson["primary_address"] = QString::fromStdString(primaryConfig.address.str());
    draftJson["delegated_address"] = QString::fromStdString(delegatedConfig.address.str());
    draftJson["primary_m"] = primaryConfig.m;
    draftJson["primary_n"] = primaryConfig.n;
    draftJson["delegated_m"] = delegatedConfig.m;
    draftJson["delegated_n"] = delegatedConfig.n;
    draftJson["valid_from"] = QString::number(validFrom);
    draftJson["valid_until"] = QString::number(validUntil);
    draftJson["nonce"] = static_cast<int>(nonce);
    draftJson["max_total_angles"] = QString::number(maxTotal);
    draftJson["max_per_tx_angles"] = QString::number(maxPerTx);
    draftJson["max_total"] = GUIUtil::formatAmount(maxTotal);
    draftJson["max_per_tx"] = GUIUtil::formatAmount(maxPerTx);
    draftJson["max_percent"] = maxPercent;
    draftJson["delegation_hash"] = hashToHexString(delegationHash);
    draftJson["tx_hex"] = bytesToHexString(delegationProof.serialize());
    draftJson["timestamp"] = QString::number(delegationProof.timestamp);  // ✅ ИСПРАВЛЕНИЕ: Сохраняем timestamp

    QJsonDocument doc(draftJson);
    emit delegationProofDraftReady(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)),
                                   hashToHexString(delegationHash));
}

void WalletModel::createAndSignDelegationProof(const QString& primaryConfigJson,
                                               const QString& delegatedConfigJson,
                                               quint64 validFrom,
                                               quint64 validUntil,
                                               quint32 nonce,
                                               qint64 maxTotal,
                                               qint64 maxPerTx,
                                               int maxPercent,
                                               const QString& privateKeyHex)
{
    try {
        // 1. Load primary config
        MultisigConfig primaryConfig;
        QString error;
        if (!loadMultisigConfigFromJson(primaryConfigJson, primaryConfig, error)) {
            emit createAndSignDelegationProofFailed(error);
            return;
        }

        // 2. Load delegated config
        MultisigConfig delegatedConfig;
        if (!loadMultisigConfigFromJson(delegatedConfigJson, delegatedConfig, error)) {
            emit createAndSignDelegationProofFailed(error);
            return;
        }

        // 3. Parse private key
        bytes privateKey;
        geocoin::utils::ScopedContainerCleanser<bytes> privateKeyCleanser(privateKey);
        if (!hexStringToBytes(privateKeyHex, privateKey) || privateKey.size() != 32) {
            emit createAndSignDelegationProofFailed(tr("Invalid private key format (expected 32 bytes hex)"));
            return;
        }

        if (maxPercent < 0 || maxPercent > 100) {
            emit createAndSignDelegationProofFailed(tr("Invalid delegation limits"));
            return;
        }
        DelegationProofRequest request;
        request.primary_config = primaryConfig;
        request.delegated_config = delegatedConfig;
        request.valid_from = validFrom;
        request.valid_until = validUntil;
        request.nonce = nonce;
        request.max_total = static_cast<Amount>(maxTotal);
        request.max_per_transaction = static_cast<Amount>(maxPerTx);
        request.max_percent_of_balance = static_cast<uint8_t>(maxPercent);

        DelegationProofResult proofResult;
        std::string serviceError;
        if (!create_delegation_proof(request, chain_state_, proofResult, serviceError, pendingTransactions())) {
            emit createAndSignDelegationProofFailed(QString::fromStdString(serviceError));
            return;
        }
        core::Transaction delegationProof = std::move(proofResult.transaction);
        const Hash256 delegationHash = proofResult.delegation_hash;

        CommitmentResult commitmentResult;
        if (!sign_configured_angle_commitment(
                primaryConfig,
                delegationHash,
                privateKey,
                commitmentResult,
                serviceError)) {
            emit createAndSignDelegationProofFailed(QString::fromStdString(serviceError));
            return;
        }
        core::Transaction& myCommitment = commitmentResult.commitment_tx;

        // 8. Отправляем МОЙ commitment в блокчейн
        if (!submitTransaction(myCommitment)) {
            emit createAndSignDelegationProofFailed(tr("Failed to broadcast commitment to network"));
            return;
        }

        Hash256 myCommitmentTxid = myCommitment.get_hash();

        // 9. Сохраняем draft для финализации позже
        QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dataDir.isEmpty()) {
            dataDir = QDir::homePath() + "/.geocoin";
        }
        QDir().mkpath(dataDir);

        QJsonObject draftJson;
        draftJson["type"] = "delegation_proof";
        draftJson["delegation_hash"] = hashToHexString(delegationHash);
        draftJson["delegation_proof_hex"] = bytesToHexString(delegationProof.serialize());
        draftJson["tx_hex"] = draftJson["delegation_proof_hex"];
        draftJson["my_commitment_txid"] = hashToHexString(myCommitmentTxid);
        draftJson["primary_address"] = QString::fromStdString(primaryConfig.address.str());
        draftJson["primary_m"] = primaryConfig.m;
        draftJson["primary_n"] = primaryConfig.n;
        draftJson["timestamp"] = QString::number(delegationProof.timestamp);

        QString draftPath = QDir(dataDir).filePath(
            QString("delegation_draft_%1.json").arg(hashToHexString(delegationHash).left(16))
        );

        QString writeError;
        if (!GUIUtil::writeFileAtomically(
                draftPath, QJsonDocument(draftJson).toJson(), writeError)) {
            qWarning() << "Unable to persist delegation draft safely:" << writeError;
        }

        // 10. Уведомляем UI
        QJsonDocument draftDoc(draftJson);
        emit createAndSignDelegationProofReady(
            hashToHexString(delegationHash),
            hashToHexString(myCommitmentTxid),
            QString::fromUtf8(draftDoc.toJson(QJsonDocument::Compact))
        );

    } catch (const std::exception& ex) {
        emit createAndSignDelegationProofFailed(QString::fromStdString(ex.what()));
    }
}

void WalletModel::createAndSignDelegationProofWithWalletKey(
    const QString& primaryConfigJson,
    const QString& delegatedConfigJson,
    quint64 validFrom,
    quint64 validUntil,
    quint32 nonce,
    qint64 maxTotal,
    qint64 maxPerTx,
    int maxPercent,
    const QString& walletAddress,
    const QString& walletPassword)
{
    geocoin::bytes privateKey;
    geocoin::utils::ScopedContainerCleanser<geocoin::bytes> privateKeyCleanser(privateKey);
    QString error;
    if (!getAuthorizedWalletPrivateKey(walletAddress, walletPassword, privateKey, error)) {
        emit createAndSignDelegationProofFailed(error);
        return;
    }

    QString privateKeyHex = bytesToHexString(privateKey);
    GUIUtil::ScopedQStringCleanser privateKeyHexCleanser(privateKeyHex);
    createAndSignDelegationProof(primaryConfigJson,
                                 delegatedConfigJson,
                                 validFrom,
                                 validUntil,
                                 nonce,
                                 maxTotal,
                                 maxPerTx,
                                 maxPercent,
                                 privateKeyHex);
    GUIUtil::cleanseQString(privateKeyHex);
}

void WalletModel::finalizeDelegationProof(const QString& draftJson,
                                          const QStringList& commitmentTxids,
                                          bool broadcast)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(draftJson.toUtf8(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        emit delegationProofFinalizeFailed(tr("Invalid draft JSON: %1").arg(parseError.errorString()));
        return;
    }

    QJsonObject obj = doc.object();
    QString txHex = obj.value("tx_hex").toString();

    if (txHex.isEmpty()) {
        txHex = obj.value("delegation_proof_hex").toString();
        if (txHex.isEmpty()) {
            emit delegationProofFinalizeFailed(tr("Draft JSON missing tx_hex"));
            return;
        }
    }

    core::Transaction draftTx;
    if (!deserializeTransaction(txHex, draftTx)) {
        emit delegationProofFinalizeFailed(tr("Invalid delegation proof transaction"));
        return;
    }

    std::vector<Hash256> commitmentHashes;
    commitmentHashes.reserve(commitmentTxids.size());
    for (const QString& txidStr : commitmentTxids) {
        Hash256 commitmentHash;
        if (!hexStringToHash(txidStr, commitmentHash)) {
            emit delegationProofFinalizeFailed(tr("Invalid commitment txid: %1").arg(txidStr));
            return;
        }
        commitmentHashes.push_back(commitmentHash);
    }

    core::Transaction finalTx;
    std::string serviceError;
    if (!finalize_delegation_proof(
            draftTx,
            commitmentHashes,
            finalTx,
            serviceError)) {
        emit delegationProofFinalizeFailed(QString::fromStdString(serviceError));
        return;
    }

    bytes finalData = finalTx.serialize();
    QString finalHex = bytesToHexString(finalData);

    QString txid;
    if (broadcast) {
        if (!submitTransaction(finalTx)) {
            emit delegationProofFinalizeFailed(tr("Delegation proof was rejected and not broadcast"));
            return;
        }

        txid = hashToHexString(finalTx.get_hash());
    }

    emit delegationProofFinalizeReady(finalHex, txid);
}

// Hierarchical spend workflow
void WalletModel::createHierarchicalSpendDraft(const QString& delegationProofTxid,
                                               const QString& delegatedConfigJson,
                                               const QString& toAddress,
                                               qint64 amount,
                                               qint64 fee)
{
    if (!chain_state_) {
        emit hierarchicalSpendDraftFailed(tr("Chain state is unavailable"));
        return;
    }

    MultisigConfig delegatedConfig;
    QString error;
    if (!loadMultisigConfigFromJson(delegatedConfigJson, delegatedConfig, error)) {
        emit hierarchicalSpendDraftFailed(error);
        return;
    }

    if (amount <= 0 || fee < 0) {
        emit hierarchicalSpendDraftFailed(tr("Invalid amount or fee"));
        return;
    }
    const Amount amountAngles = static_cast<Amount>(amount);
    const Amount feeAngles = static_cast<Amount>(qMax(
        fee,
        multisigRelayFee(delegatedConfig.n)
    ));

    Address recipient;
    try {
        recipient = Address(toAddress.toStdString());
    } catch (const std::exception& ex) {
        emit hierarchicalSpendDraftFailed(tr("Invalid recipient address: %1").arg(ex.what()));
        return;
    }

    // Parse delegation proof txid
    Hash256 delegationProofHash;
    if (!hexStringToHash(delegationProofTxid, delegationProofHash)) {
        emit hierarchicalSpendDraftFailed(tr("Invalid delegation proof txid"));
        return;
    }

    HierarchicalSpendRequest request;
    request.delegation_proof_txid = delegationProofHash;
    request.delegated_config = delegatedConfig;
    request.recipient = recipient;
    request.amount = amountAngles;
    request.fee = feeAngles;

    HierarchicalSpendDraftResult draftResult;
    std::string serviceError;
    if (!create_hierarchical_spend_draft(
            request,
            chain_state_,
            draftResult,
            serviceError,
            pendingTransactions())) {
        emit hierarchicalSpendDraftFailed(QString::fromStdString(serviceError));
        return;
    }

    QJsonObject draftJson;
    draftJson["amount_schema"] = 2;
    draftJson["type"] = "hierarchical_spend";
    draftJson["spending_hash"] = hashToHexString(draftResult.spending_hash);
    draftJson["tx_hex"] = bytesToHexString(draftResult.transaction.serialize());
    draftJson["delegation_proof_txid"] = delegationProofTxid;
    draftJson["primary_address"] = QString::fromStdString(draftResult.primary_address.str());
    draftJson["delegated_config"] = delegatedConfigJson;
    draftJson["delegated_address"] = QString::fromStdString(delegatedConfig.address.str());
    draftJson["to_address"] = toAddress;
    draftJson["amount_angles"] = QString::number(amountAngles);
    draftJson["amount"] = GUIUtil::formatAmount(amountAngles);
    draftJson["m"] = delegatedConfig.m;
    draftJson["n"] = delegatedConfig.n;
    draftJson["timestamp"] = QString::number(draftResult.transaction.timestamp);
    draftJson["total_input_angles"] = QString::number(draftResult.total_input);
    draftJson["total_input"] = GUIUtil::formatAmount(draftResult.total_input);
    draftJson["fee_angles"] = QString::number(draftResult.fee_used);
    draftJson["fee"] = GUIUtil::formatAmount(draftResult.fee_used);
    draftJson["change_angles"] = QString::number(draftResult.change);
    draftJson["change"] = GUIUtil::formatAmount(draftResult.change);

    QJsonDocument doc(draftJson);
    emit hierarchicalSpendDraftReady(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)),
                                     hashToHexString(draftResult.spending_hash));
}

void WalletModel::finalizeHierarchicalSpend(const QString& draftJson,
                                            const QStringList& commitmentTxids,
                                            bool broadcast)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(draftJson.toUtf8(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        emit hierarchicalSpendFinalizeFailed(tr("Invalid draft JSON: %1").arg(parseError.errorString()));
        return;
    }

    const QJsonObject obj = doc.object();
    const QString txHex = obj.value("tx_hex").toString();
    if (txHex.isEmpty()) {
        emit hierarchicalSpendFinalizeFailed(tr(
            "This draft predates safe hierarchical output signing; recreate it"));
        return;
    }
    core::Transaction draftTx;
    if (!deserializeTransaction(txHex, draftTx)) {
        emit hierarchicalSpendFinalizeFailed(tr("Invalid hierarchical draft transaction"));
        return;
    }

    std::vector<Hash256> commitmentHashes;
    commitmentHashes.reserve(commitmentTxids.size());
    for (const QString& txidStr : commitmentTxids) {
        Hash256 commitmentHash;
        if (!hexStringToHash(txidStr, commitmentHash)) {
            emit hierarchicalSpendFinalizeFailed(tr("Invalid commitment txid: %1").arg(txidStr));
            return;
        }
        commitmentHashes.push_back(commitmentHash);
    }

    core::Transaction finalTx;
    std::string serviceError;
    if (!finalize_hierarchical_spend(
            draftTx,
            commitmentHashes,
            chain_state_,
            finalTx,
            serviceError)) {
        emit hierarchicalSpendFinalizeFailed(QString::fromStdString(serviceError));
        return;
    }

    const QString finalHex = bytesToHexString(finalTx.serialize());

    QString txid;
    if (broadcast) {
        if (!submitTransaction(finalTx)) {
            emit hierarchicalSpendFinalizeFailed(tr("Failed to broadcast final transaction"));
            return;
        }
        txid = hashToHexString(finalTx.get_hash());
    }

    emit hierarchicalSpendFinalizeReady(finalHex, txid);
}

// Universal commitment signing
void WalletModel::signCommitment(const QString& draftJson,
                                 const QString& keySource,
                                 const QString& keyData,
                                 bool broadcast)
{
    ParsedCommitmentDraft parsed;
    QString error;
    if (!parseCommitmentDraft(draftJson, chain_state_, parsed, error)) {
        emit multisigCommitmentFailed(error);
        return;
    }

    // Get private key based on source
    bytes privateKey;
    geocoin::utils::ScopedContainerCleanser<bytes> privateKeyCleanser(privateKey);

    Address signerAddress;
    if (keySource == "wallet") {
        if (!wallet_) {
            emit multisigCommitmentFailed(tr("Wallet is not loaded"));
            return;
        }

        // ВАЖНО (безопасность): требуем пароль при подписи ключом из кошелька.
        // Это защищает от ситуации, когда пароль был "закэширован" ранее и подпись
        // могла бы выполниться без участия пользователя.
        if (isWalletEncrypted()) {
            QString password;
            GUIUtil::ScopedQStringCleanser passwordCleanser(password);
            if (!GUIUtil::getStyledText(nullptr,
                                        tr("Password wallet"),
                                        tr("Enter the wallet password to sign:"),
                                        password,
                                        QLineEdit::Password) || password.isEmpty()) {
                emit multisigCommitmentFailed(tr("Signing cancelled: password was not entered"));
                return;
            }
            if (!authorizeSigningPassword(password)) {
                emit multisigCommitmentFailed(tr("Invalid password"));
                return;
            }
        }

        try {
            signerAddress = Address(keyData.toStdString());
        } catch (const std::exception& ex) {
            emit multisigCommitmentFailed(tr("Invalid wallet address: %1").arg(ex.what()));
            return;
        }
        if (!wallet_->get_private_key(signerAddress, privateKey)) {
            emit multisigCommitmentFailed(tr("Private key not found in wallet"));
            return;
        }
    } else if (keySource == "file") {
        if (!readKeyFile(keyData, privateKey, error)) {
            emit multisigCommitmentFailed(error);
            return;
        }
        Address derivedAddr;
        if (!crypto::compute_address_from_private_key(privateKey, derivedAddr)) {
            emit multisigCommitmentFailed(tr("Failed to derive address from key file"));
            return;
        }
        signerAddress = derivedAddr;
    } else if (keySource == "manual") {
        if (!hexStringToBytes(keyData, privateKey) || privateKey.size() != 32) {
            emit multisigCommitmentFailed(tr("Manual key must be 32-byte hex"));
            return;
        }
        Address derivedAddr;
        if (!crypto::compute_address_from_private_key(privateKey, derivedAddr)) {
            emit multisigCommitmentFailed(tr("Failed to derive address from manual key"));
            return;
        }
        signerAddress = derivedAddr;
    } else {
        emit multisigCommitmentFailed(tr("Unknown key source: %1").arg(keySource));
        return;
    }

    // Derivation above reconstructed the policy from canonical transaction
    // bytes (and the confirmed delegation proof when required). Membership is
    // therefore mandatory rather than an optional JSON-array check.
    CommitmentResult result;
    std::string serviceError;
    if (!sign_configured_angle_commitment(
            parsed.intent.signer_config,
            parsed.intent.signing_hash,
            privateKey,
            result,
            serviceError)) {
        emit multisigCommitmentFailed(QString::fromStdString(serviceError));
        return;
    }

    QString commitmentHex = bytesToHexString(result.commitment_tx.serialize());
    QString commitmentHashStr = hashToHexString(result.commitment_hash);
    QString txid;

    if (broadcast) {
        if (!submitTransaction(result.commitment_tx)) {
            emit multisigCommitmentFailed(tr("Failed to broadcast commitment"));
            return;
        }

        txid = commitmentHashStr;
    }

    emit multisigCommitmentReady(commitmentHex, commitmentHashStr, txid);
}

QString WalletModel::exportPrivateKey(const QString &password)
{
    if (!wallet_) {
        return QString();
    }

    // Verify password
    if (!verifyPassword(password)) {
        return QString();
    }

    // Export private key using wallet API
    std::string passwordBytes = password.toStdString();
    GUIUtil::ScopedStringCleanser passwordCleanser(passwordBytes);
    geocoin::bytes private_key = wallet_->export_private_key(passwordBytes);
    geocoin::utils::ScopedContainerCleanser<geocoin::bytes> privateKeyCleanser(private_key);

    if (private_key.empty()) {
        return QString();
    }

    // Convert to hex string
    QString hex;
    for (uint8_t byte : private_key) {
        hex += QString("%1").arg(byte, 2, 16, QChar('0'));
    }

    return hex;
}

bool WalletModel::importPrivateKey(const QString &privateKeyHex, const QString &password, bool addToExisting)
{
    if (!wallet_) {
        wallet_ = std::make_shared<geocoin::wallet::Wallet>();
    }
    if (walletFilePath_.isEmpty()) return false;
    std::unique_ptr<QLockFile> backupLock;
    if (!addToExisting) {
        QString error;
        backupLock = lockBackupDestination(walletFilePath_, &error);
        if (!backupLock) { emit walletSaveError(error); return false; }
    }

    geocoin::bytes private_key;
    geocoin::utils::ScopedContainerCleanser<geocoin::bytes> privateKeyCleanser(private_key);

    if (!hexStringToBytes(privateKeyHex, private_key) || private_key.size() != 32) {
        return false;
    }

    if (addToExisting) {
        if (!isWalletLoaded()) {
            return false;
        }
        if (wallet_->has_password_protection() && !verifyPassword(password)) {
            return false;
        }
        if (!wallet_->import_private_key(private_key, "Imported")) {
            return false;
        }
    } else {
        // Replacement is explicit: remove every existing key and all derived
        // wallet state before initializing the imported key.
        clearWalletInPlace();
        if (!wallet_->initialize(private_key)) {
            return false;
        }
    }

    walletDirty_ = true;
    if (addToExisting) {
        QString saveError;
        if (!saveWallet(password, &saveError)) {
            emit walletSaveError(saveError);
            return false;
        }
    } else {
        std::string pwd = password.toStdString();
        GUIUtil::ScopedStringCleanser passwordCleanser(pwd);
        const bool saved = wallet_->save_to_file(
            walletFilePath_.toStdString(), pwd, /*preserve_backup=*/false);
        if (!saved) {
            emit walletSaveError(tr("Failed to save imported wallet to %1").arg(walletFilePath_));
            return false;
        }
        walletDirty_ = false;
    }

    // Rescan blockchain for transactions
    if (chain_state_) {
        startWalletRescanAsync(0);
    }

    updateBalance();
    transactionTableModel->updateTransactions();
    addressTableModel->updateAddresses();
    emitWalletStateChanged();

    return true;
}
