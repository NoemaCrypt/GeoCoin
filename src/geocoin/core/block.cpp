#include "geocoin/core/block.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/varint.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace geocoin::core {

namespace {

constexpr size_t kHeaderBytes = 124;

void write_le32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

void write_le64(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

void write_header(const BlockHeader& header, uint8_t out[kHeaderBytes]) {
    size_t offset = 0;
    write_le32(out + offset, header.version);
    offset += 4;
    write_le32(out + offset, header.timestamp);
    offset += 4;
    std::memcpy(out + offset, header.prev_block_hash.data(), header.prev_block_hash.size());
    offset += header.prev_block_hash.size();
    std::memcpy(out + offset, header.merkle_root.data(), header.merkle_root.size());
    offset += header.merkle_root.size();
    write_le32(out + offset, header.bits);
    offset += 4;
    write_le64(out + offset, header.geo_tag);
    offset += 8;
    std::memcpy(out + offset, header.witness_commitment.data(), header.witness_commitment.size());
    offset += header.witness_commitment.size();
    write_le64(out + offset, header.nonce);
}

} // namespace

bytes BlockHeader::serialize() const {
    bytes data(kHeaderBytes);
    write_header(*this, data.data());
    return data;
}

BlockHeader BlockHeader::deserialize(const bytes& data) {
    if (data.size() != 124) {
        throw std::runtime_error("Invalid header data");
    }

    BlockHeader header;
    size_t offset = 0;

    // версия
    header.version = 0;
    for (int i = 0; i < 4; i++) {
        header.version |= (static_cast<uint32_t>(data[offset++]) << (i * 8));
    }

    // timestamp
    header.timestamp = 0;
    for (int i = 0; i < 4; i++) {
        header.timestamp |= (static_cast<uint32_t>(data[offset++]) << (i * 8));
    }

    // prev_block_hash
    std::copy(data.begin() + offset, data.begin() + offset + 32, header.prev_block_hash.begin());
    offset += 32;

    // merkle_root
    std::copy(data.begin() + offset, data.begin() + offset + 32, header.merkle_root.begin());
    offset += 32;

    // bits
    header.bits = 0;
    for (int i = 0; i < 4; i++) {
        header.bits |= (static_cast<uint32_t>(data[offset++]) << (i * 8));
    }

    // geo_tag
    header.geo_tag = 0;
    for (int i = 0; i < 8; i++) {
        header.geo_tag |= (static_cast<uint64_t>(data[offset++]) << (i * 8));
    }

    // witness_commitment
    std::copy(data.begin() + offset, data.begin() + offset + 32, header.witness_commitment.begin());
    offset += 32;

    // nonce
    header.nonce = 0;
    for (int i = 0; i < 8; i++) {
        header.nonce |= (static_cast<uint64_t>(data[offset++]) << (i * 8));
    }

    return header;
}

Hash256 BlockHeader::get_hash() const {
    uint8_t raw[kHeaderBytes];
    write_header(*this, raw);
    return crypto::double_sha256(raw, kHeaderBytes);
}

Hash256 Block::calculate_merkle_root() {
    if (transactions.empty()) {
        bytes empty;
        header.merkle_root = crypto::sha256(empty);
        return header.merkle_root;
    }

    std::vector<Hash256> hashes;
    for (const auto& tx : transactions) {
        hashes.push_back(tx.get_hash());
    }

    header.merkle_root = compute_merkle_root(hashes);
    return header.merkle_root;
}

Hash256 Block::get_hash() const {
    return header.get_hash();
}

Hash256 Block::calculate_witness_commitment() const {
    static constexpr char kDomain[] = "geocoin:witness:commitment:v1";

    bytes payload;
    payload.reserve(64 + transactions.size() * 128);
    payload.insert(payload.end(), kDomain, kDomain + sizeof(kDomain) - 1);

    const auto tx_count = utils::encode_varint(transactions.size());
    payload.insert(payload.end(), tx_count.begin(), tx_count.end());

    for (const auto& tx : transactions) {
        const Hash256 txid = tx.get_hash();
        payload.insert(payload.end(), txid.begin(), txid.end());
        try {
            tx.append_witness(payload);
        } catch (const std::invalid_argument&) {
            return Hash256{};
        }
    }

    return crypto::double_sha256(payload);
}

bytes Block::serialize() const {
    bytes data;

    // Заголовок
    auto header_data = header.serialize();
    data.insert(data.end(), header_data.begin(), header_data.end());

    // Количество транзакций (VarInt)
    auto tx_count = utils::encode_varint(transactions.size());
    data.insert(data.end(), tx_count.begin(), tx_count.end());

    // Транзакции
    for (const auto& tx : transactions) {
        auto tx_data = tx.serialize();
        data.insert(data.end(), tx_data.begin(), tx_data.end());
    }

    return data;
}

Block Block::deserialize(const bytes& data) {
    if (data.size() > consensus::params::MAX_BLOCK_SIZE) {
        throw std::runtime_error("Invalid block data: block exceeds maximum size");
    }

    Block block;
    size_t offset = 0;

    // Заголовок (124 байта: 4+4+32+32+4+8+32+8)
    const size_t HEADER_SIZE = 124;
    if (offset + HEADER_SIZE > data.size()) {
        throw std::runtime_error("Invalid block data: header too short");
    }

    bytes header_data(data.begin() + offset, data.begin() + offset + HEADER_SIZE);
    block.header = BlockHeader::deserialize(header_data);
    offset += HEADER_SIZE;

    // Количество транзакций (VarInt)
    uint64_t tx_count = utils::decode_varint(data, offset);
    if (tx_count > consensus::params::MAX_BLOCK_TRANSACTIONS) {
        throw std::runtime_error("Invalid block data: too many transactions");
    }
    // An unbacked tx_count must not size the allocation; every transaction
    // encodes to more than kMinTransactionWireSize bytes.
    constexpr size_t kMinTransactionWireSize = 16;
    const size_t remaining = data.size() - offset;
    const size_t reservable = std::min(
        static_cast<size_t>(tx_count), remaining / kMinTransactionWireSize);
    block.transactions.reserve(reservable);

    // Транзакции
    for (uint64_t i = 0; i < tx_count; i++) {
        Transaction tx = Transaction::deserialize(data, offset);
        block.transactions.push_back(tx);
    }
    if (offset != data.size()) {
        throw std::runtime_error("Invalid block data: trailing bytes after transactions");
    }

    // A block has exactly one wire representation. Besides preventing parser
    // differentials, this also makes the bytes committed to disk identical to
    // the bytes used by txid/merkle and witness validation.
    if (block.serialize() != data) {
        throw std::runtime_error("Invalid block data: non-canonical encoding");
    }

    return block;
}

size_t Block::get_size() const {
    return serialize().size();
}

} // namespace geocoin::core
