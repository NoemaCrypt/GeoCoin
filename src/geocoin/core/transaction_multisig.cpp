#include "geocoin/core/transaction_codec.hpp"
#include "geocoin/core/transaction_internal.hpp"

#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/varint.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>

namespace geocoin::core::tx_codec {
namespace {

void append_hash256(bytes& data, const Hash256& hash) {
    data.insert(data.end(), hash.begin(), hash.end());
}

void append_hash256_list(bytes& data, const std::vector<Hash256>& hashes) {
    const auto count = utils::encode_varint(hashes.size());
    data.insert(data.end(), count.begin(), count.end());
    for (const auto& hash : hashes) {
        append_hash256(data, hash);
    }
}

void read_hash256(const bytes& data, size_t& offset, Hash256& out) {
    if (offset + 32 > data.size()) {
        throw std::runtime_error("Invalid transaction data");
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.begin());
    offset += 32;
}

void read_hash256_list(
    const bytes& data,
    size_t& offset,
    std::vector<Hash256>& out,
    uint64_t max_count
) {
    const uint64_t count = read_varint(data, offset);
    if (count > max_count) {
        throw std::runtime_error("Invalid transaction data: too many angle commitments");
    }
    out.clear();
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; i++) {
        Hash256 hash;
        read_hash256(data, offset, hash);
        out.push_back(hash);
    }
}

void append_le32(bytes& data, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        data.push_back((value >> (i * 8)) & 0xFF);
    }
}

void append_le64_amount(bytes& data, Amount value) {
    for (int i = 0; i < 8; i++) {
        data.push_back((value >> (i * 8)) & 0xFF);
    }
}

void append_le64_u64(bytes& data, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        data.push_back((value >> (i * 8)) & 0xFF);
    }
}

uint32_t read_le32(const bytes& data, size_t& offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("Invalid transaction data");
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= static_cast<uint32_t>(data[offset++]) << (i * 8);
    }
    return value;
}

uint64_t read_le64(const bytes& data, size_t& offset) {
    if (offset + 8 > data.size()) {
        throw std::runtime_error("Invalid transaction data");
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }
    return value;
}

std::string read_bounded_string(const bytes& data, size_t& offset, uint64_t max_len, const char* too_long) {
    const uint64_t len = read_varint(data, offset);
    if (len > max_len) {
        throw std::runtime_error(too_long);
    }
    require_remaining(data, offset, len);
    std::string value(data.begin() + offset, data.begin() + offset + static_cast<size_t>(len));
    offset += static_cast<size_t>(len);
    return value;
}

void append_address_list(bytes& data, const std::vector<Address>& addresses) {
    const auto count = utils::encode_varint(addresses.size());
    data.insert(data.end(), count.begin(), count.end());
    for (const auto& addr : addresses) {
        append_varbytes(data, addr.str());
    }
}

std::vector<Address> read_address_list(const bytes& data, size_t& offset, uint64_t max_count, const char* too_many) {
    const uint64_t count = read_varint(data, offset);
    if (count > max_count) {
        throw std::runtime_error(too_many);
    }
    std::vector<Address> addresses;
    addresses.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; i++) {
        addresses.emplace_back(read_bounded_string(
            data,
            offset,
            consensus::params::MAX_ADDRESS_SIZE,
            "Invalid transaction data: participant address too long"
        ));
    }
    return addresses;
}

bool is_hierarchical_spend(const Hash256& delegation_proof_txid) {
    for (auto b : delegation_proof_txid) {
        if (b != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

void append_multisig_payload(bytes& data, const Transaction& tx) {
    if (tx.type == TransactionType::ANGLE_COMMITMENT) {
        append_varbytes(data, tx.commitment_network_id);
        append_varbytes(data, tx.multisig_address.str());
        append_hash256(data, tx.spending_tx_hash);
        append_varbytes(data, tx.signer_address.str());
        data.push_back(0); // Reserved extension byte; must remain zero.
        return;
    }

    if (tx.type == TransactionType::MULTISIG_SPEND) {
        data.push_back(tx.m);
        data.push_back(tx.n);
        append_hash256(data, tx.delegation_proof_txid);
        append_hash256_list(data, tx.angle_commitment_hashes);
        if (!is_hierarchical_spend(tx.delegation_proof_txid)) {
            append_address_list(data, tx.spend_participants);
        }
        return;
    }

    if (tx.type == TransactionType::DELEGATION_PROOF) {
        append_varbytes(data, tx.primary_multisig_address.str());
        append_varbytes(data, tx.delegated_multisig_address.str());
        data.push_back(tx.primary_m);
        data.push_back(tx.primary_n);
        append_address_list(data, tx.primary_participants);
        data.push_back(tx.delegated_m);
        data.push_back(tx.delegated_n);
        append_address_list(data, tx.delegated_participants);
        append_le64_u64(data, tx.valid_from);
        append_le64_u64(data, tx.valid_until);
        append_le32(data, tx.nonce);
        append_le64_amount(data, tx.max_total_delegated);
        append_le64_amount(data, tx.max_per_transaction);
        data.push_back(tx.max_percent_of_balance);
        append_hash256_list(data, tx.angle_commitment_hashes);
        return;
    }

    if (tx.type == TransactionType::REVOKE_DELEGATION) {
        append_hash256(data, tx.revoked_delegation_proof_txid);
        append_hash256_list(data, tx.angle_commitment_hashes);
        return;
    }

    throw std::runtime_error("multisig payload requested for non-multisig transaction");
}

void read_multisig_payload(Transaction& tx, const bytes& data, size_t& offset) {
    if (tx.type == TransactionType::ANGLE_COMMITMENT) {
        uint64_t network_len = read_varint(data, offset);
        if (network_len == 0 || network_len > 32) {
            throw std::runtime_error("Invalid transaction data: commitment network id");
        }
        require_remaining(data, offset, network_len);
        tx.commitment_network_id.assign(
            data.begin() + offset,
            data.begin() + offset + static_cast<size_t>(network_len)
        );
        offset += static_cast<size_t>(network_len);

        tx.multisig_address = Address(read_bounded_string(
            data,
            offset,
            consensus::params::MAX_ADDRESS_SIZE,
            "Invalid transaction data: multisig address too long"
        ));
        read_hash256(data, offset, tx.spending_tx_hash);
        tx.signer_address = Address(read_bounded_string(
            data,
            offset,
            consensus::params::MAX_ADDRESS_SIZE,
            "Invalid transaction data: signer address too long"
        ));
        require_remaining(data, offset, 1);
        if (data[offset++] != 0) {
            throw std::runtime_error("Invalid transaction data: nonzero angle reserved byte");
        }
        return;
    }

    if (tx.type == TransactionType::MULTISIG_SPEND) {
        if (offset + 1 > data.size()) {
            throw std::runtime_error("Invalid transaction data");
        }
        tx.m = data[offset++];
        if (offset + 1 > data.size()) {
            throw std::runtime_error("Invalid transaction data");
        }
        tx.n = data[offset++];
        read_hash256(data, offset, tx.delegation_proof_txid);
        read_hash256_list(data, offset, tx.angle_commitment_hashes, consensus::params::MAX_ANGLE_COMMITMENTS);
        if (!is_hierarchical_spend(tx.delegation_proof_txid)) {
            if (offset >= data.size()) {
                throw std::runtime_error("Invalid transaction data");
            }
            tx.spend_participants = read_address_list(
                data,
                offset,
                consensus::params::MAX_MULTISIG_PARTICIPANTS,
                "Invalid transaction data: too many participants"
            );
        }
        return;
    }

    if (tx.type == TransactionType::DELEGATION_PROOF) {
        tx.primary_multisig_address = Address(read_bounded_string(
            data,
            offset,
            consensus::params::MAX_ADDRESS_SIZE,
            "Invalid transaction data: primary address too long"
        ));
        tx.delegated_multisig_address = Address(read_bounded_string(
            data,
            offset,
            consensus::params::MAX_ADDRESS_SIZE,
            "Invalid transaction data: delegated address too long"
        ));
        if (offset + 1 > data.size()) {
            throw std::runtime_error("Invalid transaction data");
        }
        tx.primary_m = data[offset++];
        if (offset + 1 > data.size()) {
            throw std::runtime_error("Invalid transaction data");
        }
        tx.primary_n = data[offset++];
        tx.primary_participants = read_address_list(
            data,
            offset,
            consensus::params::MAX_MULTISIG_PARTICIPANTS,
            "Invalid transaction data: too many primary participants"
        );
        if (offset + 1 > data.size()) {
            throw std::runtime_error("Invalid transaction data");
        }
        tx.delegated_m = data[offset++];
        if (offset + 1 > data.size()) {
            throw std::runtime_error("Invalid transaction data");
        }
        tx.delegated_n = data[offset++];
        tx.delegated_participants = read_address_list(
            data,
            offset,
            consensus::params::MAX_MULTISIG_PARTICIPANTS,
            "Invalid transaction data: too many delegated participants"
        );
        tx.valid_from = read_le64(data, offset);
        tx.valid_until = read_le64(data, offset);
        tx.nonce = read_le32(data, offset);
        tx.max_total_delegated = static_cast<Amount>(read_le64(data, offset));
        tx.max_per_transaction = static_cast<Amount>(read_le64(data, offset));
        if (offset + 1 > data.size()) {
            throw std::runtime_error("Invalid transaction data");
        }
        tx.max_percent_of_balance = data[offset++];
        read_hash256_list(data, offset, tx.angle_commitment_hashes, consensus::params::MAX_ANGLE_COMMITMENTS);
        return;
    }

    if (tx.type == TransactionType::REVOKE_DELEGATION) {
        read_hash256(data, offset, tx.revoked_delegation_proof_txid);
        read_hash256_list(data, offset, tx.angle_commitment_hashes, consensus::params::MAX_ANGLE_COMMITMENTS);
        return;
    }

    throw std::runtime_error("multisig payload requested for non-multisig transaction");
}

} // namespace geocoin::core::tx_codec

namespace geocoin::core {

bool verify_angle_commitment_transaction(const Transaction& tx) {
    if (tx.inputs.size() < 1) {
        return false;
    }
    const auto& input = tx.inputs[0];
    return crypto::verify_angle_commitment_signature(
        tx.get_angle_commitment_message_hash(),
        input.signature
    );
}



namespace {
constexpr std::array<byte, 29> kAngleCommitmentDomain{{
    'G', 'E', 'O', 'C', 'O', 'I', 'N', '/',
    'A', 'N', 'G', 'L', 'E', '_', 'C', 'O', 'M', 'M', 'I', 'T', 'M', 'E', 'N', 'T', '/',
    'V', '1', '/', 0
}};
} // namespace

Hash256 Transaction::get_angle_commitment_message_hash() const {
    require_canonical_payload();
    if (type != TransactionType::ANGLE_COMMITMENT) {
        throw std::logic_error("angle commitment message requested for another transaction type");
    }

    bytes preimage;
    preimage.reserve(
        kAngleCommitmentDomain.size() +
        commitment_network_id.size() +
        multisig_address.str().size() +
        signer_address.str().size() +
        spending_tx_hash.size() + 32
    );
    preimage.insert(preimage.end(), kAngleCommitmentDomain.begin(), kAngleCommitmentDomain.end());
    tx_codec::append_varbytes(preimage, commitment_network_id);

    for (int i = 0; i < 4; ++i) {
        preimage.push_back(static_cast<byte>((version >> (i * 8)) & 0xFF));
    }
    preimage.push_back(static_cast<byte>(type));
    for (int i = 0; i < 8; ++i) {
        preimage.push_back(static_cast<byte>((timestamp >> (i * 8)) & 0xFF));
    }
    for (int i = 0; i < 4; ++i) {
        preimage.push_back(static_cast<byte>((lock_time >> (i * 8)) & 0xFF));
    }
    preimage.push_back(witness_version);

    const auto input_count = utils::encode_varint(inputs.size());
    preimage.insert(preimage.end(), input_count.begin(), input_count.end());
    for (const auto& input : inputs) {
        for (int i = 0; i < 4; ++i) {
            preimage.push_back(static_cast<byte>((input.vout >> (i * 8)) & 0xFF));
        }
        for (int i = 0; i < 4; ++i) {
            preimage.push_back(static_cast<byte>((input.sequence >> (i * 8)) & 0xFF));
        }
        const auto script_size = utils::encode_varint(input.script_sig.size());
        preimage.insert(preimage.end(), script_size.begin(), script_size.end());
        preimage.insert(preimage.end(), input.script_sig.begin(), input.script_sig.end());
    }

    const auto output_count = utils::encode_varint(outputs.size());
    preimage.insert(preimage.end(), output_count.begin(), output_count.end());
    for (const auto& output : outputs) {
        const auto serialized = output.serialize();
        preimage.insert(preimage.end(), serialized.begin(), serialized.end());
    }

    tx_codec::append_varbytes(preimage, multisig_address.str());
    preimage.insert(preimage.end(), spending_tx_hash.begin(), spending_tx_hash.end());
    tx_codec::append_varbytes(preimage, signer_address.str());
    preimage.push_back(0); // The same fixed reserved byte as the wire body.

    return crypto::double_sha256(preimage);
}

Transaction Transaction::create_angle_commitment(
    const Address& multisig_address,
    const Hash256& spending_tx_hash,
    const bytes& private_key,
    const Address& signer_address,
    const std::string& network_id
) {
    Transaction tx;
    tx.version = 2;
    tx.type = TransactionType::ANGLE_COMMITMENT;
    // Freshness belongs to the full draft hash. Mutable timestamps would
    // allow multiple encodings of the same authorization, so consensus fixes
    // the timestamp to zero for commitments.
    tx.timestamp = 0;

    if (network_id.empty() || network_id.size() > 32) {
        throw std::runtime_error("Invalid commitment network id");
    }
    tx.commitment_network_id = network_id;

    // Заполняем поля ANGLE_COMMITMENT
    tx.multisig_address = multisig_address;
    tx.spending_tx_hash = spending_tx_hash;

    Address signer_addr;
    if (!crypto::compute_address_from_private_key(private_key, signer_addr)) {
        throw std::runtime_error("Failed to derive signer address");
    }
    if (!signer_address.str().empty() && signer_address.str() != signer_addr.str()) {
        throw std::runtime_error("Private key does not match signer address");
    }
    tx.signer_address = signer_addr;

    // The synthetic input is both the anti-replay key and the signed intent.
    TxInput commitment_input;
    commitment_input.vout = 0xFFFFFFFE;
    tx.inputs.push_back(commitment_input);
    const Hash256 intent_hash = tx.get_angle_commitment_message_hash();
    tx.inputs[0].prev_tx_hash = intent_hash;
    tx.inputs[0].signature = crypto::sign_angle_commitment(intent_hash, private_key);

    // ANGLE_COMMITMENT не имеет выходов (это просто публикация подписи)

    return tx;
}

Transaction Transaction::create_multisig_spend(
    const std::vector<TxInput>& inputs,
    const std::vector<TxOutput>& outputs,
    const std::vector<Hash256>& angle_commitment_hashes,
    uint8_t m,
    uint8_t n
) {
    Transaction tx;
    // В прод-версии MULTISIG_SPEND используем версию 2:
    // v1 не содержит ончейн-конфига для обычной мультиподписи и не может быть бездоверенным.
    tx.version = 2;
    tx.type = TransactionType::MULTISIG_SPEND;
    tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    tx.inputs = inputs;
    tx.outputs = outputs;
    tx.angle_commitment_hashes = angle_commitment_hashes;
    tx.m = m;
    tx.n = n;

    // ВАЖНО: Инициализируем delegation_proof_txid нулями для обычного multisig
    tx.delegation_proof_txid = Hash256{};  // Все нули

    return tx;
}

Hash256 Transaction::get_hash_without_commitments() const {
    require_canonical_payload();
    // Создаем копию транзакции без angle_commitment_hashes
    Transaction temp = *this;
    temp.angle_commitment_hashes.clear();

    auto data = temp.serialize_without_witness();
    return crypto::double_sha256(data);
}

Hash256 Transaction::get_hierarchical_spend_hash() const {
    // The draft fixes the inputs before signing. Bind the complete transaction,
    // including its fee inputs, sequence, version and lock time; finalization
    // may only attach the commitment references.
    return get_hash_without_commitments();
}

Hash256 Transaction::get_delegation_proof_hash() const {
    return get_hash_without_commitments();
}

Hash256 Transaction::get_revoke_delegation_hash() const {
    return get_hash_without_commitments();
}

} // namespace geocoin::core
