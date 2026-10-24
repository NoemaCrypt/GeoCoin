#include "geocoin/network/peer.hpp"
#include "geocoin/network/messages.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

class FakeTransport final : public geocoin::network::PeerTransport {
public:
    bool connect(std::chrono::milliseconds timeout) override {
        last_connect_timeout = timeout;
        open = connect_succeeds;
        return open;
    }

    void close() noexcept override {
        open = false;
        ++close_calls;
    }

    bool is_open() const noexcept override {
        return open;
    }

    std::ptrdiff_t send(const geocoin::bytes& data) override {
        if (!open) {
            return -1;
        }
        last_sent = data;
        return forced_send_result == kAutomatic
            ? static_cast<std::ptrdiff_t>(data.size())
            : forced_send_result;
    }

    std::ptrdiff_t receive(geocoin::bytes& buffer, size_t max_size) override {
        if (!open) {
            return -1;
        }
        buffer = next_receive;
        if (buffer.size() > max_size) {
            buffer.resize(max_size);
        }
        if (forced_receive_result != kAutomatic) {
            if (forced_receive_result == 0 && close_on_zero_receive) {
                open = false;
            }
            return forced_receive_result;
        }
        return static_cast<std::ptrdiff_t>(buffer.size());
    }

    static constexpr std::ptrdiff_t kAutomatic = (std::numeric_limits<std::ptrdiff_t>::min)();

    bool open = false;
    bool connect_succeeds = true;
    bool close_on_zero_receive = false;
    int close_calls = 0;
    std::chrono::milliseconds last_connect_timeout{0};
    geocoin::bytes last_sent;
    geocoin::bytes next_receive;
    std::ptrdiff_t forced_send_result = kAutomatic;
    std::ptrdiff_t forced_receive_result = kAutomatic;
};

geocoin::bytes make_plaintext_hello_frame() {
    geocoin::network::SecureHelloMessage hello;
    hello.hello.x25519_public_key.assign(32, 0x11);
    hello.hello.nonce.assign(32, 0x22);
    hello.hello.ml_kem_public_key.assign(
        geocoin::crypto::ml_kem768::kPublicKeySize, 0x33);
    return geocoin::network::create_message(
        geocoin::network::MessageType::SECURE_HELLO,
        hello.serialize());
}

void test_injected_transport_lifecycle_and_metrics() {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* observed = transport.get();
    geocoin::network::Peer peer("fake.example:3030", std::move(transport));

    check(!peer.is_connected(), "closed injected transport starts disconnected");
    check(peer.connect(std::chrono::milliseconds(0)), "injected transport can connect");
    check(observed->last_connect_timeout == std::chrono::milliseconds(1),
          "Peer clamps a non-positive connect timeout");
    check(peer.get_state() == geocoin::network::PeerState::CONNECTED,
          "successful transport connect advances peer state");

    const geocoin::bytes outbound = make_plaintext_hello_frame();
    check(peer.send(outbound) == static_cast<ssize_t>(outbound.size()),
          "Peer delegates a complete send");
    check(observed->last_sent == outbound, "transport receives exact outbound bytes");
    check(peer.get_info().bytes_sent == outbound.size(), "Peer owns sent-byte metrics");

    observed->next_receive = {9, 8, 7};
    geocoin::bytes inbound;
    check(peer.receive(inbound, 16) == 3, "Peer delegates receive");
    check(inbound == observed->next_receive, "Peer preserves received bytes");
    check(peer.get_info().bytes_received == 3, "Peer owns received-byte metrics");

    peer.disconnect();
    check(!peer.is_connected(), "disconnect closes injected transport");
    check(observed->close_calls >= 1, "disconnect reaches transport boundary");
}

void test_transport_contract_failures_close_session() {
    {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* observed = transport.get();
        observed->open = true;
        observed->forced_send_result = 1;
        geocoin::network::Peer peer("partial.example:3030", std::move(transport));

        check(peer.send(make_plaintext_hello_frame()) == -1,
              "partial framed send reports session failure instead of a reusable prefix");
        check(!peer.is_connected(), "partial framed send closes peer session");
        check(!observed->open, "partial send closes underlying transport");
    }

    {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* observed = transport.get();
        observed->open = true;
        observed->next_receive = {0xaa};
        observed->forced_receive_result = 2;
        geocoin::network::Peer peer("invalid-read.example:3030", std::move(transport));
        geocoin::bytes buffer;

        check(peer.receive(buffer, 8) == -1,
              "Peer rejects a transport result larger than its output buffer");
        check(buffer.empty(), "invalid transport output is discarded");
        check(!peer.is_connected(), "invalid transport result closes peer session");
    }

    {
        auto transport = std::make_unique<FakeTransport>();
        FakeTransport* observed = transport.get();
        observed->open = true;
        observed->forced_receive_result = 0;
        observed->close_on_zero_receive = true;
        geocoin::network::Peer peer("eof.example:3030", std::move(transport));
        geocoin::bytes buffer;

        check(peer.receive(buffer, 8) == 0, "orderly transport EOF reports zero bytes");
        check(!peer.is_connected(), "orderly transport EOF closes peer session");
    }
}

void test_null_transport_is_rejected() {
    bool rejected = false;
    try {
        geocoin::network::Peer peer(
            "null.example:3030",
            std::unique_ptr<geocoin::network::PeerTransport>{}
        );
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "Peer rejects null transport ownership");
}

void test_peer_secure_channel_is_transparent_to_callers() {
    using namespace geocoin;
    using namespace geocoin::network;

    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* observed = transport.get();
    observed->open = true;
    Peer peer("encrypted.example:3030", std::move(transport));

    SecureChannelKeyMaterial peer_keys;
    SecureChannelKeyMaterial remote_keys;
    std::fill(peer_keys.send_key.begin(), peer_keys.send_key.end(), 0x11);
    std::fill(peer_keys.receive_key.begin(), peer_keys.receive_key.end(), 0x22);
    std::fill(remote_keys.send_key.begin(), remote_keys.send_key.end(), 0x22);
    std::fill(remote_keys.receive_key.begin(), remote_keys.receive_key.end(), 0x11);
    std::fill(peer_keys.send_nonce_prefix.begin(), peer_keys.send_nonce_prefix.end(), 0x33);
    std::fill(peer_keys.receive_nonce_prefix.begin(), peer_keys.receive_nonce_prefix.end(), 0x44);
    std::fill(remote_keys.send_nonce_prefix.begin(), remote_keys.send_nonce_prefix.end(), 0x44);
    std::fill(remote_keys.receive_nonce_prefix.begin(), remote_keys.receive_nonce_prefix.end(), 0x33);
    peer_keys.channel_binding[0] = 0x55;
    remote_keys.channel_binding = peer_keys.channel_binding;

    SecureChannel remote(std::move(remote_keys));
    check(peer.activate_secure_channel(std::move(peer_keys)),
          "Peer activates secure channel once");
    check(peer.is_secure(), "Peer reports active encryption");
    check(peer.get_info().transport_encrypted, "Peer diagnostics report encryption");

    const bytes frame = create_message(MessageType::PING, {});
    check(peer.send(frame) == static_cast<ssize_t>(frame.size()),
          "encrypted Peer::send reports logical frame size");
    check(observed->last_sent != frame, "transport never receives plaintext frame after activation");
    check(observed->last_sent.size() ==
              frame.size() + kSecureRecordPrefixSize + kSecureRecordTagSize,
          "transport receives framed AEAD ciphertext");
    check(peer.get_info().bytes_sent == observed->last_sent.size(),
          "Peer byte counters measure actual wire traffic");

    bytes decrypted;
    check(remote.unprotect(observed->last_sent, decrypted),
          "remote channel decrypts Peer wire record");
    check(decrypted == frame, "Peer encryption preserves complete P2P frame");

    peer.disconnect();
    check(!peer.is_secure(), "disconnect destroys session keys");
    check(!peer.get_info().transport_encrypted,
          "disconnect clears encryption diagnostics");
}

} // namespace

int main() {
    test_injected_transport_lifecycle_and_metrics();
    test_transport_contract_failures_close_session();
    test_null_transport_is_rejected();
    test_peer_secure_channel_is_transparent_to_callers();

    if (failures == 0) {
        std::cout << "Peer transport unit tests passed.\n";
        return 0;
    }
    std::cerr << failures << " peer transport unit test(s) failed.\n";
    return 1;
}
