#pragma once

#include "geocoin/types.hpp"

namespace geocoin::crypto::falcon512 {

// Возвращает true, если в этой сборке подключен реальный backend Falcon-512.
bool available();

// Подписывает 32-байтовый дайджест сообщения.
// private_key_seed должен быть длиной 32 байта.
bool sign_digest32(
    const Hash256& msg32,
    const bytes& private_key_seed,
    bytes& out_public_key,
    bytes& out_signature
);

// Детерминированно получает Falcon public key из 32-байтового seed.
bool public_key_from_seed32(
    const bytes& private_key_seed,
    bytes& out_public_key
);

// Проверяет подпись 32-байтового дайджеста сообщения.
bool verify_digest32(
    const Hash256& msg32,
    const bytes& public_key,
    const bytes& signature
);

// Возвращает true, если подпись закодирована минимально: декодер Falcon
// потребляет все байты сжатой части. Вариант, дополненный нулями до 666 байт,
// тоже проходит verify_digest32, но меняет байты подписи (и txid).
bool is_canonical_signature_encoding(const bytes& signature);

} // namespace geocoin::crypto::falcon512
