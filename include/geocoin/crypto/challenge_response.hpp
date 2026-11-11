#pragma once

#include "geocoin/types.hpp"
#include <vector>

namespace geocoin::crypto {

/**
 * @brief Challenge для p2p-аутентификации
 */
struct ChallengeData {
    bytes value;                 // Challenge value (32 байта)
    uint64_t timestamp;          // Unix timestamp (мс)
    bytes session_id;            // Уникальный ID сессии (16 байт)
    Hash256 channel_binding{};    // SHA-256 защищённого P2P handshake
    bytes nonce;                 // Alias для совместимости
    std::vector<bytes> rounds;   // Для обратной совместимости
};

/**
 * @brief Response на challenge
 */
struct ChallengeResponse {
    bytes proof;                   // Falcon-512 подпись challenge-hash
    std::vector<bytes> responses;  // Для обратной совместимости
};

/**
 * @brief Создать challenge (Verifier)
 *
 * @return Challenge
 */
ChallengeData create_challenge(const Hash256& channel_binding = {});

/**
 * @brief Ответить на challenge (Prover) в формате Falcon-512
 *
 * @param private_key Приватный ключ (32 байта)
 * @param challenge Challenge от верификатора
 * @return Response с подписью Falcon-512
 */
ChallengeResponse respond_to_challenge(
    const bytes& private_key,
    const ChallengeData& challenge
);

/**
 * @brief Генерация response (alias для совместимости)
 *
 * @param private_key Приватный ключ (32 байта)
 * @param challenge Challenge от верификатора
 * @return Response с подписью Falcon-512
 */
inline ChallengeResponse generate_response(
    const bytes& private_key,
    const ChallengeData& challenge
) {
    return respond_to_challenge(private_key, challenge);
}

/**
 * @brief Верифицировать response (Verifier) по ожидаемому адресу
 *
 * @param response Response от prover
 * @param expected_address Ожидаемый адрес prover
 * @param challenge Отправленный challenge
 * @return true если верификация успешна
 */
bool verify_challenge_response(
    const ChallengeResponse& response,
    const Address& expected_address,
    const ChallengeData& challenge
);

/**
 * @brief Многораундовая аутентификация для повышенной безопасности
 *
 * Для N раундов выполняется N независимых challenge-sign-verify.
 */
class MultiRoundAuth {
public:
    /**
     * @brief Конструктор
     * @param num_rounds Количество раундов (по умолчанию 3)
     */
    explicit MultiRoundAuth(size_t num_rounds = 3);

    /**
     * @brief Начать новый раунд (Verifier)
     * @return Challenge для текущего раунда
     */
    ChallengeData start_round();

    /**
     * @brief Верифицировать ответ на текущий раунд (Verifier)
     * @param response Response от prover
     * @param expected_address Ожидаемый адрес
     * @return true если раунд пройден
     */
    bool verify_round(const ChallengeResponse& response, const Address& expected_address);

    /**
     * @brief Проверить, завершены ли все раунды
     * @return true если все раунды пройдены
     */
    bool is_complete() const;

    /**
     * @brief Финализировать аутентификацию
     * @return true если аутентификация успешна
     */
    bool finalize();

    /**
     * @brief Сбросить состояние для новой аутентификации
     */
    void reset();

    /**
     * @brief Получить текущий раунд (0-based)
     */
    size_t current_round() const { return current_round_; }

    /**
     * @brief Получить общее количество раундов
     */
    size_t total_rounds() const { return num_rounds_; }

    /**
     * @brief Получить уровень безопасности в битах
     * @return Effective classical security of Falcon-512 (rounds do not
     * multiply the security of one underlying key)
     */
    double get_security_bits() const;

private:
    size_t num_rounds_;
    size_t current_round_;
    bool completed_;
    std::vector<ChallengeData> challenges_;
    std::vector<ChallengeResponse> responses_;
};

/**
 * @brief Сериализация challenge для передачи по сети
 */
bytes serialize_challenge(const ChallengeData& challenge);

/**
 * @brief Десериализация challenge
 */
ChallengeData deserialize_challenge(const bytes& data);

/**
 * @brief Сериализация response для передачи по сети
 */
bytes serialize_response(const ChallengeResponse& response);

/**
 * @brief Десериализация response
 */
ChallengeResponse deserialize_response(const bytes& data);

} // namespace geocoin::crypto
