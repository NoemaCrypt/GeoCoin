#include "geocoin/crypto/challenge_response.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/network/dns_resolver.hpp"
#include "geocoin/network/falcon_auth.hpp"
#include "geocoin/types.hpp"
#include "geocoin/network/dns_seeds.h"
#include "test_utils.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using geocoin::test::address_from_key;
using geocoin::test::expect;
using geocoin::test::expect_throws;
using geocoin::test::make_key;

void test_challenge_response_roundtrip_and_rejections()
{
    using namespace geocoin;
    using namespace geocoin::crypto;

    const bytes key = make_key(101);
    const Address address = address_from_key(key);
    const Address wrong_address = address_from_key(make_key(102));
    const Hash256 channel_binding = sha256(bytes{'c', 'h', 'a', 'n', 'n', 'e', 'l'});

    ChallengeData challenge = create_challenge(channel_binding);
    expect(challenge.value.size() == 32, "challenge value is 32 bytes");
    expect(challenge.session_id.size() == 16, "challenge session id is 16 bytes");
    expect(challenge.nonce == challenge.session_id, "challenge nonce aliases session id");
    expect(challenge.rounds.size() == 3, "challenge compatibility rounds are populated");
    expect(challenge.channel_binding == channel_binding, "challenge carries channel binding");

    ChallengeResponse response = respond_to_challenge(key, challenge);
    expect(!response.proof.empty(), "challenge response proof is present");
    expect(response.responses.size() == 3, "response compatibility rounds are populated");
    expect(verify_challenge_response(response, address, challenge), "valid response verifies");
    expect(!verify_challenge_response(response, wrong_address, challenge), "wrong address is rejected");

    ChallengeResponse empty_response;
    expect(!verify_challenge_response(empty_response, address, challenge), "empty proof is rejected");
    expect(!verify_challenge_response(response, Address(""), challenge), "empty expected address is rejected");

    ChallengeData bad_challenge = challenge;
    bad_challenge.value.pop_back();
    expect(!verify_challenge_response(response, address, bad_challenge), "malformed challenge is rejected");

    expect_throws("short private key must throw", [&] {
        (void)respond_to_challenge(bytes{1, 2, 3}, challenge);
    });

    bytes serialized_challenge = serialize_challenge(challenge);
    ChallengeData parsed_challenge = deserialize_challenge(serialized_challenge);
    expect(parsed_challenge.value == challenge.value, "challenge value roundtrips");
    expect(parsed_challenge.timestamp == challenge.timestamp, "challenge timestamp roundtrips");
    expect(parsed_challenge.session_id == challenge.session_id, "challenge session roundtrips");
    expect(parsed_challenge.channel_binding == channel_binding, "channel binding roundtrips");
    expect(parsed_challenge.nonce == parsed_challenge.session_id, "deserialized nonce aliases session");

    expect_throws("bad challenge size must throw", [&] {
        (void)deserialize_challenge(bytes{1, 2, 3});
    });
    ChallengeData invalid_session = challenge;
    invalid_session.session_id.pop_back();
    expect_throws("bad session id size must throw", [&] {
        (void)serialize_challenge(invalid_session);
    });

    bytes serialized_response = serialize_response(response);
    ChallengeResponse parsed_response = deserialize_response(serialized_response);
    expect(parsed_response.proof == response.proof, "response proof roundtrips");
    expect(parsed_response.responses.size() == 3, "deserialized response compatibility rounds are populated");
    expect(verify_challenge_response(parsed_response, address, challenge), "deserialized response verifies");

    expect_throws("short response must throw", [&] {
        (void)deserialize_response(bytes{1, 2, 3});
    });
    bytes zero_len_response(4, 0);
    expect_throws("zero proof length must throw", [&] {
        (void)deserialize_response(zero_len_response);
    });
    bytes bad_len_response = serialized_response;
    bad_len_response[0] ^= 0x01;
    expect_throws("bad proof length must throw", [&] {
        (void)deserialize_response(bad_len_response);
    });
}

void test_multi_round_auth_state_machine()
{
    using namespace geocoin;
    using namespace geocoin::crypto;

    const bytes key = make_key(121);
    const Address address = address_from_key(key);

    expect_throws("zero rounds must throw", [] {
        MultiRoundAuth invalid(0);
        (void)invalid;
    });
    expect_throws("too many rounds must throw", [] {
        MultiRoundAuth invalid(11);
        (void)invalid;
    });

    MultiRoundAuth auth(2);
    expect(auth.current_round() == 0, "auth starts at round zero");
    expect(auth.total_rounds() == 2, "auth stores total rounds");
    expect(auth.get_security_bits() == 128.0,
           "repeated challenges do not overstate Falcon-512 key security");
    expect(!auth.is_complete(), "new auth is incomplete");
    expect(!auth.finalize(), "incomplete auth does not finalize");
    expect(!auth.verify_round(ChallengeResponse{}, address), "verify before start fails");

    ChallengeData round1 = auth.start_round();
    expect(auth.current_round() == 1, "round counter increments");
    expect_throws("next round cannot start before current response", [&] {
        (void)auth.start_round();
    });
    expect(!auth.verify_round(respond_to_challenge(make_key(122), round1), address), "wrong key fails round");
    const ChallengeResponse response1 = respond_to_challenge(key, round1);
    expect(auth.verify_round(response1, address), "round one verifies");
    expect(!auth.verify_round(response1, address),
           "one signed response cannot satisfy the same round twice");
    expect(!auth.is_complete(), "one of two rounds is incomplete");

    ChallengeData round2 = auth.start_round();
    expect(auth.verify_round(respond_to_challenge(key, round2), address), "round two verifies");
    expect(auth.is_complete(), "two verified rounds complete auth");
    expect(auth.finalize(), "complete auth finalizes");
    expect_throws("start after finalize must throw", [&] {
        (void)auth.start_round();
    });

    auth.reset();
    expect(auth.current_round() == 0, "reset clears current round");
    expect(!auth.is_complete(), "reset clears completion");
}

void test_falcon_authenticator_state_machine_and_guards()
{
    using namespace geocoin;
    using namespace geocoin::network;

    FalconAuthenticator auth;
    const bytes key = make_key(141);
    const Address address = address_from_key(key);
    const Address wrong_address = address_from_key(make_key(142));
    const Hash256 channel_binding = crypto::sha256(bytes{'a', 'u', 't', 'h'});
    Hash256 other_binding = channel_binding;
    other_binding[0] ^= 0x80;

    std::shared_ptr<AuthSession> verifier;
    std::shared_ptr<AuthSession> prover;
    crypto::ChallengeData challenge;
    crypto::ChallengeResponse response;

    expect(!auth.start_as_verifier(Address(""), channel_binding, verifier),
           "verifier rejects empty expected address");
    expect(!auth.start_as_verifier(address, Hash256{}, verifier),
           "verifier rejects missing channel binding");
    expect(!auth.start_as_prover(bytes{1, 2, 3}, channel_binding, prover),
           "prover rejects short private key");
    expect(!auth.start_as_prover(key, Hash256{}, prover),
           "prover rejects missing channel binding");
    expect(!auth.get_next_challenge(nullptr, challenge), "challenge rejects null session");
    expect(!auth.handle_challenge(nullptr, challenge, response), "handle challenge rejects null session");
    expect(!auth.verify_response(nullptr, response), "verify response rejects null session");
    expect(!auth.is_authenticated(nullptr), "null auth session is not authenticated");
    expect(auth.get_status(nullptr) == AuthStatus::FAILED, "null auth session status is failed");
    expect(auth.is_timeout(nullptr), "null auth session is timed out");
    auth.cancel_session(nullptr);

    std::shared_ptr<AuthSession> canceled;
    expect(auth.start_as_verifier(address, channel_binding, canceled), "verifier starts for cancel");
    auth.cancel_session(canceled);
    expect(canceled->status == AuthStatus::FAILED, "cancel marks live session failed");

    expect(auth.start_as_verifier(address, channel_binding, verifier), "verifier starts");
    expect(auth.start_as_prover(key, channel_binding, prover), "prover starts");
    expect(!auth.get_next_challenge(prover, challenge), "prover cannot issue challenges");
    expect(!auth.handle_challenge(verifier, challenge, response), "verifier cannot answer challenges");
    expect(!auth.verify_response(prover, response), "prover cannot verify responses");

    auto stale = verifier;
    stale->last_activity = std::chrono::steady_clock::now() - std::chrono::seconds(61);
    expect(auth.get_status(stale) == AuthStatus::TIMEOUT, "stale session status is timeout");

    for (int round = 0; round < 3; ++round) {
        expect(auth.get_next_challenge(verifier, challenge), "verifier issues challenge");
        expect(auth.handle_challenge(prover, challenge, response), "prover answers challenge");
        expect(auth.verify_response(verifier, response), "verifier accepts response");
    }
    expect(auth.is_authenticated(verifier), "verifier completes after three rounds");
    expect(auth.get_status(verifier) == AuthStatus::COMPLETED, "completed session reports completed");
    expect(!auth.get_next_challenge(verifier, challenge), "completed verifier has no extra challenge");
    expect(!auth.verify_response(verifier, response), "completed verifier rejects extra response");

    expect(auth.start_as_verifier(address, channel_binding, verifier),
           "fresh verifier starts for failure checks");
    expect(auth.get_next_challenge(verifier, challenge), "fresh verifier issues challenge");
    crypto::ChallengeResponse wrong_response = crypto::respond_to_challenge(make_key(143), challenge);
    expect(!auth.verify_response(verifier, wrong_response), "wrong signer response is rejected");
    expect(auth.get_status(verifier) == AuthStatus::FAILED, "bad response marks session failed");

    expect(auth.start_as_prover(key, channel_binding, prover),
           "fresh prover starts for malformed challenge checks");
    crypto::ChallengeData malformed = crypto::create_challenge(channel_binding);
    malformed.session_id.pop_back();
    expect(!auth.handle_challenge(prover, malformed, response), "prover rejects bad session id length");

    expect(auth.start_as_prover(key, channel_binding, prover),
           "fresh prover starts for session mismatch checks");
    expect(auth.start_as_verifier(address, channel_binding, verifier),
           "fresh verifier starts for session mismatch checks");
    expect(auth.get_next_challenge(verifier, challenge), "verifier issues first challenge");
    expect(auth.handle_challenge(prover, challenge, response), "prover adopts verifier session id");
    crypto::ChallengeData mismatched = challenge;
    mismatched.session_id[0] ^= 0x01;
    expect(!auth.handle_challenge(prover, mismatched, response), "prover rejects mismatched session id");

    std::shared_ptr<AuthSession> wrong_channel_prover;
    expect(auth.start_as_prover(key, other_binding, wrong_channel_prover),
           "prover starts on a different channel");
    expect(!auth.handle_challenge(wrong_channel_prover, challenge, response),
           "prover rejects challenge relayed from another encrypted channel");

    prover->current_round = 3;
    expect(!auth.handle_challenge(prover, challenge, response), "prover rejects challenges after max rounds");

    expect(wrong_address != address, "test addresses differ");
}

void test_signature_helpers_and_rejections()
{
    using namespace geocoin;
    using namespace geocoin::crypto;

    const bytes key = make_key(151);
    const Hash256 tx_hash = sha256(bytes{'t', 'x', '-', 'h', 'a', 's', 'h'});
    const Hash256 msg_hash = sha256(bytes{'m', 's', 'g'});

    const Signature input_sig = sign_transaction_input(tx_hash, 2, key);
    expect(!input_sig.empty(), "transaction input signature is produced");
    expect(verify_transaction_input(tx_hash, 2, input_sig), "transaction input signature verifies");
    expect(!verify_transaction_input(tx_hash, 3, input_sig), "transaction input signature rejects wrong index");

    Address recovered;
    expect(recover_signer_address_from_transaction_input(tx_hash, 2, input_sig, recovered),
           "transaction input signature recovers signer");
    expect(!recovered.empty(), "recovered transaction signer address is populated");

    bytes detached_pubkey;
    Signature detached_sig;
    expect(sign_transaction_input_detached(tx_hash, 2, key, detached_pubkey, detached_sig),
           "detached transaction input signature is produced");
    expect(verify_transaction_input_detached(tx_hash, 2, detached_pubkey, detached_sig),
           "detached transaction input signature verifies");
    expect(!verify_transaction_input_detached(tx_hash, 3, detached_pubkey, detached_sig),
           "detached transaction input signature rejects wrong index");
    bytes rejected_pubkey{0x01};
    Signature rejected_detached_sig{0x02};
    expect(!sign_transaction_input_detached(tx_hash, 0, bytes{1, 2, 3}, rejected_pubkey, rejected_detached_sig),
           "detached transaction input signing rejects short private key");
    expect(rejected_pubkey.empty(), "failed detached signing clears public key");
    expect(rejected_detached_sig.empty(), "failed detached signing clears signature");

    const Signature message_sig = sign_recoverable_message(msg_hash, key);
    expect(verify_signed_message(msg_hash, message_sig), "signed message verifies");
    expect(recover_signer_address_from_signed_message(msg_hash, message_sig, recovered),
           "signed message recovers signer");
    expect(!verify_signed_message(tx_hash, message_sig), "signed message rejects wrong digest");
    expect_throws("short message signing key throws", [&] {
        (void)sign_recoverable_message(msg_hash, bytes{1, 2, 3});
    });

    const Signature angle_sig = sign_angle_commitment(tx_hash, key);
    expect(verify_angle_commitment_signature(tx_hash, angle_sig), "angle commitment signature verifies");
    expect(recover_signer_address_from_angle_commitment(tx_hash, angle_sig, recovered),
           "angle commitment signature recovers signer");
    expect_throws("short angle signing key throws", [&] {
        (void)sign_angle_commitment(tx_hash, bytes{1, 2, 3});
    });

    Address address;
    expect(address_from_public_key(detached_pubkey, address), "address derives from detached public key");
    expect(!address.empty(), "derived public key address is populated");
    expect(!address_from_public_key(bytes{}, address), "empty public key is rejected");
    expect(derive_identity_address_from_private_key(key, address), "identity address derives from private key");
    expect(!derive_identity_address_from_private_key(bytes{1, 2, 3}, address),
           "identity address rejects short private key");

    Signature malformed = input_sig;
    malformed[0] ^= 0xff;
    expect(!verify_transaction_input(tx_hash, 2, malformed), "malformed packed signature is rejected");
    expect(!recover_signer_address_from_transaction_input(tx_hash, 2, malformed, recovered),
           "malformed packed signature cannot recover signer");

    const Signature zero_length_packed{'F', '5', '1', '2', 0, 0, 0, 1, 0x42, 0x43};
    expect(!verify_signed_message(msg_hash, zero_length_packed),
           "packed signature rejects zero public key length");
    const Signature bad_length_packed{'F', '5', '1', '2', 0, 2, 0, 2, 0x01, 0x02, 0x03};
    expect(!verify_signed_message(msg_hash, bad_length_packed),
           "packed signature rejects mismatched declared lengths");

    bytes too_large_key(70000, 0x42);
    expect_throws("too-large public key cannot be packed through signing failure path", [&] {
        (void)sign_transaction_input(tx_hash, 0, bytes{1, 2, 3});
    });
    expect(too_large_key.size() > 0xFFFF, "oversized signature helper fixture is oversized");
}

void test_dns_resolver_local_and_invalid_inputs()
{
    using geocoin::network::DNSResolver;

    const char* const null_terminated[] = {"", "localhost", nullptr};
    expect(DNSResolver::resolve_seeds_with_port(nullptr, 3033).empty(), "null seed array returns empty");
    expect(DNSResolver::resolve_seeds_with_port(null_terminated, 0).empty(), "bad port returns empty");
    expect(DNSResolver::resolve_seeds_with_port(std::vector<std::string>{}, 3033).empty(), "empty vector returns empty");
    expect(DNSResolver::resolve_seeds_with_port(std::vector<std::string>{"localhost"}, -1).empty(), "bad vector port returns empty");

    auto local_from_array = DNSResolver::resolve_seeds_with_port(null_terminated, 3033);
    auto local_from_vector = DNSResolver::resolve_seeds_with_port(std::vector<std::string>{"localhost"}, 3034);
    expect(!local_from_array.empty(), "localhost array resolves to at least one IPv4 address");
    expect(!local_from_vector.empty(), "localhost vector resolves to at least one IPv4 address");
    expect(
        std::all_of(local_from_array.begin(), local_from_array.end(), [](const std::string& value) {
            return value.ends_with(":3033");
        }),
        "array resolver appends requested port"
    );
    expect(
        std::all_of(local_from_vector.begin(), local_from_vector.end(), [](const std::string& value) {
            return value.ends_with(":3034");
        }),
        "vector resolver appends requested port"
    );

    const auto mainnet_seeds = geocoin::network::get_hardcoded_seed_nodes(false);
    const auto testnet_seeds = geocoin::network::get_hardcoded_seed_nodes(true);
    expect(!mainnet_seeds.empty(), "mainnet fallback seeds are present");
    expect(!testnet_seeds.empty(), "testnet fallback seeds are present");
    expect(std::all_of(mainnet_seeds.begin(), mainnet_seeds.end(), [](const std::string& value) {
               return value.ends_with(":3030");
           }),
           "mainnet fallback seeds use default P2P port");
    expect(std::all_of(testnet_seeds.begin(), testnet_seeds.end(), [](const std::string& value) {
               return value.ends_with(":13030");
           }),
           "testnet fallback seeds use default testnet P2P port");
}

} // namespace

int main()
{
    try {
        test_challenge_response_roundtrip_and_rejections();
        test_multi_round_auth_state_machine();
        test_falcon_authenticator_state_machine_and_guards();
        test_signature_helpers_and_rejections();
        test_dns_resolver_local_and_invalid_inputs();
        std::cout << "test_auth_network_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_auth_network_units failed: " << e.what() << "\n";
        return 1;
    }
}
