#include "geocoin/network/falcon_auth.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace geocoin::network {

AuthSession::~AuthSession() {
    utils::cleanse_container(private_key);
}

FalconAuthenticator::FalconAuthenticator() {
}

FalconAuthenticator::~FalconAuthenticator() {
}

bool FalconAuthenticator::start_as_verifier(
    const Address& expected_address,
    const Hash256& channel_binding,
    std::shared_ptr<AuthSession>& session
) {
    if (expected_address.empty() ||
        std::all_of(channel_binding.begin(), channel_binding.end(), [](byte value) {
            return value == 0;
        })) {
        return false;
    }
    session = std::make_shared<AuthSession>();
    session->session_id = generate_session_id();
    session->role = AuthRole::VERIFIER;
    session->status = AuthStatus::NOT_STARTED;
    session->current_round = 0;
    session->expected_address = expected_address;
    session->channel_binding = channel_binding;
    session->started_at = std::chrono::steady_clock::now();
    session->last_activity = session->started_at;

    return true;
}

bool FalconAuthenticator::start_as_prover(
    const bytes& private_key,
    const Hash256& channel_binding,
    std::shared_ptr<AuthSession>& session
) {
    if (private_key.size() != 32 ||
        std::all_of(channel_binding.begin(), channel_binding.end(), [](byte value) {
            return value == 0;
        })) {
        return false;
    }

    session = std::make_shared<AuthSession>();
    session->session_id = generate_session_id();
    session->role = AuthRole::PROVER;
    session->status = AuthStatus::NOT_STARTED;
    session->current_round = 0;
    session->private_key = private_key;
    session->channel_binding = channel_binding;
    session->started_at = std::chrono::steady_clock::now();
    session->last_activity = session->started_at;

    return true;
}

bool FalconAuthenticator::get_next_challenge(
    std::shared_ptr<AuthSession> session,
    crypto::ChallengeData& challenge
) {
    if (!session || session->role != AuthRole::VERIFIER) {
        return false;
    }

    if (session->current_round >= 3) {
        return false;
    }

    // Создаем challenge для текущего раунда
    challenge = crypto::create_challenge(session->channel_binding);

    // Используем session_id из сессии (копируем в bytes)
    challenge.session_id.assign(session->session_id.begin(), session->session_id.end());
    challenge.nonce = challenge.session_id;

    // Сохраняем challenge
    session->challenges[session->current_round] = challenge;

    // Обновляем статус
    if (session->status == AuthStatus::NOT_STARTED) {
        session->status = AuthStatus::IN_PROGRESS;
    }

    update_activity(session);

    return true;
}

bool FalconAuthenticator::handle_challenge(
    std::shared_ptr<AuthSession> session,
    const crypto::ChallengeData& challenge,
    crypto::ChallengeResponse& response
) {
    if (!session || session->role != AuthRole::PROVER) {
        return false;
    }

    if (session->current_round >= 3) {
        return false;
    }

    if (challenge.session_id.size() != session->session_id.size()) {
        return false;
    }
    if (CRYPTO_memcmp(
            challenge.channel_binding.data(),
            session->channel_binding.data(),
            session->channel_binding.size()) != 0) {
        return false;
    }

    // Проверяем session_id
    if (session->status != AuthStatus::NOT_STARTED) {
        // Сравниваем bytes с array
        const bool session_id_match = CRYPTO_memcmp(
            challenge.session_id.data(),
            session->session_id.data(),
            session->session_id.size()) == 0;
        if (!session_id_match) {
            return false;
        }
    } else {
        // Первый раунд - запоминаем session_id из challenge (задает Verifier)
        for (size_t i = 0; i < session->session_id.size(); i++) {
            session->session_id[i] = challenge.session_id[i];
        }
        session->status = AuthStatus::IN_PROGRESS;
    }

    // Создаем response на challenge
    response = crypto::respond_to_challenge(session->private_key, challenge);

    update_activity(session);

    return true;
}

bool FalconAuthenticator::verify_response(
    std::shared_ptr<AuthSession> session,
    const crypto::ChallengeResponse& response
) {
    if (!session || session->role != AuthRole::VERIFIER) {
        return false;
    }

    if (session->current_round >= 3) {
        return false;
    }

    if (session->status != AuthStatus::IN_PROGRESS) {
        return false;
    }

    // Получаем challenge для текущего раунда
    const auto& challenge = session->challenges[session->current_round];

    // Проверяем подпись challenge и адрес подписанта.
    if (!crypto::verify_challenge_response(response, session->expected_address, challenge)) {
        session->status = AuthStatus::FAILED;
        return false;
    }

    // Сохраняем response
    session->responses[session->current_round] = response;

    // Переходим к следующему раунду
    session->current_round++;

    // Если все 3 раунда пройдены - аутентификация завершена
    if (session->current_round >= 3) {
        session->status = AuthStatus::COMPLETED;
    }

    update_activity(session);

    return true;
}

bool FalconAuthenticator::is_authenticated(
    const std::shared_ptr<AuthSession>& session
) const {
    if (!session) {
        return false;
    }

    return session->status == AuthStatus::COMPLETED &&
           session->current_round >= 3;
}

AuthStatus FalconAuthenticator::get_status(
    const std::shared_ptr<AuthSession>& session
) const {
    if (!session) {
        return AuthStatus::FAILED;
    }

    // Проверяем таймаут
    if (is_timeout(session)) {
        return AuthStatus::TIMEOUT;
    }

    return session->status;
}

bool FalconAuthenticator::is_timeout(
    const std::shared_ptr<AuthSession>& session,
    int timeout_seconds
) const {
    if (!session) {
        return true;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - session->last_activity
    ).count();

    return elapsed > timeout_seconds;
}

void FalconAuthenticator::cancel_session(std::shared_ptr<AuthSession> session) {
    if (session) {
        session->status = AuthStatus::FAILED;
    }
}

std::array<byte, 16> FalconAuthenticator::generate_session_id() {
    std::array<byte, 16> session_id;

    if (RAND_bytes(session_id.data(), static_cast<int>(session_id.size())) != 1) {
        // Authentication nonces require a cryptographic RNG. std::random_device
        // is not guaranteed to be cryptographic on every supported platform;
        // fail the handshake closed instead of silently weakening replay safety.
        throw std::runtime_error("CSPRNG failure while creating authentication session");
    }

    return session_id;
}

void FalconAuthenticator::update_activity(std::shared_ptr<AuthSession> session) {
    if (session) {
        session->last_activity = std::chrono::steady_clock::now();
    }
}

} // namespace geocoin::network
