#include "geocoin/core/transaction.hpp"
#include "geocoin/core/transaction_codec.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/varint.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace geocoin::core {

bytes TxInput::serialize() const {
    bytes data;

    // prev_tx_hash (32 байта)
    data.insert(data.end(), prev_tx_hash.begin(), prev_tx_hash.end());

    // vout (4 байта)
    for (int i = 0; i < 4; i++) {
        data.push_back((vout >> (i * 8)) & 0xFF);
    }

    // длина script_sig (VarInt) + данные
    auto len_bytes = utils::encode_varint(script_sig.size());
    data.insert(data.end(), len_bytes.begin(), len_bytes.end());
    data.insert(data.end(), script_sig.begin(), script_sig.end());

    // sequence (4 байта)
    for (int i = 0; i < 4; i++) {
        data.push_back((sequence >> (i * 8)) & 0xFF);
    }

    return data;
}

bytes TxOutput::serialize() const {
    bytes data;

    // value (8 байт)
    for (int i = 0; i < 8; i++) {
        data.push_back((value >> (i * 8)) & 0xFF);
    }

    // address (VarInt длина + строка)
    std::string addr_str = address.str();
    auto addr_len = utils::encode_varint(addr_str.size());
    data.insert(data.end(), addr_len.begin(), addr_len.end());
    data.insert(data.end(), addr_str.begin(), addr_str.end());

    // длина script_pubkey (VarInt) + данные
    auto len_bytes = utils::encode_varint(script_pubkey.size());
    data.insert(data.end(), len_bytes.begin(), len_bytes.end());
    data.insert(data.end(), script_pubkey.begin(), script_pubkey.end());

    return data;
}

bytes Transaction::serialize_without_witness() const {
    require_canonical_payload();
    bytes data;

    // version (4 байта)
    for (int i = 0; i < 4; i++) {
        data.push_back((version >> (i * 8)) & 0xFF);
    }

    // type (1 байт)
    data.push_back(static_cast<uint8_t>(type));

    // timestamp (8 байт)
    for (int i = 0; i < 8; i++) {
        data.push_back((timestamp >> (i * 8)) & 0xFF);
    }

    // количество входов (VarInt)
    auto inputs_count = utils::encode_varint(inputs.size());
    data.insert(data.end(), inputs_count.begin(), inputs_count.end());
    for (const auto& input : inputs) {
        auto input_data = input.serialize();
        data.insert(data.end(), input_data.begin(), input_data.end());

        // НОВОЕ: подпись во входе хранится ТОЛЬКО для ANGLE_COMMITMENT.
        if (type == TransactionType::ANGLE_COMMITMENT) {
            auto sig_len_bytes = utils::encode_varint(input.signature.size());
            data.insert(data.end(), sig_len_bytes.begin(), sig_len_bytes.end());
            data.insert(data.end(), input.signature.begin(), input.signature.end());
        } else {
            // Для остальных типов храним только хеш подписи (32 байта)
            data.insert(data.end(), input.signature_hash.begin(), input.signature_hash.end());
        }
    }

    // количество выходов (VarInt)
    auto outputs_count = utils::encode_varint(outputs.size());
    data.insert(data.end(), outputs_count.begin(), outputs_count.end());
    for (const auto& output : outputs) {
        auto output_data = output.serialize();
        data.insert(data.end(), output_data.begin(), output_data.end());
    }

    // lock_time (4 байта)
    for (int i = 0; i < 4; i++) {
        data.push_back((lock_time >> (i * 8)) & 0xFF);
    }

    // === Дополнительные поля в зависимости от типа ===

    if (is_multisig_family_type(type)) {
        tx_codec::append_multisig_payload(data, *this);
    }
    else if (type == TransactionType::CADASTRAL_DOC) {
        tx_codec::append_cadastral_payload(data, *this);
    }
    else if (is_card_transition_type(type)) {
        tx_codec::append_card_payload(data, *this);
    }
    else if (is_sidechain_transition_type(type)) {
        tx_codec::append_sidechain_payload(data, *this);
    }
    else if (is_nft_transition_type(type)) {
        tx_codec::append_nft_payload(data, *this);
    }
    else if (type == TransactionType::UPDATE_FOUNDATION_ADDRESS) {
        tx_codec::append_foundation_payload(data, *this);
    }

    return data;
}

bytes Transaction::serialize() const {
    bytes data = serialize_without_witness();
    append_witness(data);
    return data;
}

void Transaction::append_witness(bytes& data) const {
    const size_t witness_count = std::max(witnesses.size(), inputs.size());
    const TxWitness empty_witness;

    data.push_back(witness_version);

    auto wcount = utils::encode_varint(witness_count);
    data.insert(data.end(), wcount.begin(), wcount.end());
    for (size_t i = 0; i < witness_count; ++i) {
        const auto& w = i < witnesses.size() ? witnesses[i] : empty_witness;
        data.insert(data.end(), w.public_key_hash.begin(), w.public_key_hash.end());
        uint8_t flags = 0;
        if (!w.public_key.empty()) {
            if (w.public_key.size() != consensus::params::WITNESS_PUBKEY_SIZE) {
                throw std::invalid_argument("Invalid witness public key size for serialization");
            }
            flags |= 0x01;
        }
        data.push_back(flags);
        if ((flags & 0x01u) != 0) {
            data.insert(data.end(), w.public_key.begin(), w.public_key.end());
        }

        auto sig_len = utils::encode_varint(w.signature.size());
        data.insert(data.end(), sig_len.begin(), sig_len.end());
        data.insert(data.end(), w.signature.begin(), w.signature.end());
    }
}

Transaction Transaction::deserialize(const bytes& data, size_t& offset) {
    Transaction tx;

    if (offset > data.size()) {
        throw std::runtime_error("Invalid transaction data");
    }

    auto require_remaining = [&](uint64_t len) {
        if (len > data.size() - offset) {
            throw std::runtime_error("Invalid transaction data");
        }
    };
    auto read_varint = [&]() -> uint64_t {
        return utils::decode_varint(data, offset);
    };

    // version (4 байта)
    if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
    tx.version = 0;
    for (int i = 0; i < 4; i++) {
        tx.version |= (uint32_t)data[offset++] << (i * 8);
    }

    // type (1 байт)
    if (offset + 1 > data.size()) throw std::runtime_error("Invalid transaction data");
    const auto parsed_type = static_cast<TransactionType>(data[offset++]);
    if (!is_known_transaction_type(parsed_type)) {
        throw std::runtime_error("Invalid transaction data: unknown transaction type");
    }
    tx.type = parsed_type;

    // timestamp (8 байт)
    if (offset + 8 > data.size()) throw std::runtime_error("Invalid transaction data");
    tx.timestamp = 0;
    for (int i = 0; i < 8; i++) {
        tx.timestamp |= (uint64_t)data[offset++] << (i * 8);
    }

    // количество входов (VarInt)
    uint64_t inputs_count = read_varint();
    if (inputs_count > consensus::params::MAX_TX_INPUTS) {
        throw std::runtime_error("Invalid transaction data: too many inputs");
    }

    // входы
    for (uint64_t i = 0; i < inputs_count; i++) {
        TxInput input;

        // prev_tx_hash (32 байта)
        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, input.prev_tx_hash.begin());
        offset += 32;

        // vout (4 байта)
        if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
        input.vout = 0;
        for (int j = 0; j < 4; j++) {
            input.vout |= (uint32_t)data[offset++] << (j * 8);
        }

        // длина script_sig (VarInt) + данные
        uint64_t script_len = read_varint();
        if (script_len > consensus::params::MAX_SCRIPT_SIG_SIZE) {
            throw std::runtime_error("Invalid transaction data: script_sig too large");
        }
        require_remaining(script_len);
        input.script_sig.assign(data.begin() + offset, data.begin() + offset + script_len);
        offset += script_len;

        // sequence (4 байта)
        if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
        input.sequence = 0;
        for (int j = 0; j < 4; j++) {
            input.sequence |= (uint32_t)data[offset++] << (j * 8);
        }

        // Подпись во входе присутствует только для ANGLE_COMMITMENT
        if (tx.type == TransactionType::ANGLE_COMMITMENT) {
            uint64_t sig_len = read_varint();
            if (sig_len > consensus::params::MAX_SIGNATURE_SIZE) {
                throw std::runtime_error("Invalid transaction data: signature too large");
            }
            require_remaining(sig_len);

            input.signature.resize(sig_len);
            if (sig_len > 0) {
                std::copy(data.begin() + offset, data.begin() + offset + sig_len, input.signature.begin());
            }
            offset += sig_len;
        } else {
            input.signature.clear();
            if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data: signature_hash");
            std::copy(data.begin() + offset, data.begin() + offset + 32, input.signature_hash.begin());
            offset += 32;
        }

        tx.inputs.push_back(input);
    }

    // количество выходов (VarInt)
    uint64_t outputs_count = read_varint();
    if (outputs_count > consensus::params::MAX_TX_OUTPUTS) {
        throw std::runtime_error("Invalid transaction data: too many outputs");
    }

    // выходы
    for (uint64_t i = 0; i < outputs_count; i++) {
        TxOutput output;

        // value (8 байт)
        if (offset + 8 > data.size()) throw std::runtime_error("Invalid transaction data");
        output.value = 0;
        for (int j = 0; j < 8; j++) {
            output.value |= (uint64_t)data[offset++] << (j * 8);
        }

        // address (VarInt длина + строка)
        uint64_t addr_len = read_varint();
        if (addr_len > consensus::params::MAX_ADDRESS_SIZE) {
            throw std::runtime_error("Invalid transaction data: address too long");
        }
        require_remaining(addr_len);
        std::string addr_str(data.begin() + offset, data.begin() + offset + addr_len);
        output.address = Address(addr_str);
        offset += addr_len;

        // длина script_pubkey (VarInt) + данные
        uint64_t script_len = read_varint();
        if (script_len > consensus::params::MAX_SCRIPT_PUBKEY_SIZE) {
            throw std::runtime_error("Invalid transaction data: script_pubkey too large");
        }
        require_remaining(script_len);
        output.script_pubkey.assign(data.begin() + offset, data.begin() + offset + script_len);
        offset += script_len;

        tx.outputs.push_back(output);
    }

    // lock_time (4 байта)
    if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
    tx.lock_time = 0;
    for (int i = 0; i < 4; i++) {
        tx.lock_time |= (uint32_t)data[offset++] << (i * 8);
    }

    // === Дополнительные поля в зависимости от типа ===

    if (is_multisig_family_type(tx.type)) {
        tx_codec::read_multisig_payload(tx, data, offset);
    }
    else if (tx.type == TransactionType::CADASTRAL_DOC) {
        tx_codec::read_cadastral_payload(tx, data, offset);
    }
    else if (is_card_transition_type(tx.type)) {
        tx_codec::read_card_payload(tx, data, offset);
    }
    else if (is_sidechain_transition_type(tx.type)) {
        tx_codec::read_sidechain_payload(tx, data, offset);
    }
    else if (is_nft_transition_type(tx.type)) {
        tx_codec::read_nft_payload(tx, data, offset);
    }
    else if (tx.type == TransactionType::UPDATE_FOUNDATION_ADDRESS) {
        tx_codec::read_foundation_payload(tx, data, offset);
    }

    // Версия witness-формата (обязательна)
    if (offset + 1 > data.size()) {
        throw std::runtime_error("Invalid transaction data: witness version missing");
    }
    tx.witness_version = data[offset++];
    if (tx.witness_version != consensus::params::WITNESS_FORMAT_VERSION_CURRENT) {
        throw std::runtime_error("Invalid transaction data: unsupported witness version");
    }

    // Данные witness (обязательны)
    uint64_t witness_count = read_varint();
    if (witness_count > consensus::params::MAX_TX_INPUTS) {
        throw std::runtime_error("Invalid transaction data: too many witnesses");
    }
    if (witness_count != tx.inputs.size()) {
        throw std::runtime_error("Invalid transaction data: witness count mismatch");
    }
    tx.witnesses.clear();
    tx.witnesses.reserve(witness_count);
    for (uint64_t i = 0; i < witness_count; ++i) {
        TxWitness w;
        if (offset + 32 > data.size()) {
            throw std::runtime_error("Invalid transaction data: witness pubkey hash missing");
        }
        std::copy(data.begin() + offset, data.begin() + offset + 32, w.public_key_hash.begin());
        offset += 32;
        if (offset + 1 > data.size()) {
            throw std::runtime_error("Invalid transaction data: witness flags missing");
        }
        const uint8_t flags = data[offset++];
        if ((flags & 0xFEu) != 0) {
            throw std::runtime_error("Invalid transaction data: witness flags invalid");
        }
        if ((flags & 0x01u) != 0) {
            if (offset + consensus::params::WITNESS_PUBKEY_SIZE > data.size()) {
                throw std::runtime_error("Invalid transaction data: witness pubkey missing");
            }
            w.public_key.assign(
                data.begin() + offset,
                data.begin() + offset + consensus::params::WITNESS_PUBKEY_SIZE
            );
            offset += consensus::params::WITNESS_PUBKEY_SIZE;
        }

        uint64_t sig_len = read_varint();
        if (sig_len > consensus::params::MAX_WITNESS_SIG_SIZE) {
            throw std::runtime_error("Invalid transaction data: witness signature too large");
        }
        require_remaining(sig_len);
        w.signature.assign(data.begin() + offset, data.begin() + offset + sig_len);
        offset += sig_len;

        tx.witnesses.push_back(std::move(w));
    }

    return tx;
}

} // namespace geocoin::core
