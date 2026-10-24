#pragma once

#include "geocoin/types.hpp"
#include "geocoin/crypto/challenge_response.hpp"
#include <memory>
#include <chrono>
#include <array>

namespace geocoin::network {

/**
 * @brief Статус аутентификации
 */
enum class AuthStatus {
    NOT_STARTED,        // Аутентификация не начата
    IN_PROGRESS,        // В процессе (раунды 1-3)
    COMPLETED,          // Успешно завершена
    FAILED,             // Провалена
    TIMEOUT             // Таймаут
};

/**
 * @brief Роль в аутентификации
 */
enum class AuthRole {
    VERIFIER,           // Проверяющий (отправляет challenges)
    PROVER              // Доказывающий (отвечает на challenges)
};

/**
 * @brief Сессия аутентификации
 */
struct AuthSession {
    ~AuthSession();

    std::array<byte, 16> session_id;
    AuthRole role;
    AuthStatus status;
    int current_round;                          // 0-2 (3 раунда)
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point last_activity;
    Hash256 channel_binding{};

    // Для Verifier
    Address expected_address;                   // Ожидаемый адрес Prover
    std::array<crypto::ChallengeData, 3> challenges;
    std::array<crypto::ChallengeResponse, 3> responses;

    // Для Prover
    bytes private_key;                          // Приватный ключ (32 байта)
};

/**
 * @brief P2P аутентификатор (challenge-response)
 *
 * Управляет 3-раундовым протоколом challenge-sign-verify
 * для аутентификации пиров в P2P сети.
 *
 * Безопасность зависит от стойкости Falcon-512 и корректности challenge-response протокола.
 */
class FalconAuthenticator {
public:
    FalconAuthenticator();
    ~FalconAuthenticator();

    /**
     * @brief Начать аутентификацию как Verifier
     *
     * @param expected_address Ожидаемый адрес Prover
     * @param session [out] Созданная сессия
     * @return true при успехе
     */
    bool start_as_verifier(
        const Address& expected_address,
        const Hash256& channel_binding,
        std::shared_ptr<AuthSession>& session
    );

    /**
     * @brief Начать аутентификацию как Prover
     *
     * @param private_key Приватный ключ (32 байта)
     * @param session [out] Созданная сессия
     * @return true при успехе
     */
    bool start_as_prover(
        const bytes& private_key,
        const Hash256& channel_binding,
        std::shared_ptr<AuthSession>& session
    );

    /**
     * @brief Получить следующий challenge (Verifier)
     *
     * @param session Сессия аутентификации
     * @param challenge [out] Challenge для отправки
     * @return true если есть следующий раунд
     */
    bool get_next_challenge(
        std::shared_ptr<AuthSession> session,
        crypto::ChallengeData& challenge
    );

    /**
     * @brief Обработать challenge и создать response (Prover)
     *
     * @param session Сессия аутентификации
     * @param challenge Полученный challenge
     * @param response [out] Response для отправки
     * @return true при успехе
     */
    bool handle_challenge(
        std::shared_ptr<AuthSession> session,
        const crypto::ChallengeData& challenge,
        crypto::ChallengeResponse& response
    );

    /**
     * @brief Верифицировать response (Verifier)
     *
     * @param session Сессия аутентификации
     * @param response Полученный response
     * @return true если response валиден
     */
    bool verify_response(
        std::shared_ptr<AuthSession> session,
        const crypto::ChallengeResponse& response
    );

    /**
     * @brief Проверить, завершена ли аутентификация
     *
     * @param session Сессия аутентификации
     * @return true если все 3 раунда пройдены успешно
     */
    bool is_authenticated(const std::shared_ptr<AuthSession>& session) const;

    /**
     * @brief Получить статус аутентификации
     *
     * @param session Сессия аутентификации
     * @return Статус
     */
    AuthStatus get_status(const std::shared_ptr<AuthSession>& session) const;

    /**
     * @brief Проверить таймаут сессии
     *
     * @param session Сессия аутентификации
     * @param timeout_seconds Таймаут в секундах (по умолчанию 60)
     * @return true если истек таймаут
     */
    bool is_timeout(
        const std::shared_ptr<AuthSession>& session,
        int timeout_seconds = 60
    ) const;

    /**
     * @brief Отменить сессию
     *
     * @param session Сессия аутентификации
     */
    void cancel_session(std::shared_ptr<AuthSession> session);

private:
    /**
     * @brief Генерировать уникальный session_id
     */
    std::array<byte, 16> generate_session_id();

    /**
     * @brief Обновить время последней активности
     */
    void update_activity(std::shared_ptr<AuthSession> session);
};

} // namespace geocoin::network
