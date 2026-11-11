#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/challenge_response.hpp"
#include "geocoin/crypto/ml_kem768.hpp"
#include "geocoin/network/secure_channel.hpp"
#include "geocoin/network/secure_handshake.hpp"
#include <vector>
#include <string>

namespace geocoin::network {

// Максимальное количество заголовков в одном сообщении HEADERS (аналог Bitcoin: 2000)
constexpr uint32_t kMaxHeadersPerMessage = 2000;
constexpr uint32_t kP2PProtocolVersion = kSecureWireVersion;
constexpr size_t kSecureHelloPayloadSize =
    2 + 2 + 32 + 32 + crypto::ml_kem768::kPublicKeySize;
constexpr size_t kSecureReplyPayloadSize =
    2 + 2 + 32 + 32 + crypto::ml_kem768::kCiphertextSize;

/**
 * @brief Service flags (битовая маска возможностей ноды)
 */
enum ServiceFlags : uint64_t {
    NODE_NONE = 0,
    NODE_NETWORK = (1ULL << 0),          // Полная нода (может отдавать блоки)
    NODE_WITNESS = (1ULL << 1),          // Поддержка witness
    NODE_ARCHIVE = (1ULL << 2),          // Архивная нода (хранит witness для всех блоков)
    NODE_NETWORK_LIMITED = (1ULL << 10)  // Pruned нода (ограниченная история)
};

/**
 * @brief Типы сообщений протокола
 */
enum class MessageType : uint8_t {
    VERSION = 0x00,         // Рукопожатие
    VERACK = 0x01,          // Подтверждение версии
    ADDR = 0x02,            // Обмен адресами пиров
    INV = 0x03,             // Инвентаризация (блоки/транзакции)
    GETDATA = 0x04,         // Запрос данных
    BLOCK = 0x05,           // Передача блока
    TX = 0x06,              // Передача транзакции
    GETBLOCKS = 0x07,       // Запрос заголовков блоков
    GETHEADERS = 0x08,      // Запрос только заголовков
    HEADERS = 0x09,         // Ответ с заголовками
    CHALLENGE = 0x0A,       // Вызов аутентификации
    RESPONSE = 0x0B,        // Ответ аутентификации
    PING = 0x0C,            // Пинг
    PONG = 0x0D,            // Понг
    REJECT = 0x0E,          // Отклонение
    SECURE_HELLO = 0x0F,    // Гибридный ephemeral handshake (initiator)
    SECURE_REPLY = 0x10     // Гибридный ephemeral handshake (responder)
};

bool is_known_message_type(MessageType type);

/**
 * @brief Заголовок сообщения (13 байт в wire-формате)
 */
struct MessageHeader {
    uint32_t magic;         // 4 байта - magic number (0xD3670000 для degree)
    MessageType type;       // 1 байт - тип сообщения
    uint32_t length;        // 4 байта - длина payload
    Hash256 checksum;       // 32 байта - SHA-256(payload) (первые 4 байта)

    static constexpr uint32_t MAGIC_MAINNET = 0xD3670000;  // degree
    static constexpr uint32_t MAGIC_TESTNET = 0xD3670001;
    static constexpr uint32_t MAGIC_REGTEST = 0xD3670002;
    static constexpr uint32_t MAGIC_LOADNET = 0xD3670003;  // degree-load, isolated from regtest

    bytes serialize() const;
    static MessageHeader deserialize(const bytes& data);
};

/**
 * @brief VERSION сообщение
 */
struct VersionMessage {
    uint32_t version;           // Версия протокола
    uint64_t services;          // Поддерживаемые сервисы
    uint64_t timestamp;         // Текущее время
    std::string user_agent;     // "degree/0.1.0"
    Height height;              // Высота блокчейна

    // Аутентификация пира
    Address auth_address;       // Адрес пира

    bytes serialize() const;
    static VersionMessage deserialize(const bytes& data);
};

/** Первый и единственный открытый P2P handshake-пакет инициатора. */
struct SecureHelloMessage {
    uint16_t version = kSecureWireVersion;
    uint16_t suite = kSecureSuiteHybridMlKem768X25519ChaCha20Poly1305;
    SecureHelloData hello;

    bytes serialize() const;
    static SecureHelloMessage deserialize(const bytes& data);
};

/** Открытый ответ handshake; после него plaintext запрещён. */
struct SecureReplyMessage {
    uint16_t version = kSecureWireVersion;
    uint16_t suite = kSecureSuiteHybridMlKem768X25519ChaCha20Poly1305;
    SecureReplyData reply;

    bytes serialize() const;
    static SecureReplyMessage deserialize(const bytes& data);
};

/**
 * @brief CHALLENGE сообщение аутентификации
 */
struct ChallengeMessage {
    crypto::ChallengeData challenge;

    bytes serialize() const;
    static ChallengeMessage deserialize(const bytes& data);
};

/**
 * @brief RESPONSE сообщение аутентификации
 */
struct ResponseMessage {
    crypto::ChallengeResponse response;

    bytes serialize() const;
    static ResponseMessage deserialize(const bytes& data);
};

/**
 * @brief INV сообщение (инвентаризация)
 */
struct InvMessage {
    enum class InvType : uint32_t {
        ERROR_ITEM = 0,
        TX = 1,
        BLOCK = 2
    };

    struct InvItem {
        InvType type;
        Hash256 hash;
    };

    std::vector<InvItem> items;

    bytes serialize() const;
    static InvMessage deserialize(const bytes& data);
};

/**
 * @brief GETDATA сообщение
 */
using GetDataMessage = InvMessage;

/**
 * @brief BLOCK сообщение
 */
struct BlockMessage {
    core::Block block;

    bytes serialize() const;
    static BlockMessage deserialize(const bytes& data);
};

/**
 * @brief TX сообщение
 */
struct TxMessage {
    core::Transaction tx;

    bytes serialize() const;
    static TxMessage deserialize(const bytes& data);
};

/**
 * @brief GETBLOCKS сообщение
 */
struct GetBlocksMessage {
    uint32_t version;
    std::vector<Hash256> locator_hashes;  // Хеши известных блоков
    Hash256 stop_hash;                     // Хеш последнего нужного блока

    bytes serialize() const;
    static GetBlocksMessage deserialize(const bytes& data);
};

/**
 * @brief HEADERS сообщение (ответ на GETHEADERS)
 */
struct HeadersMessage {
    std::vector<core::BlockHeader> headers;

    bytes serialize() const;
    static HeadersMessage deserialize(const bytes& data);
};

/**
 * @brief ADDR сообщение (адреса пиров)
 */
struct AddrMessage {
    struct NetAddr {
        uint64_t services;
        std::string ip;
        uint16_t port;
        uint64_t timestamp;
    };

    std::vector<NetAddr> addresses;

    bytes serialize() const;
    static AddrMessage deserialize(const bytes& data);
};

/**
 * @brief REJECT сообщение (причина отклонения)
 */
struct RejectMessage {
    MessageType rejected_type = MessageType::TX;
    uint8_t code = 0;
    std::string reason;
    Hash256 rejected_hash{}; // optional object hash (0x00..00 если нет)

    bytes serialize() const;
    static RejectMessage deserialize(const bytes& data);
};

/** Magic текущей выбранной сети, либо 0 если сеть неизвестна. */
uint32_t current_network_magic();

/**
 * @brief Создать полное сообщение (header + payload)
 *
 * @param magic Если 0, используется magic текущей выбранной сети.
 */
bytes create_message(MessageType type, const bytes& payload, uint32_t magic = 0);

/**
 * @brief Парсинг сообщения
 */
bool parse_message(const bytes& data, MessageHeader& header, bytes& payload);

} // namespace geocoin::network
