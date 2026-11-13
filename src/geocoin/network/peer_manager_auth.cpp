#include "geocoin/utils/version.hpp"
#include "geocoin/network/peer_manager.hpp"
#include "geocoin/network/peer_manager_internal.hpp"
#include "geocoin/network/services.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"
#include "geocoin/utils/secure_memory.hpp"

#include <cctype>
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace geocoin::network {
namespace {

inline bool SyncDebugEnabled() {
    return detail::sync_debug_enabled();
}

constexpr size_t kAuthRounds = detail::kAuthRounds;

uint32_t selected_network_magic() {
    return current_network_magic();
}

bool is_printable_user_agent(const std::string& user_agent) {
    for (const unsigned char ch : user_agent) {
        if (ch < 0x20 || ch > 0x7e) {
            return false;
        }
    }
    return true;
}

bool validate_version_message(const VersionMessage& version, std::string& reason) {
    if (version.version != kP2PProtocolVersion) {
        reason = "version-unsupported";
        return false;
    }
    if (!has_service(version.services, ServiceFlags::NODE_NETWORK) ||
        !has_service(version.services, ServiceFlags::NODE_WITNESS)) {
        reason = "version-missing-required-services";
        return false;
    }
    if (version.user_agent.empty() || version.user_agent.size() > 128 ||
        !is_printable_user_agent(version.user_agent)) {
        reason = "version-invalid-user-agent";
        return false;
    }
    if (version.auth_address.empty() || !crypto::verify_address(version.auth_address)) {
        reason = "version-invalid-auth-address";
        return false;
    }
    return true;
}

}  // namespace

bool PeerManager::admit_authenticated_identity_unlocked(PeerContext& candidate) {
    if (!candidate.peer) {
        return false;
    }

    const Address remote_address = candidate.peer->get_info().auth_address;
    const Address local_address = candidate.local_auth_address;
    const bool have_local_address = !local_address.empty();

    // Simultaneous A->B and B->A connections must make the same decision at
    // both endpoints. The lexicographically smaller identity keeps outbound;
    // the larger identity keeps the matching inbound side of that socket.
    const bool candidate_preferred_direction =
        have_local_address && local_address != remote_address &&
        (local_address < remote_address ? !candidate.is_inbound : candidate.is_inbound);

    for (auto& [transport_address, other] : peers_) {
        (void)transport_address;
        if (&other == &candidate || !other.peer || !other.peer->is_connected() ||
            other.peer->get_state() != PeerState::READY) {
            continue;
        }

        const PeerInfo other_info = other.peer->get_info();
        if (!other_info.auth_verified || other_info.auth_address != remote_address) {
            continue;
        }

        const bool other_preferred_direction =
            have_local_address && local_address != remote_address &&
            (local_address < remote_address ? !other.is_inbound : other.is_inbound);

        bool keep_candidate = false;
        if (candidate_preferred_direction != other_preferred_direction) {
            keep_candidate = candidate_preferred_direction;
        } else {
            // Same-direction aliases for one identity are not useful. Keep the
            // older transport so reconnect churn cannot evict a healthy peer.
            keep_candidate = candidate.connected_at < other.connected_at;
        }

        if (!keep_candidate) {
            disconnect_peer_unlocked(candidate, "duplicate-auth-identity");
            return false;
        }
        disconnect_peer_unlocked(other, "duplicate-auth-identity");
    }

    return true;
}

bool PeerManager::start_authentication(
    PeerContext& ctx,
    const bytes& our_private_key
) {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "PeerManager::start_authentication()");
    }

    if (!ctx.peer) {
        GEO_LOG_ERROR("peers", "start_authentication: peer is null");
        return false;
    }

    if (our_private_key.size() != 32) {
        // КРИТИЧНО: Не логируем размер приватного ключа
        GEO_LOG_ERROR("peers", "start_authentication: invalid private key format");
        return false;
    }

    // Вычисляем наш адрес
    Address our_address;

    if (!crypto::compute_address_from_private_key(our_private_key, our_address)) {
        GEO_LOG_ERROR("peers", "start_authentication: failed to compute address from private key");
        return false;
    }

    auto handshake = SecureHandshake::create_initiator();
    if (!handshake) {
        GEO_LOG_ERROR("peers", "start_authentication: secure handshake initialization failed");
        return false;
    }

    SecureHelloMessage hello;
    hello.hello = handshake->hello();
    bytes payload;
    try {
        payload = hello.serialize();
    } catch (...) {
        return false;
    }

    ctx.local_auth_address = std::move(our_address);
    ctx.pending_private_key.assign(our_private_key.begin(), our_private_key.end());
    ctx.secure_hello_payload = payload;
    ctx.secure_handshake = std::move(handshake);
    ctx.peer->set_state(PeerState::AUTHENTICATING);

    if (!send_message(ctx.peer, MessageType::SECURE_HELLO, payload)) {
        GEO_LOG_ERROR("peers", "start_authentication: failed to send SECURE_HELLO");
        return false;
    }
    return true;
}

bool PeerManager::send_local_version(PeerContext& ctx) {
    if (!ctx.peer || !ctx.secure_established || !ctx.peer->is_secure() ||
        ctx.local_auth_address.empty()) {
        return false;
    }

    VersionMessage version;
    version.version = kP2PProtocolVersion;
    version.services = build_local_services(is_archive_mode_);
    version.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    version.user_agent = geocoin::version::USER_AGENT;
    version.height = get_height_callback_ ? get_height_callback_() : 0;
    version.auth_address = ctx.local_auth_address;
    return send_message(ctx.peer, MessageType::VERSION, version.serialize());
}

void PeerManager::handle_secure_hello(PeerContext& ctx, const bytes& payload) {
    try {
        if (!ctx.peer || !ctx.is_inbound || ctx.secure_established ||
            ctx.peer->is_secure() || ctx.secure_handshake) {
            throw std::runtime_error("unexpected secure hello");
        }

        const SecureHelloMessage hello = SecureHelloMessage::deserialize(payload);
        // Refuse before any key exchange work. The peer may be honest, so a
        // throttled handshake is dropped without a ban score.
        if (!admit_inbound_handshake_unlocked(ctx.peer->get_address())) {
            GEO_METRIC_INC("peers.secure_handshake_throttled_total", 1);
            GEO_LOG_WARN("peers", "SECURE_HELLO throttled from " << ctx.peer->get_address());
            disconnect_peer_unlocked(ctx, "secure-hello-throttled");
            return;
        }
        auto handshake = SecureHandshake::create_responder(hello.hello);
        if (!handshake) {
            throw std::runtime_error("secure responder initialization failed");
        }

        SecureReplyMessage reply;
        reply.reply = handshake->reply();
        const bytes reply_payload = reply.serialize();
        if (!handshake->complete_responder(
                selected_network_magic(), payload, reply_payload)) {
            throw std::runtime_error("secure responder key derivation failed");
        }

        const Hash256 channel_binding = handshake->channel_binding();
        SecureChannelKeyMaterial key_material;
        if (!handshake->take_key_material(key_material)) {
            throw std::runtime_error("secure responder keys unavailable");
        }

        // Reply itself is plaintext; activation happens immediately after its
        // complete write. Every following frame is encrypted.
        if (!send_message(ctx.peer, MessageType::SECURE_REPLY, reply_payload) ||
            !ctx.peer->activate_secure_channel(std::move(key_material))) {
            throw std::runtime_error("secure responder activation failed");
        }

        ctx.channel_binding = channel_binding;
        ctx.secure_established = true;
        ctx.secure_handshake.reset();
        ctx.last_useful_message_at = std::chrono::steady_clock::now();
        GEO_METRIC_INC("peers.secure_handshake_ok_total", 1);
    } catch (const std::exception& e) {
        GEO_LOG_WARN("peers", "SECURE_HELLO rejected: " << e.what());
        GEO_METRIC_INC("peers.secure_handshake_fail_total", 1);
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "secure-hello-invalid");
        disconnect_peer_unlocked(ctx, "secure-hello-invalid");
    }
}

void PeerManager::handle_secure_reply(PeerContext& ctx, const bytes& payload) {
    try {
        if (!ctx.peer || ctx.is_inbound || ctx.secure_established ||
            ctx.peer->is_secure() || !ctx.secure_handshake ||
            !ctx.secure_handshake->is_initiator() || ctx.secure_hello_payload.empty()) {
            throw std::runtime_error("unexpected secure reply");
        }

        const SecureReplyMessage reply = SecureReplyMessage::deserialize(payload);
        if (!ctx.secure_handshake->complete_initiator(
                reply.reply,
                selected_network_magic(),
                ctx.secure_hello_payload,
                payload)) {
            throw std::runtime_error("secure initiator key derivation failed");
        }

        const Hash256 channel_binding = ctx.secure_handshake->channel_binding();
        SecureChannelKeyMaterial key_material;
        if (!ctx.secure_handshake->take_key_material(key_material) ||
            !ctx.peer->activate_secure_channel(std::move(key_material))) {
            throw std::runtime_error("secure initiator activation failed");
        }

        ctx.channel_binding = channel_binding;
        ctx.secure_established = true;
        ctx.secure_handshake.reset();
        ctx.secure_hello_payload.clear();

        bytes auth_key(ctx.pending_private_key.begin(), ctx.pending_private_key.end());
        utils::ScopedContainerCleanser<bytes> auth_key_cleanser(auth_key);
        if (!authenticator_.start_as_prover(
                auth_key, ctx.channel_binding, ctx.auth_session)) {
            throw std::runtime_error("Falcon prover initialization failed");
        }
        utils::cleanse_container(ctx.pending_private_key);

        if (!send_local_version(ctx)) {
            throw std::runtime_error("encrypted VERSION send failed");
        }
        ctx.last_useful_message_at = std::chrono::steady_clock::now();
        GEO_METRIC_INC("peers.secure_handshake_ok_total", 1);
    } catch (const std::exception& e) {
        GEO_LOG_WARN("peers", "SECURE_REPLY rejected: " << e.what());
        GEO_METRIC_INC("peers.secure_handshake_fail_total", 1);
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "secure-reply-invalid");
        disconnect_peer_unlocked(ctx, "secure-reply-invalid");
    }
}

void PeerManager::handle_version(PeerContext& ctx, const bytes& payload) {
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "PeerManager::handle_version() from " << ctx.peer->get_address());
    }

    try {
        if (!ctx.peer || !ctx.secure_established || !ctx.peer->is_secure()) {
            throw std::runtime_error("VERSION outside secure channel");
        }
        VersionMessage version = VersionMessage::deserialize(payload);

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG(
                "peers",
                "  Received VERSION: user_agent=" << version.user_agent
                    << ", address=" << version.auth_address.str()
                    << ", height=" << version.height
            );
        }

        std::string reject_reason;
        if (!validate_version_message(version, reject_reason)) {
            GEO_LOG_ERROR("peers", "handle_version: invalid VERSION: " << reject_reason);
            penalize_peer_unlocked(ctx, 50, reject_reason.c_str());
            disconnect_peer_unlocked(ctx, reject_reason);
            return;
        }

        if (!ctx.local_auth_address.empty() &&
            version.auth_address == ctx.local_auth_address) {
            GEO_LOG_WARN("peers", "Rejecting self-connection by authenticated identity");
            disconnect_peer_unlocked(ctx, "self-auth-identity");
            return;
        }
        if (ctx.version_received) {
            penalize_peer_unlocked(ctx, 50, "duplicate-version");
            disconnect_peer_unlocked(ctx, "duplicate-version");
            return;
        }
        ctx.version_received = true;
        ctx.last_useful_message_at = std::chrono::steady_clock::now();

        // Обновляем информацию о пире
        PeerInfo info = ctx.peer->get_info();
        info.version = version.version;
        info.services = version.services;
        info.user_agent = version.user_agent;
        info.height = version.height;
        info.auth_address = version.auth_address;
        ctx.peer->update_info(info);

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  Peer info updated");
        }

        // Если мы Verifier (входящее соединение), отправляем свой VERSION и начинаем challenge
        if (ctx.is_inbound && !ctx.auth_session) {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  We are inbound (Verifier), sending VERSION back...");
            }

            if (!send_local_version(ctx)) {
                GEO_LOG_ERROR("peers", "handle_version: failed to send encrypted VERSION reply");
                disconnect_peer_unlocked(ctx, "version-reply-send-failed");
                return;
            }

            // Создаем сессию как Verifier
            if (!authenticator_.start_as_verifier(
                    version.auth_address,
                    ctx.channel_binding,
                    ctx.auth_session)) {
                disconnect_peer_unlocked(ctx, "auth-verifier-start-failed");
                return;
            }
            ctx.peer->set_state(PeerState::AUTHENTICATING);

            // Отправляем первый challenge
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  Sending first CHALLENGE...");
            }
            crypto::ChallengeData challenge;
            if (authenticator_.get_next_challenge(ctx.auth_session, challenge)) {
                ChallengeMessage msg;
                msg.challenge = challenge;
                bool sent = send_message(ctx.peer, MessageType::CHALLENGE, msg.serialize());
                if (SyncDebugEnabled()) {
                    GEO_LOG_DEBUG("peers", "  CHALLENGE sent: " << (sent ? "YES" : "NO"));
                }
                if (!sent) {
                    GEO_LOG_ERROR("peers", "handle_version: failed to send CHALLENGE");
                    disconnect_peer_unlocked(ctx, "challenge-send-failed");
                    return;
                }
            } else {
                GEO_LOG_ERROR("peers", "handle_version: failed to get next challenge");
                disconnect_peer_unlocked(ctx, "challenge-create-failed");
                return;
            }
        }

        // Если мы Prover (исходящее соединение), просто ждем CHALLENGE
        if (!ctx.is_inbound && ctx.auth_session && ctx.auth_session->role == AuthRole::PROVER) {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  We are outbound (Prover), waiting for CHALLENGE...");
            }
        }

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  handle_version completed");
        }

    } catch (const std::exception& e) {
        GEO_LOG_ERROR("peers", "Exception in handle_version: " << e.what());
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "version-malformed");
        disconnect_peer_unlocked(ctx, "version-malformed");
    } catch (...) {
        GEO_LOG_ERROR("peers", "Unknown exception in handle_version");
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "version-malformed");
        disconnect_peer_unlocked(ctx, "version-malformed");
    }
}

void PeerManager::handle_challenge(
    PeerContext& ctx,
    const bytes& payload,
    std::unique_lock<std::mutex>& lock,
    std::vector<std::function<void()>>& deferred_tasks
) {
    // Keep mutex_ held while this handler owns a PeerContext& from peers_.
    // RPC/maintenance threads may erase that map element as soon as the lock is
    // released, which would turn every subsequent ctx access into a UAF.
    (void)lock;
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "PeerManager::handle_challenge() from " << ctx.peer->get_address());
        GEO_LOG_DEBUG("peers", "  Payload size: " << payload.size() << " bytes");
    }

    try {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  Calling ChallengeMessage::deserialize...");
        }
        ChallengeMessage msg = ChallengeMessage::deserialize(payload);

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  Challenge deserialized successfully");
        }

        if (!ctx.auth_session) {
            GEO_LOG_ERROR("peers", "handle_challenge: no auth_session");
            penalize_peer_unlocked(ctx, 50, "challenge-without-session");
            disconnect_peer_unlocked(ctx, "challenge-without-session");
            return;
        }

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG(
                "peers",
                "  Auth session exists, role="
                    << (ctx.auth_session->role == AuthRole::PROVER ? "PROVER" : "VERIFIER")
            );
        }

        // Создаем response
        crypto::ChallengeResponse response;
        if (!authenticator_.handle_challenge(ctx.auth_session, msg.challenge, response)) {
            GEO_LOG_ERROR("peers", "handle_challenge: failed to handle challenge");
            penalize_peer_unlocked(ctx, 50, "challenge-verification-failed");
            disconnect_peer_unlocked(ctx, "challenge-verification-failed");
            return;
        }

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  Response created, sending...");
        }

        // Отправляем response
        ResponseMessage resp_msg;
        resp_msg.response = response;
        bytes serialized = resp_msg.serialize();
        const bool sent = send_message(ctx.peer, MessageType::RESPONSE, serialized);

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  RESPONSE sent: " << (sent ? "YES" : "NO"));
        }
        if (!sent) {
            GEO_LOG_ERROR("peers", "handle_challenge: failed to send RESPONSE");
            disconnect_peer_unlocked(ctx, "response-send-failed");
            return;
        }
        ctx.last_useful_message_at = std::chrono::steady_clock::now();

        // Увеличиваем счетчик раундов для Prover
        ctx.auth_session->current_round++;

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG(
                "peers",
                "  Current round: " << ctx.auth_session->current_round << "/" << kAuthRounds
            );
        }

        // Если все 3 раунда пройдены, мы доказали удалённой стороне владение
        // своим ключом. Для исходящего соединения это только первая половина
        // взаимной аутентификации: теперь обязаны проверить ключ listener-а.
        if (ctx.auth_session->current_round >= static_cast<int>(kAuthRounds)) {
            ctx.auth_session->status = AuthStatus::COMPLETED;

            if (!ctx.is_inbound) {
                const Address expected_peer_address = ctx.peer->get_info().auth_address;
                if (!authenticator_.start_as_verifier(
                        expected_peer_address, ctx.channel_binding, ctx.auth_session)) {
                    GEO_LOG_ERROR("peers", "Failed to start reverse peer authentication");
                    disconnect_peer_unlocked(ctx, "reverse-auth-start-failed");
                    return;
                }

                crypto::ChallengeData reverse_challenge;
                if (!authenticator_.get_next_challenge(ctx.auth_session, reverse_challenge)) {
                    disconnect_peer_unlocked(ctx, "reverse-challenge-create-failed");
                    return;
                }

                ChallengeMessage reverse_message;
                reverse_message.challenge = std::move(reverse_challenge);
                const bool reverse_sent = send_message(
                    ctx.peer,
                    MessageType::CHALLENGE,
                    reverse_message.serialize()
                );
                if (!reverse_sent) {
                    disconnect_peer_unlocked(ctx, "reverse-challenge-send-failed");
                    return;
                }
                ctx.last_useful_message_at = std::chrono::steady_clock::now();
                return;
            }

            // Inbound уже проверил outbound в первой половине, а теперь сам
            // успешно доказал владение своим ключом. Только здесь соединение
            // становится полностью взаимно аутентифицированным.
            if (!ctx.secure_established || !ctx.peer->is_secure()) {
                disconnect_peer_unlocked(ctx, "secure-channel-lost-before-ready");
                return;
            }
            if (!admit_authenticated_identity_unlocked(ctx)) {
                return;
            }
            GEO_LOG_INFO("peers", "Mutual authentication completed");
            ctx.peer->set_state(PeerState::READY);
            PeerInfo info = ctx.peer->get_info();
            info.auth_verified = true;
            ctx.peer->update_info(info);

            (void)send_message(ctx.peer, MessageType::VERACK, bytes());

            auto callback = peer_authenticated_callback_;
            if (callback) {
                auto peer = ctx.peer;
                auto height = info.height;
                deferred_tasks.emplace_back([callback = std::move(callback), peer, height]() {
                    callback(peer, height);
                });
            }
        }

    } catch (const std::exception& e) {
        GEO_LOG_ERROR("peers", "Exception in handle_challenge: " << e.what());
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "challenge-malformed");
        disconnect_peer_unlocked(ctx, "challenge-malformed");
    } catch (...) {
        GEO_LOG_ERROR("peers", "Unknown exception in handle_challenge");
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "challenge-malformed");
        disconnect_peer_unlocked(ctx, "challenge-malformed");
    }
}

void PeerManager::handle_response(
    PeerContext& ctx,
    const bytes& payload,
    std::unique_lock<std::mutex>& lock,
    std::vector<std::function<void()>>& deferred_tasks
) {
    // See handle_challenge(): ctx is a reference into peers_ and must not
    // outlive an unlocked interval in which another thread can erase it.
    (void)lock;
    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("peers", "PeerManager::handle_response() from " << ctx.peer->get_address());
    }

    try {
        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  Deserializing ResponseMessage...");
        }
        ResponseMessage msg = ResponseMessage::deserialize(payload);

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  Response deserialized");
        }

        if (!ctx.auth_session) {
            GEO_LOG_ERROR("peers", "handle_response: no auth_session");
            penalize_peer_unlocked(ctx, 50, "response-without-session");
            disconnect_peer_unlocked(ctx, "response-without-session");
            return;
        }

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  Verifying response...");
        }
        // Верифицируем response
        if (!authenticator_.verify_response(ctx.auth_session, msg.response)) {
            GEO_LOG_ERROR("peers", "handle_response: response verification failed");
            penalize_peer_unlocked(ctx, 50, "response-verification-failed");
            disconnect_peer_unlocked(ctx, "response-verification-failed");
            return;
        }
        ctx.last_useful_message_at = std::chrono::steady_clock::now();

        if (SyncDebugEnabled()) {
            GEO_LOG_DEBUG("peers", "  Response verified successfully");
        }

        // Если аутентификация завершена, verifier проверил ключ удалённой
        // стороны. На inbound это первая половина: переключаемся в prover и
        // ждём reverse challenge от outbound, не выставляя READY преждевременно.
        if (authenticator_.is_authenticated(ctx.auth_session)) {
            if (ctx.is_inbound) {
                bytes auth_key(
                    ctx.pending_private_key.begin(), ctx.pending_private_key.end());
                utils::ScopedContainerCleanser<bytes> auth_key_cleanser(auth_key);
                if (!authenticator_.start_as_prover(
                        auth_key, ctx.channel_binding, ctx.auth_session)) {
                    GEO_LOG_ERROR("peers", "Failed to start reverse authentication prover");
                    disconnect_peer_unlocked(ctx, "reverse-auth-prover-start-failed");
                    return;
                }
                utils::cleanse_container(ctx.pending_private_key);
                ctx.peer->set_state(PeerState::AUTHENTICATING);
                ctx.last_useful_message_at = std::chrono::steady_clock::now();
                return;
            }

            if (!ctx.secure_established || !ctx.peer->is_secure()) {
                disconnect_peer_unlocked(ctx, "secure-channel-lost-before-ready");
                return;
            }
            if (!admit_authenticated_identity_unlocked(ctx)) {
                return;
            }
            GEO_LOG_INFO("peers", "Mutual authentication completed");
            ctx.peer->set_state(PeerState::READY);

            PeerInfo info = ctx.peer->get_info();
            info.auth_verified = true;
            ctx.peer->update_info(info);

            // Отправляем VERACK
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  Sending VERACK...");
            }
            (void)send_message(ctx.peer, MessageType::VERACK, bytes());

            // Вызываем callback аутентификации
            auto callback = peer_authenticated_callback_;
            if (callback) {
                auto peer = ctx.peer;
                auto height = info.height;
                deferred_tasks.emplace_back([callback = std::move(callback), peer, height]() {
                    callback(peer, height);
                });
            }
        } else {
            if (SyncDebugEnabled()) {
                GEO_LOG_DEBUG("peers", "  Need more rounds, sending next CHALLENGE...");
            }
            // Отправляем следующий challenge
            crypto::ChallengeData challenge;
            if (authenticator_.get_next_challenge(ctx.auth_session, challenge)) {
                ChallengeMessage chal_msg;
                chal_msg.challenge = challenge;
                const bool sent = send_message(
                    ctx.peer,
                    MessageType::CHALLENGE,
                    chal_msg.serialize()
                );
                if (SyncDebugEnabled()) {
                    GEO_LOG_DEBUG("peers", "  Next CHALLENGE sent: " << (sent ? "YES" : "NO"));
                }
                if (!sent) {
                    GEO_LOG_ERROR("peers", "handle_response: failed to send next CHALLENGE");
                    disconnect_peer_unlocked(ctx, "challenge-send-failed");
                    return;
                }
            } else {
                GEO_LOG_ERROR("peers", "handle_response: failed to get next challenge");
                disconnect_peer_unlocked(ctx, "challenge-create-failed");
                return;
            }
        }

    } catch (const std::exception& e) {
        GEO_LOG_ERROR("peers", "Exception in handle_response: " << e.what());
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "response-malformed");
        disconnect_peer_unlocked(ctx, "response-malformed");
    } catch (...) {
        GEO_LOG_ERROR("peers", "Unknown exception in handle_response");
        penalize_peer_unlocked(ctx, detail::kPolicyBanScoreThreshold, "response-malformed");
        disconnect_peer_unlocked(ctx, "response-malformed");
    }
}

void PeerManager::handle_ping(PeerContext& ctx) {
    send_pong(ctx.peer);
}

void PeerManager::handle_reject(PeerContext& ctx, const bytes& payload) {
    try {
        const RejectMessage reject = RejectMessage::deserialize(payload);
        GEO_LOG_WARN(
            "peers",
            "Peer " << ctx.peer->get_address()
                << " sent REJECT: type=" << static_cast<int>(reject.rejected_type)
                << ", code=" << static_cast<int>(reject.code)
                << ", reason=" << reject.reason
        );
    } catch (const std::exception& e) {
        GEO_LOG_WARN("peers", "Malformed REJECT from " << ctx.peer->get_address() << ": " << e.what());
        penalize_peer_unlocked(ctx, 20, "reject-malformed");
        disconnect_peer_unlocked(ctx, "reject-malformed");
    }
}

void PeerManager::send_pong(std::shared_ptr<Peer> peer) {
    send_message(peer, MessageType::PONG, bytes());
}

}  // namespace geocoin::network
