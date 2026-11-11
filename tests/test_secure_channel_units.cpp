#include "geocoin/network/messages.hpp"
#include "geocoin/network/secure_channel.hpp"
#include "geocoin/network/secure_handshake.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using namespace geocoin;
using namespace geocoin::network;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Fn>
void check_throws(Fn&& fn, const char* message) {
    try {
        fn();
        check(false, message);
    } catch (const std::exception&) {
    }
}

struct ChannelPair {
    std::unique_ptr<SecureChannel> initiator;
    std::unique_ptr<SecureChannel> responder;
    Hash256 binding{};
};

ChannelPair make_channel_pair() {
    auto initiator_handshake = SecureHandshake::create_initiator();
    check(initiator_handshake != nullptr, "initiator handshake is created");
    if (!initiator_handshake) {
        return {};
    }

    SecureHelloMessage hello;
    hello.hello = initiator_handshake->hello();
    const bytes hello_payload = hello.serialize();
    check(hello_payload.size() == kSecureHelloPayloadSize, "SECURE_HELLO has fixed size");
    const SecureHelloMessage parsed_hello = SecureHelloMessage::deserialize(hello_payload);

    auto responder_handshake = SecureHandshake::create_responder(parsed_hello.hello);
    check(responder_handshake != nullptr, "responder handshake is created");
    if (!responder_handshake) {
        return {};
    }

    SecureReplyMessage reply;
    reply.reply = responder_handshake->reply();
    const bytes reply_payload = reply.serialize();
    check(reply_payload.size() == kSecureReplyPayloadSize, "SECURE_REPLY has fixed size");
    const SecureReplyMessage parsed_reply = SecureReplyMessage::deserialize(reply_payload);

    check(responder_handshake->complete_responder(
              MessageHeader::MAGIC_MAINNET, hello_payload, reply_payload),
          "responder derives channel keys");
    check(initiator_handshake->complete_initiator(
              parsed_reply.reply,
              MessageHeader::MAGIC_MAINNET,
              hello_payload,
              reply_payload),
          "initiator derives channel keys");
    check(initiator_handshake->channel_binding() == responder_handshake->channel_binding(),
          "both endpoints derive the same channel binding");
    check(std::any_of(
              initiator_handshake->channel_binding().begin(),
              initiator_handshake->channel_binding().end(),
              [](byte value) { return value != 0; }),
          "channel binding is non-zero");

    SecureChannelKeyMaterial initiator_keys;
    SecureChannelKeyMaterial responder_keys;
    check(initiator_handshake->take_key_material(initiator_keys),
          "initiator key material is available once");
    SecureChannelKeyMaterial duplicate_keys;
    check(!initiator_handshake->take_key_material(duplicate_keys),
          "initiator key material cannot be taken twice");
    check(responder_handshake->take_key_material(responder_keys),
          "responder key material is available once");

    ChannelPair result;
    result.binding = initiator_handshake->channel_binding();
    result.initiator = std::make_unique<SecureChannel>(std::move(initiator_keys));
    result.responder = std::make_unique<SecureChannel>(std::move(responder_keys));
    return result;
}

void test_hybrid_handshake_and_bidirectional_records() {
    ChannelPair pair = make_channel_pair();
    if (!pair.initiator || !pair.responder) {
        return;
    }

    const bytes ping_frame = create_message(MessageType::PING, {});
    bytes encrypted_ping;
    check(pair.initiator->protect(ping_frame, encrypted_ping),
          "initiator encrypts a complete P2P frame");
    check(encrypted_ping.size() ==
              kSecureRecordPrefixSize + ping_frame.size() + kSecureRecordTagSize,
          "record overhead is one length prefix and one AEAD tag");
    check(std::search(
              encrypted_ping.begin(), encrypted_ping.end(),
              ping_frame.begin(), ping_frame.end()) == encrypted_ping.end(),
          "wire record does not expose its plaintext frame");

    bytes decrypted_ping;
    check(pair.responder->unprotect(encrypted_ping, decrypted_ping),
          "responder authenticates and decrypts initiator record");
    check(decrypted_ping == ping_frame, "decrypted initiator frame is exact");

    const bytes pong_frame = create_message(MessageType::PONG, {});
    bytes encrypted_pong;
    bytes decrypted_pong;
    check(pair.responder->protect(pong_frame, encrypted_pong),
          "responder encrypts reverse-direction frame");
    check(pair.initiator->unprotect(encrypted_pong, decrypted_pong),
          "initiator decrypts reverse-direction frame");
    check(decrypted_pong == pong_frame, "decrypted responder frame is exact");

    bytes replay_output{0xaa};
    check(!pair.responder->unprotect(encrypted_ping, replay_output),
          "implicit sequence number rejects replayed record");
    check(replay_output.empty(), "failed record authentication clears output");

    const bytes second_frame = create_message(MessageType::VERACK, {});
    bytes second_record;
    check(pair.initiator->protect(second_frame, second_record),
          "initiator encrypts next ordered record");
    bytes tampered = second_record;
    tampered.back() ^= 0x01;
    check(!pair.responder->unprotect(tampered, replay_output),
          "modified AEAD tag is rejected");
    check(pair.responder->unprotect(second_record, replay_output),
          "failed authentication does not advance receive counter");
    check(replay_output == second_frame, "original record remains valid after tamper rejection");
}

void test_record_bounds_and_handshake_transcript_binding() {
    SecureChannelKeyMaterial local_keys;
    std::fill(local_keys.send_key.begin(), local_keys.send_key.end(), 1);
    std::fill(local_keys.receive_key.begin(), local_keys.receive_key.end(), 2);
    std::fill(local_keys.send_nonce_prefix.begin(), local_keys.send_nonce_prefix.end(), 3);
    std::fill(local_keys.receive_nonce_prefix.begin(), local_keys.receive_nonce_prefix.end(), 4);
    local_keys.channel_binding[0] = 1;
    SecureChannel channel(std::move(local_keys));
    bytes record;
    check(!channel.protect({}, record), "record layer rejects an empty non-frame");
    check(!channel.protect(bytes(12, 0), record), "record layer rejects a short non-frame");

    size_t record_size = 99;
    const bytes partial_prefix{1, 2, 3};
    check(SecureChannel::inspect_record_size(
              partial_prefix.data(), partial_prefix.size(), record_size) ==
              SecureRecordSizeStatus::INCOMPLETE,
          "record parser waits for complete length prefix");
    const bytes undersized_record{16, 0, 0, 0};
    check(SecureChannel::inspect_record_size(
              undersized_record.data(), undersized_record.size(), record_size) ==
              SecureRecordSizeStatus::INVALID,
          "record parser rejects tag-only record");

    auto initiator = SecureHandshake::create_initiator();
    SecureHelloMessage hello;
    hello.hello = initiator->hello();
    const bytes hello_payload = hello.serialize();
    auto responder = SecureHandshake::create_responder(hello.hello);
    SecureReplyMessage reply;
    reply.reply = responder->reply();
    const bytes original_reply_payload = reply.serialize();
    check(responder->complete_responder(
              MessageHeader::MAGIC_MAINNET, hello_payload, original_reply_payload),
          "responder binds original transcript");

    bytes modified_reply_payload = original_reply_payload;
    modified_reply_payload[36] ^= 0x40; // responder nonce, not a key-exchange input
    const SecureReplyMessage modified_reply =
        SecureReplyMessage::deserialize(modified_reply_payload);
    check(initiator->complete_initiator(
              modified_reply.reply,
              MessageHeader::MAGIC_MAINNET,
              hello_payload,
              modified_reply_payload),
          "initiator still derives secrets from transcript-modified reply");
    check(initiator->channel_binding() != responder->channel_binding(),
          "any handshake transcript change produces a different binding");

    SecureChannelKeyMaterial initiator_keys;
    SecureChannelKeyMaterial responder_keys;
    check(initiator->take_key_material(initiator_keys), "modified initiator keys are available");
    check(responder->take_key_material(responder_keys), "original responder keys are available");
    SecureChannel initiator_channel(std::move(initiator_keys));
    SecureChannel responder_channel(std::move(responder_keys));
    const bytes frame = create_message(MessageType::PING, {});
    check(initiator_channel.protect(frame, record), "modified transcript channel encrypts");
    bytes output;
    check(!responder_channel.unprotect(record, output),
          "transcript modification prevents record authentication");
}

void test_fixed_handshake_message_validation() {
    auto initiator = SecureHandshake::create_initiator();
    SecureHelloMessage hello;
    hello.hello = initiator->hello();
    const bytes payload = hello.serialize();

    bytes trailing = payload;
    trailing.push_back(0);
    check_throws([&] { (void)SecureHelloMessage::deserialize(trailing); },
                 "SECURE_HELLO rejects trailing data");
    bytes wrong_version = payload;
    wrong_version[0] ^= 1;
    check_throws([&] { (void)SecureHelloMessage::deserialize(wrong_version); },
                 "SECURE_HELLO rejects downgrade version");

    SecureHelloMessage invalid = hello;
    invalid.hello.ml_kem_public_key.pop_back();
    check_throws([&] { (void)invalid.serialize(); },
                 "SECURE_HELLO rejects malformed ML-KEM key");
}

// A low-order X25519 point forces an all-zero shared secret. Accepting it
// would silently remove the X25519 half of the advertised hybrid exchange.
void test_low_order_x25519_point_is_rejected() {
    for (const byte low_order_u : {byte{0}, byte{1}}) {
        bytes low_order_key(32, 0);
        low_order_key[0] = low_order_u;

        auto initiator = SecureHandshake::create_initiator();
        SecureHelloMessage hello;
        hello.hello = initiator->hello();
        const bytes hello_payload = hello.serialize();
        auto responder = SecureHandshake::create_responder(hello.hello);
        SecureReplyMessage reply;
        reply.reply = responder->reply();
        reply.reply.x25519_public_key = low_order_key;
        const bytes reply_payload = reply.serialize();
        check(!initiator->complete_initiator(
                  reply.reply, MessageHeader::MAGIC_MAINNET, hello_payload, reply_payload),
              "initiator rejects a low-order responder X25519 key");
        SecureChannelKeyMaterial initiator_keys;
        check(!initiator->take_key_material(initiator_keys),
              "initiator exposes no keys after a low-order responder key");

        SecureHelloMessage hostile_hello = hello;
        hostile_hello.hello.x25519_public_key = low_order_key;
        const bytes hostile_hello_payload = hostile_hello.serialize();
        auto hostile_responder = SecureHandshake::create_responder(hostile_hello.hello);
        if (hostile_responder) {
            SecureReplyMessage hostile_reply;
            hostile_reply.reply = hostile_responder->reply();
            check(!hostile_responder->complete_responder(
                      MessageHeader::MAGIC_MAINNET, hostile_hello_payload, hostile_reply.serialize()),
                  "responder rejects a low-order initiator X25519 key");
            SecureChannelKeyMaterial responder_keys;
            check(!hostile_responder->take_key_material(responder_keys),
                  "responder exposes no keys after a low-order initiator key");
        }
    }
}

} // namespace

int main() {
    test_hybrid_handshake_and_bidirectional_records();
    test_record_bounds_and_handshake_transcript_binding();
    test_fixed_handshake_message_validation();
    test_low_order_x25519_point_is_rejected();

    if (failures == 0) {
        std::cout << "Secure channel unit tests passed.\n";
        return 0;
    }
    std::cerr << failures << " secure channel unit test(s) failed.\n";
    return 1;
}
