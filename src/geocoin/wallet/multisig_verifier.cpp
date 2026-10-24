#include "geocoin/multisig/verifier.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/logger.hpp"
#include <set>
#include <iomanip>
#include <cstdlib>
#include <sstream>

namespace geocoin::multisig {

namespace {
inline bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}

bool is_null_hash256(const Hash256& h) {
    for (auto b : h) {
        if (b != 0) return false;
    }
    return true;
}

bool unique_addresses(const std::vector<Address>& addrs) {
    std::set<std::string> seen;
    for (const auto& a : addrs) {
        if (!seen.insert(a.str()).second) {
            return false;
        }
    }
    return true;
}

bool address_matches_multisig_config(
    const Address& expected_multisig_addr,
    uint8_t m,
    uint8_t n,
    const std::vector<Address>& participants
) {
    try {
        // create_multisig_address() внутри сам сортирует адреса для детерминизма,
        // поэтому порядок в participants не важен, но дубликаты/размеры мы проверяем отдельно.
        const auto computed = create_multisig_address(m, n, participants);
        return computed.address == expected_multisig_addr.str();
    } catch (...) {
        return false;
    }
}
}

bool MultisigVerifier::verify_multisig_spend(
    const core::Transaction& tx,
    const MultisigConfig& multisig_config
) const {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "Verifying MULTISIG_SPEND transaction");
    }

    // 1. Проверяем тип транзакции
    if (tx.type != core::TransactionType::MULTISIG_SPEND) {
        GEO_LOG_ERROR("multisig", "Wrong transaction type");
        return false;
    }

    if (!is_null_hash256(tx.delegation_proof_txid) ||
        !validate_multisig_config(multisig_config.address, multisig_config) ||
        tx.spend_participants.size() != tx.n ||
        !unique_addresses(tx.spend_participants) ||
        !address_matches_multisig_config(
            multisig_config.address, tx.m, tx.n, tx.spend_participants)) {
        GEO_LOG_ERROR("multisig", "Invalid multisig policy or participant preimage");
        return false;
    }

    // 2. Проверяем M и N
    if (tx.m != multisig_config.m || tx.n != multisig_config.n) {
        GEO_LOG_ERROR(
            "multisig",
            "M/N mismatch. Expected " << (int)multisig_config.m << "-of-" << (int)multisig_config.n
                << ", got " << (int)tx.m << "-of-" << (int)tx.n
        );
        return false;
    }

    // 3. Проверяем количество commitments
    if (tx.angle_commitment_hashes.size() < tx.m || tx.angle_commitment_hashes.size() > tx.n) {
        GEO_LOG_ERROR(
            "multisig",
            "Not enough commitments. Need " << (int)tx.m << ", got " << tx.angle_commitment_hashes.size()
        );
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "multisig",
            "Found " << tx.angle_commitment_hashes.size() << " commitments (need " << (int)tx.m << ")"
        );
    }

    // 4. Вычисляем хеш транзакции без commitments (это то, что подписывали)
    Hash256 spending_tx_hash = tx.get_hash_without_commitments();

    if (SyncDebugEnabled()) {
        std::ostringstream ss;
        ss << "Spending tx hash: ";
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 8; ++i) {
            ss << std::setw(2) << (int)spending_tx_hash[i];
        }
        ss << "..." << std::dec;
        GEO_LOG_DEBUG("multisig", ss.str());
    }

    // 5. Загружаем и проверяем все commitments
    std::set<std::string> signers;  // Для проверки уникальности

    for (size_t i = 0; i < tx.angle_commitment_hashes.size(); i++) {
        const auto& commitment_hash = tx.angle_commitment_hashes[i];

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("multisig", "Checking commitment " << i);
        }

        // Загружаем commitment
        auto commitment = angle_store_->get_by_hash(commitment_hash);
        if (!commitment) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " not found in store");
            return false;
        }

        // Проверяем тип
        if (commitment->type != core::TransactionType::ANGLE_COMMITMENT) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " has wrong type");
            return false;
        }

        // Проверяем multisig_address
        if (commitment->multisig_address.str() != multisig_config.address.str()) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " is for wrong multisig address");
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("multisig", "  Expected: " << multisig_config.address.str());
                GEO_LOG_DEBUG("multisig", "  Got: " << commitment->multisig_address.str());
            }
            return false;
        }

        // Проверяем spending_tx_hash
        if (commitment->spending_tx_hash != spending_tx_hash) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " is for wrong spending tx");
            return false;
        }

        // Проверяем подпись commitment
        if (!verify_angle_commitment(*commitment)) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " has invalid signature");
            return false;
        }

        // Проверяем, что подписант является участником
        if (!is_participant(commitment->signer_address, multisig_config)) {
            GEO_LOG_ERROR(
                "multisig",
                "Signer " << commitment->signer_address.str() << " is not a participant"
            );
            return false;
        }

        // Проверяем уникальность подписанта
        std::string signer_key = commitment->signer_address.str();
        if (signers.count(signer_key) > 0) {
            // КРИТИЧНО: Не логируем адреса подписывателей (информация о конфиденциальности)
            GEO_LOG_ERROR("multisig", "Duplicate signer detected");
            return false;
        }
        signers.insert(signer_key);

        if (SyncDebugEnabled()) {
            // КРИТИЧНО: Не логируем адреса в debug логах
            GEO_LOG_DEBUG("multisig", "Commitment OK");
        }
    }

    // 6. Проверяем, что собрано достаточно уникальных подписей
    if (signers.size() < tx.m) {
        GEO_LOG_ERROR(
            "multisig",
            "Not enough unique signers. Need " << (int)tx.m << ", got " << signers.size()
        );
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "MULTISIG_SPEND verification success");
    }
    return true;
}

bool MultisigVerifier::verify_angle_commitment(const core::Transaction& tx) const {
    // 1. Проверяем тип
    if (tx.type != core::TransactionType::ANGLE_COMMITMENT) {
        return false;
    }

    // 2. Проверяем, что есть ровно один вход
    if (tx.inputs.size() != 1) {
        GEO_LOG_ERROR("multisig", "ANGLE_COMMITMENT must have exactly 1 input");
        return false;
    }

    const auto& input = tx.inputs[0];

    // 3. Проверяем маркер vout
    if (input.vout != 0xFFFFFFFE) {
        GEO_LOG_ERROR("multisig", "Invalid vout marker for ANGLE_COMMITMENT");
        return false;
    }

    Hash256 intent_hash{};
    try {
        intent_hash = tx.get_angle_commitment_message_hash();
    } catch (...) {
        return false;
    }

    // 4. Восстанавливаем signer_address из domain-separated intent signature
    Address expected_signer;
    if (!geocoin::crypto::recover_signer_address_from_angle_commitment(
            intent_hash,
            input.signature,
            expected_signer)) {
        GEO_LOG_ERROR("multisig", "Failed to recover signer from ANGLE_COMMITMENT signature");
        return false;
    }

    if (tx.signer_address.str() != expected_signer.str()) {
        GEO_LOG_ERROR("multisig", "ANGLE_COMMITMENT signer_address mismatch");
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("multisig", "  signer_address:  " << tx.signer_address.str());
            GEO_LOG_DEBUG("multisig", "  expected_signer: " << expected_signer.str());
        }
        return false;
    }

    // 4.2. Anti-replay key is exactly the signed canonical intent.
    if (input.prev_tx_hash != intent_hash) {
        GEO_LOG_ERROR(
            "multisig",
            "ANGLE_COMMITMENT prev_tx_hash does not match signed intent"
        );
        return false;
    }

    // Signer recovery above already verifies the Falcon signature for this
    // exact intent before deriving the address from the public key.
    return true;
}

bool MultisigVerifier::is_participant(
    const Address& address,
    const MultisigConfig& config
) const {
    for (const auto& participant : config.participants) {
        if (participant.address.str() == address.str()) {
            return true;
        }
    }
    for (const auto& addr : config.addresses) {
        if (addr.str() == address.str()) {
            return true;
        }
    }
    return false;
}

bool MultisigVerifier::verify_multisig_spend(const core::Transaction& tx) const {
    // Получаем multisig адрес из первого angle commitment
    if (tx.angle_commitment_hashes.empty()) {
        GEO_LOG_ERROR("multisig", "No angle commitments in MULTISIG_SPEND");
        return false;
    }

    // Загружаем первый commitment чтобы узнать multisig адрес
    auto first_commitment = angle_store_->get_by_hash(tx.angle_commitment_hashes[0]);
    if (!first_commitment) {
        GEO_LOG_ERROR("multisig", "First commitment not found in store");
        return false;
    }

    const Address expected_multisig_addr = first_commitment->multisig_address;

    // Иерархическую трату здесь не проверяем (она проверяется через verify_hierarchical_spend),
    // но если вдруг сюда попали — отклоняем, чтобы не валидировать по неверным правилам.
    if (!is_null_hash256(tx.delegation_proof_txid)) {
        GEO_LOG_ERROR("multisig", "verify_multisig_spend() called for hierarchical spend");
        return false;
    }

    // Обычный MULTISIG_SPEND должен нести ончейн preimage конфига (spend_participants),
    // иначе невозможно бездоверенно доказать состав участников для GEOM...
    if (tx.spend_participants.size() != tx.n) {
        GEO_LOG_ERROR("multisig", "MULTISIG_SPEND missing spend_participants or count != n");
        return false;
    }
    if (!unique_addresses(tx.spend_participants)) {
        GEO_LOG_ERROR("multisig", "MULTISIG_SPEND spend_participants contains duplicates");
        return false;
    }
    if (!address_matches_multisig_config(expected_multisig_addr, tx.m, tx.n, tx.spend_participants)) {
        GEO_LOG_ERROR("multisig", "MULTISIG_SPEND spend_participants do not match expected GEOM address");
        return false;
    }

    MultisigConfig cfg;
    cfg.address = expected_multisig_addr;
    cfg.m = tx.m;
    cfg.n = tx.n;
    cfg.addresses = tx.spend_participants;
    cfg.participants.clear();
    for (const auto& a : tx.spend_participants) {
        cfg.participants.emplace_back(a);
    }

    return verify_multisig_spend(tx, cfg);
}

bool MultisigVerifier::verify_delegation_proof(const core::Transaction& tx) const {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "Verifying DELEGATION_PROOF transaction");
    }

    // 1. Проверяем тип транзакции
    if (tx.type != core::TransactionType::DELEGATION_PROOF) {
        GEO_LOG_ERROR("multisig", "Wrong transaction type");
        return false;
    }

    // 2. Проверяем наличие angle_commitment_hashes
    if (tx.angle_commitment_hashes.empty()) {
        GEO_LOG_ERROR("multisig", "No commitment hashes in DELEGATION_PROOF");
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "Found " << tx.angle_commitment_hashes.size() << " commitments");
    }

    // 3. Проверяем M и N primary multisig из delegation proof
    uint8_t primary_m = tx.primary_m;
    uint8_t primary_n = tx.primary_n;

    if (primary_m == 0 || primary_n == 0 || primary_m > primary_n) {
        GEO_LOG_ERROR("multisig", "Invalid primary M/N in delegation proof");
        return false;
    }

    // Прод: якорим участников к адресу (P2SH-подобно): GEOM... должен быть хешем (m,n,participants).
    if (tx.primary_participants.size() != primary_n) {
        GEO_LOG_ERROR("multisig", "primary_participants count != primary_n");
        return false;
    }
    if (!unique_addresses(tx.primary_participants)) {
        GEO_LOG_ERROR("multisig", "primary_participants contains duplicates");
        return false;
    }
    if (!address_matches_multisig_config(tx.primary_multisig_address, primary_m, primary_n, tx.primary_participants)) {
        GEO_LOG_ERROR("multisig", "primary_multisig_address does not match (m,n,primary_participants)");
        return false;
    }

    uint8_t delegated_m = tx.delegated_m;
    uint8_t delegated_n = tx.delegated_n;
    if (delegated_m == 0 || delegated_n == 0 || delegated_m > delegated_n) {
        GEO_LOG_ERROR("multisig", "Invalid delegated M/N in delegation proof");
        return false;
    }
    if (tx.delegated_participants.size() != delegated_n) {
        GEO_LOG_ERROR("multisig", "delegated_participants count != delegated_n");
        return false;
    }
    if (!unique_addresses(tx.delegated_participants)) {
        GEO_LOG_ERROR("multisig", "delegated_participants contains duplicates");
        return false;
    }
    if (!address_matches_multisig_config(
            tx.delegated_multisig_address, delegated_m, delegated_n, tx.delegated_participants)) {
        GEO_LOG_ERROR("multisig", "delegated_multisig_address does not match (m,n,delegated_participants)");
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "Primary multisig: " << (int)primary_m << "-of-" << (int)primary_n);
    }

    // 4. Проверяем количество commitments >= m
    if (tx.angle_commitment_hashes.size() < primary_m) {
        GEO_LOG_ERROR(
            "multisig",
            "Not enough commitments. Need " << (int)primary_m << ", got " << tx.angle_commitment_hashes.size()
        );
        return false;
    }

    // 5. Вычисляем хеш delegation proof (то, что подписывали)
    Hash256 delegation_hash = tx.get_delegation_proof_hash();

    if (SyncDebugEnabled()) {
        std::ostringstream ss;
        ss << "Delegation proof hash: ";
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 8; ++i) {
            ss << std::setw(2) << (int)delegation_hash[i];
        }
        ss << "..." << std::dec;
        GEO_LOG_DEBUG("multisig", ss.str());
    }

    // 6. Проверяем все commitments
    std::set<std::string> signers;  // Для проверки уникальности

    for (size_t i = 0; i < tx.angle_commitment_hashes.size(); i++) {
        const auto& commitment_hash = tx.angle_commitment_hashes[i];

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("multisig", "Checking commitment " << i);
        }

        // Загружаем commitment
        auto commitment = angle_store_->get_by_hash(commitment_hash);
        if (!commitment) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " not found in store");
            return false;
        }

        // Проверяем тип
        if (commitment->type != core::TransactionType::ANGLE_COMMITMENT) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " has wrong type");
            return false;
        }

        // Проверяем multisig_address (должен быть primary)
        if (commitment->multisig_address.str() != tx.primary_multisig_address.str()) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " is for wrong multisig address");
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("multisig", "  Expected: " << tx.primary_multisig_address.str());
                GEO_LOG_DEBUG("multisig", "  Got: " << commitment->multisig_address.str());
            }
            return false;
        }

        // Проверяем spending_tx_hash (должен быть delegation_hash)
        if (commitment->spending_tx_hash != delegation_hash) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " is for wrong delegation hash");
            return false;
        }

        // Проверяем подпись Falcon-512
        if (!verify_angle_commitment(*commitment)) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " has invalid signature");
            return false;
        }

        // Проверяем, что подписант является участником primary multisig
        bool is_participant = false;
        for (const auto& participant : tx.primary_participants) {
            if (participant.str() == commitment->signer_address.str()) {
                is_participant = true;
                break;
            }
        }

        if (!is_participant) {
            GEO_LOG_ERROR(
                "multisig",
                "Signer " << commitment->signer_address.str() << " is not a participant of primary multisig"
            );
            return false;
        }

        // Проверяем уникальность подписанта
        std::string signer_key = commitment->signer_address.str();
        if (signers.count(signer_key) > 0) {
            // КРИТИЧНО: Не логируем адреса подписывателей (информация о конфиденциальности)
            GEO_LOG_ERROR("multisig", "Duplicate signer detected");
            return false;
        }
        signers.insert(signer_key);

        if (SyncDebugEnabled()) {
            // КРИТИЧНО: Не логируем адреса в debug логах
            GEO_LOG_DEBUG("multisig", "Commitment OK");
        }
    }

    // 7. Проверяем, что собрано достаточно уникальных подписей
    if (signers.size() < primary_m) {
        GEO_LOG_ERROR(
            "multisig",
            "Not enough unique signers. Need " << (int)primary_m << ", got " << signers.size()
        );
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "DELEGATION_PROOF verification success");
    }
    return true;
}

bool MultisigVerifier::verify_revoke_delegation(
    const core::Transaction& tx,
    const core::Transaction& delegation_proof_tx
) const {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "Verifying REVOKE_DELEGATION transaction");
    }

    // 1. Проверяем тип транзакции
    if (tx.type != core::TransactionType::REVOKE_DELEGATION) {
        GEO_LOG_ERROR("multisig", "Wrong transaction type");
        return false;
    }

    // 2. Проверяем что отзываемая транзакция - это DELEGATION_PROOF
    if (delegation_proof_tx.type != core::TransactionType::DELEGATION_PROOF) {
        GEO_LOG_ERROR("multisig", "Revoked transaction is not DELEGATION_PROOF");
        return false;
    }

    if (tx.revoked_delegation_proof_txid != delegation_proof_tx.get_hash()) {
        GEO_LOG_ERROR("multisig", "Revocation references a different delegation proof");
        return false;
    }

    // Прод: делегация, которую отзываем, должна быть валидна по бездоверенным правилам.
    if (!verify_delegation_proof(delegation_proof_tx)) {
        GEO_LOG_ERROR("multisig", "Revoked delegation proof is invalid");
        return false;
    }

    // 3. Проверяем наличие angle_commitment_hashes
    if (tx.angle_commitment_hashes.empty()) {
        GEO_LOG_ERROR("multisig", "No commitment hashes in REVOKE_DELEGATION");
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "Found " << tx.angle_commitment_hashes.size() << " commitments");
    }

    // 4. Получаем M и N primary multisig из delegation proof
    uint8_t primary_m = delegation_proof_tx.primary_m;
    uint8_t primary_n = delegation_proof_tx.primary_n;

    if (primary_m == 0 || primary_n == 0 || primary_m > primary_n) {
        GEO_LOG_ERROR("multisig", "Invalid primary M/N in delegation proof");
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "Primary multisig: " << (int)primary_m << "-of-" << (int)primary_n);
    }

    // 5. Проверяем количество commitments >= m
    if (tx.angle_commitment_hashes.size() < primary_m) {
        GEO_LOG_ERROR(
            "multisig",
            "Not enough commitments. Need " << (int)primary_m << ", got " << tx.angle_commitment_hashes.size()
        );
        return false;
    }

    // Sign the complete immutable revoke intent, including fee inputs/change.
    // Signing only the target proof id allowed a relay to substitute inputs and
    // burn an arbitrary amount from the primary multisig wallet.
    Hash256 revoke_hash = tx.get_revoke_delegation_hash();

    if (SyncDebugEnabled()) {
        std::ostringstream ss;
        ss << "Revoke hash: ";
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 8; ++i) {
            ss << std::setw(2) << (int)revoke_hash[i];
        }
        ss << "..." << std::dec;
        GEO_LOG_DEBUG("multisig", ss.str());
    }

    // 7. Проверяем все commitments
    std::set<std::string> signers;  // Для проверки уникальности

    for (size_t i = 0; i < tx.angle_commitment_hashes.size(); i++) {
        const auto& commitment_hash = tx.angle_commitment_hashes[i];

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("multisig", "Checking commitment " << i);
        }

        // Загружаем commitment
        auto commitment = angle_store_->get_by_hash(commitment_hash);
        if (!commitment) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " not found in store");
            return false;
        }

        // Проверяем тип
        if (commitment->type != core::TransactionType::ANGLE_COMMITMENT) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " has wrong type");
            return false;
        }

        // Проверяем multisig_address (должен быть primary)
        if (commitment->multisig_address.str() != delegation_proof_tx.primary_multisig_address.str()) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " is for wrong multisig address");
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("multisig", "  Expected: " << delegation_proof_tx.primary_multisig_address.str());
                GEO_LOG_DEBUG("multisig", "  Got: " << commitment->multisig_address.str());
            }
            return false;
        }

        if (commitment->spending_tx_hash != revoke_hash) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " is for wrong revoke target");
            return false;
        }

        // Проверяем подпись Falcon-512
        if (!verify_angle_commitment(*commitment)) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " has invalid signature");
            return false;
        }

        // Проверяем, что подписант является участником primary multisig
        bool is_participant = false;
        for (const auto& participant : delegation_proof_tx.primary_participants) {
            if (participant.str() == commitment->signer_address.str()) {
                is_participant = true;
                break;
            }
        }

        if (!is_participant) {
            GEO_LOG_ERROR(
                "multisig",
                "Signer " << commitment->signer_address.str() << " is not a participant of primary multisig"
            );
            return false;
        }

        // Проверяем уникальность подписанта
        std::string signer_str = commitment->signer_address.str();
        if (signers.count(signer_str) > 0) {
            GEO_LOG_ERROR("multisig", "Duplicate signer: " << signer_str);
            return false;
        }
        signers.insert(signer_str);

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG(
                "multisig",
                "Commitment " << i << " OK (signer: " << commitment->signer_address.str() << ")"
            );
        }
    }

    // Нужно минимум primary_m уникальных подписей.
    if (signers.size() < primary_m) {
        GEO_LOG_ERROR(
            "multisig",
            "Not enough unique signers for REVOKE_DELEGATION. Need " << (int)primary_m << ", got " << signers.size()
        );
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "REVOKE_DELEGATION verification success");
    }
    return true;
}

bool MultisigVerifier::verify_hierarchical_spend(
    const core::Transaction& tx,
    const core::Transaction& delegation_proof_tx
) const {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("multisig", "Verifying HIERARCHICAL_SPEND transaction");
    }

    // 1. Проверяем тип транзакции
    if (tx.type != core::TransactionType::MULTISIG_SPEND) {
        GEO_LOG_ERROR("multisig", "Wrong transaction type");
        return false;
    }

    // 2. Проверяем наличие delegation_proof_txid
    bool is_null_hash = true;
    for (auto byte : tx.delegation_proof_txid) {
        if (byte != 0) {
            is_null_hash = false;
            break;
        }
    }

    if (is_null_hash) {
        GEO_LOG_ERROR("multisig", "No delegation proof txid in HIERARCHICAL_SPEND");
        return false;
    }

    if (SyncDebugEnabled()) {
        std::ostringstream ss;
        ss << "Delegation proof txid: ";
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 8; ++i) {
            ss << std::setw(2) << (int)tx.delegation_proof_txid[i];
        }
        ss << "..." << std::dec;
        GEO_LOG_DEBUG("multisig", ss.str());
    }

    if (tx.delegation_proof_txid != delegation_proof_tx.get_hash()) {
        GEO_LOG_ERROR("multisig", "Hierarchical spend references a different delegation proof");
        return false;
    }

    // 3. Верифицируем delegation proof
    if (!verify_delegation_proof(delegation_proof_tx)) {
        GEO_LOG_ERROR("multisig", "Delegation proof verification failed");
        return false;
    }

    // ВАЖНО: Лимиты делегирования (max_total_delegated / max_per_transaction / max_percent_of_balance)
    // проверяются и применяются атомарно на уровне ChainState (UTXO state), а не здесь.
    // MultisigVerifier отвечает только за криптографическую и структурную валидность.

    // 5. Проверяем M и N делегированного мультисига
    if (tx.m != delegation_proof_tx.delegated_m || tx.n != delegation_proof_tx.delegated_n) {
        GEO_LOG_ERROR(
            "multisig",
            "M/N mismatch with delegation proof. Expected "
                << (int)delegation_proof_tx.delegated_m << "-of-" << (int)delegation_proof_tx.delegated_n
                << ", got " << (int)tx.m << "-of-" << (int)tx.n
        );
        return false;
    }

    // 6. Проверяем количество commitments
    if (tx.angle_commitment_hashes.size() < tx.m) {
        GEO_LOG_ERROR(
            "multisig",
            "Not enough commitments. Need " << (int)tx.m << ", got " << tx.angle_commitment_hashes.size()
        );
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "multisig",
            "Found " << tx.angle_commitment_hashes.size() << " commitments (need " << (int)tx.m << ")"
        );
    }

    // 7. Bind the complete draft, including the selected inputs and fee.
    Hash256 spending_tx_hash = tx.get_hierarchical_spend_hash();

    if (SyncDebugEnabled()) {
        std::ostringstream ss;
        ss << "Hierarchical spend hash: ";
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 8; ++i) {
            ss << std::setw(2) << (int)spending_tx_hash[i];
        }
        ss << "..." << std::dec;
        GEO_LOG_DEBUG("multisig", ss.str());
    }

    // 8. Загружаем и проверяем все commitments от делегированного мультисига
    std::set<std::string> signers;  // Для проверки уникальности

    for (size_t i = 0; i < tx.angle_commitment_hashes.size(); i++) {
        const auto& commitment_hash = tx.angle_commitment_hashes[i];

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("multisig", "Checking commitment " << i);
        }

        // Загружаем commitment
        auto commitment = angle_store_->get_by_hash(commitment_hash);
        if (!commitment) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " not found in store");
            return false;
        }

        // Проверяем тип
        if (commitment->type != core::TransactionType::ANGLE_COMMITMENT) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " has wrong type");
            return false;
        }

        // Проверяем multisig_address (должен быть delegated)
        if (commitment->multisig_address.str() != delegation_proof_tx.delegated_multisig_address.str()) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " is for wrong multisig address");
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("multisig", "  Expected: " << delegation_proof_tx.delegated_multisig_address.str());
                GEO_LOG_DEBUG("multisig", "  Got: " << commitment->multisig_address.str());
            }
            return false;
        }

        // Проверяем spending_tx_hash
        if (commitment->spending_tx_hash != spending_tx_hash) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " is for wrong spending tx");
            return false;
        }

        // Проверяем подпись Falcon-512
        if (!verify_angle_commitment(*commitment)) {
            GEO_LOG_ERROR("multisig", "Commitment " << i << " has invalid signature");
            return false;
        }

        // Проверяем, что подписант является участником делегированного мультисига
        bool is_participant = false;
        for (const auto& participant : delegation_proof_tx.delegated_participants) {
            if (participant.str() == commitment->signer_address.str()) {
                is_participant = true;
                break;
            }
        }

        if (!is_participant) {
            GEO_LOG_ERROR(
                "multisig",
                "Signer " << commitment->signer_address.str() << " is not a participant of delegated multisig"
            );
            return false;
        }

        // Проверяем уникальность подписанта
        std::string signer_key = commitment->signer_address.str();
        if (signers.count(signer_key) > 0) {
            // КРИТИЧНО: Не логируем адреса подписывателей (информация о конфиденциальности)
            GEO_LOG_ERROR("multisig", "Duplicate signer detected");
            return false;
        }
        signers.insert(signer_key);

        if (SyncDebugEnabled()) {
            // КРИТИЧНО: Не логируем адреса в debug логах
            GEO_LOG_DEBUG("multisig", "Commitment OK");
        }
    }

    // 10. Проверяем, что собрано достаточно уникальных подписей
    if (signers.size() < tx.m) {
        GEO_LOG_ERROR(
            "multisig",
            "Not enough unique signers. Need " << (int)tx.m << ", got " << signers.size()
        );
        return false;
    }

    // 11. ВАЖНО: Проверяем что входы берутся из PRIMARY адреса
    // Эта проверка должна выполняться на уровне ChainState/State
    // так как MultisigVerifier не имеет доступа к UTXO
    if (SyncDebugEnabled()) {
        GEO_LOG_WARN("multisig", "UTXO ownership check must be done at State level");
        GEO_LOG_DEBUG(
            "multisig",
            "Inputs must be from: " << delegation_proof_tx.primary_multisig_address.str()
        );
        GEO_LOG_DEBUG("multisig", "This is verified in State::validate_transaction()");
        GEO_LOG_DEBUG("multisig", "HIERARCHICAL_SPEND verification success");
    }
    return true;
}

} // namespace geocoin::multisig
