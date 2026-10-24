#include "geocoin/network/messages.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/logger.hpp"
#include <cstring>
#include <stdexcept>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <set>

namespace geocoin::network {

namespace {
inline bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}
}

uint32_t current_network_magic() {
    switch (consensus::Params().get_network()) {
        case consensus::Network::MAIN:
            return MessageHeader::MAGIC_MAINNET;
        case consensus::Network::TESTNET:
            return MessageHeader::MAGIC_TESTNET;
        case consensus::Network::LOADNET:
            return MessageHeader::MAGIC_LOADNET;
        case consensus::Network::MAX_NETWORK_TYPES:
            break;
    }
    return 0;
}

bool is_known_message_type(MessageType type) {
    switch (type) {
        case MessageType::VERSION:
        case MessageType::VERACK:
        case MessageType::ADDR:
        case MessageType::INV:
        case MessageType::GETDATA:
        case MessageType::BLOCK:
        case MessageType::TX:
        case MessageType::GETBLOCKS:
        case MessageType::GETHEADERS:
        case MessageType::HEADERS:
        case MessageType::CHALLENGE:
        case MessageType::RESPONSE:
        case MessageType::PING:
        case MessageType::PONG:
        case MessageType::REJECT:
        case MessageType::SECURE_HELLO:
        case MessageType::SECURE_REPLY:
            return true;
    }
    return false;
}

// ============================================================================
// Кросс-платформенные макросы для endianness (network byte order = big-endian)
// ============================================================================

#ifndef htobe64
  #ifdef __BYTE_ORDER__
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      #define htobe64(x) __builtin_bswap64(x)
      #define be64toh(x) __builtin_bswap64(x)
    #else
      #define htobe64(x) (x)
      #define be64toh(x) (x)
    #endif
  #else
    // Резервный вариант для систем без __BYTE_ORDER__
    #define htobe64(x) ((((x) & 0xFF00000000000000ULL) >> 56) | \
                        (((x) & 0x00FF000000000000ULL) >> 40) | \
                        (((x) & 0x0000FF0000000000ULL) >> 24) | \
                        (((x) & 0x000000FF00000000ULL) >> 8)  | \
                        (((x) & 0x00000000FF000000ULL) << 8)  | \
                        (((x) & 0x0000000000FF0000ULL) << 24) | \
                        (((x) & 0x000000000000FF00ULL) << 40) | \
                        (((x) & 0x00000000000000FFULL) << 56))
    #define be64toh(x) htobe64(x)
  #endif
#endif

// ============================================================================
// Утилиты для сериализации
// ============================================================================

namespace {
    constexpr uint32_t kMaxMessagePayload = 4 * 1024 * 1024; // 4 MB
    constexpr uint32_t kMaxVectorItems = 50000;
    constexpr uint32_t kMaxStringSize = 4096;
    constexpr uint32_t kMaxAddressCount = 1000;
    constexpr uint32_t kMaxInvItems = 50000;
    constexpr uint32_t kMaxLocatorHashes = kMaxHeadersPerMessage;
    constexpr uint32_t kMaxRejectReasonSize = 256;
    constexpr uint32_t kAuthChallengeValueSize = 32;
    constexpr uint32_t kAuthSessionIdSize = 16;
    constexpr uint32_t kAuthChannelBindingSize = 32;
    constexpr uint32_t kMaxAuthProofSize = 4096;

    void require_available(const bytes& buf, size_t offset, size_t count) {
        if (offset > buf.size() || count > buf.size() - offset) {
            throw std::runtime_error("Buffer underflow");
        }
    }
}

static void write_uint8(bytes& buf, uint8_t value) {
    buf.push_back(static_cast<byte>(value));
}

static void write_uint16(bytes& buf, uint16_t value) {
    buf.push_back(static_cast<byte>(value & 0xFF));
    buf.push_back(static_cast<byte>((value >> 8) & 0xFF));
}

static void write_uint32(bytes& buf, uint32_t value) {
    buf.push_back(static_cast<byte>(value & 0xFF));
    buf.push_back(static_cast<byte>((value >> 8) & 0xFF));
    buf.push_back(static_cast<byte>((value >> 16) & 0xFF));
    buf.push_back(static_cast<byte>((value >> 24) & 0xFF));
}

static void write_uint64(bytes& buf, uint64_t value) {
    // Используем big-endian (network byte order) для кросс-платформенности
    uint64_t be = htobe64(value);
    for (int i = 0; i < 8; i++) {
        buf.push_back(((uint8_t*)&be)[i]);
    }
}

static void write_double(bytes& buf, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(double));
    write_uint64(buf, bits);
}

static void write_bytes(bytes& buf, const bytes& data) {
    if (data.size() > kMaxMessagePayload) {
        throw std::runtime_error("Payload too large");
    }
    write_uint32(buf, static_cast<uint32_t>(data.size()));
    buf.insert(buf.end(), data.begin(), data.end());
}

static void write_string(bytes& buf, const std::string& str) {
    if (str.size() > kMaxStringSize) {
        throw std::runtime_error("String too long");
    }
    write_uint32(buf, static_cast<uint32_t>(str.size()));
    buf.insert(buf.end(), str.begin(), str.end());
}

static void write_hash256(bytes& buf, const Hash256& hash) {
    buf.insert(buf.end(), hash.begin(), hash.end());
}

static uint8_t read_uint8(const bytes& buf, size_t& offset) {
    require_available(buf, offset, 1);
    return buf[offset++];
}

static uint16_t read_uint16(const bytes& buf, size_t& offset) {
    require_available(buf, offset, 2);
    const uint16_t value =
        static_cast<uint16_t>(buf[offset]) |
        static_cast<uint16_t>(static_cast<uint16_t>(buf[offset + 1]) << 8);
    offset += 2;
    return value;
}

static uint32_t read_uint32(const bytes& buf, size_t& offset) {
    require_available(buf, offset, 4);
    const uint32_t value =
        static_cast<uint32_t>(buf[offset]) |
        (static_cast<uint32_t>(buf[offset + 1]) << 8) |
        (static_cast<uint32_t>(buf[offset + 2]) << 16) |
        (static_cast<uint32_t>(buf[offset + 3]) << 24);
    offset += 4;
    return value;
}

static uint64_t read_uint64(const bytes& buf, size_t& offset) {
    require_available(buf, offset, 8);
    // Читаем big-endian (network byte order)
    uint64_t be;
    for (int i = 0; i < 8; i++) {
        ((uint8_t*)&be)[i] = buf[offset + i];
    }
    offset += 8;
    return be64toh(be);
}

static double read_double(const bytes& buf, size_t& offset) {
    uint64_t bits = read_uint64(buf, offset);
    double value;
    std::memcpy(&value, &bits, sizeof(double));

    // Валидация: проверяем на NaN и Infinity
    if (std::isnan(value) || std::isinf(value)) {
        throw std::runtime_error("Invalid double value (NaN or Infinity)");
    }

    return value;
}

static bytes read_bytes(const bytes& buf, size_t& offset) {
    uint32_t len = read_uint32(buf, offset);
    if (len > kMaxMessagePayload) throw std::runtime_error("Payload too large");

    require_available(buf, offset, len);

    bytes data(buf.begin() + offset, buf.begin() + offset + len);
    offset += len;
    return data;
}

static std::string read_string(const bytes& buf, size_t& offset) {
    uint32_t len = read_uint32(buf, offset);
    if (len > kMaxStringSize) throw std::runtime_error("String too long");

    require_available(buf, offset, len);

    std::string str(buf.begin() + offset, buf.begin() + offset + len);
    offset += len;
    return str;
}

static Hash256 read_hash256(const bytes& buf, size_t& offset) {
    require_available(buf, offset, 32);
    Hash256 hash;
    std::copy(buf.begin() + offset, buf.begin() + offset + 32, hash.begin());
    offset += 32;
    return hash;
}

// ============================================================================
// Заголовок сообщения
// ============================================================================

bytes MessageHeader::serialize() const {
    bytes buf;
    write_uint32(buf, magic);
    write_uint8(buf, static_cast<uint8_t>(type));
    write_uint32(buf, length);
    // Контрольная сумма - только первые 4 байта
    for (int i = 0; i < 4; i++) {
        buf.push_back(checksum[i]);
    }
    return buf;
}

MessageHeader MessageHeader::deserialize(const bytes& data) {
    if (data.size() != 13) {
        throw std::runtime_error("MessageHeader too short");
    }

    MessageHeader header;
    size_t offset = 0;

    header.magic = read_uint32(data, offset);
    header.type = static_cast<MessageType>(read_uint8(data, offset));
    header.length = read_uint32(data, offset);
    if (!is_known_message_type(header.type)) {
        throw std::runtime_error("Unknown message type");
    }

    // Контрольная сумма - только первые 4 байта
    for (int i = 0; i < 4; i++) {
        header.checksum[i] = data[offset++];
    }

    if (header.length > kMaxMessagePayload) {
        throw std::runtime_error("Payload length too large");
    }

    return header;
}

// ============================================================================
// Сообщение Version
// ============================================================================

bytes VersionMessage::serialize() const {
    bytes buf;
    write_uint32(buf, version);
    write_uint64(buf, services);
    write_uint64(buf, timestamp);
    write_string(buf, user_agent);
    write_uint64(buf, height);
    write_string(buf, auth_address);
    return buf;
}

VersionMessage VersionMessage::deserialize(const bytes& data) {
    VersionMessage msg;
    size_t offset = 0;

    msg.version = read_uint32(data, offset);
    msg.services = read_uint64(data, offset);
    msg.timestamp = read_uint64(data, offset);
    msg.user_agent = read_string(data, offset);
    msg.height = read_uint64(data, offset);
    msg.auth_address = read_string(data, offset);

    if (offset != data.size()) {
        throw std::runtime_error("Trailing bytes in VersionMessage");
    }

    return msg;
}

// ============================================================================
// Обязательный гибридный handshake до VERSION
// ============================================================================

bytes SecureHelloMessage::serialize() const {
    if (version != kSecureWireVersion ||
        suite != kSecureSuiteHybridMlKem768X25519ChaCha20Poly1305 ||
        hello.x25519_public_key.size() != 32 ||
        hello.nonce.size() != 32 ||
        hello.ml_kem_public_key.size() != crypto::ml_kem768::kPublicKeySize) {
        throw std::runtime_error("Invalid SecureHelloMessage");
    }

    bytes buf;
    buf.reserve(kSecureHelloPayloadSize);
    write_uint16(buf, version);
    write_uint16(buf, suite);
    buf.insert(buf.end(), hello.x25519_public_key.begin(), hello.x25519_public_key.end());
    buf.insert(buf.end(), hello.nonce.begin(), hello.nonce.end());
    buf.insert(buf.end(), hello.ml_kem_public_key.begin(), hello.ml_kem_public_key.end());
    return buf;
}

SecureHelloMessage SecureHelloMessage::deserialize(const bytes& data) {
    if (data.size() != kSecureHelloPayloadSize) {
        throw std::runtime_error("Invalid SecureHelloMessage size");
    }
    SecureHelloMessage msg;
    size_t offset = 0;
    msg.version = read_uint16(data, offset);
    msg.suite = read_uint16(data, offset);
    if (msg.version != kSecureWireVersion ||
        msg.suite != kSecureSuiteHybridMlKem768X25519ChaCha20Poly1305) {
        throw std::runtime_error("Unsupported secure P2P protocol");
    }
    msg.hello.x25519_public_key.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;
    msg.hello.nonce.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;
    msg.hello.ml_kem_public_key.assign(data.begin() + offset, data.end());
    return msg;
}

bytes SecureReplyMessage::serialize() const {
    if (version != kSecureWireVersion ||
        suite != kSecureSuiteHybridMlKem768X25519ChaCha20Poly1305 ||
        reply.x25519_public_key.size() != 32 ||
        reply.nonce.size() != 32 ||
        reply.ml_kem_ciphertext.size() != crypto::ml_kem768::kCiphertextSize) {
        throw std::runtime_error("Invalid SecureReplyMessage");
    }

    bytes buf;
    buf.reserve(kSecureReplyPayloadSize);
    write_uint16(buf, version);
    write_uint16(buf, suite);
    buf.insert(buf.end(), reply.x25519_public_key.begin(), reply.x25519_public_key.end());
    buf.insert(buf.end(), reply.nonce.begin(), reply.nonce.end());
    buf.insert(buf.end(), reply.ml_kem_ciphertext.begin(), reply.ml_kem_ciphertext.end());
    return buf;
}

SecureReplyMessage SecureReplyMessage::deserialize(const bytes& data) {
    if (data.size() != kSecureReplyPayloadSize) {
        throw std::runtime_error("Invalid SecureReplyMessage size");
    }
    SecureReplyMessage msg;
    size_t offset = 0;
    msg.version = read_uint16(data, offset);
    msg.suite = read_uint16(data, offset);
    if (msg.version != kSecureWireVersion ||
        msg.suite != kSecureSuiteHybridMlKem768X25519ChaCha20Poly1305) {
        throw std::runtime_error("Unsupported secure P2P protocol");
    }
    msg.reply.x25519_public_key.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;
    msg.reply.nonce.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;
    msg.reply.ml_kem_ciphertext.assign(data.begin() + offset, data.end());
    return msg;
}

// ============================================================================
// Сообщение Challenge
// ============================================================================

bytes ChallengeMessage::serialize() const {
    bytes buf;
    if (challenge.value.size() != kAuthChallengeValueSize) {
        throw std::runtime_error("Invalid challenge value size");
    }
    if (challenge.session_id.size() != kAuthSessionIdSize) {
        throw std::runtime_error("Invalid session id size");
    }
    write_bytes(buf, challenge.value);
    write_uint64(buf, challenge.timestamp);
    buf.insert(buf.end(), challenge.session_id.begin(), challenge.session_id.end());
    buf.insert(
        buf.end(), challenge.channel_binding.begin(), challenge.channel_binding.end());
    return buf;
}

ChallengeMessage ChallengeMessage::deserialize(const bytes& data) {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("net", "ChallengeMessage::deserialize() data.size=" << data.size());
    }

    ChallengeMessage msg;
    size_t offset = 0;

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("net", "  Reading challenge value...");
    }
    const uint32_t challenge_size = read_uint32(data, offset);
    if (challenge_size != kAuthChallengeValueSize) {
        throw std::runtime_error("Invalid challenge value size");
    }
    require_available(data, offset, challenge_size);
    msg.challenge.value.assign(data.begin() + offset, data.begin() + offset + challenge_size);
    offset += challenge_size;
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("net", "  Challenge value size: " << msg.challenge.value.size());
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("net", "  Reading timestamp...");
    }
    msg.challenge.timestamp = read_uint64(data, offset);
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("net", "  Timestamp: " << msg.challenge.timestamp);
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("net", "  Reading session_id (offset=" << offset << ", need 16 bytes)...");
    }
    if (offset > data.size() || kAuthSessionIdSize > data.size() - offset) {
        GEO_LOG_ERROR(
            "net",
            "ChallengeMessage too short (offset=" << offset << ", data.size=" << data.size() << ")"
        );
        throw std::runtime_error("ChallengeMessage too short");
    }

    // Изменяем размер session_id перед копированием
    msg.challenge.session_id.resize(kAuthSessionIdSize);
    std::copy(data.begin() + offset, data.begin() + offset + kAuthSessionIdSize,
              msg.challenge.session_id.begin());
    offset += kAuthSessionIdSize;
    require_available(data, offset, kAuthChannelBindingSize);
    std::copy_n(
        data.begin() + offset,
        kAuthChannelBindingSize,
        msg.challenge.channel_binding.begin()
    );
    offset += kAuthChannelBindingSize;
    msg.challenge.nonce = msg.challenge.session_id;
    msg.challenge.rounds = {
        msg.challenge.session_id,
        msg.challenge.session_id,
        msg.challenge.session_id
    };

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "net",
            "  Deserialization complete (offset=" << offset << ", data.size=" << data.size() << ")"
        );
    }

    if (offset != data.size()) {
        GEO_LOG_WARN("net", "Trailing bytes in ChallengeMessage");
        throw std::runtime_error("Trailing bytes in ChallengeMessage");
    }

    return msg;
}

// ============================================================================
// Сообщение Response
// ============================================================================

bytes ResponseMessage::serialize() const {
    bytes buf;
    if (response.proof.empty() || response.proof.size() > kMaxAuthProofSize) {
        throw std::runtime_error("Invalid proof size");
    }
    write_uint32(buf, static_cast<uint32_t>(response.proof.size()));
    buf.insert(buf.end(), response.proof.begin(), response.proof.end());
    return buf;
}

ResponseMessage ResponseMessage::deserialize(const bytes& data) {
    ResponseMessage msg;
    size_t offset = 0;

    uint32_t proof_size = read_uint32(data, offset);
    if (proof_size == 0 || proof_size > kMaxAuthProofSize) {
        throw std::runtime_error("Invalid proof size");
    }
    if (offset > data.size() || proof_size > data.size() - offset) {
        throw std::runtime_error("ResponseMessage proof overflow");
    }
    msg.response.proof.assign(data.begin() + offset, data.begin() + offset + proof_size);
    msg.response.responses = {msg.response.proof, msg.response.proof, msg.response.proof};
    offset += proof_size;

    if (offset != data.size()) {
        throw std::runtime_error("Trailing bytes in ResponseMessage");
    }

    return msg;
}

// ============================================================================
// Сообщение Inv
// ============================================================================

bytes InvMessage::serialize() const {
    bytes buf;
    if (items.size() > kMaxInvItems) {
        throw std::runtime_error("Too many inv items");
    }
    write_uint32(buf, static_cast<uint32_t>(items.size()));

    std::set<std::pair<uint32_t, Hash256>> seen;
    for (const auto& item : items) {
        const uint32_t raw_type = static_cast<uint32_t>(item.type);
        if ((item.type != InvType::TX && item.type != InvType::BLOCK) ||
            !seen.emplace(raw_type, item.hash).second) {
            throw std::runtime_error("Invalid or duplicate inventory item");
        }
        write_uint32(buf, raw_type);
        write_hash256(buf, item.hash);
    }

    return buf;
}

InvMessage InvMessage::deserialize(const bytes& data) {
    InvMessage msg;
    size_t offset = 0;

    uint32_t count = read_uint32(data, offset);
    if (count > kMaxInvItems) {
        throw std::runtime_error("Too many inv items");
    }
    msg.items.reserve(count);
    std::set<std::pair<uint32_t, Hash256>> seen;

    for (uint32_t i = 0; i < count; i++) {
        InvItem item;
        const uint32_t raw_type = read_uint32(data, offset);
        item.type = static_cast<InvType>(raw_type);
        if (item.type != InvType::TX && item.type != InvType::BLOCK) {
            throw std::runtime_error("Invalid inventory type");
        }
        item.hash = read_hash256(data, offset);
        if (!seen.emplace(raw_type, item.hash).second) {
            throw std::runtime_error("Duplicate inventory item");
        }
        msg.items.push_back(item);
    }

    if (offset != data.size()) {
        throw std::runtime_error("Trailing bytes in InvMessage");
    }

    return msg;
}

// ============================================================================
// Сообщение Block
// ============================================================================

bytes BlockMessage::serialize() const {
    return block.serialize();
}

BlockMessage BlockMessage::deserialize(const bytes& data) {
    BlockMessage msg;
    msg.block = core::Block::deserialize(data);
    return msg;
}

// ============================================================================
// Сообщение Tx
// ============================================================================

bytes TxMessage::serialize() const {
    return tx.serialize();
}

TxMessage TxMessage::deserialize(const bytes& data) {
    TxMessage msg;
    msg.tx = core::Transaction::deserialize_canonical(data);
    return msg;
}

// ============================================================================
// Сообщение GetBlocks
// ============================================================================

bytes GetBlocksMessage::serialize() const {
    bytes buf;
    write_uint32(buf, version);
    if (locator_hashes.size() > kMaxLocatorHashes) {
        throw std::runtime_error("Too many locator hashes");
    }
    write_uint32(buf, static_cast<uint32_t>(locator_hashes.size()));

    for (const auto& hash : locator_hashes) {
        write_hash256(buf, hash);
    }

    write_hash256(buf, stop_hash);
    return buf;
}

GetBlocksMessage GetBlocksMessage::deserialize(const bytes& data) {
    GetBlocksMessage msg;
    size_t offset = 0;

    msg.version = read_uint32(data, offset);
    uint32_t count = read_uint32(data, offset);
    if (count > kMaxLocatorHashes) {
        throw std::runtime_error("Too many locator hashes");
    }

    msg.locator_hashes.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        msg.locator_hashes.push_back(read_hash256(data, offset));
    }

    msg.stop_hash = read_hash256(data, offset);
    if (offset != data.size()) {
        throw std::runtime_error("Trailing bytes in GetBlocksMessage");
    }
    return msg;
}
// Сообщение Headers
// ============================================================================

bytes HeadersMessage::serialize() const {
    bytes buf;

    if (headers.size() > kMaxHeadersPerMessage) {
        throw std::runtime_error("Too many headers");
    }
    write_uint32(buf, static_cast<uint32_t>(headers.size()));

    for (const auto& header : headers) {
        // HEADERS must use the same canonical header bytes as block hashing.
        // A previous hand-written encoding skipped witness_commitment, which
        // changed header hashes on the receiving side and broke P2P sync.
        const bytes header_bytes = header.serialize();
        buf.insert(buf.end(), header_bytes.begin(), header_bytes.end());
    }

    return buf;
}

HeadersMessage HeadersMessage::deserialize(const bytes& data) {
    HeadersMessage msg;
    size_t offset = 0;

    uint32_t count = read_uint32(data, offset);

    if (count > kMaxHeadersPerMessage) {
        throw std::runtime_error("Too many headers");
    }

    msg.headers.reserve(count);

    constexpr size_t kCanonicalHeaderBytes = 124;
    const size_t remaining = data.size() - offset;
    if (remaining != static_cast<size_t>(count) * kCanonicalHeaderBytes) {
        throw std::runtime_error("Invalid HeadersMessage size");
    }

    for (uint32_t i = 0; i < count; i++) {
        bytes header_bytes(data.begin() + offset, data.begin() + offset + kCanonicalHeaderBytes);
        msg.headers.push_back(core::BlockHeader::deserialize(header_bytes));
        offset += kCanonicalHeaderBytes;
    }

    if (offset != data.size()) {
        throw std::runtime_error("Trailing bytes in HeadersMessage");
    }

    return msg;
}

// ============================================================================
// Сообщение Addr
// ============================================================================

bytes AddrMessage::serialize() const {
    bytes buf;
    if (addresses.size() > kMaxAddressCount) {
        throw std::runtime_error("Too many addresses");
    }
    write_uint32(buf, static_cast<uint32_t>(addresses.size()));

    for (const auto& addr : addresses) {
        write_uint64(buf, addr.services);
        write_string(buf, addr.ip);
        write_uint16(buf, addr.port);
        write_uint64(buf, addr.timestamp);
    }

    return buf;
}

AddrMessage AddrMessage::deserialize(const bytes& data) {
    AddrMessage msg;
    size_t offset = 0;

    uint32_t count = read_uint32(data, offset);
    if (count > kMaxAddressCount) {
        throw std::runtime_error("Too many addresses");
    }
    msg.addresses.reserve(count);

    for (uint32_t i = 0; i < count; i++) {
        NetAddr addr;
        addr.services = read_uint64(data, offset);
        addr.ip = read_string(data, offset);
        addr.port = read_uint16(data, offset);
        addr.timestamp = read_uint64(data, offset);
        msg.addresses.push_back(addr);
    }

    if (offset != data.size()) {
        throw std::runtime_error("Trailing bytes in AddrMessage");
    }

    return msg;
}

// ============================================================================
// Сообщение Reject
// ============================================================================

bytes RejectMessage::serialize() const {
    if (reason.size() > kMaxRejectReasonSize) {
        throw std::runtime_error("Reject reason too long");
    }
    bytes buf;
    write_uint8(buf, static_cast<uint8_t>(rejected_type));
    write_uint8(buf, code);
    write_string(buf, reason);
    write_hash256(buf, rejected_hash);
    return buf;
}

RejectMessage RejectMessage::deserialize(const bytes& data) {
    RejectMessage msg;
    size_t offset = 0;
    msg.rejected_type = static_cast<MessageType>(read_uint8(data, offset));
    if (!is_known_message_type(msg.rejected_type)) {
        throw std::runtime_error("Unknown rejected message type");
    }
    msg.code = read_uint8(data, offset);
    msg.reason = read_string(data, offset);
    if (msg.reason.size() > kMaxRejectReasonSize) {
        throw std::runtime_error("Reject reason too long");
    }
    msg.rejected_hash = read_hash256(data, offset);
    if (offset != data.size()) {
        throw std::runtime_error("Trailing bytes in RejectMessage");
    }
    return msg;
}

// ============================================================================
// Утилиты для создания и парсинга сообщений
// ============================================================================

bytes create_message(MessageType type, const bytes& payload, uint32_t magic) {
    if (!is_known_message_type(type)) {
        throw std::runtime_error("Unknown message type");
    }
    if (payload.size() > kMaxMessagePayload) {
        throw std::runtime_error("Payload too large");
    }
    if (magic == 0) {
        magic = current_network_magic();
    }
    MessageHeader header;
    header.magic = magic;
    header.type = type;
    header.length = static_cast<uint32_t>(payload.size());

    // Вычисляем checksum (SHA-256 payload, первые 4 байта)
    const auto hash = crypto::sha256(payload);
    std::memcpy(header.checksum.data(), hash.data(), 4);

    // Сериализуем header + payload
    bytes message = header.serialize();
    message.insert(message.end(), payload.begin(), payload.end());

    return message;
}

bool parse_message(const bytes& data, MessageHeader& header, bytes& payload) {
    if (data.size() < 13) {
        return false;
    }

    try {
        const bytes header_bytes(data.begin(), data.begin() + 13);
        header = MessageHeader::deserialize(header_bytes);

        if (header.magic != current_network_magic()) {
            return false;
        }

        if (header.length > kMaxMessagePayload) {
            return false;
        }

        if (data.size() - 13 != header.length) {
            return false;
        }

        payload.assign(data.begin() + 13, data.begin() + 13 + header.length);

        // Проверяем checksum
        const auto hash = crypto::sha256(payload);

        for (int i = 0; i < 4; i++) {
            if (header.checksum[i] != hash[i]) {
                return false;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

} // namespace geocoin::network
