
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "walletmodel.h"
#include "optionsmodel.h"
#include "transactiontablemodel.h"
#include "addresstablemodel.h"
#include "guiutil.h"
#include "qtlogging.h"
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
#include <cmath>
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <unordered_set>
#include <utility>

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
}

WalletModel::~WalletModel()
{
    try {
        waitForRescanThread();
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

void WalletModel::startWalletRescanAsync(geocoin::Height fromHeight)
{
    if (!wallet_ || !chain_state_) {
        return;
    }

    geocoin::Height current_height = chain_state_->get_height();
    geocoin::Height start_height = std::min(fromHeight, current_height);

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
        rescanThread_ = std::thread([this, wallet_snapshot, chain_snapshot, start_height]() {
            try {
                wallet_snapshot->scan_blockchain(chain_snapshot, start_height);
            } catch (const std::exception& exception) {
                qWarning() << "Wallet rescan failed:" << exception.what();
            } catch (...) {
                qWarning() << "Wallet rescan failed with an unknown exception";
            }
            QMetaObject::invokeMethod(
                this,
                [this]() { onWalletRescanFinished(); },
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
    return wallet_ != nullptr && wallet_->get_key_count() > 0;
}

bool WalletModel::isWalletEncrypted() const
{
    return wallet_ && wallet_->has_password_protection();
}

bool WalletModel::isWalletLocked() const
{
    // GeoCoin wallets don't have lock/unlock state
    // Password is required for each transaction
    return false;
}

bool WalletModel::verifyPassword(const QString &password) const
{
    if (!wallet_) {
        return false;
    }

    std::string pwd = password.toStdString();
    GUIUtil::ScopedStringCleanser passwordCleanser(pwd);

    return wallet_->verify_password(pwd);
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
        if (!wallet_->verify_password(pwd)) {
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
    if (!wallet_->verify_password(passwordBytes)) {
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
    Q_ASSERT(QThread::currentThread() == thread());

    if (!wallet_) {
        wallet_ = std::make_shared<geocoin::wallet::Wallet>();
    }

    const QString walletPath = walletFilePath_.isEmpty() ? filename : walletFilePath_;
    if (walletPath.isEmpty() ||
        (!filename.isEmpty() &&
         QFileInfo(filename).absoluteFilePath() != QFileInfo(walletPath).absoluteFilePath())) {
        return false;
    }

    std::string pwd = password.toStdString();
    GUIUtil::ScopedStringCleanser passwordCleanser(pwd);

    bool success = wallet_->load_from_file(
        walletPath.toStdString(),
        pwd
    );

    if (success) {
        walletFilePath_ = walletPath;
        walletDirty_ = false;

        // ВАЖНО: после рестарта ноды новые блоки могут не приходить,
        // поэтому без сканирования кошелек не узнает о coinbase/входящих транзакциях,
        // а баланс останется 0.
        if (chain_state_) {
            geocoin::Height current_height = chain_state_->get_height();
            geocoin::Height wallet_best_height = wallet_->get_last_scanned_height();
            geocoin::Hash256 active_anchor{};
            const bool anchor_matches = wallet_best_height <= current_height &&
                chain_state_->get_active_block_hash(wallet_best_height, active_anchor) &&
                active_anchor == wallet_->get_last_scanned_block_hash();
            if (!anchor_matches) {
                startWalletRescanAsync(0);
            } else if (wallet_best_height < current_height) {
                startWalletRescanAsync(wallet_best_height + 1);
            }
        }

        updateBalance();
        transactionTableModel->updateTransactions();
        addressTableModel->updateAddresses();
        emitWalletStateChanged();
    }

    return success;
}

bool WalletModel::createWallet(const QString &filename, const QString &password)
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (!wallet_) {
        wallet_ = std::make_shared<geocoin::wallet::Wallet>();
    }

    const QString walletPath = walletFilePath_.isEmpty() ? filename : walletFilePath_;
    if (walletPath.isEmpty() ||
        (!filename.isEmpty() &&
         QFileInfo(filename).absoluteFilePath() != QFileInfo(walletPath).absoluteFilePath()) ||
        QFileInfo::exists(walletPath) || isWalletLoaded()) {
        return false;
    }

    // Generate new random private key
    geocoin::bytes private_key = generate_random_private_key();
    geocoin::utils::ScopedContainerCleanser<geocoin::bytes> privateKeyCleanser(private_key);

    clearWalletInPlace();

    if (!wallet_->initialize(private_key)) {
        return false;
    }

    std::string pwd = password.toStdString();
    GUIUtil::ScopedStringCleanser passwordCleanser(pwd);

    bool success = wallet_->save_to_file(
        walletPath.toStdString(),
        pwd
    );

    if (success) {
        walletFilePath_ = walletPath;
        walletDirty_ = false;

        // Remember password
        if (!pwd.empty()) {
            wallet_->remember_password(pwd);
        }

        // Scan blockchain for transactions
        if (chain_state_) {
            startWalletRescanAsync(0);
        }

        // Update all data
        updateBalance();
        transactionTableModel->updateTransactions();
        addressTableModel->updateAddresses();

        // Force emit signals to update UI
        emit balanceChanged(cachedBalance, cachedUnconfirmedBalance, cachedImmatureBalance);
        emit watchOnlyBalanceChanged(
            cachedWatchOnlyBalance,
            cachedWatchOnlyUnconfirmedBalance,
            cachedWatchOnlyImmatureBalance
        );
        emitWalletStateChanged();
    } else {
        clearWalletInPlace();
        walletDirty_ = false;
    }

    return success;
}

void WalletModel::closeWallet()
{
    Q_ASSERT(QThread::currentThread() == thread());

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

    if (wallet_) {
        clearWalletInPlace();
    }

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
        if (!wallet_->verify_password(pwd)) {
            if (error) *error = tr("Incorrect wallet password");
            return false;
        }
    }

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

    std::string pwd;
    GUIUtil::ScopedStringCleanser passwordCleanser(pwd);
    if (wallet_->has_password_protection()) {
        pwd = password.toStdString();
        if (pwd.empty() || !wallet_->verify_password(pwd)) {
            if (error) *error = tr("Incorrect wallet password");
            return false;
        }
    }
    const bool success = wallet_->save_to_file(filename.toStdString(), pwd);
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

    if (wallet_->get_key_count() > 0 || !wallet_->get_watch_only_addresses().empty()) {
        const geocoin::Height current_height = chain_state_->get_height();
        const geocoin::Height wallet_best_height = wallet_->get_last_scanned_height();
        geocoin::Hash256 active_anchor{};
        const bool anchor_matches = wallet_best_height <= current_height &&
            chain_state_->get_active_block_hash(wallet_best_height, active_anchor) &&
            active_anchor == wallet_->get_last_scanned_block_hash();
        if (!anchor_matches) {
            startWalletRescanAsync(0);
        } else if (wallet_best_height < current_height) {
            startWalletRescanAsync(wallet_best_height + 1);
        }
    }

    cachedNumBlocks = chain_state_->get_height();
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
    walletFilePath_ = QDir(dataDirectory_).filePath(QStringLiteral("wallet.dat"));
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
    if (!wallet_ || !chain_state_) {
        cachedBalance = 0;
        cachedUnconfirmedBalance = 0;
        cachedImmatureBalance = 0;
        emit balanceChanged(0, 0, 0);
        cachedWatchOnlyBalance = 0;
        cachedWatchOnlyUnconfirmedBalance = 0;
        cachedWatchOnlyImmatureBalance = 0;
        emit watchOnlyBalanceChanged(0, 0, 0);
        return;
    }

    // UTXO-based balance (authoritative, matches consensus/ChainState)
    // Это избегает ошибок старого метода (суммирование net_amount по истории транзакций).
    qint64 newBalance = 0;
    qint64 newUnconfirmedBalance = 0;
    qint64 newImmatureBalance = 0;

    qint64 newWatchOnlyBalance = 0;
    qint64 newWatchOnlyUnconfirmedBalance = 0;
    qint64 newWatchOnlyImmatureBalance = 0;
    bool balanceAccountingValid = true;

    auto checked_accumulate = [&balanceAccountingValid](qint64& total, qint64 value) {
        if (!balanceAccountingValid) return false;
        geocoin::Amount next = 0;
        if (value < 0 || !geocoin::checked_add_amount(
                static_cast<geocoin::Amount>(total),
                static_cast<geocoin::Amount>(value), next)) {
            balanceAccountingValid = false;
            return false;
        }
        total = next;
        return true;
    };

    const auto pending = pendingTransactions();
    auto sum_addresses = [&](const std::vector<geocoin::Address>& addrs,
                             qint64& out_available,
                             qint64& out_unconfirmed,
                             qint64& out_immature) {
        for (const auto& addr : addrs) {
            const auto funds = geocoin::wallet::classify_address_funds(*chain_state_, addr, pending);
            if (!funds.valid ||
                !checked_accumulate(out_available, funds.available) ||
                !checked_accumulate(out_unconfirmed, funds.unconfirmed) ||
                !checked_accumulate(out_immature, funds.immature)) {
                balanceAccountingValid = false;
                return;
            }
        }
    };

    const auto spendable_addrs = wallet_->get_spendable_addresses();
    const auto watch_only_addrs = wallet_->get_watch_only_addresses();
    sum_addresses(spendable_addrs, newBalance, newUnconfirmedBalance, newImmatureBalance);
    if (balanceAccountingValid) {
        sum_addresses(
            watch_only_addrs,
            newWatchOnlyBalance,
            newWatchOnlyUnconfirmedBalance,
            newWatchOnlyImmatureBalance);
    }

    if (!balanceAccountingValid) {
        // Corrupt/overflowing state must never invoke signed-overflow UB or show
        // a wrapped positive balance. Fail closed until the next clean poll.
        qWarning() << "[WalletModel] Balance accounting overflow or negative UTXO; balances reset to zero";
        newBalance = 0;
        newUnconfirmedBalance = 0;
        newImmatureBalance = 0;
        newWatchOnlyBalance = 0;
        newWatchOnlyUnconfirmedBalance = 0;
        newWatchOnlyImmatureBalance = 0;
    }

    // Update cached balances and emit signal if changed
    if (newBalance != cachedBalance || newUnconfirmedBalance != cachedUnconfirmedBalance || newImmatureBalance != cachedImmatureBalance) {
        cachedBalance = newBalance;
        cachedUnconfirmedBalance = newUnconfirmedBalance;
        cachedImmatureBalance = newImmatureBalance;
        emit balanceChanged(cachedBalance, cachedUnconfirmedBalance, cachedImmatureBalance);
    }

    if (newWatchOnlyBalance != cachedWatchOnlyBalance ||
        newWatchOnlyUnconfirmedBalance != cachedWatchOnlyUnconfirmedBalance ||
        newWatchOnlyImmatureBalance != cachedWatchOnlyImmatureBalance) {
        cachedWatchOnlyBalance = newWatchOnlyBalance;
        cachedWatchOnlyUnconfirmedBalance = newWatchOnlyUnconfirmedBalance;
        cachedWatchOnlyImmatureBalance = newWatchOnlyImmatureBalance;
        emit watchOnlyBalanceChanged(
            cachedWatchOnlyBalance,
            cachedWatchOnlyUnconfirmedBalance,
            cachedWatchOnlyImmatureBalance
        );
    }
}

void WalletModel::pollBalanceChanged()
{
    // Вызывается таймером.
    // Проверяем, нужно ли обновить баланс и таблицу транзакций.

    if (!wallet_ || !chain_state_) {
        return;
    }

    geocoin::Height current_height = chain_state_->get_height();
    const geocoin::Height previous_height = cachedNumBlocks;

    geocoin::Hash256 current_best_hash{};
    bool have_current_best_hash = false;
    if (nodeServices_.chain_query) {
        geocoin::core::Block active_tip;
        if (nodeServices_.chain_query->get_block_by_height(current_height, active_tip)) {
            current_best_hash = active_tip.get_hash();
            have_current_best_hash = true;
        }
    }

    bool activeChainReorg = false;
    if (haveCachedBestBlockHash_ && nodeServices_.chain_query) {
        if (current_height < previous_height) {
            activeChainReorg = true;
        } else {
            geocoin::core::Block block_at_previous_tip;
            if (nodeServices_.chain_query->get_block_by_height(
                    previous_height,
                    block_at_previous_tip
                ) &&
                block_at_previous_tip.get_hash() != cachedBestBlockHash_) {
                activeChainReorg = true;
            }
        }
    }

    // IBD-aware one-time rescan: during IBD UI per-tx notifications may be suppressed.
    // When IBD finishes, do one scan to catch anything missed.
    bool isIbd = false;
    if (nodeServices_.info) {
        isIbd = nodeServices_.info->is_initial_block_download();
    }
    if (lastIbdState && !isIbd && !didPostIbdRescan &&
        (wallet_->get_key_count() > 0 || !wallet_->get_watch_only_addresses().empty())) {
        geocoin::Height wallet_best_height = wallet_->get_last_scanned_height();
        geocoin::Hash256 active_anchor{};
        const bool anchor_matches = wallet_best_height <= current_height &&
            chain_state_->get_active_block_hash(wallet_best_height, active_anchor) &&
            active_anchor == wallet_->get_last_scanned_block_hash();
        if (!anchor_matches) {
            startWalletRescanAsync(0);
        } else if (wallet_best_height < current_height) {
            startWalletRescanAsync(wallet_best_height + 1);
        }
        didPostIbdRescan = true;
    }
    lastIbdState = isIbd;

    if (activeChainReorg) {
        qInfo() << "[WalletModel] Active chain changed by reorg; wallet keys are kept and balance will be rescanned";
        if (wallet_->get_key_count() > 0 || !wallet_->get_watch_only_addresses().empty()) {
            startWalletRescanAsync(0);
        }
        transactionRefreshPending_ = true;
        fForceCheckBalanceChanged = true;
    }

    if (have_current_best_hash) {
        cachedBestBlockHash_ = current_best_hash;
        haveCachedBestBlockHash_ = true;
    }

    const bool heightChanged = current_height != cachedNumBlocks;
    if (fForceCheckBalanceChanged || heightChanged || activeChainReorg) {
        fForceCheckBalanceChanged = false;
        cachedNumBlocks = current_height;

        // Balance can change on every block (confirmations, maturity, mempool state).
        updateBalance();

        // Rebuild the transaction table only when wallet data changed, or when
        // a pending transaction might have switched state after a new block.
        const bool shouldRefreshTransactions =
            transactionRefreshPending_ ||
            (heightChanged && transactionTableModel && transactionTableModel->hasPendingTransactions()) ||
            isRescanInProgress();

        if (shouldRefreshTransactions && transactionTableModel) {
            transactionTableModel->updateTransactions();
            transactionRefreshPending_ = false;
        }
    }
}

void WalletModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    // Ставим флаг: обновить данные на следующем тике таймера.
    fForceCheckBalanceChanged = true;
    transactionRefreshPending_ = true;
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
    walletTransactionChangedConnection_ = wallet_->NotifyTransactionChanged.connect(
        [this](geocoin::wallet::Wallet* wallet, const geocoin::Hash256& hash) {
            NotifyTransactionChanged(this, wallet, hash);
        }
    );
    walletScanFinishedConnection_ = wallet_->NotifyScanFinished.connect(
        [this](geocoin::wallet::Wallet*, const geocoin::wallet::WalletScanResult& result) {
            const QString error = QString::fromStdString(result.error);
            QMetaObject::invokeMethod(this, [this, error]() {
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
