#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/types.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/wallet/angle_store.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/multisig_service.hpp"
#include "geocoin/wallet/multisig_verifier.hpp"
#include "wallet_state_fixture.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

geocoin::Address address_from_key(const geocoin::bytes& key)
{
    geocoin::Address address;
    expect(geocoin::crypto::compute_address_from_private_key(key, address), "failed to derive address");
    return address;
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 19);
    }
    return hash;
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

geocoin::core::TxOutput output_to(const geocoin::Address& address, geocoin::Amount amount)
{
    geocoin::core::TxOutput output;
    output.value = amount;
    output.address = address;
    return output;
}

geocoin::core::TxInput dummy_input(uint8_t seed)
{
    geocoin::core::TxInput input;
    input.prev_tx_hash = hash_from_seed(seed);
    input.vout = 0;
    return input;
}

geocoin::Hash256 store_commitment(
    const std::shared_ptr<geocoin::wallet::AngleCommitmentStore>& store,
    const geocoin::Address& multisig_address,
    const geocoin::Hash256& signed_hash,
    const geocoin::bytes& signer_key
)
{
    geocoin::core::Transaction commitment = geocoin::core::Transaction::create_angle_commitment(
        multisig_address,
        signed_hash,
        signer_key,
        geocoin::Address(""),
        "degree"
    );
    const geocoin::Hash256 hash = commitment.get_hash();
    store->add_commitment(commitment);
    return hash;
}

geocoin::Hash256 store_invalid_signature_commitment(
    const std::shared_ptr<geocoin::wallet::AngleCommitmentStore>& store,
    const geocoin::Address& multisig_address,
    const geocoin::Hash256& signed_hash,
    const geocoin::bytes& signer_key
)
{
    geocoin::core::Transaction commitment = geocoin::core::Transaction::create_angle_commitment(
        multisig_address,
        signed_hash,
        signer_key,
        geocoin::Address(""),
        "degree"
    );
    expect(!commitment.inputs.empty(), "commitment has input");
    expect(!commitment.inputs[0].signature.empty(), "commitment has signature");
    commitment.inputs[0].signature[0] ^= 0x01;
    const geocoin::Hash256 hash = commitment.get_hash();
    store->add_commitment(commitment);
    return hash;
}

void mutate_stored_type(
    const std::shared_ptr<geocoin::wallet::AngleCommitmentStore>& store,
    const geocoin::Hash256& hash,
    geocoin::core::TransactionType type
)
{
    auto stored = store->get_by_hash(hash);
    expect(stored != nullptr, "stored commitment is available for mutation");
    stored->type = type;
}

geocoin::core::Transaction make_plain_multisig_spend(
    const geocoin::wallet::MultisigConfig& config,
    const geocoin::Address& recipient
)
{
    geocoin::core::Transaction tx = geocoin::core::Transaction::create_multisig_spend(
        {dummy_input(0x11)},
        {output_to(recipient, 1000)},
        {},
        config.m,
        config.n
    );
    tx.spend_participants = config.addresses;
    return tx;
}

geocoin::core::Transaction make_delegation_proof(
    const geocoin::wallet::MultisigConfig& primary,
    const geocoin::wallet::MultisigConfig& delegated
)
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::DELEGATION_PROOF;
    tx.timestamp = 1710000000;
    tx.primary_multisig_address = primary.address;
    tx.primary_m = primary.m;
    tx.primary_n = primary.n;
    tx.primary_participants = primary.addresses;
    tx.delegated_multisig_address = delegated.address;
    tx.delegated_m = delegated.m;
    tx.delegated_n = delegated.n;
    tx.delegated_participants = delegated.addresses;
    tx.valid_from = 0;
    tx.valid_until = 0;
    tx.nonce = 7;
    tx.max_total_delegated = 100000;
    tx.max_per_transaction = 50000;
    tx.max_percent_of_balance = 50;
    return tx;
}

geocoin::core::Transaction with_primary_commitments(
    const std::shared_ptr<geocoin::wallet::AngleCommitmentStore>& store,
    geocoin::core::Transaction proof,
    const geocoin::wallet::MultisigConfig& primary,
    const std::vector<geocoin::bytes>& primary_keys
)
{
    const geocoin::Hash256 proof_hash = proof.get_delegation_proof_hash();
    for (uint8_t i = 0; i < primary.m; ++i) {
        proof.angle_commitment_hashes.push_back(
            store_commitment(store, primary.address, proof_hash, primary_keys[i])
        );
    }
    return proof;
}

geocoin::core::Transaction make_revoke(
    const std::shared_ptr<geocoin::wallet::AngleCommitmentStore>& store,
    const geocoin::core::Transaction& proof,
    const geocoin::wallet::MultisigConfig& primary,
    const std::vector<geocoin::bytes>& primary_keys
)
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
    tx.timestamp = 1710000100;
    tx.revoked_delegation_proof_txid = proof.get_hash();
    tx.inputs.push_back(dummy_input(0x29));
    const geocoin::Hash256 revoke_hash = tx.get_revoke_delegation_hash();
    for (uint8_t i = 0; i < primary.m; ++i) {
        tx.angle_commitment_hashes.push_back(
            store_commitment(store, primary.address, revoke_hash, primary_keys[i])
        );
    }
    return tx;
}

geocoin::core::Transaction make_hierarchical_spend(
    const std::shared_ptr<geocoin::wallet::AngleCommitmentStore>& store,
    const geocoin::core::Transaction& proof,
    const geocoin::wallet::MultisigConfig& delegated,
    const std::vector<geocoin::bytes>& delegated_keys,
    const geocoin::Address& recipient
)
{
    geocoin::core::Transaction tx = geocoin::core::Transaction::create_multisig_spend(
        {dummy_input(0x22)},
        {output_to(recipient, 2500)},
        {},
        delegated.m,
        delegated.n
    );
    tx.delegation_proof_txid = proof.get_hash();
    const geocoin::Hash256 spend_hash = tx.get_hierarchical_spend_hash();
    for (uint8_t i = 0; i < delegated.m; ++i) {
        tx.angle_commitment_hashes.push_back(
            store_commitment(store, delegated.address, spend_hash, delegated_keys[i])
        );
    }
    return tx;
}

void test_angle_commitment_validation()
{
    auto store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
    geocoin::wallet::MultisigVerifier verifier(store);
    const auto key = make_key(3);
    const auto signer = address_from_key(key);
    const auto config = make_config(1, 1, {signer});
    const geocoin::Hash256 spend_hash = hash_from_seed(0x33);

    geocoin::core::Transaction commitment = geocoin::core::Transaction::create_angle_commitment(
        config.address,
        spend_hash,
        key,
        geocoin::Address(""),
        "degree"
    );
    expect(verifier.verify_angle_commitment(commitment), "valid angle commitment verifies");
    expect(commitment.verify(), "transaction-level commitment signature verifies");

    geocoin::consensus::ValidationState mainnet_state;
    expect(geocoin::consensus::check_transaction(commitment, mainnet_state),
           "canonical mainnet commitment passes context-free consensus checks");
    geocoin::consensus::SelectParams(geocoin::consensus::Network::TESTNET);
    geocoin::consensus::ValidationState cross_network_state;
    expect(!geocoin::consensus::check_transaction(commitment, cross_network_state),
           "mainnet commitment is rejected on testnet");
    expect(cross_network_state.get_reject_reason() == "bad-angle-commitment-network",
           "cross-network commitment has a stable reject reason");
    geocoin::consensus::SelectParams(geocoin::consensus::Network::MAIN);

    geocoin::core::Transaction wrong_type = commitment;
    wrong_type.type = geocoin::core::TransactionType::STANDARD;
    expect(!verifier.verify_angle_commitment(wrong_type), "wrong commitment type rejects");

    geocoin::core::Transaction no_input = commitment;
    no_input.inputs.clear();
    expect(!verifier.verify_angle_commitment(no_input), "commitment without one input rejects");

    geocoin::core::Transaction wrong_vout = commitment;
    wrong_vout.inputs[0].vout = 0;
    expect(!verifier.verify_angle_commitment(wrong_vout), "commitment with wrong marker rejects");

    geocoin::core::Transaction wrong_signer = commitment;
    wrong_signer.signer_address = address_from_key(make_key(4));
    expect(!verifier.verify_angle_commitment(wrong_signer), "commitment signer mismatch rejects");

    geocoin::core::Transaction wrong_network = commitment;
    wrong_network.commitment_network_id = "degree-test";
    expect(!verifier.verify_angle_commitment(wrong_network), "commitment network mutation rejects");

    geocoin::core::Transaction wrong_multisig = commitment;
    wrong_multisig.multisig_address = make_config(1, 1, {address_from_key(make_key(5))}).address;
    expect(!verifier.verify_angle_commitment(wrong_multisig), "commitment multisig mutation rejects");

    geocoin::core::Transaction wrong_spend = commitment;
    wrong_spend.spending_tx_hash[0] ^= 0x01;
    expect(!verifier.verify_angle_commitment(wrong_spend), "commitment spending hash mutation rejects");

    geocoin::core::Transaction wrong_time = commitment;
    wrong_time.timestamp = 1;
    expect(!verifier.verify_angle_commitment(wrong_time), "commitment timestamp mutation rejects");

    geocoin::core::Transaction wrong_payload = commitment;
    wrong_payload.card_status = 1;
    expect(!verifier.verify_angle_commitment(wrong_payload), "commitment with inactive payload rejects");

    geocoin::core::Transaction wrong_prev = commitment;
    wrong_prev.inputs[0].prev_tx_hash = hash_from_seed(0x44);
    expect(!verifier.verify_angle_commitment(wrong_prev), "commitment uniqueness hash mismatch rejects");

    geocoin::core::Transaction bad_sig = commitment;
    bad_sig.inputs[0].signature[0] ^= 0x01;
    expect(!verifier.verify_angle_commitment(bad_sig), "commitment with tampered signature rejects");
}

void test_plain_multisig_spend_validation()
{
    auto store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
    geocoin::wallet::MultisigVerifier verifier(store);
    const auto keys = make_keys(10, 4);
    const auto addresses = addresses_from_keys(keys);
    const auto config = make_config(2, 3, {addresses[0], addresses[1], addresses[2]});
    const geocoin::Address recipient = addresses[3];

    geocoin::core::Transaction tx = make_plain_multisig_spend(config, recipient);
    const geocoin::Hash256 spend_hash = tx.get_hash_without_commitments();
    tx.angle_commitment_hashes.push_back(store_commitment(store, config.address, spend_hash, keys[0]));
    tx.angle_commitment_hashes.push_back(store_commitment(store, config.address, spend_hash, keys[1]));

    expect(verifier.verify_multisig_spend(tx, config), "valid configured multisig spend verifies");
    expect(verifier.verify_multisig_spend(tx), "valid trustless multisig spend verifies");

    geocoin::core::Transaction wrong_type = tx;
    wrong_type.type = geocoin::core::TransactionType::STANDARD;
    expect(!verifier.verify_multisig_spend(wrong_type, config), "wrong multisig spend type rejects");

    geocoin::core::Transaction wrong_mn = tx;
    wrong_mn.m = 1;
    expect(!verifier.verify_multisig_spend(wrong_mn, config), "m/n mismatch rejects");

    geocoin::core::Transaction too_few = tx;
    too_few.angle_commitment_hashes.pop_back();
    expect(!verifier.verify_multisig_spend(too_few, config), "too few commitments rejects");

    geocoin::core::Transaction missing = tx;
    missing.angle_commitment_hashes[0] = hash_from_seed(0x55);
    expect(!verifier.verify_multisig_spend(missing, config), "missing commitment rejects");

    geocoin::core::Transaction wrong_commitment_type = make_plain_multisig_spend(config, recipient);
    const geocoin::Hash256 wrong_type_hash = wrong_commitment_type.get_hash_without_commitments();
    wrong_commitment_type.angle_commitment_hashes.push_back(
        store_commitment(store, config.address, wrong_type_hash, keys[0]));
    wrong_commitment_type.angle_commitment_hashes.push_back(
        store_commitment(store, config.address, wrong_type_hash, keys[1]));
    mutate_stored_type(store, wrong_commitment_type.angle_commitment_hashes[0],
                       geocoin::core::TransactionType::STANDARD);
    expect(!verifier.verify_multisig_spend(wrong_commitment_type, config),
           "stored commitment with wrong type rejects configured spend");

    geocoin::core::Transaction duplicate = tx;
    duplicate.angle_commitment_hashes[1] = duplicate.angle_commitment_hashes[0];
    expect(!verifier.verify_multisig_spend(duplicate, config), "duplicate signer rejects");

    geocoin::core::Transaction trustless_missing_first = tx;
    trustless_missing_first.angle_commitment_hashes[0] = hash_from_seed(0x56);
    expect(!verifier.verify_multisig_spend(trustless_missing_first),
           "trustless spend with missing first commitment rejects");

    geocoin::core::Transaction trustless_hierarchical = tx;
    trustless_hierarchical.delegation_proof_txid = hash_from_seed(0x57);
    expect(!verifier.verify_multisig_spend(trustless_hierarchical),
           "trustless plain spend rejects hierarchical delegation marker");

    geocoin::core::Transaction no_participants = tx;
    no_participants.spend_participants.clear();
    expect(!verifier.verify_multisig_spend(no_participants), "trustless spend without participants rejects");

    geocoin::core::Transaction duplicate_participants = tx;
    duplicate_participants.spend_participants[2] = duplicate_participants.spend_participants[1];
    expect(!verifier.verify_multisig_spend(duplicate_participants), "duplicate spend participants reject");

    geocoin::core::Transaction bad_preimage = tx;
    bad_preimage.spend_participants[2] = recipient;
    expect(!verifier.verify_multisig_spend(bad_preimage), "wrong multisig preimage rejects");

    geocoin::core::Transaction outsider = make_plain_multisig_spend(config, recipient);
    const geocoin::Hash256 outsider_hash = outsider.get_hash_without_commitments();
    outsider.angle_commitment_hashes.push_back(store_commitment(store, config.address, outsider_hash, keys[0]));
    outsider.angle_commitment_hashes.push_back(store_commitment(store, config.address, outsider_hash, keys[3]));
    expect(!verifier.verify_multisig_spend(outsider, config), "non-participant signer rejects");

    geocoin::core::Transaction wrong_multisig = make_plain_multisig_spend(config, recipient);
    const geocoin::Hash256 wrong_multisig_hash = wrong_multisig.get_hash_without_commitments();
    const auto other_config = make_config(1, 1, {recipient});
    wrong_multisig.angle_commitment_hashes.push_back(store_commitment(store, config.address, wrong_multisig_hash, keys[0]));
    wrong_multisig.angle_commitment_hashes.push_back(store_commitment(store, other_config.address, wrong_multisig_hash, keys[1]));
    expect(!verifier.verify_multisig_spend(wrong_multisig, config), "commitment for wrong multisig rejects");

    geocoin::core::Transaction wrong_hash = make_plain_multisig_spend(config, recipient);
    const geocoin::Hash256 right_hash = wrong_hash.get_hash_without_commitments();
    wrong_hash.angle_commitment_hashes.push_back(store_commitment(store, config.address, right_hash, keys[0]));
    wrong_hash.angle_commitment_hashes.push_back(store_commitment(store, config.address, hash_from_seed(0x77), keys[1]));
    expect(!verifier.verify_multisig_spend(wrong_hash, config), "commitment for wrong spending hash rejects");

    geocoin::core::Transaction bad_sig = make_plain_multisig_spend(config, recipient);
    const geocoin::Hash256 bad_sig_hash = bad_sig.get_hash_without_commitments();
    bad_sig.angle_commitment_hashes.push_back(store_commitment(store, config.address, bad_sig_hash, keys[0]));
    bad_sig.angle_commitment_hashes.push_back(store_invalid_signature_commitment(store, config.address, bad_sig_hash, keys[1]));
    expect(!verifier.verify_multisig_spend(bad_sig, config), "invalid commitment signature rejects spend");
}

void test_delegation_revoke_and_hierarchical_validation()
{
    auto store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
    geocoin::wallet::MultisigVerifier verifier(store);
    const auto primary_keys = make_keys(40, 4);
    const auto delegated_keys = make_keys(90, 3);
    const auto primary_addresses = addresses_from_keys(primary_keys);
    const auto delegated_addresses = addresses_from_keys(delegated_keys);
    const auto primary = make_config(2, 3, {primary_addresses[0], primary_addresses[1], primary_addresses[2]});
    const auto delegated = make_config(2, 2, {delegated_addresses[0], delegated_addresses[1]});
    const geocoin::Address recipient = primary_addresses[3];

    geocoin::core::Transaction proof = with_primary_commitments(
        store,
        make_delegation_proof(primary, delegated),
        primary,
        primary_keys
    );
    expect(verifier.verify_delegation_proof(proof), "valid delegation proof verifies");

    geocoin::core::Transaction wrong_type = proof;
    wrong_type.type = geocoin::core::TransactionType::STANDARD;
    expect(!verifier.verify_delegation_proof(wrong_type), "wrong delegation proof type rejects");

    geocoin::core::Transaction no_commitments = proof;
    no_commitments.angle_commitment_hashes.clear();
    expect(!verifier.verify_delegation_proof(no_commitments), "delegation proof without commitments rejects");

    geocoin::core::Transaction bad_primary_m = proof;
    bad_primary_m.primary_m = 0;
    expect(!verifier.verify_delegation_proof(bad_primary_m), "invalid primary m/n rejects");

    geocoin::core::Transaction bad_primary_count = proof;
    bad_primary_count.primary_participants.pop_back();
    expect(!verifier.verify_delegation_proof(bad_primary_count), "bad primary participant count rejects");

    geocoin::core::Transaction duplicate_primary = proof;
    duplicate_primary.primary_participants[1] = duplicate_primary.primary_participants[0];
    expect(!verifier.verify_delegation_proof(duplicate_primary), "duplicate primary participant rejects");

    geocoin::core::Transaction bad_primary_addr = proof;
    bad_primary_addr.primary_participants[2] = recipient;
    expect(!verifier.verify_delegation_proof(bad_primary_addr), "primary address preimage mismatch rejects");

    geocoin::core::Transaction bad_delegated_m = proof;
    bad_delegated_m.delegated_m = 0;
    expect(!verifier.verify_delegation_proof(bad_delegated_m), "invalid delegated m/n rejects");

    geocoin::core::Transaction bad_delegated_count = proof;
    bad_delegated_count.delegated_participants.pop_back();
    expect(!verifier.verify_delegation_proof(bad_delegated_count), "bad delegated participant count rejects");

    geocoin::core::Transaction duplicate_delegated = proof;
    duplicate_delegated.delegated_participants[1] = duplicate_delegated.delegated_participants[0];
    expect(!verifier.verify_delegation_proof(duplicate_delegated), "duplicate delegated participant rejects");

    geocoin::core::Transaction bad_delegated_addr = proof;
    bad_delegated_addr.delegated_participants[1] = delegated_addresses[2];
    expect(!verifier.verify_delegation_proof(bad_delegated_addr), "delegated address preimage mismatch rejects");

    geocoin::core::Transaction missing_commitment = proof;
    missing_commitment.angle_commitment_hashes[0] = hash_from_seed(0x88);
    expect(!verifier.verify_delegation_proof(missing_commitment), "missing delegation commitment rejects");

    geocoin::core::Transaction duplicate_signer = proof;
    duplicate_signer.angle_commitment_hashes[1] = duplicate_signer.angle_commitment_hashes[0];
    expect(!verifier.verify_delegation_proof(duplicate_signer), "duplicate delegation signer rejects");

    geocoin::core::Transaction one_commitment = proof;
    one_commitment.angle_commitment_hashes.pop_back();
    expect(!verifier.verify_delegation_proof(one_commitment), "too few delegation commitments reject");

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction variant = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        mutate_stored_type(case_store, variant.angle_commitment_hashes[0],
                           geocoin::core::TransactionType::STANDARD);
        expect(!case_verifier.verify_delegation_proof(variant),
               "delegation commitment with wrong type rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction variant = make_delegation_proof(primary, delegated);
        const geocoin::Hash256 delegation_hash = variant.get_delegation_proof_hash();
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, delegated.address, delegation_hash, primary_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, delegation_hash, primary_keys[1]));
        expect(!case_verifier.verify_delegation_proof(variant),
               "delegation commitment for wrong multisig rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction variant = make_delegation_proof(primary, delegated);
        const geocoin::Hash256 delegation_hash = variant.get_delegation_proof_hash();
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, hash_from_seed(0x89), primary_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, delegation_hash, primary_keys[1]));
        expect(!case_verifier.verify_delegation_proof(variant),
               "delegation commitment for wrong signed hash rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction variant = make_delegation_proof(primary, delegated);
        const geocoin::Hash256 delegation_hash = variant.get_delegation_proof_hash();
        variant.angle_commitment_hashes.push_back(
            store_invalid_signature_commitment(case_store, primary.address, delegation_hash, primary_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, delegation_hash, primary_keys[1]));
        expect(!case_verifier.verify_delegation_proof(variant),
               "delegation commitment with invalid signature rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction variant = make_delegation_proof(primary, delegated);
        const geocoin::Hash256 delegation_hash = variant.get_delegation_proof_hash();
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, delegation_hash, delegated_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, delegation_hash, primary_keys[1]));
        expect(!case_verifier.verify_delegation_proof(variant),
               "delegation commitment from non-primary participant rejects");
    }

    geocoin::core::Transaction revoke = make_revoke(store, proof, primary, primary_keys);
    expect(verifier.verify_revoke_delegation(revoke, proof), "valid revoke delegation verifies");

    geocoin::core::Transaction revoke_wrong_type = revoke;
    revoke_wrong_type.type = geocoin::core::TransactionType::STANDARD;
    expect(!verifier.verify_revoke_delegation(revoke_wrong_type, proof), "wrong revoke type rejects");

    geocoin::core::Transaction revoked_not_delegation = proof;
    revoked_not_delegation.type = geocoin::core::TransactionType::STANDARD;
    expect(!verifier.verify_revoke_delegation(revoke, revoked_not_delegation),
           "revoke rejects non-delegation revoked transaction");

    geocoin::core::Transaction invalid_revoked = proof;
    invalid_revoked.primary_m = 0;
    expect(!verifier.verify_revoke_delegation(revoke, invalid_revoked), "invalid revoked proof rejects revoke");

    geocoin::core::Transaction revoke_no_commitments = revoke;
    revoke_no_commitments.angle_commitment_hashes.clear();
    expect(!verifier.verify_revoke_delegation(revoke_no_commitments, proof),
           "revoke without commitments rejects");

    geocoin::core::Transaction revoke_too_few = revoke;
    revoke_too_few.angle_commitment_hashes.pop_back();
    expect(!verifier.verify_revoke_delegation(revoke_too_few, proof), "too few revoke commitments reject");

    geocoin::core::Transaction revoke_missing = revoke;
    revoke_missing.angle_commitment_hashes[0] = hash_from_seed(0x8a);
    expect(!verifier.verify_revoke_delegation(revoke_missing, proof), "missing revoke commitment rejects");

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant = make_revoke(case_store, case_proof, primary, primary_keys);
        mutate_stored_type(case_store, variant.angle_commitment_hashes[0],
                           geocoin::core::TransactionType::STANDARD);
        expect(!case_verifier.verify_revoke_delegation(variant, case_proof),
               "revoke commitment with wrong type rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant;
        variant.version = 2;
        variant.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
        variant.revoked_delegation_proof_txid = case_proof.get_hash();
        variant.inputs.push_back(dummy_input(0x2a));
        const auto revoke_hash = variant.get_revoke_delegation_hash();
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, delegated.address, revoke_hash, primary_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, revoke_hash, primary_keys[1]));
        expect(!case_verifier.verify_revoke_delegation(variant, case_proof),
               "revoke commitment for wrong multisig rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant;
        variant.version = 2;
        variant.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
        variant.revoked_delegation_proof_txid = case_proof.get_hash();
        variant.inputs.push_back(dummy_input(0x2b));
        const auto revoke_hash = variant.get_revoke_delegation_hash();
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, hash_from_seed(0x8b), primary_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, revoke_hash, primary_keys[1]));
        expect(!case_verifier.verify_revoke_delegation(variant, case_proof),
               "revoke commitment for wrong target rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant;
        variant.version = 2;
        variant.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
        variant.revoked_delegation_proof_txid = case_proof.get_hash();
        variant.inputs.push_back(dummy_input(0x2c));
        const auto revoke_hash = variant.get_revoke_delegation_hash();
        variant.angle_commitment_hashes.push_back(
            store_invalid_signature_commitment(case_store, primary.address, revoke_hash, primary_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, revoke_hash, primary_keys[1]));
        expect(!case_verifier.verify_revoke_delegation(variant, case_proof),
               "revoke commitment with invalid signature rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant;
        variant.version = 2;
        variant.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
        variant.revoked_delegation_proof_txid = case_proof.get_hash();
        variant.inputs.push_back(dummy_input(0x2d));
        const auto revoke_hash = variant.get_revoke_delegation_hash();
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, revoke_hash, delegated_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, revoke_hash, primary_keys[1]));
        expect(!case_verifier.verify_revoke_delegation(variant, case_proof),
               "revoke commitment from non-primary participant rejects");
    }

    geocoin::core::Transaction revoke_duplicate = revoke;
    revoke_duplicate.angle_commitment_hashes[1] = revoke_duplicate.angle_commitment_hashes[0];
    expect(!verifier.verify_revoke_delegation(revoke_duplicate, proof), "duplicate revoke signer rejects");

    geocoin::core::Transaction hierarchical = make_hierarchical_spend(store, proof, delegated, delegated_keys, recipient);
    expect(verifier.verify_hierarchical_spend(hierarchical, proof), "valid hierarchical spend verifies");

    geocoin::core::Transaction hierarchical_wrong_type = hierarchical;
    hierarchical_wrong_type.type = geocoin::core::TransactionType::STANDARD;
    expect(!verifier.verify_hierarchical_spend(hierarchical_wrong_type, proof), "wrong hierarchical spend type rejects");

    geocoin::core::Transaction hierarchical_no_proof = hierarchical;
    hierarchical_no_proof.delegation_proof_txid = geocoin::Hash256{};
    expect(!verifier.verify_hierarchical_spend(hierarchical_no_proof, proof), "hierarchical spend without proof txid rejects");

    geocoin::core::Transaction invalid_hierarchical_proof = proof;
    invalid_hierarchical_proof.delegated_m = 0;
    expect(!verifier.verify_hierarchical_spend(hierarchical, invalid_hierarchical_proof),
           "hierarchical spend rejects invalid delegation proof");

    geocoin::core::Transaction hierarchical_bad_mn = hierarchical;
    hierarchical_bad_mn.m = 1;
    expect(!verifier.verify_hierarchical_spend(hierarchical_bad_mn, proof), "hierarchical m/n mismatch rejects");

    geocoin::core::Transaction hierarchical_too_few = hierarchical;
    hierarchical_too_few.angle_commitment_hashes.pop_back();
    expect(!verifier.verify_hierarchical_spend(hierarchical_too_few, proof), "hierarchical too few commitments reject");

    geocoin::core::Transaction hierarchical_missing = hierarchical;
    hierarchical_missing.angle_commitment_hashes[0] = hash_from_seed(0x99);
    expect(!verifier.verify_hierarchical_spend(hierarchical_missing, proof), "hierarchical missing commitment rejects");

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant =
            make_hierarchical_spend(case_store, case_proof, delegated, delegated_keys, recipient);
        mutate_stored_type(case_store, variant.angle_commitment_hashes[0],
                           geocoin::core::TransactionType::STANDARD);
        expect(!case_verifier.verify_hierarchical_spend(variant, case_proof),
               "hierarchical commitment with wrong type rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant = geocoin::core::Transaction::create_multisig_spend(
            {dummy_input(0x23)},
            {output_to(recipient, 2500)},
            {},
            delegated.m,
            delegated.n
        );
        variant.delegation_proof_txid = case_proof.get_hash();
        const geocoin::Hash256 spend_hash = variant.get_hierarchical_spend_hash();
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, primary.address, spend_hash, delegated_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, delegated.address, spend_hash, delegated_keys[1]));
        expect(!case_verifier.verify_hierarchical_spend(variant, case_proof),
               "hierarchical commitment for wrong multisig rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant = geocoin::core::Transaction::create_multisig_spend(
            {dummy_input(0x24)},
            {output_to(recipient, 2500)},
            {},
            delegated.m,
            delegated.n
        );
        variant.delegation_proof_txid = case_proof.get_hash();
        const geocoin::Hash256 spend_hash = variant.get_hierarchical_spend_hash();
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, delegated.address, hash_from_seed(0x9a), delegated_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, delegated.address, spend_hash, delegated_keys[1]));
        expect(!case_verifier.verify_hierarchical_spend(variant, case_proof),
               "hierarchical commitment for wrong spend hash rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant = geocoin::core::Transaction::create_multisig_spend(
            {dummy_input(0x25)},
            {output_to(recipient, 2500)},
            {},
            delegated.m,
            delegated.n
        );
        variant.delegation_proof_txid = case_proof.get_hash();
        const geocoin::Hash256 spend_hash = variant.get_hierarchical_spend_hash();
        variant.angle_commitment_hashes.push_back(
            store_invalid_signature_commitment(case_store, delegated.address, spend_hash, delegated_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, delegated.address, spend_hash, delegated_keys[1]));
        expect(!case_verifier.verify_hierarchical_spend(variant, case_proof),
               "hierarchical commitment with invalid signature rejects");
    }

    {
        auto case_store = std::make_shared<geocoin::wallet::AngleCommitmentStore>();
        geocoin::wallet::MultisigVerifier case_verifier(case_store);
        geocoin::core::Transaction case_proof = with_primary_commitments(
            case_store, make_delegation_proof(primary, delegated), primary, primary_keys);
        geocoin::core::Transaction variant = geocoin::core::Transaction::create_multisig_spend(
            {dummy_input(0x26)},
            {output_to(recipient, 2500)},
            {},
            delegated.m,
            delegated.n
        );
        variant.delegation_proof_txid = case_proof.get_hash();
        const geocoin::Hash256 spend_hash = variant.get_hierarchical_spend_hash();
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, delegated.address, spend_hash, primary_keys[0]));
        variant.angle_commitment_hashes.push_back(
            store_commitment(case_store, delegated.address, spend_hash, delegated_keys[1]));
        expect(!case_verifier.verify_hierarchical_spend(variant, case_proof),
               "hierarchical commitment from non-delegated participant rejects");
    }

    geocoin::core::Transaction hierarchical_duplicate = hierarchical;
    hierarchical_duplicate.angle_commitment_hashes[1] = hierarchical_duplicate.angle_commitment_hashes[0];
    expect(!verifier.verify_hierarchical_spend(hierarchical_duplicate, proof), "hierarchical duplicate signer rejects");
}

void test_multisig_workflow_builders()
{
    const auto primary_keys = make_keys(0x31, 3);
    const auto delegated_keys = make_keys(0x61, 3);
    const auto primary = make_config(2, 3, addresses_from_keys(primary_keys));
    const auto delegated = make_config(2, 3, addresses_from_keys(delegated_keys));
    const auto recipient = address_from_key(make_key(0x91));

    geocoin::wallet::DelegationProofRequest proof_request;
    proof_request.primary_config = primary;
    proof_request.delegated_config = delegated;
    proof_request.nonce = 17;
    proof_request.max_total = 50000;
    proof_request.max_per_transaction = 10000;
    proof_request.max_percent_of_balance = 50;
    proof_request.timestamp = 1710000000;

    auto chain_state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    geocoin::core::Transaction funding;
    funding.outputs.push_back(output_to(primary.address, 100000));
    geocoin::state::ChainStateTestAccess::seed_funding(*chain_state, funding, 1);
    chain_state->set_height(100);

    geocoin::wallet::DelegationProofResult proof_draft;
    std::string error;
    expect(geocoin::wallet::create_delegation_proof(
               proof_request, chain_state, proof_draft, error),
           "delegation workflow builds canonical draft");
    expect(!proof_draft.transaction.inputs.empty() &&
               proof_draft.fee_used >= geocoin::consensus::params::MIN_TX_FEE,
           "delegation draft pays a small fee from primary multisig funds");
    expect(proof_draft.transaction.primary_participants == primary.addresses,
           "delegation draft carries primary participant preimage");
    expect(proof_draft.delegation_hash ==
               proof_draft.transaction.get_delegation_proof_hash(),
           "delegation signing hash matches serialized draft");

    geocoin::wallet::CommitmentSigningIntent proof_intent;
    expect(geocoin::wallet::derive_commitment_signing_intent(
               proof_draft.transaction, nullptr, proof_intent, error),
           "signing inspection derives a delegation intent from the full draft");
    expect(proof_intent.kind ==
               geocoin::wallet::CommitmentIntentKind::DELEGATION_PROOF &&
               proof_intent.signing_hash == proof_draft.delegation_hash &&
               proof_intent.signer_config.address == primary.address,
           "delegation intent binds the canonical hash to the primary policy");

    geocoin::wallet::CommitmentResult configured_commitment;
    expect(geocoin::wallet::sign_configured_angle_commitment(
               primary,
               proof_draft.delegation_hash,
               primary_keys.front(),
               configured_commitment,
               error),
           "configured commitment accepts a policy participant");
    expect(configured_commitment.signer_address == primary.addresses.front(),
           "configured commitment reports the derived signer");
    expect(!geocoin::wallet::sign_configured_angle_commitment(
               primary,
               proof_draft.delegation_hash,
               make_key(0xf1),
               configured_commitment,
               error),
           "configured commitment rejects a key outside the policy");

    const geocoin::Hash256 proof_commitment_a = hash_from_seed(0xa1);
    const geocoin::Hash256 proof_commitment_b = hash_from_seed(0xb1);
    geocoin::core::Transaction finalized_proof;
    expect(!geocoin::wallet::finalize_delegation_proof(
               proof_draft.transaction,
               {proof_commitment_a, proof_commitment_a},
               finalized_proof,
               error),
           "delegation workflow rejects duplicate commitments");
    expect(geocoin::wallet::finalize_delegation_proof(
               proof_draft.transaction,
               {proof_commitment_a, proof_commitment_b},
               finalized_proof,
               error),
           "delegation workflow accepts a unique quorum");

    expect(geocoin::state::ChainStateTestAccess::store_delegation(*chain_state, finalized_proof, 101),
           "workflow fixture stores confirmed delegation proof");
    chain_state->set_height(101);

    geocoin::wallet::HierarchicalSpendRequest spend_request;
    spend_request.delegation_proof_txid = finalized_proof.get_hash();
    spend_request.delegated_config = delegated;
    spend_request.recipient = recipient;
    spend_request.amount = 2000;
    spend_request.fee = geocoin::consensus::params::MIN_TX_FEE;
    spend_request.timestamp = 1710000100;

    geocoin::wallet::HierarchicalSpendDraftResult spend_draft;
    expect(geocoin::wallet::create_hierarchical_spend_draft(
               spend_request, chain_state, spend_draft, error),
           "hierarchical workflow builds a complete draft");
    expect(spend_draft.change > 0 && spend_draft.transaction.outputs.size() == 2,
           "hierarchical draft includes change before signing");
    expect(spend_draft.spending_hash ==
               spend_draft.transaction.get_hierarchical_spend_hash(),
           "hierarchical signing hash covers the final output set");

    geocoin::wallet::CommitmentSigningIntent spend_intent;
    expect(geocoin::wallet::derive_commitment_signing_intent(
               spend_draft.transaction, &finalized_proof, spend_intent, error),
           "signing inspection derives a hierarchical intent with its confirmed proof");
    expect(spend_intent.kind ==
               geocoin::wallet::CommitmentIntentKind::HIERARCHICAL_SPEND &&
               spend_intent.signing_hash == spend_draft.spending_hash &&
               spend_intent.signer_config.address == delegated.address,
           "hierarchical intent binds the canonical hash to the delegated policy");
    expect(!geocoin::wallet::derive_commitment_signing_intent(
               spend_draft.transaction, nullptr, spend_intent, error),
           "hierarchical intent cannot be derived without the referenced proof");
    geocoin::core::Transaction unrelated_proof = finalized_proof;
    ++unrelated_proof.nonce;
    expect(!geocoin::wallet::derive_commitment_signing_intent(
               spend_draft.transaction, &unrelated_proof, spend_intent, error),
           "hierarchical intent rejects a proof with a different transaction id");

    geocoin::core::Transaction revoke_draft;
    revoke_draft.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
    revoke_draft.timestamp = 1710000200;
    revoke_draft.revoked_delegation_proof_txid = finalized_proof.get_hash();
    revoke_draft.inputs.push_back(dummy_input(0xe1));
    geocoin::wallet::CommitmentSigningIntent revoke_intent;
    expect(geocoin::wallet::derive_commitment_signing_intent(
               revoke_draft, &finalized_proof, revoke_intent, error),
           "signing inspection derives a revocation intent with its confirmed proof");
    expect(revoke_intent.kind ==
               geocoin::wallet::CommitmentIntentKind::REVOKE_DELEGATION &&
               revoke_intent.signing_hash == revoke_draft.get_revoke_delegation_hash() &&
               revoke_intent.signer_config.address == primary.address,
           "revocation intent binds the full draft to the primary policy");
    revoke_draft.angle_commitment_hashes.push_back(hash_from_seed(0xf1));
    expect(!geocoin::wallet::derive_commitment_signing_intent(
               revoke_draft, &finalized_proof, revoke_intent, error),
           "signing inspection rejects a draft that already contains commitments");

    const geocoin::Hash256 hash_before_finalize =
        spend_draft.transaction.get_hierarchical_spend_hash();
    geocoin::core::Transaction finalized_spend;
    expect(geocoin::wallet::finalize_hierarchical_spend(
               spend_draft.transaction,
               {hash_from_seed(0xc1), hash_from_seed(0xd1)},
               chain_state,
               finalized_spend,
               error),
           "hierarchical workflow accepts a unique delegated quorum");
    expect(finalized_spend.get_hierarchical_spend_hash() == hash_before_finalize,
           "adding commitments cannot change the hierarchical signing hash");

    geocoin::core::Transaction changed_output = finalized_spend;
    ++changed_output.outputs.back().value;
    expect(changed_output.get_hierarchical_spend_hash() != hash_before_finalize,
           "changing the change output invalidates the hierarchical signing hash");
    expect(!geocoin::wallet::finalize_hierarchical_spend(
               spend_draft.transaction,
               {hash_from_seed(0xc1), hash_from_seed(0xc1)},
               chain_state,
               finalized_spend,
               error),
           "hierarchical workflow rejects duplicate commitments");
}

} // namespace

int main()
{
    try {
        test_angle_commitment_validation();
        test_plain_multisig_spend_validation();
        test_delegation_revoke_and_hierarchical_validation();
        test_multisig_workflow_builders();
        std::cout << "test_multisig_verifier_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_multisig_verifier_units failed: " << e.what() << "\n";
        return 1;
    }
}
