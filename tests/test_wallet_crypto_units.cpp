#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/state/storage.hpp"
#include "geocoin/types.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/wallet/angle_store.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/multisig_service.hpp"
#include "test_utils.hpp"
#include "wallet_state_fixture.hpp"

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using geocoin::test::address_from_key;
using geocoin::test::expect;
using geocoin::test::hash_from_seed;
using geocoin::test::make_key;

std::vector<geocoin::Address> make_addresses(size_t count)
{
    std::vector<geocoin::Address> addresses;
    addresses.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        addresses.push_back(address_from_key(make_key(static_cast<uint8_t>(11 + i))));
    }
    return addresses;
}

std::vector<geocoin::bytes> make_keys(uint8_t seed, size_t count)
{
    std::vector<geocoin::bytes> keys;
    keys.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        keys.push_back(make_key(static_cast<uint8_t>(seed + i * 7)));
    }
    return keys;
}

std::vector<geocoin::Address> addresses_from_keys(const std::vector<geocoin::bytes>& keys)
{
    std::vector<geocoin::Address> addresses;
    addresses.reserve(keys.size());
    for (const auto& key : keys) {
        addresses.push_back(address_from_key(key));
    }
    return addresses;
}

geocoin::wallet::MultisigConfig make_config(
    uint8_t m,
    uint8_t n,
    const std::vector<geocoin::Address>& addresses
)
{
    auto multisig = geocoin::wallet::create_multisig_address(m, n, addresses);
    geocoin::wallet::MultisigConfig config = multisig.config;
    config.address = geocoin::Address(multisig.address);
    config.addresses = addresses;
    config.participants.clear();
    for (const auto& address : addresses) {
        config.participants.emplace_back(address);
    }
    return config;
}

geocoin::core::TxInput input_from(
    const geocoin::Hash256& tx_hash,
    uint32_t vout
)
{
    geocoin::core::TxInput input;
    input.prev_tx_hash = tx_hash;
    input.vout = vout;
    return input;
}

geocoin::core::TxOutput output_to(
    const geocoin::Address& address,
    geocoin::Amount amount
)
{
    geocoin::core::TxOutput output;
    output.address = address;
    output.value = amount;
    return output;
}

std::vector<geocoin::Hash256> apply_angle_commitments(
    const std::shared_ptr<geocoin::state::ChainState>& chain_state,
    const geocoin::Address& multisig_address,
    const geocoin::Hash256& signed_hash,
    const std::vector<geocoin::bytes>& signer_keys,
    size_t count,
    geocoin::Height height
)
{
    expect(count <= signer_keys.size(), "commitment count exceeds signer keys");
    std::vector<geocoin::Hash256> hashes;
    hashes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        geocoin::core::Transaction commitment = geocoin::core::Transaction::create_angle_commitment(
            multisig_address,
            signed_hash,
            signer_keys[i],
            geocoin::Address(""),
            "degree"
        );
        const geocoin::Hash256 commitment_hash = commitment.get_hash();
        expect(chain_state->apply_transaction(commitment, height, false), "angle commitment should apply");
        hashes.push_back(commitment_hash);
    }
    return hashes;
}

geocoin::core::TxInput delegation_marker_input()
{
    geocoin::core::TxInput input;
    input.prev_tx_hash = geocoin::Hash256{};
    input.vout = 0xFFFFFFFD;
    return input;
}

void test_encryption_roundtrips_and_rejects_tampering()
{
    using namespace geocoin;
    using namespace geocoin::crypto;

    const bytes plain = {'g', 'e', 'o', 'c', 'o', 'i', 'n', 0, 1, 2, 3};
    bytes encrypted;
    bytes decrypted;

    expect(encrypt_aes256(plain, "correct horse battery staple", encrypted, 1000), "encrypt should succeed");
    expect(encrypted.size() > plain.size(), "encrypted payload includes salt/iv/tag");
    expect(encrypted != plain, "ciphertext must not equal plaintext");

    expect(decrypt_aes256(encrypted, "correct horse battery staple", decrypted, 1000), "decrypt should succeed");
    expect(decrypted == plain, "decrypt returns original plaintext");

    bytes default_encrypted;
    bytes default_decrypted;
    expect(encrypt_aes256(plain, "default-iterations", default_encrypted),
           "default AES wrapper encrypts");
    expect(decrypt_aes256(default_encrypted, "default-iterations", default_decrypted),
           "default AES wrapper decrypts");
    expect(default_decrypted == plain, "default AES wrapper roundtrip matches plaintext");

    bytes empty_encrypted;
    expect(encrypt_aes256({}, "empty", empty_encrypted, 1000), "empty plaintext encrypts");
    expect(decrypt_aes256(empty_encrypted, "empty", decrypted, 1000), "empty plaintext decrypts");
    expect(decrypted.empty(), "empty plaintext roundtrip remains empty");

    decrypted = {0xaa, 0xbb};
    expect(!decrypt_aes256(encrypted, "wrong password", decrypted, 1000), "wrong password must fail");
    expect(decrypted.empty(), "wrong-password output must not expose unauthenticated plaintext");

    bytes tampered = encrypted;
    tampered[tampered.size() - 1] ^= 0x01;
    decrypted = {0xaa, 0xbb};
    expect(!decrypt_aes256(tampered, "correct horse battery staple", decrypted, 1000), "tag tamper must fail");
    expect(decrypted.empty(), "tampered-ciphertext output must be empty");

    bytes truncated(encrypted.begin(), encrypted.begin() + 8);
    expect(!decrypt_aes256(truncated, "correct horse battery staple", decrypted, 1000), "short payload must fail");

    bytes invalid_key;
    expect(!derive_key_pbkdf2("password", bytes{}, 0, invalid_key),
           "pbkdf2 rejects invalid iteration count");
    expect(invalid_key.empty(), "failed PBKDF2 must not retain key material");

    bytes invalid_encrypted;
    expect(!encrypt_aes256(plain, "invalid-iterations", invalid_encrypted, 0),
           "encrypt rejects invalid iteration count");
    expect(!decrypt_aes256(encrypted, "correct horse battery staple", decrypted, 0),
           "decrypt rejects invalid iteration count");

    bytes salt(16, 0x42);
    bytes key_a;
    bytes key_b;
    bytes key_c;
    expect(derive_key_pbkdf2("password", salt, 1000, key_a), "pbkdf2 derives key");
    expect(derive_key_pbkdf2("password", salt, 1000, key_b), "pbkdf2 repeat derives key");
    expect(derive_key_pbkdf2("password", salt, 1001, key_c), "pbkdf2 different iteration derives key");
    expect(key_a.size() == 32, "pbkdf2 key is 32 bytes");
    expect(key_a == key_b, "pbkdf2 is deterministic for same parameters");
    expect(key_a != key_c, "pbkdf2 changes when iteration count changes");
}

void test_address_codec_guards()
{
    using namespace geocoin;
    using namespace geocoin::crypto;

    expect(base58_encode(bytes{}).empty(), "empty Base58 encode returns empty string");
    expect(base58_decode("").empty(), "empty Base58 decode returns empty bytes");
    expect(base58_decode("0").empty(), "Base58 decode rejects invalid alphabet character");
    expect(!verify_address(Address("G")), "short address is rejected");
    expect(!verify_address(Address("GEOM1")), "short multisig address payload is rejected");

    const Address normal = address_from_key(make_key(77));
    expect(verify_address(normal), "wallet-generated normal address is canonical");
    const auto multisig = geocoin::wallet::create_multisig_address(1, 1, {normal});
    expect(verify_address(Address(multisig.address)),
           "wallet-generated multisig address is canonical");

    auto encoded_address = [](const char* prefix, geocoin::bytes payload) {
        const geocoin::Hash256 checksum = geocoin::crypto::double_sha256(payload);
        payload.insert(payload.end(), checksum.begin(), checksum.begin() + 4);
        return geocoin::Address(std::string(prefix) + geocoin::crypto::base58_encode(payload));
    };

    geocoin::bytes oversized_payload(22, 0x42);
    oversized_payload[0] = 0x00;
    expect(!verify_address(encoded_address("GEO", oversized_payload)),
           "address validator rejects checksummed non-canonical payload length");

    geocoin::bytes wrong_normal_version(21, 0x11);
    wrong_normal_version[0] = 0x05;
    expect(!verify_address(encoded_address("GEO", wrong_normal_version)),
           "normal prefix rejects multisig version byte");

    geocoin::bytes wrong_multisig_version(21, 0x22);
    wrong_multisig_version[0] = 0x00;
    expect(!verify_address(encoded_address("GEOM", wrong_multisig_version)),
           "multisig prefix rejects normal version byte");
}

void test_angle_store_indexes_and_removal()
{
    using namespace geocoin;
    using namespace geocoin::core;
    using namespace geocoin::wallet;

    AngleCommitmentStore store;
    const bytes key_a = make_key(31);
    const bytes key_b = make_key(32);
    const Address multisig_a("GEOMTESTANGLESTOREA");
    const Address multisig_b("GEOMTESTANGLESTOREB");
    const Hash256 spend_a = hash_from_seed(51);
    const Hash256 spend_b = hash_from_seed(91);

    Transaction ignored;
    ignored.type = TransactionType::STANDARD;
    store.add_commitment(ignored);
    expect(store.size() == 0, "non-commitment tx is ignored");

    Transaction commitment_a = Transaction::create_angle_commitment(multisig_a, spend_a, key_a, Address(""), "degree");
    Transaction commitment_b = Transaction::create_angle_commitment(multisig_a, spend_a, key_b, Address(""), "degree");
    Transaction commitment_c = Transaction::create_angle_commitment(multisig_b, spend_b, key_a, Address(""), "degree");
    const Hash256 hash_a = commitment_a.get_hash();
    const Hash256 hash_b = commitment_b.get_hash();
    const Hash256 hash_c = commitment_c.get_hash();

    store.add_commitment(commitment_a);
    store.add_commitment(commitment_a);
    store.add_commitment(commitment_b);
    store.add_commitment(commitment_c);
    expect(store.size() == 3, "duplicate commitment is ignored");
    expect(store.has_commitment(hash_a), "commitment hash is indexed");
    expect(store.get_by_hash(hash_b)->signer_address.str() == commitment_b.signer_address.str(), "get by hash works");
    expect(store.get_by_multisig_address(multisig_a).size() == 2, "multisig index returns two commitments");
    expect(store.get_by_multisig_address(Address("GEOMMISSING")).empty(), "missing multisig returns empty");
    expect(store.get_by_spending_tx(spend_a).size() == 2, "spending hash index returns two commitments");
    expect(store.get_by_spending_tx(hash_from_seed(1)).empty(), "missing spending hash returns empty");

    store.remove_commitment(hash_a);
    expect(!store.has_commitment(hash_a), "removed commitment is gone");
    expect(store.get_by_multisig_address(multisig_a).size() == 1, "multisig index updates after remove");
    expect(store.get_by_spending_tx(spend_a).size() == 1, "spending index updates after remove");
    store.remove_commitment(hash_a);
    expect(store.size() == 2, "removing absent commitment is no-op");

    store.remove_commitment(hash_b);
    expect(store.get_by_multisig_address(multisig_a).empty(), "empty multisig index bucket is erased");
    expect(store.get_by_spending_tx(spend_a).empty(), "empty spending index bucket is erased");

    store.clear();
    expect(store.size() == 0, "clear removes all commitments");
    expect(!store.has_commitment(hash_c), "clear removes remaining hash");
}

void test_multisig_service_errors_and_success()
{
    using namespace geocoin;
    using namespace geocoin::core;
    using namespace geocoin::wallet;

    const auto addresses = make_addresses(3);
    auto multisig = create_multisig_address(2, 3, addresses);
    MultisigConfig config = multisig.config;
    config.address = Address(multisig.address);
    config.participants = {
        MultisigParticipant(addresses[0]),
        MultisigParticipant(addresses[1]),
        MultisigParticipant(addresses[2]),
    };
    const Address recipient = address_from_key(make_key(77));

    MultisigDraftResult draft;
    std::string error;
    Amount arithmetic_result = 0;
    expect(!checked_add_amount((std::numeric_limits<Amount>::max)(), 1, arithmetic_result),
           "checked Amount addition rejects overflow");
    expect(!checked_sub_amount((std::numeric_limits<Amount>::min)(), 1, arithmetic_result),
           "checked Amount subtraction rejects underflow");
    expect(
        !create_multisig_draft(config, recipient, COIN, consensus::params::MIN_TX_FEE, nullptr, draft, error),
        "draft without chain state fails"
    );
    expect(error == "Chain state is not available", "chain state error is specific");

    auto chain_state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    chain_state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));

    MultisigConfig invalid = config;
    invalid.m = 0;
    expect(
        !create_multisig_draft(invalid, recipient, COIN, consensus::params::MIN_TX_FEE, chain_state, draft, error),
        "invalid config fails"
    );

    MultisigConfig missing_address = config;
    missing_address.address = Address("");
    expect(
        !create_multisig_draft(missing_address, recipient, COIN, consensus::params::MIN_TX_FEE, chain_state, draft, error),
        "missing multisig address fails"
    );

    expect(
        !create_multisig_draft(config, recipient, 0, consensus::params::MIN_TX_FEE, chain_state, draft, error),
        "zero amount fails"
    );

    error.clear();
    expect(
        !create_multisig_draft(
            config,
            recipient,
            (std::numeric_limits<Amount>::max)(),
            1,
            chain_state,
            draft,
            error
        ),
        "draft rejects amount plus fee overflow"
    );
    expect(error == "Amount plus fee is out of range", "draft reports amount plus fee overflow");

    expect(
        !create_multisig_draft(config, recipient, COIN, consensus::params::MIN_TX_FEE, chain_state, draft, error),
        "no utxo fails"
    );

    auto apply_coinbase_to_multisig = [&](Height height, Amount reward) -> Amount {
        Transaction coinbase = Transaction::create_coinbase(height, config.address, reward);
        const Amount miner_output = coinbase.outputs.empty() ? 0 : coinbase.outputs.front().value;
        expect(chain_state->apply_transaction(coinbase, height, true), "coinbase funding should apply");
        chain_state->set_height(height);
        return miner_output;
    };

    apply_coinbase_to_multisig(10, 10);
    expect(
        !create_multisig_draft(config, recipient, COIN, consensus::params::MIN_TX_FEE, chain_state, draft, error),
        "insufficient utxo fails"
    );

    const Amount enough_value = apply_coinbase_to_multisig(11, 3 * COIN);
    expect(!create_multisig_draft(config, recipient, COIN, 0, chain_state, draft, error),
           "sufficient but immature rewards cannot fund a draft");
    chain_state->set_height(110);

    expect(
        create_multisig_draft(config, recipient, COIN, 0, chain_state, draft, error),
        "draft with enough utxo succeeds"
    );
    expect(draft.transaction.type == TransactionType::MULTISIG_SPEND, "draft is multisig spend");
    expect(draft.transaction.inputs.size() == 1, "draft selects one sufficient mature UTXO");
    expect(draft.transaction.outputs.size() == 2, "draft has payment and change");
    expect(draft.transaction.spend_participants.size() == 3, "participants are copied to spend preimage");
    expect(draft.total_input == enough_value, "draft total input includes only the selected UTXO");
    expect(draft.fee_used >= consensus::params::MIN_TX_FEE,
           "default multisig package fee covers the root and commitments");
    expect(
        create_multisig_draft(config, recipient, COIN, 1, chain_state, draft, error),
        "draft clamps a positive sub-minimum fee"
    );
    expect(draft.fee_used >= consensus::params::MIN_TX_FEE,
           "sub-minimum multisig fee is raised to the package floor");

    CommitmentSigningIntent direct_intent;
    error.clear();
    expect(derive_commitment_signing_intent(
               draft.transaction, nullptr, direct_intent, error),
           "complete direct draft derives a signing intent");
    expect(direct_intent.kind == CommitmentIntentKind::DIRECT_SPEND &&
               direct_intent.signing_hash == draft.transaction.get_hash_without_commitments() &&
               direct_intent.signer_config.address.str() == config.address.str(),
           "direct intent derives hash and policy from transaction bytes");
    Transaction referenced_draft = draft.transaction;
    referenced_draft.angle_commitment_hashes.push_back(hash_from_seed(0x71));
    expect(!derive_commitment_signing_intent(
               referenced_draft, nullptr, direct_intent, error),
           "signer rejects a mutated draft that already carries commitments");

    auto overflow_state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    auto add_extreme_utxo = [&](Amount value, uint64_t timestamp) {
        Transaction funding;
        funding.version = 2;
        funding.type = TransactionType::STANDARD;
        funding.timestamp = timestamp;
        funding.outputs.push_back(output_to(config.address, value));
        state::ChainStateTestAccess::seed_funding(*overflow_state, funding, timestamp);
    };
    add_extreme_utxo((std::numeric_limits<Amount>::max)() - 5, 100);
    add_extreme_utxo(10, 101);

    error.clear();
    expect(
        !create_multisig_draft(
            config,
            recipient,
            (std::numeric_limits<Amount>::max)() - consensus::params::MIN_TX_FEE,
            consensus::params::MIN_TX_FEE,
            overflow_state,
            draft,
            error
        ),
        "draft rejects an amount whose package fee overflows the total"
    );
    expect(error == "Amount plus fee is out of range", "draft reports amount plus package-fee overflow");

    CommitmentResult commitment;
    expect(!sign_angle_commitment(config.address, draft.spending_tx_hash, bytes{1, 2, 3}, commitment, error),
           "short private key fails");
    expect(sign_angle_commitment(config.address, draft.spending_tx_hash, make_key(11), commitment, error),
           "valid commitment signs");
    expect(commitment.commitment_tx.type == TransactionType::ANGLE_COMMITMENT, "commitment tx type is correct");
    expect(commitment.commitment_hash == commitment.commitment_tx.get_hash(), "commitment hash matches tx hash");

    Transaction final_tx;
    expect(!finalize_multisig_spend(draft.transaction, {commitment.commitment_hash}, 2, final_tx, error),
           "not enough commitments fails");
    expect(finalize_multisig_spend(draft.transaction, {hash_from_seed(1), hash_from_seed(2)}, 2, final_tx, error),
           "enough commitments finalizes");
    expect(final_tx.angle_commitment_hashes.size() == 2, "commitments are attached");

    Transaction low_m = draft.transaction;
    low_m.m = 1;
    expect(!finalize_multisig_spend(
               low_m, {hash_from_seed(1), hash_from_seed(2)}, 2, final_tx, error),
           "finalize cannot rewrite the signed threshold");
    expect(!finalize_multisig_spend(
               draft.transaction, {hash_from_seed(1), hash_from_seed(1)}, 2, final_tx, error),
           "finalize rejects duplicate commitment references");
}

void test_reward_to_5_of_5_multisig_delegation_flow()
{
    using namespace geocoin;
    using namespace geocoin::core;
    using namespace geocoin::wallet;

    auto chain_state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    chain_state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));

    const auto primary_keys = make_keys(101, 5);
    const auto primary_addresses = addresses_from_keys(primary_keys);
    const MultisigConfig primary = make_config(5, 5, primary_addresses);

    const auto delegated_keys = make_keys(151, 3);
    const auto delegated_addresses = addresses_from_keys(delegated_keys);
    const MultisigConfig delegated = make_config(2, 3, delegated_addresses);

    const Height funding_height = 1;
    Transaction coinbase = Transaction::create_coinbase(funding_height, primary.address, 50 * COIN);
    expect(coinbase.outputs.size() == 2, "foundation reward coinbase has miner and foundation outputs");
    expect(coinbase.outputs[0].address.str() == primary.address.str(), "coinbase pays primary multisig first");
    const Amount primary_reward = coinbase.outputs[0].value;
    expect(primary_reward == 45 * COIN, "primary multisig receives miner share after foundation cut");
    expect(chain_state->apply_transaction(coinbase, funding_height, true), "reward coinbase should apply");
    chain_state->set_height(funding_height);
    expect(chain_state->get_balance(primary.address) == primary_reward, "primary multisig balance includes reward");

    const Height spend_height = funding_height + consensus::params::COINBASE_MATURITY;
    chain_state->set_height(spend_height - 1);

    const Address recipient = address_from_key(make_key(201));
    const Amount delegated_amount = 1 * COIN;
    const TxInput reward_input = input_from(coinbase.get_hash(), 0);

    Transaction direct_spend = Transaction::create_multisig_spend(
        {reward_input},
        {
            output_to(recipient, delegated_amount),
            output_to(primary.address, primary_reward - delegated_amount),
        },
        {},
        primary.m,
        primary.n
    );
    direct_spend.spend_participants = primary.addresses;
    direct_spend.angle_commitment_hashes = apply_angle_commitments(
        chain_state,
        primary.address,
        direct_spend.get_hash_without_commitments(),
        primary_keys,
        4,
        spend_height
    );
    expect(!chain_state->validate_transaction(direct_spend), "direct reward spend rejects 4 of 5 signatures");

    Transaction proof;
    proof.version = 2;
    proof.type = TransactionType::DELEGATION_PROOF;
    proof.timestamp = 1710000000;
    constexpr Amount proof_fee = 50'000;
    proof.inputs.push_back(reward_input);
    proof.outputs.push_back(output_to(primary.address, primary_reward - proof_fee));
    proof.primary_multisig_address = primary.address;
    proof.primary_m = primary.m;
    proof.primary_n = primary.n;
    proof.primary_participants = primary.addresses;
    proof.delegated_multisig_address = delegated.address;
    proof.delegated_m = delegated.m;
    proof.delegated_n = delegated.n;
    proof.delegated_participants = delegated.addresses;
    proof.valid_from = 0;
    proof.valid_until = 0;
    proof.nonce = 77;
    proof.max_total_delegated = 5 * COIN;
    proof.max_per_transaction = 2 * COIN;
    proof.max_percent_of_balance = 0;

    Transaction proof_with_four = proof;
    proof_with_four.angle_commitment_hashes = apply_angle_commitments(
        chain_state,
        primary.address,
        proof_with_four.get_delegation_proof_hash(),
        primary_keys,
        4,
        spend_height
    );
    expect(!chain_state->validate_transaction(proof_with_four), "delegation proof rejects 4 of 5 primary signatures");

    proof.angle_commitment_hashes = proof_with_four.angle_commitment_hashes;
    const auto fifth_proof_commitment = apply_angle_commitments(
        chain_state,
        primary.address,
        proof.get_delegation_proof_hash(),
        {primary_keys[4]},
        1,
        spend_height
    );
    proof.angle_commitment_hashes.insert(
        proof.angle_commitment_hashes.end(),
        fifth_proof_commitment.begin(),
        fifth_proof_commitment.end()
    );
    expect(chain_state->apply_transaction(proof, spend_height, false), "5-of-5 delegation proof should apply");

    constexpr Amount spend_fee = 10'000;
    const Amount proof_change = primary_reward - proof_fee;
    Transaction hierarchical_spend = Transaction::create_multisig_spend(
        {input_from(proof.get_hash(), 0)},
        {
            output_to(recipient, delegated_amount),
            output_to(primary.address, proof_change - delegated_amount - spend_fee),
        },
        {},
        delegated.m,
        delegated.n
    );
    hierarchical_spend.delegation_proof_txid = proof.get_hash();
    hierarchical_spend.angle_commitment_hashes = apply_angle_commitments(
        chain_state,
        delegated.address,
        hierarchical_spend.get_hierarchical_spend_hash(),
        delegated_keys,
        delegated.m,
        spend_height
    );

    expect(chain_state->apply_transaction(hierarchical_spend, spend_height, false),
           "delegated multisig should spend primary reward UTXO");
    expect(chain_state->get_balance(recipient) == delegated_amount, "recipient receives delegated spend amount");
    expect(chain_state->get_balance(primary.address) ==
               proof_change - delegated_amount - spend_fee,
           "primary multisig receives change");
    expect(chain_state->get_delegation_total_spent(proof.get_hash()) == delegated_amount + spend_fee,
           "delegation total spent includes the transfer and fee");
}

} // namespace

int main()
{
    try {
        test_encryption_roundtrips_and_rejects_tampering();
        test_address_codec_guards();
        test_angle_store_indexes_and_removal();
        test_multisig_service_errors_and_success();
        test_reward_to_5_of_5_multisig_delegation_flow();
        std::cout << "test_wallet_crypto_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_wallet_crypto_units failed: " << e.what() << "\n";
        return 1;
    }
}
