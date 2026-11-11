#include "geocoin/core/transaction.hpp"
#include "geocoin/core/transaction_internal.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/logger.hpp"
#include <cstdlib>

namespace geocoin::core {

namespace {
Hash256 compute_input_signature_message_hash(const Transaction& tx) {
    Transaction temp = tx;
    // Для не-ANGLE транзакций подписи хранятся в witness, поэтому
    // signature_hash не должен входить в preimage подписи (иначе круговая зависимость).
    if (temp.type != TransactionType::ANGLE_COMMITMENT) {
        for (auto& input : temp.inputs) {
            input.signature_hash = {};
        }
    }
    return crypto::double_sha256(temp.serialize_without_witness());
}

inline bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}
} // namespace

void Transaction::sign(const bytes& private_key) {
    if (private_key.size() != 32) {
        return;
    }

    // Подписываем все входы одним ключом
    for (size_t i = 0; i < inputs.size(); i++) {
        sign_input(i, private_key);
    }
}

void Transaction::sign_input(size_t input_index, const bytes& private_key) {
    if (input_index >= inputs.size()) {
        GEO_LOG_ERROR("tx", "sign_input: input_index >= inputs.size()");
        return;
    }

    if (private_key.size() != 32) {
        GEO_LOG_ERROR("tx", "sign_input: private_key.size() != 32");
        return;
    }

    if (type == TransactionType::ANGLE_COMMITMENT ||
        type == TransactionType::MULTISIG_SPEND ||
        type == TransactionType::DELEGATION_PROOF ||
        type == TransactionType::REVOKE_DELEGATION) {
        return;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("tx", "sign_input: Signing input " << input_index);
    }

    // Validate and hash before mutating the witness vector.
    Hash256 tx_hash = compute_input_signature_message_hash(*this);

    // Подпись уходит в witness (pubkey + signature)
    if (witnesses.size() != inputs.size()) {
        witnesses.resize(inputs.size());
    }

    bytes pub;
    Signature sig;
    if (!crypto::sign_transaction_input_detached(tx_hash, input_index, private_key, pub, sig)) {
        GEO_LOG_ERROR("tx", "sign_input: sign_transaction_input_detached failed");
        return;
    }

    witnesses[input_index].public_key = std::move(pub);
    witnesses[input_index].signature = std::move(sig);
    witnesses[input_index].public_key_hash = crypto::sha256(witnesses[input_index].public_key);
    inputs[input_index].signature.clear();
    inputs[input_index].signature_hash = crypto::sha256(witnesses[input_index].signature);

}

bool verify_all_inputs(const Transaction& tx) {
    for (size_t i = 0; i < tx.inputs.size(); i++) {
        if (!tx.verify_input(i)) {
            return false;
        }
    }
    return true;
}

bool Transaction::verify() const {
    if (unused_payload_field()) return false;
    if (!is_known_transaction_type(type)) {
        GEO_LOG_ERROR("tx", "verify: unknown transaction type");
        return false;
    }

    if (witness_version != consensus::params::WITNESS_FORMAT_VERSION_CURRENT) {
        return false;
    }

    // Coinbase транзакции не имеют подписи.
    if (is_coinbase()) {
        return true;
    }

    // Подписи во входах запрещены для всех типов, кроме ANGLE_COMMITMENT.
    // Для остальных типов валидность входов проверяется в verify_input().

    // ANGLE_COMMITMENT: единственная криптографическая проверка - подпись commitment.
    if (type == TransactionType::ANGLE_COMMITMENT) {
        return verify_angle_commitment_transaction(*this);
    }

    // Multisig-root cryptography is verified by MultisigVerifier against the
    // referenced ANGLE_COMMITMENT package.
    if (type == TransactionType::MULTISIG_SPEND ||
        type == TransactionType::DELEGATION_PROOF ||
        type == TransactionType::REVOKE_DELEGATION) {
        return true;
    }

    if (is_nft_transition_type(type)) {
        return verify_nft_transaction(*this);
    }

    if (type == TransactionType::CADASTRAL_DOC) {
        return verify_cadastral_transaction(*this);
    }

    if (type == TransactionType::UPDATE_FOUNDATION_ADDRESS) {
        return verify_foundation_transaction(*this);
    }

    if (type == TransactionType::MINT_SIDECHAIN ||
        type == TransactionType::REDEEM_SIDECHAIN ||
        type == TransactionType::SIDECHAIN_CHECKPOINT ||
        type == TransactionType::UPDATE_SIDECHAIN_COMMITTEE ||
        type == TransactionType::SLASH_DEPOSIT ||
        type == TransactionType::BATCH_SIDECHAIN_CHECKPOINT ||
        type == TransactionType::INTEREST_DISTRIBUTION) {
        // Криптопроверка выполняется в ChainState/consensus для state-зависимых типов.
        return true;
    }

    if (type == TransactionType::LOCK_DEPOSIT ||
        type == TransactionType::TOP_UP_SIDECHAIN_DEPOSIT ||
        type == TransactionType::SPEND_DEPOSIT ||
        type == TransactionType::UNLOCK_DEPOSIT ||
        type == TransactionType::ISSUE_SIDECHAIN ||
        type == TransactionType::REGISTER_ACCOUNTING_SIDECHAIN ||
        type == TransactionType::DECOMMISSION_SIDECHAIN) {
        return verify_sidechain_owner_transaction(*this);
    }

    // Остальные транзакции: проверяем подписи входов.
    return verify_all_inputs(*this);
}

bool Transaction::verify_input(size_t input_index) const {
    if (unused_payload_field()) return false;
    // Coinbase транзакции не имеют witness и не требуют подписи.
    if (is_coinbase()) {
        return true;
    }

    if (input_index >= inputs.size()) {
        GEO_LOG_ERROR("tx", "verify_input: input_index >= inputs.size()");
        return false;
    }

    if (input_index >= witnesses.size()) {
        GEO_LOG_ERROR("tx", "verify_input: missing witness");
        return false;
    }

    const auto& w = witnesses[input_index];

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("tx", "verify_input: Verifying input " << input_index);
    }

    if (w.signature.empty()) {
        GEO_LOG_ERROR("tx", "verify_input: No signature");
        return false;
    }
    if (w.public_key.empty()) {
        GEO_LOG_ERROR("tx", "verify_input: Missing witness public key");
        return false;
    }
    if (w.public_key.size() != consensus::params::WITNESS_PUBKEY_SIZE) {
        GEO_LOG_ERROR("tx", "verify_input: Invalid witness public key size");
        return false;
    }
    if (crypto::sha256(w.public_key) != w.public_key_hash) {
        GEO_LOG_ERROR("tx", "verify_input: witness public_key_hash mismatch");
        return false;
    }

    // Стабильный preimage для проверки подписи входа.
    Hash256 tx_hash = compute_input_signature_message_hash(*this);

    // Верифицируем подпись Falcon-512 с включением input_index
    bool result = crypto::verify_transaction_input_detached(
        tx_hash,
        input_index,
        w.public_key,
        w.signature
    );

    if (!result) {
        GEO_LOG_ERROR("tx", "verify_input: verify_transaction_input failed");
    } else {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("tx", "verify_input: success for input " << input_index);
        }
    }

    if (!result) {
        return false;
    }

    // Сверяем хеш подписи с тем, что хранится в input.signature_hash
    Hash256 expected = crypto::sha256(w.signature);
    if (expected != inputs[input_index].signature_hash) {
        GEO_LOG_ERROR("tx", "verify_input: signature_hash mismatch");
        return false;
    }

    return true;
}

} // namespace geocoin::core
