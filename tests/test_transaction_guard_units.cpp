#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/ioke_sha256.h"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/varint.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void expect_throws(Fn&& fn, const char* message)
{
    bool threw = false;
    try {
        fn();
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, message);
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 19);
    }
    return hash;
}

geocoin::Address address_from_seed(uint8_t seed)
{
    geocoin::Address address;
    expect(geocoin::crypto::compute_address_from_private_key(make_key(seed), address),
           "test address can be derived from seed");
    return address;
}

geocoin::core::TxInput make_input(uint8_t seed, uint32_t vout = 0)
{
    geocoin::core::TxInput input;
    input.prev_tx_hash = hash_from_seed(seed);
    input.vout = vout;
    return input;
}

geocoin::core::TxOutput make_output(
    geocoin::Amount value = geocoin::consensus::params::DUST_LIMIT
)
{
    geocoin::core::TxOutput output;
    output.value = value;
    output.address = address_from_seed(0x7a);
    return output;
}

std::vector<geocoin::Address> sorted_addresses(std::initializer_list<geocoin::Address> addresses)
{
    std::vector<geocoin::Address> out(addresses);
    std::sort(out.begin(), out.end(), [](const geocoin::Address& a, const geocoin::Address& b) {
        return a.str() < b.str();
    });
    return out;
}

void expect_check_reject(
    const geocoin::core::Transaction& tx,
    const std::string& expected_reason,
    const std::string& message
)
{
    geocoin::consensus::ValidationState state;
    if (geocoin::consensus::check_transaction(tx, state)) {
        throw std::runtime_error(message + ": transaction unexpectedly accepted");
    }
    if (state.get_reject_reason() != expected_reason) {
        throw std::runtime_error(
            message + ": expected " + expected_reason + ", got " + state.get_reject_reason()
        );
    }
}

geocoin::core::Transaction make_standard_tx()
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::STANDARD;
    tx.timestamp = 123456;

    geocoin::core::TxInput input;
    input.prev_tx_hash = hash_from_seed(0x21);
    input.vout = 0;
    tx.inputs.push_back(input);

    geocoin::core::TxOutput output;
    output.value = geocoin::COIN;
    output.address = geocoin::Address("GEOtransaction-guard-recipient");
    tx.outputs.push_back(output);
    return tx;
}

geocoin::nft::NFTMetadata make_nft_metadata()
{
    geocoin::nft::NFTMetadata metadata;
    metadata.name = "PoL plot";
    metadata.description = "transaction guard NFT";
    metadata.image_uri = "ipfs://transaction-guard";
    metadata.content_hash.assign(64, 'a');
    metadata.geometry.polygons.front().exterior = {{0LL, 0LL}, {0LL, 100000000LL}, {100000000LL, 100000000LL}, {100000000LL, 0LL}};
    metadata.area_bits = 0;
    metadata.created_at = 1700000000;
    metadata.block_height = 10;
    metadata.block_hash = hash_from_seed(0xa1);
    metadata.location_name = "guard";
    metadata.tags = {"guard"};
    metadata.creator = "tester";
    return metadata;
}

void sign_nft_pol_payload(geocoin::nft::NFTCreationTransaction& tx, const geocoin::bytes& private_key)
{
    tx.signature = geocoin::crypto::sign_recoverable_message(
        geocoin::crypto::sha256(tx.serialize_for_signing()),
        private_key
    );
}

void sign_nft_regular_payload(geocoin::nft::NFTCreationTransaction& tx, const geocoin::bytes& private_key)
{
    tx.signature = geocoin::crypto::sign_recoverable_message(
        geocoin::crypto::sha256(tx.serialize_for_signing()),
        private_key
    );
}

geocoin::Signature sign_payload(const geocoin::bytes& payload, const geocoin::bytes& private_key)
{
    return geocoin::crypto::sign_recoverable_message(
        geocoin::crypto::sha256(payload),
        private_key
    );
}

geocoin::nft::NFTCreationTransaction make_nft_creation_payload(const geocoin::bytes& private_key)
{
    geocoin::nft::NFTCreationTransaction payload;
    payload.metadata = make_nft_metadata();
    payload.nft_id = payload.metadata.calculate_hash();
    payload.creator_address = address_from_seed(private_key.front());
    payload.royalty_percent = 250;
    return payload;
}

void expect_deserialize_throws(const geocoin::bytes& data, const char* message)
{
    expect_throws([&]() {
        size_t offset = 0;
        (void)geocoin::core::Transaction::deserialize(data, offset);
    }, message);
}

void append_varint(geocoin::bytes& data, uint64_t value)
{
    const auto encoded = geocoin::utils::encode_varint(value);
    data.insert(data.end(), encoded.begin(), encoded.end());
}

void append_u32(geocoin::bytes& data, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        data.push_back(static_cast<geocoin::byte>((value >> (i * 8)) & 0xffu));
    }
}

void append_u64(geocoin::bytes& data, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<geocoin::byte>((value >> (i * 8)) & 0xffu));
    }
}

geocoin::bytes make_minimal_serialized_transaction(geocoin::core::TransactionType type)
{
    geocoin::bytes data;
    append_u32(data, 2);
    data.push_back(static_cast<geocoin::byte>(type));
    append_u64(data, 123456);
    append_varint(data, 0);
    append_varint(data, 0);
    append_u32(data, 0);
    return data;
}

geocoin::core::Transaction deserialize_ok(const geocoin::bytes& data)
{
    size_t offset = 0;
    auto tx = geocoin::core::Transaction::deserialize(data, offset);
    expect(offset == data.size(), "deserialize consumed all bytes");
    return tx;
}

geocoin::core::Transaction make_angle_transaction()
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::ANGLE_COMMITMENT;
    tx.commitment_network_id = "degree";
    tx.timestamp = 123456;
    tx.inputs.push_back(make_input(0x30));
    tx.inputs.front().signature = geocoin::Signature{0x01, 0x02, 0x03};
    tx.multisig_address = address_from_seed(0x31);
    tx.spending_tx_hash = hash_from_seed(0x32);
    tx.signer_address = address_from_seed(0x33);
    return tx;
}

geocoin::core::Transaction make_multisig_spend_transaction(bool hierarchical)
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::MULTISIG_SPEND;
    tx.timestamp = 123456;
    tx.inputs.push_back(make_input(0x34));
    tx.outputs.push_back(make_output());
    tx.m = 2;
    tx.n = 3;
    tx.angle_commitment_hashes = {hash_from_seed(0x35), hash_from_seed(0x36)};
    if (hierarchical) {
        tx.delegation_proof_txid = hash_from_seed(0x37);
    } else {
        tx.spend_participants = sorted_addresses({
            address_from_seed(0x38),
            address_from_seed(0x39),
            address_from_seed(0x3a)
        });
    }
    return tx;
}

geocoin::core::Transaction make_delegation_proof_transaction()
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::DELEGATION_PROOF;
    tx.timestamp = 123456;
    tx.primary_multisig_address = address_from_seed(0x3b);
    tx.delegated_multisig_address = address_from_seed(0x3c);
    tx.primary_m = 2;
    tx.primary_n = 3;
    tx.primary_participants = sorted_addresses({
        address_from_seed(0x3d),
        address_from_seed(0x3e),
        address_from_seed(0x3f)
    });
    tx.delegated_m = 1;
    tx.delegated_n = 2;
    tx.delegated_participants = sorted_addresses({
        address_from_seed(0x40),
        address_from_seed(0x41)
    });
    tx.valid_from = 10;
    tx.valid_until = 1000;
    tx.nonce = 7;
    tx.max_total_delegated = 100 * geocoin::COIN;
    tx.max_per_transaction = geocoin::COIN;
    tx.max_percent_of_balance = 25;
    tx.angle_commitment_hashes = {hash_from_seed(0x42), hash_from_seed(0x43)};
    return tx;
}

geocoin::core::Transaction make_cadastral_doc_transaction()
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::CADASTRAL_DOC;
    tx.timestamp = 123456;
    tx.cadastral_doc_hash = hash_from_seed(0x43);
    tx.cadastral_number = "123:456:789";
    tx.cadastral_owner = address_from_seed(0x44);
    tx.cadastral_nft_id = hash_from_seed(42);
    return tx;
}

geocoin::core::Transaction make_card_program_transaction()
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::CARD_PROGRAM_DEFINE;
    tx.timestamp = 123456;
    tx.card_program_id = hash_from_seed(0x45);
    tx.card_issuer = address_from_seed(0x46);
    tx.card_chip_vendor = "NXP";
    tx.card_chip_os = "JCOP";
    tx.card_applet_aid = "A000000001";
    tx.card_applet_version = 3;
    tx.card_applet_package_hash = hash_from_seed(0x47);
    tx.card_policy_id = 9;
    tx.card_scope_id = hash_from_seed(0x48);
    return tx;
}

geocoin::core::Transaction make_card_issue_transaction()
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::CARD_ISSUE;
    tx.timestamp = 123456;
    tx.card_program_id = hash_from_seed(0x49);
    tx.card_issuer = address_from_seed(0x4a);
    tx.card_policy_id = 2;
    tx.card_scope_id = hash_from_seed(0x4b);
    tx.card_uid_hash = hash_from_seed(0x4c);
    tx.card_pubkey_hash = hash_from_seed(0x4d);
    tx.card_valid_from = 100;
    tx.card_valid_to = 200;
    tx.card_status = 0;
    tx.card_metadata_hash = hash_from_seed(0x4e);
    return tx;
}

geocoin::core::Transaction make_card_update_transaction()
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::CARD_UPDATE_STATUS;
    tx.timestamp = 123456;
    tx.card_program_id = hash_from_seed(0x4f);
    tx.card_issuer = address_from_seed(0x50);
    tx.card_uid_hash = hash_from_seed(0x51);
    tx.card_status = 1;
    tx.card_metadata_hash = hash_from_seed(0x52);
    return tx;
}

geocoin::core::BatchCheckpointItem make_batch_checkpoint_item()
{
    geocoin::core::BatchCheckpointItem item;
    item.sidechain_id = hash_from_seed(0x53);
    item.checkpoint = hash_from_seed(0x54);
    item.checkpoint_height = 25;
    item.fee = 2;
    item.version = 2;
    item.committee_id = hash_from_seed(0x55);
    item.committee_signatures.push_back(geocoin::Signature{0x01, 0x02});
    item.proof = geocoin::bytes{0x03, 0x04};
    return item;
}

geocoin::core::Transaction make_sidechain_transaction(geocoin::core::TransactionType type)
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = type;
    tx.timestamp = 123456;
    tx.sidechain_id = hash_from_seed(0x56);
    tx.sidechain_owner = address_from_seed(0x57);
    tx.sidechain_amount = 3 * geocoin::COIN;
    tx.sidechain_unlock_height = 200;
    tx.sidechain_checkpoint = hash_from_seed(0x58);

    if (type == geocoin::core::TransactionType::ISSUE_SIDECHAIN) {
        tx.sidechain_token_symbol = "SCD";
        tx.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        tx.sidechain_fee_bps = 10;
        tx.sidechain_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
        tx.sidechain_checkpoint_policy_id = 2;
        tx.sidechain_checkpoint_version = 2;
        tx.sidechain_committee_m = 2;
        tx.sidechain_committee_n = 3;
        tx.sidechain_committee_members = sorted_addresses({
            address_from_seed(0x59),
            address_from_seed(0x5a),
            address_from_seed(0x5b)
        });
        tx.sidechain_committee_id = geocoin::consensus::compute_sidechain_committee_id(
            tx.sidechain_committee_m,
            tx.sidechain_committee_n,
            tx.sidechain_committee_members
        );
        tx.sidechain_signature = geocoin::Signature{0x09, 0x0a};
    } else if (type == geocoin::core::TransactionType::MINT_SIDECHAIN ||
               type == geocoin::core::TransactionType::REDEEM_SIDECHAIN ||
               type == geocoin::core::TransactionType::SLASH_DEPOSIT) {
        tx.sidechain_committee_id = hash_from_seed(0x5c);
        tx.sidechain_committee_signatures = {geocoin::Signature{0x05, 0x06}};
        if (type == geocoin::core::TransactionType::SLASH_DEPOSIT) {
            tx.sidechain_fraud_checkpoint_a = hash_from_seed(0x5d);
            tx.sidechain_fraud_checkpoint_b = hash_from_seed(0x5e);
            tx.sidechain_fraud_checkpoint_index_a = 1;
            tx.sidechain_fraud_checkpoint_index_b = 2;
        }
    } else if (type == geocoin::core::TransactionType::SIDECHAIN_CHECKPOINT) {
        tx.sidechain_checkpoint_height = 300;
        tx.sidechain_committee_id = hash_from_seed(0x5f);
        tx.sidechain_committee_signatures = {geocoin::Signature{0x07}};
    } else if (type == geocoin::core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE) {
        tx.sidechain_committee_m = 1;
        tx.sidechain_committee_n = 2;
        tx.sidechain_committee_members = sorted_addresses({
            address_from_seed(0x60),
            address_from_seed(0x61)
        });
        tx.sidechain_committee_id = hash_from_seed(0x62);
        tx.sidechain_committee_signatures = {geocoin::Signature{0x08}};
    } else {
        tx.sidechain_signature = geocoin::Signature{0x09, 0x0a};
    }

    return tx;
}

geocoin::core::Transaction make_foundation_update_transaction()
{
    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::UPDATE_FOUNDATION_ADDRESS;
    tx.timestamp = 123456;
    tx.foundation_old_address = address_from_seed(0x63);
    tx.foundation_new_address = address_from_seed(0x64);
    tx.foundation_m = 1;
    tx.foundation_n = 2;
    tx.foundation_participants = sorted_addresses({address_from_seed(0x65), address_from_seed(0x66)});
    tx.foundation_new_m = 1;
    tx.foundation_new_n = 1;
    tx.foundation_new_participants = {address_from_seed(0x67)};
    tx.foundation_signatures = {geocoin::Signature{0x0b, 0x0c}};
    return tx;
}

void test_serialize_and_signing_guards()
{
    using geocoin::core::Transaction;
    using geocoin::core::TransactionType;

    Transaction nft_create_missing;
    nft_create_missing.type = TransactionType::NFT_CREATE;
    expect_throws([&]() { (void)nft_create_missing.serialize(); },
                  "NFT_CREATE without payload refuses serialization");

    Transaction nft_transfer_missing;
    nft_transfer_missing.type = TransactionType::NFT_TRANSFER;
    expect_throws([&]() { (void)nft_transfer_missing.serialize(); },
                  "NFT_TRANSFER without payload refuses serialization");

    Transaction nft_burn_missing;
    nft_burn_missing.type = TransactionType::NFT_BURN;
    expect_throws([&]() { (void)nft_burn_missing.serialize(); },
                  "NFT_BURN without payload refuses serialization");

    Transaction bad_witness_size = make_standard_tx();
    bad_witness_size.witnesses.resize(1);
    bad_witness_size.witnesses[0].public_key = geocoin::bytes{1, 2, 3};
    expect_throws([&]() { (void)bad_witness_size.serialize(); },
                  "witness public key size is validated during serialization");

    Transaction no_inputs;
    no_inputs.sign_input(0, make_key(1));
    expect(no_inputs.witnesses.empty(), "sign_input ignores missing input");

    Transaction short_key = make_standard_tx();
    short_key.sign(make_key(2));
    expect(!short_key.witnesses.empty(), "valid sign populates witness");
    const size_t signed_size = short_key.get_size();
    expect(signed_size == short_key.serialize().size(), "get_size matches serialized size");

    Transaction bad_private_key = make_standard_tx();
    bad_private_key.sign(geocoin::bytes{1, 2, 3});
    expect(bad_private_key.witnesses.empty(), "sign ignores invalid private key length");
    bad_private_key.sign_input(0, geocoin::bytes{1, 2, 3});
    expect(bad_private_key.witnesses.empty(), "sign_input ignores invalid private key length");

    Transaction excluded_type = make_standard_tx();
    excluded_type.type = TransactionType::MULTISIG_SPEND;
    excluded_type.sign_input(0, make_key(3));
    expect(excluded_type.witnesses.empty(), "multisig spend is not signed through standard witness path");
}

void test_verify_input_guards()
{
    using geocoin::core::TxWitness;
    using geocoin::core::Transaction;

    Transaction tx = make_standard_tx();
    expect(!tx.verify_input(9), "verify_input rejects out-of-range input");
    expect(!tx.verify(), "standard transaction without witness is rejected");

    tx.witnesses.resize(1);
    expect(!tx.verify_input(0), "empty witness signature is rejected");

    tx.witnesses[0].signature = geocoin::Signature{1, 2, 3};
    expect(!tx.verify_input(0), "witness without public key is rejected");

    tx.witnesses[0].public_key = geocoin::bytes{1, 2, 3};
    expect(!tx.verify_input(0), "short witness public key is rejected");

    tx.witnesses[0].public_key.assign(geocoin::consensus::params::WITNESS_PUBKEY_SIZE, 0x42);
    tx.witnesses[0].public_key_hash = hash_from_seed(0xee);
    expect(!tx.verify_input(0), "witness public key hash mismatch is rejected");

    tx.witnesses[0].public_key_hash = geocoin::crypto::sha256(tx.witnesses[0].public_key);
    expect(!tx.verify_input(0), "invalid detached signature is rejected");

    Transaction signed_tx = make_standard_tx();
    signed_tx.sign_input(0, make_key(9));
    expect(signed_tx.verify_input(0), "valid signed input verifies");
    signed_tx.inputs[0].signature_hash[0] ^= 0x01;
    expect(!signed_tx.verify_input(0), "signature hash mismatch is rejected");

    Transaction coinbase = Transaction::create_coinbase(
        1,
        geocoin::Address("GEOtransaction-guard-miner"),
        geocoin::COIN
    );
    expect(coinbase.verify_input(0), "coinbase input verify is a no-op");

    const geocoin::Amount maximum = (std::numeric_limits<geocoin::Amount>::max)();
    expect_throws(
        [&]() {
            (void)Transaction::create_coinbase(
                1,
                geocoin::Address("GEOtransaction-guard-miner"),
                maximum,
                1
            );
        },
        "coinbase construction rejects reward overflow"
    );
    expect_throws(
        [&]() {
            (void)Transaction::create_coinbase(
                1,
                geocoin::Address("GEOtransaction-guard-miner"),
                -1,
                0
            );
        },
        "coinbase construction rejects negative reward"
    );
}

void test_deserialize_witness_guards()
{
    geocoin::core::Transaction signed_tx = make_standard_tx();
    signed_tx.sign_input(0, make_key(17));
    const geocoin::bytes without_witness = signed_tx.serialize_without_witness();
    const geocoin::bytes serialized = signed_tx.serialize();
    const size_t witness_offset = without_witness.size();
    expect(serialized.size() > witness_offset + 2 + 32,
           "signed fixture has witness payload");
    expect(serialized[witness_offset] == geocoin::consensus::params::WITNESS_FORMAT_VERSION_CURRENT,
           "signed fixture uses current witness version");
    expect(serialized[witness_offset + 1] == 1,
           "signed fixture has one-byte witness count");

    geocoin::bytes trailing = serialized;
    trailing.push_back(0xff);
    expect_throws([&]() { (void)geocoin::core::Transaction::canonical_hash_from_serialized(trailing); },
                  "canonical hash rejects trailing transaction bytes");

    geocoin::core::Transaction nft_tx;
    nft_tx.version = 2;
    nft_tx.type = geocoin::core::TransactionType::NFT_CREATE;
    nft_tx.timestamp = 123456;
    auto nft_payload = make_nft_creation_payload(make_key(0x4a));
    geocoin::nft::NFTMetadata::CadastralData cadastral;
    cadastral.cadastral_number = "audit-canonical";
    cadastral.owner_name = "owner";
    cadastral.land_category = "land";
    cadastral.permitted_use = "test";
    cadastral.has_buildings = true;
    cadastral.building_count = 1;
    nft_payload.metadata.cadastral = cadastral;
    nft_payload.nft_id = nft_payload.metadata.calculate_hash();
    nft_tx.nft_create = nft_payload;

    geocoin::bytes noncanonical_nft = nft_tx.serialize();
    const geocoin::bytes metadata_bytes = nft_payload.metadata.serialize();
    const auto metadata_pos = std::search(
        noncanonical_nft.begin(),
        noncanonical_nft.end(),
        metadata_bytes.begin(),
        metadata_bytes.end()
    );
    expect(metadata_pos != noncanonical_nft.end() && metadata_bytes.size() >= 5,
           "NFT metadata bytes are located in serialized transaction");
    const size_t building_flag_offset = static_cast<size_t>(
        std::distance(noncanonical_nft.begin(), metadata_pos)
    ) + metadata_bytes.size() - 5;
    expect(noncanonical_nft[building_flag_offset] == 1,
           "NFT cadastral fixture has canonical boolean flag");
    noncanonical_nft[building_flag_offset] = 2;
    expect_deserialize_throws(
        noncanonical_nft,
        "non-canonical NFT boolean representation is rejected by the parser"
    );
    expect_throws(
        [&]() {
            (void)geocoin::core::Transaction::canonical_hash_from_serialized(noncanonical_nft);
        },
        "canonical hash rejects alternate NFT boolean representation"
    );

    expect_deserialize_throws(without_witness, "deserializer requires witness version");

    geocoin::bytes version_two = serialized;
    version_two[witness_offset] = 2;
    expect_deserialize_throws(version_two, "witness version 2 is not accepted yet");

    geocoin::bytes unsupported_version = serialized;
    unsupported_version[witness_offset] = 9;
    expect_deserialize_throws(unsupported_version, "unsupported witness version is rejected");

    geocoin::bytes count_mismatch = serialized;
    count_mismatch[witness_offset + 1] = 0;
    expect_deserialize_throws(count_mismatch, "witness count must match input count");

    const size_t flags_offset = witness_offset + 2 + 32;
    geocoin::bytes invalid_flags = serialized;
    invalid_flags[flags_offset] = 0x02;
    expect_deserialize_throws(invalid_flags, "invalid witness flags are rejected");

    geocoin::bytes missing_pubkey(serialized.begin(), serialized.begin() + flags_offset + 1);
    missing_pubkey[flags_offset] = 0x01;
    expect_deserialize_throws(missing_pubkey, "witness pubkey material is required when flag is set");

    const size_t sig_len_offset = flags_offset + 1 + geocoin::consensus::params::WITNESS_PUBKEY_SIZE;
    expect(serialized.size() > sig_len_offset, "signed fixture has witness signature length");
    geocoin::bytes oversized_sig(serialized.begin(), serialized.begin() + sig_len_offset);
    const auto huge_sig_len = geocoin::utils::encode_varint(
        static_cast<uint64_t>(geocoin::consensus::params::MAX_WITNESS_SIG_SIZE + 1)
    );
    oversized_sig.insert(oversized_sig.end(), huge_sig_len.begin(), huge_sig_len.end());
    expect_deserialize_throws(oversized_sig, "oversized witness signature is rejected");

    geocoin::bytes too_many_witnesses =
        make_minimal_serialized_transaction(geocoin::core::TransactionType::STANDARD);
    too_many_witnesses.push_back(geocoin::consensus::params::WITNESS_FORMAT_VERSION_CURRENT);
    append_varint(too_many_witnesses, geocoin::consensus::params::MAX_TX_INPUTS + 1);
    expect_deserialize_throws(too_many_witnesses, "witness count is bounded before allocation");

    geocoin::bytes missing_pubkey_hash = make_standard_tx().serialize_without_witness();
    missing_pubkey_hash.push_back(geocoin::consensus::params::WITNESS_FORMAT_VERSION_CURRENT);
    append_varint(missing_pubkey_hash, 1);
    expect_deserialize_throws(missing_pubkey_hash, "witness pubkey hash is required");

    geocoin::bytes missing_flags = missing_pubkey_hash;
    missing_flags.insert(missing_flags.end(), 32, 0x00);
    expect_deserialize_throws(missing_flags, "witness flags are required");
}

void test_deserialize_structured_roundtrips()
{
    using geocoin::core::TransactionType;

    {
        const auto parsed = deserialize_ok(make_angle_transaction().serialize());
        expect(parsed.type == TransactionType::ANGLE_COMMITMENT, "angle type roundtrips");
        expect(parsed.inputs.size() == 1, "angle input roundtrips");
        expect(parsed.serialize_without_witness().back() == 0, "angle extension byte is fixed to zero");
        expect(parsed.multisig_address == address_from_seed(0x31), "angle multisig address roundtrips");
        expect(parsed.signer_address == address_from_seed(0x33), "angle signer address roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_multisig_spend_transaction(false).serialize());
        expect(parsed.type == TransactionType::MULTISIG_SPEND, "multisig spend type roundtrips");
        expect(parsed.spend_participants.size() == 3, "non-hierarchical participants roundtrip");
        expect(parsed.angle_commitment_hashes.size() == 2, "multisig angle refs roundtrip");
    }
    {
        const auto parsed = deserialize_ok(make_multisig_spend_transaction(true).serialize());
        expect(parsed.type == TransactionType::MULTISIG_SPEND, "hierarchical spend type roundtrips");
        expect(parsed.spend_participants.empty(), "hierarchical spend omits inline participants");
        expect(parsed.delegation_proof_txid == hash_from_seed(0x37), "delegation proof ref roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_delegation_proof_transaction().serialize());
        expect(parsed.type == TransactionType::DELEGATION_PROOF, "delegation proof type roundtrips");
        expect(parsed.primary_participants.size() == 3, "primary participants roundtrip");
        expect(parsed.delegated_participants.size() == 2, "delegated participants roundtrip");
        expect(parsed.max_percent_of_balance == 25, "delegation limits roundtrip");
        expect(parsed.angle_commitment_hashes.size() == 2, "delegation angle refs roundtrip");
    }
    {
        geocoin::core::Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::REVOKE_DELEGATION;
        tx.revoked_delegation_proof_txid = hash_from_seed(0x68);
        tx.angle_commitment_hashes = {hash_from_seed(0x69)};
        const auto parsed = deserialize_ok(tx.serialize());
        expect(parsed.type == TransactionType::REVOKE_DELEGATION, "revoke delegation type roundtrips");
        expect(parsed.angle_commitment_hashes.size() == 1, "revoke delegation refs roundtrip");
    }
    {
        const auto parsed = deserialize_ok(make_cadastral_doc_transaction().serialize());
        expect(parsed.type == TransactionType::CADASTRAL_DOC, "cadastral doc type roundtrips");
        expect(parsed.cadastral_number == "123:456:789", "cadastral number roundtrips");
        expect(parsed.cadastral_owner == address_from_seed(0x44), "cadastral owner roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_card_program_transaction().serialize());
        expect(parsed.type == TransactionType::CARD_PROGRAM_DEFINE, "card program type roundtrips");
        expect(parsed.card_chip_vendor == "NXP", "card vendor roundtrips");
        expect(parsed.card_applet_version == 3, "card applet version roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_card_issue_transaction().serialize());
        expect(parsed.type == TransactionType::CARD_ISSUE, "card issue type roundtrips");
        expect(parsed.card_valid_from == 100, "card validity start roundtrips");
        expect(parsed.card_metadata_hash == hash_from_seed(0x4e), "card metadata hash roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_card_update_transaction().serialize());
        expect(parsed.type == TransactionType::CARD_UPDATE_STATUS, "card update type roundtrips");
        expect(parsed.card_status == 1, "card status roundtrips");
    }
    {
        geocoin::core::Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        tx.sidechain_batch_checkpoints = {make_batch_checkpoint_item()};
        const auto parsed = deserialize_ok(tx.serialize());
        expect(parsed.type == TransactionType::BATCH_SIDECHAIN_CHECKPOINT, "batch checkpoint type roundtrips");
        expect(parsed.sidechain_batch_checkpoints.size() == 1, "batch checkpoint item roundtrips");
        expect(parsed.sidechain_batch_checkpoints.front().proof == geocoin::bytes({0x03, 0x04}),
               "batch checkpoint proof roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_sidechain_transaction(TransactionType::ISSUE_SIDECHAIN).serialize());
        expect(parsed.type == TransactionType::ISSUE_SIDECHAIN, "issue sidechain type roundtrips");
        expect(parsed.sidechain_token_symbol == "SCD", "sidechain token symbol roundtrips");
        expect(parsed.sidechain_committee_members.size() == 3, "sidechain issue committee roundtrips");
        expect(parsed.sidechain_committee_id == geocoin::consensus::compute_sidechain_committee_id(
                   parsed.sidechain_committee_m,
                   parsed.sidechain_committee_n,
                   parsed.sidechain_committee_members
               ),
               "sidechain issue committee id roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_sidechain_transaction(TransactionType::MINT_SIDECHAIN).serialize());
        expect(parsed.type == TransactionType::MINT_SIDECHAIN, "mint sidechain type roundtrips");
        expect(parsed.sidechain_committee_signatures.size() == 1, "mint committee signatures roundtrip");
    }
    {
        const auto parsed = deserialize_ok(make_sidechain_transaction(TransactionType::REDEEM_SIDECHAIN).serialize());
        expect(parsed.type == TransactionType::REDEEM_SIDECHAIN, "redeem sidechain type roundtrips");
        expect(parsed.sidechain_committee_id == hash_from_seed(0x5c), "redeem committee id roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_sidechain_transaction(TransactionType::SIDECHAIN_CHECKPOINT).serialize());
        expect(parsed.type == TransactionType::SIDECHAIN_CHECKPOINT, "sidechain checkpoint type roundtrips");
        expect(parsed.sidechain_checkpoint_height == 300, "sidechain checkpoint height roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_sidechain_transaction(TransactionType::UPDATE_SIDECHAIN_COMMITTEE).serialize());
        expect(parsed.type == TransactionType::UPDATE_SIDECHAIN_COMMITTEE, "sidechain committee update type roundtrips");
        expect(parsed.sidechain_committee_members.size() == 2, "sidechain committee update members roundtrip");
    }
    {
        const auto parsed = deserialize_ok(make_sidechain_transaction(TransactionType::SLASH_DEPOSIT).serialize());
        expect(parsed.type == TransactionType::SLASH_DEPOSIT, "slash deposit type roundtrips");
        expect(parsed.sidechain_fraud_checkpoint_index_a == 1, "slash fraud index a roundtrips");
        expect(parsed.sidechain_fraud_checkpoint_index_b == 2, "slash fraud index b roundtrips");
    }
    {
        auto tx = make_sidechain_transaction(TransactionType::SLASH_DEPOSIT);
        geocoin::bytes data = tx.serialize_without_witness();
        data.resize(data.size() - 4);
        data.push_back(geocoin::consensus::params::WITNESS_FORMAT_VERSION_CURRENT);
        append_varint(data, 0);
        const auto parsed = deserialize_ok(data);
        expect(parsed.sidechain_fraud_checkpoint_index_a == 0xffff,
               "legacy slash checkpoint index a defaults to single-tx reference");
        expect(parsed.sidechain_fraud_checkpoint_index_b == 0xffff,
               "legacy slash checkpoint index b defaults to single-tx reference");
    }
    {
        const auto parsed = deserialize_ok(make_sidechain_transaction(TransactionType::LOCK_DEPOSIT).serialize());
        expect(parsed.type == TransactionType::LOCK_DEPOSIT, "lock deposit type roundtrips");
        expect(parsed.sidechain_signature == geocoin::Signature({0x09, 0x0a}),
               "sidechain owner signature roundtrips");
    }
    {
        const auto parsed = deserialize_ok(make_foundation_update_transaction().serialize());
        expect(parsed.type == TransactionType::UPDATE_FOUNDATION_ADDRESS, "foundation update type roundtrips");
        expect(parsed.foundation_participants.size() == 2, "foundation participants roundtrip");
        expect(parsed.foundation_signatures.size() == 1, "foundation signatures roundtrip");
    }
}

void test_deserialize_size_limit_guards()
{
    using geocoin::core::Transaction;
    using geocoin::core::TransactionType;

    {
        const geocoin::bytes data{0x01};
        size_t offset = (std::numeric_limits<size_t>::max)();
        expect_throws(
            [&]() { (void)Transaction::deserialize(data, offset); },
            "out-of-range initial transaction offset is rejected"
        );
    }
    {
        geocoin::bytes data;
        append_u32(data, 2);
        data.push_back(static_cast<geocoin::byte>(TransactionType::STANDARD));
        append_u64(data, 123456);
        append_varint(data, geocoin::consensus::params::MAX_TX_INPUTS + 1);
        expect_deserialize_throws(data, "input count is bounded before allocation");
    }
    {
        geocoin::bytes data;
        append_u32(data, 2);
        data.push_back(static_cast<geocoin::byte>(TransactionType::STANDARD));
        append_u64(data, 123456);
        append_varint(data, 1);
        data.insert(data.end(), 32, 0x01);
        append_u32(data, 0);
        append_varint(data, 1);
        expect_deserialize_throws(data, "truncated variable-length input field is rejected");
    }

    {
        Transaction tx = make_standard_tx();
        tx.inputs.front().script_sig.assign(geocoin::consensus::params::MAX_SCRIPT_SIG_SIZE + 1, 0x01);
        expect_deserialize_throws(tx.serialize(), "oversized script_sig is rejected");
    }
    {
        Transaction tx = make_standard_tx();
        tx.outputs.assign(geocoin::consensus::params::MAX_TX_OUTPUTS + 1, make_output());
        expect_deserialize_throws(tx.serialize(), "output count is bounded before allocation");
    }
    {
        Transaction tx = make_standard_tx();
        tx.outputs.front().address = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'a'));
        expect_deserialize_throws(tx.serialize(), "oversized output address is rejected");
    }
    {
        Transaction tx = make_standard_tx();
        tx.outputs.front().script_pubkey.assign(geocoin::consensus::params::MAX_SCRIPT_PUBKEY_SIZE + 1, 0x02);
        expect_deserialize_throws(tx.serialize(), "oversized script_pubkey is rejected");
    }
    {
        Transaction tx = make_angle_transaction();
        tx.commitment_network_id.clear();
        expect_deserialize_throws(tx.serialize(), "empty angle network id is rejected");
    }
    {
        Transaction tx = make_angle_transaction();
        tx.commitment_network_id.assign(33, 'n');
        expect_deserialize_throws(tx.serialize(), "oversized angle network id is rejected");
    }
    {
        Transaction tx = make_angle_transaction();
        tx.inputs.front().signature.assign(geocoin::consensus::params::MAX_SIGNATURE_SIZE + 1, 0x03);
        expect_deserialize_throws(tx.serialize(), "oversized angle signature is rejected");
    }
    {
        Transaction tx = make_angle_transaction();
        tx.multisig_address = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'm'));
        expect_deserialize_throws(tx.serialize(), "oversized angle multisig address is rejected");
    }
    {
        Transaction tx = make_angle_transaction();
        tx.signer_address = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 's'));
        expect_deserialize_throws(tx.serialize(), "oversized angle signer address is rejected");
    }
    {
        Transaction tx = make_angle_transaction();
        auto raw = tx.serialize();
        raw[tx.serialize_without_witness().size() - 1] = 1;
        expect_deserialize_throws(raw, "nonzero angle extension byte is rejected");
    }

    {
        Transaction tx = make_multisig_spend_transaction(false);
        tx.angle_commitment_hashes.assign(geocoin::consensus::params::MAX_ANGLE_COMMITMENTS + 1, hash_from_seed(0x10));
        expect_deserialize_throws(tx.serialize(), "multisig angle commitment count is bounded");
    }
    {
        Transaction tx = make_multisig_spend_transaction(false);
        tx.spend_participants.assign(
            geocoin::consensus::params::MAX_MULTISIG_PARTICIPANTS + 1,
            address_from_seed(0x11)
        );
        expect_deserialize_throws(tx.serialize(), "multisig participant count is bounded");
    }
    {
        Transaction tx = make_multisig_spend_transaction(false);
        tx.spend_participants = {geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'p'))};
        expect_deserialize_throws(tx.serialize(), "multisig participant address is bounded");
    }

    {
        Transaction tx = make_delegation_proof_transaction();
        tx.primary_multisig_address = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'p'));
        expect_deserialize_throws(tx.serialize(), "primary multisig address is bounded");
    }
    {
        Transaction tx = make_delegation_proof_transaction();
        tx.delegated_multisig_address = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'd'));
        expect_deserialize_throws(tx.serialize(), "delegated multisig address is bounded");
    }
    {
        Transaction tx = make_delegation_proof_transaction();
        tx.primary_participants.assign(
            geocoin::consensus::params::MAX_MULTISIG_PARTICIPANTS + 1,
            address_from_seed(0x12)
        );
        expect_deserialize_throws(tx.serialize(), "primary participant count is bounded");
    }
    {
        Transaction tx = make_delegation_proof_transaction();
        tx.primary_participants = {geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'p'))};
        expect_deserialize_throws(tx.serialize(), "primary participant address is bounded");
    }
    {
        Transaction tx = make_delegation_proof_transaction();
        tx.delegated_participants.assign(
            geocoin::consensus::params::MAX_MULTISIG_PARTICIPANTS + 1,
            address_from_seed(0x13)
        );
        expect_deserialize_throws(tx.serialize(), "delegated participant count is bounded");
    }
    {
        Transaction tx = make_delegation_proof_transaction();
        tx.delegated_participants = {geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'd'))};
        expect_deserialize_throws(tx.serialize(), "delegated participant address is bounded");
    }
    {
        Transaction tx = make_delegation_proof_transaction();
        tx.angle_commitment_hashes.assign(geocoin::consensus::params::MAX_ANGLE_COMMITMENTS + 1, hash_from_seed(0x14));
        expect_deserialize_throws(tx.serialize(), "delegation proof angle refs are bounded");
    }
    {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::REVOKE_DELEGATION;
        tx.revoked_delegation_proof_txid = hash_from_seed(0x15);
        tx.angle_commitment_hashes.assign(geocoin::consensus::params::MAX_ANGLE_COMMITMENTS + 1, hash_from_seed(0x16));
        expect_deserialize_throws(tx.serialize(), "revoke delegation angle refs are bounded");
    }

    {
        Transaction tx = make_cadastral_doc_transaction();
        tx.cadastral_number.assign(geocoin::consensus::params::MAX_CADASTRAL_NUMBER + 1, '1');
        expect_deserialize_throws(tx.serialize(), "cadastral number is bounded");
    }
    {
        Transaction tx = make_cadastral_doc_transaction();
        tx.cadastral_owner = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'o'));
        expect_deserialize_throws(tx.serialize(), "cadastral owner address is bounded");
    }

    {
        Transaction tx = make_card_program_transaction();
        tx.card_issuer = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'i'));
        expect_deserialize_throws(tx.serialize(), "card program issuer is bounded");
    }
    {
        Transaction tx = make_card_program_transaction();
        tx.card_chip_vendor.assign(geocoin::consensus::params::MAX_CARD_CHIP_VENDOR + 1, 'v');
        expect_deserialize_throws(tx.serialize(), "card chip vendor is bounded");
    }
    {
        Transaction tx = make_card_program_transaction();
        tx.card_chip_os.assign(geocoin::consensus::params::MAX_CARD_CHIP_OS + 1, 'o');
        expect_deserialize_throws(tx.serialize(), "card chip os is bounded");
    }
    {
        Transaction tx = make_card_program_transaction();
        tx.card_applet_aid.assign(geocoin::consensus::params::MAX_CARD_APPLET_AID + 1, 'a');
        expect_deserialize_throws(tx.serialize(), "card applet aid is bounded");
    }
    {
        Transaction tx = make_card_issue_transaction();
        tx.card_issuer = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'i'));
        expect_deserialize_throws(tx.serialize(), "card issue issuer is bounded");
    }
    {
        Transaction tx = make_card_update_transaction();
        tx.card_issuer = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'i'));
        expect_deserialize_throws(tx.serialize(), "card update issuer is bounded");
    }

    {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        tx.sidechain_batch_checkpoints.assign(
            geocoin::consensus::params::MAX_BATCH_SIDECHAIN_CHECKPOINTS + 1,
            make_batch_checkpoint_item()
        );
        expect_deserialize_throws(tx.serialize(), "batch checkpoint count is bounded");
    }
    {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        auto item = make_batch_checkpoint_item();
        item.committee_signatures.assign(
            geocoin::consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES + 1,
            geocoin::Signature{0x01}
        );
        tx.sidechain_batch_checkpoints = {std::move(item)};
        expect_deserialize_throws(tx.serialize(), "batch checkpoint signature count is bounded");
    }
    {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        auto item = make_batch_checkpoint_item();
        item.committee_signatures = {
            geocoin::Signature(geocoin::consensus::params::MAX_SIGNATURE_SIZE + 1, 0x02)
        };
        tx.sidechain_batch_checkpoints = {std::move(item)};
        expect_deserialize_throws(tx.serialize(), "batch checkpoint signature size is bounded");
    }
    {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        auto item = make_batch_checkpoint_item();
        item.proof.assign(geocoin::consensus::params::MAX_BATCH_CHECKPOINT_PROOF + 1, 0x03);
        tx.sidechain_batch_checkpoints = {std::move(item)};
        expect_deserialize_throws(tx.serialize(), "batch checkpoint proof size is bounded");
    }

    {
        Transaction tx = make_sidechain_transaction(TransactionType::LOCK_DEPOSIT);
        tx.sidechain_owner = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'o'));
        expect_deserialize_throws(tx.serialize(), "sidechain owner address is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::LOCK_DEPOSIT);
        tx.sidechain_signature.assign(geocoin::consensus::params::MAX_SIGNATURE_SIZE + 1, 0x04);
        expect_deserialize_throws(tx.serialize(), "sidechain owner signature size is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::ISSUE_SIDECHAIN);
        tx.sidechain_token_symbol.assign(geocoin::consensus::params::MAX_SIDECHAIN_TOKEN_SYMBOL_SIZE + 1, 's');
        expect_deserialize_throws(tx.serialize(), "sidechain token symbol is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::ISSUE_SIDECHAIN);
        tx.sidechain_committee_members.assign(
            geocoin::consensus::params::MAX_SIDECHAIN_COMMITTEE_MEMBERS + 1,
            address_from_seed(0x17)
        );
        expect_deserialize_throws(tx.serialize(), "issue sidechain committee count is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::ISSUE_SIDECHAIN);
        tx.sidechain_committee_members = {geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'c'))};
        expect_deserialize_throws(tx.serialize(), "issue sidechain committee address is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::REGISTER_ACCOUNTING_SIDECHAIN);
        tx.sidechain_namespace.assign(
            geocoin::consensus::params::MAX_SIDECHAIN_NAMESPACE + 1,
            'n'
        );
        expect_deserialize_throws(tx.serialize(), "accounting sidechain namespace is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::REGISTER_ACCOUNTING_SIDECHAIN);
        tx.sidechain_namespace = "ledger";
        tx.sidechain_committee_members.assign(
            geocoin::consensus::params::MAX_SIDECHAIN_COMMITTEE_MEMBERS + 1,
            address_from_seed(0x19)
        );
        expect_deserialize_throws(tx.serialize(), "accounting sidechain committee count is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::REGISTER_ACCOUNTING_SIDECHAIN);
        tx.sidechain_namespace = "ledger";
        tx.sidechain_committee_members = {
            geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'c'))
        };
        expect_deserialize_throws(tx.serialize(), "accounting sidechain committee address is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::MINT_SIDECHAIN);
        tx.sidechain_committee_signatures.assign(
            geocoin::consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES + 1,
            geocoin::Signature{0x05}
        );
        expect_deserialize_throws(tx.serialize(), "mint sidechain committee signature count is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::MINT_SIDECHAIN);
        tx.sidechain_committee_signatures = {
            geocoin::Signature(geocoin::consensus::params::MAX_SIGNATURE_SIZE + 1, 0x06)
        };
        expect_deserialize_throws(tx.serialize(), "mint sidechain committee signature size is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::SIDECHAIN_CHECKPOINT);
        tx.sidechain_committee_signatures.assign(
            geocoin::consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES + 1,
            geocoin::Signature{0x07}
        );
        expect_deserialize_throws(tx.serialize(), "sidechain checkpoint signature count is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::SIDECHAIN_CHECKPOINT);
        tx.sidechain_committee_signatures = {
            geocoin::Signature(geocoin::consensus::params::MAX_SIGNATURE_SIZE + 1, 0x08)
        };
        expect_deserialize_throws(tx.serialize(), "sidechain checkpoint signature size is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::UPDATE_SIDECHAIN_COMMITTEE);
        tx.sidechain_committee_members.assign(
            geocoin::consensus::params::MAX_SIDECHAIN_COMMITTEE_MEMBERS + 1,
            address_from_seed(0x18)
        );
        expect_deserialize_throws(tx.serialize(), "update sidechain committee count is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::UPDATE_SIDECHAIN_COMMITTEE);
        tx.sidechain_committee_members = {geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'c'))};
        expect_deserialize_throws(tx.serialize(), "update sidechain committee address is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::UPDATE_SIDECHAIN_COMMITTEE);
        tx.sidechain_committee_signatures.assign(
            geocoin::consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES + 1,
            geocoin::Signature{0x09}
        );
        expect_deserialize_throws(tx.serialize(), "update sidechain committee signature count is bounded");
    }
    {
        Transaction tx = make_sidechain_transaction(TransactionType::UPDATE_SIDECHAIN_COMMITTEE);
        tx.sidechain_committee_signatures = {
            geocoin::Signature(geocoin::consensus::params::MAX_SIGNATURE_SIZE + 1, 0x0a)
        };
        expect_deserialize_throws(tx.serialize(), "update sidechain committee signature size is bounded");
    }

    for (TransactionType type : {
             TransactionType::NFT_CREATE,
             TransactionType::NFT_TRANSFER,
             TransactionType::NFT_BURN
         }) {
        geocoin::bytes data = make_minimal_serialized_transaction(type);
        append_varint(data, geocoin::consensus::params::MAX_TX_SIZE + 1);
        expect_deserialize_throws(data, "oversized NFT payload is rejected");
    }

    {
        Transaction tx = make_foundation_update_transaction();
        tx.foundation_old_address = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'o'));
        expect_deserialize_throws(tx.serialize(), "foundation old address is bounded");
    }
    {
        Transaction tx = make_foundation_update_transaction();
        tx.foundation_new_address = geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'n'));
        expect_deserialize_throws(tx.serialize(), "foundation new address is bounded");
    }
    {
        Transaction tx = make_foundation_update_transaction();
        tx.foundation_participants.assign(256, address_from_seed(0x19));
        expect_deserialize_throws(tx.serialize(), "foundation participant count is bounded");
    }
    {
        Transaction tx = make_foundation_update_transaction();
        tx.foundation_participants = {geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'p'))};
        expect_deserialize_throws(tx.serialize(), "foundation participant address is bounded");
    }
    {
        Transaction tx = make_foundation_update_transaction();
        tx.foundation_new_participants.assign(256, address_from_seed(0x1a));
        expect_deserialize_throws(tx.serialize(), "foundation new participant count is bounded");
    }
    {
        Transaction tx = make_foundation_update_transaction();
        tx.foundation_new_participants = {geocoin::Address(std::string(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'p'))};
        expect_deserialize_throws(tx.serialize(), "foundation new participant address is bounded");
    }
    {
        Transaction tx = make_foundation_update_transaction();
        tx.foundation_signatures.assign(256, geocoin::Signature{0x0b});
        expect_deserialize_throws(tx.serialize(), "foundation signature count is bounded");
    }
    {
        Transaction tx = make_foundation_update_transaction();
        tx.foundation_signatures = {
            geocoin::Signature(geocoin::consensus::params::MAX_SIGNATURE_SIZE + 1, 0x0c)
        };
        expect_deserialize_throws(tx.serialize(), "foundation signature size is bounded");
    }
}

void test_transaction_type_verify_guards()
{
    using geocoin::core::Transaction;
    using geocoin::core::TransactionType;

    Transaction unknown = make_standard_tx();
    unknown.type = static_cast<TransactionType>(250);
    expect(!unknown.verify(), "unknown transaction type is rejected");

    Transaction bad_witness_version = make_standard_tx();
    bad_witness_version.witness_version = 0;
    expect(!bad_witness_version.verify(), "bad witness version is rejected");

    Transaction angle_no_inputs;
    angle_no_inputs.type = TransactionType::ANGLE_COMMITMENT;
    expect(!angle_no_inputs.verify(), "angle commitment requires an input");

    Transaction nft_create_missing;
    nft_create_missing.type = TransactionType::NFT_CREATE;
    expect(!nft_create_missing.verify(), "NFT_CREATE verify rejects missing payload");

    Transaction nft_transfer_missing;
    nft_transfer_missing.type = TransactionType::NFT_TRANSFER;
    expect(!nft_transfer_missing.verify(), "NFT_TRANSFER verify rejects missing payload");

    Transaction nft_burn_missing;
    nft_burn_missing.type = TransactionType::NFT_BURN;
    expect(!nft_burn_missing.verify(), "NFT_BURN verify rejects missing payload");
}

void test_nft_pol_signature_verify_guards()
{
    using geocoin::core::Transaction;
    using geocoin::core::TransactionType;

    const auto creator_key = make_key(0x90);
    auto payload = make_nft_creation_payload(creator_key);
    payload.pol_data = geocoin::nft::ProofOfLocationData{
        12345678,
        -2345678,
        1700000000123ULL,
        500,
        static_cast<uint8_t>(geocoin::nft::PoLSourceType::GPS_NATIVE)
    };
    sign_nft_pol_payload(payload, creator_key);

    Transaction tx;
    tx.version = 2;
    tx.type = TransactionType::NFT_CREATE;
    tx.nft_create = payload;
    expect(tx.verify(), "NFT_CREATE with valid PoL signature verifies");

    auto bad_accuracy = tx;
    bad_accuracy.nft_create->pol_data->gps_accuracy_cm = 0;
    expect(!bad_accuracy.verify(), "NFT PoL verify rejects invalid accuracy");

    auto bad_source = tx;
    bad_source.nft_create->pol_data->source_type = 99;
    expect(!bad_source.verify(), "NFT PoL verify rejects unknown source type");

    auto wrong_signature = tx;
    sign_nft_pol_payload(*wrong_signature.nft_create, make_key(0x91));
    expect(!wrong_signature.verify(), "NFT PoL verify rejects signer mismatch");

    auto empty_signature = tx;
    empty_signature.nft_create->signature.clear();
    expect(!empty_signature.verify(), "NFT PoL verify rejects an empty payload signature");

    auto empty_expected_signer = tx;
    empty_expected_signer.nft_create->creator_address = geocoin::Address();
    sign_nft_pol_payload(*empty_expected_signer.nft_create, creator_key);
    expect(!empty_expected_signer.verify(), "NFT PoL verify rejects an empty expected signer");

    auto changed_royalty = tx;
    ++changed_royalty.nft_create->royalty_percent;
    expect(!changed_royalty.verify(), "NFT PoL signature binds the royalty field");

    auto changed_metadata = tx;
    changed_metadata.nft_create->metadata.location_name += " changed";
    expect(!changed_metadata.verify(), "NFT PoL signature binds all metadata fields");

    auto changed_coordinates = tx;
    ++changed_coordinates.nft_create->pol_data->latitude_ndeg;
    expect(!changed_coordinates.verify(), "NFT PoL signature binds location data");

    auto regular_payload = make_nft_creation_payload(creator_key);
    sign_nft_regular_payload(regular_payload, creator_key);
    Transaction regular_tx;
    regular_tx.version = 2;
    regular_tx.type = TransactionType::NFT_CREATE;
    regular_tx.nft_create = regular_payload;
    expect(regular_tx.verify(), "NFT_CREATE with regular payload signature verifies");

    auto bad_regular_signature = regular_tx;
    sign_nft_regular_payload(*bad_regular_signature.nft_create, make_key(0x92));
    expect(!bad_regular_signature.verify(), "NFT regular payload verify rejects signer mismatch");

    auto empty_regular_signer = regular_tx;
    empty_regular_signer.nft_create->creator_address = geocoin::Address();
    sign_nft_regular_payload(*empty_regular_signer.nft_create, creator_key);
    expect(!empty_regular_signer.verify(), "NFT regular payload verify rejects an empty expected signer");
}

void test_hash_constructor_and_typed_signature_guards()
{
    using geocoin::core::Transaction;
    using geocoin::core::TransactionType;

    Transaction canonical = make_standard_tx();
    canonical.sign(make_key(0xa0));
    expect(
        Transaction::canonical_hash_from_serialized(canonical.serialize()) == canonical.get_hash(),
        "canonical transaction bytes reproduce the transaction hash"
    );
    expect_throws(
        [&]() { (void)canonical.get_angle_commitment_message_hash(); },
        "angle message hash rejects another transaction type"
    );

    Transaction angle = make_angle_transaction();
    const geocoin::Hash256 angle_hash = angle.get_angle_commitment_message_hash();
    angle.outputs.push_back(make_output());
    expect(
        angle.get_angle_commitment_message_hash() != angle_hash,
        "angle message hash binds every output"
    );

    Transaction checkpoint = make_sidechain_transaction(TransactionType::SIDECHAIN_CHECKPOINT);
    const geocoin::bytes checkpoint_payload =
        geocoin::core::build_sidechain_checkpoint_payload_for_signing(checkpoint);
    expect(
        geocoin::core::build_sidechain_checkpoint_payload_for_signing(checkpoint) == checkpoint_payload,
        "checkpoint signing payload is deterministic"
    );
    ++checkpoint.sidechain_checkpoint_height;
    expect(
        geocoin::core::build_sidechain_checkpoint_payload_for_signing(checkpoint) != checkpoint_payload,
        "checkpoint signing payload binds height"
    );
    --checkpoint.sidechain_checkpoint_height;
    checkpoint.sidechain_checkpoint[0] ^= 0x01;
    expect(
        geocoin::core::build_sidechain_checkpoint_payload_for_signing(checkpoint) != checkpoint_payload,
        "checkpoint signing payload binds checkpoint hash"
    );
    checkpoint.sidechain_checkpoint[0] ^= 0x01;
    checkpoint.sidechain_committee_id[0] ^= 0x01;
    expect(
        geocoin::core::build_sidechain_checkpoint_payload_for_signing(checkpoint) != checkpoint_payload,
        "checkpoint signing payload binds committee id"
    );

    const Transaction coinbase = Transaction::create_coinbase(
        1,
        address_from_seed(0xa1),
        geocoin::COIN
    );
    expect(coinbase.verify(), "coinbase verifies without an ordinary witness");

    const geocoin::bytes nft_owner_key = make_key(0xa2);
    const geocoin::Address nft_owner = address_from_seed(0xa2);
    const geocoin::Address nft_recipient = address_from_seed(0xa3);

    geocoin::nft::NFTTransferTransaction transfer;
    transfer.nft_id = hash_from_seed(0xa4);
    transfer.from = nft_owner;
    transfer.to = nft_recipient;
    transfer.fee = 17;
    transfer.signature = sign_payload(transfer.serialize_for_signing(), nft_owner_key);
    Transaction transfer_tx;
    transfer_tx.version = 2;
    transfer_tx.type = TransactionType::NFT_TRANSFER;
    transfer_tx.nft_transfer = transfer;
    expect(transfer_tx.verify(), "NFT transfer verifies with its owner signature");
    auto transfer_with_input = transfer_tx;
    transfer_with_input.inputs.push_back(make_input(0xa5));
    expect(!transfer_with_input.verify(), "NFT transfer rejects an unsigned outer input");

    auto changed_transfer = transfer_tx;
    ++changed_transfer.nft_transfer->fee;
    expect(!changed_transfer.verify(), "NFT transfer signature binds fee");
    changed_transfer = transfer_tx;
    changed_transfer.nft_transfer->to = address_from_seed(0xa5);
    expect(!changed_transfer.verify(), "NFT transfer signature binds recipient");
    changed_transfer = transfer_tx;
    changed_transfer.nft_transfer->nft_id[0] ^= 0x01;
    expect(!changed_transfer.verify(), "NFT transfer signature binds NFT id");
    changed_transfer = transfer_tx;
    changed_transfer.nft_transfer->signature.clear();
    expect(!changed_transfer.verify(), "NFT transfer rejects an empty signature");
    changed_transfer = transfer_tx;
    changed_transfer.nft_transfer->from = geocoin::Address();
    changed_transfer.nft_transfer->signature =
        sign_payload(changed_transfer.nft_transfer->serialize_for_signing(), nft_owner_key);
    expect(!changed_transfer.verify(), "NFT transfer rejects an empty expected signer");

    geocoin::nft::NFTBurnTransaction burn;
    burn.nft_id = hash_from_seed(0xa6);
    burn.owner = nft_owner;
    burn.fee = 19;
    burn.signature = sign_payload(burn.serialize_for_signing(), nft_owner_key);
    Transaction burn_tx;
    burn_tx.version = 2;
    burn_tx.type = TransactionType::NFT_BURN;
    burn_tx.nft_burn = burn;
    expect(burn_tx.verify(), "NFT burn verifies with its owner signature");
    auto burn_with_input = burn_tx;
    burn_with_input.inputs.push_back(make_input(0xa7));
    expect(!burn_with_input.verify(), "NFT burn rejects an unsigned outer input");

    auto changed_burn = burn_tx;
    ++changed_burn.nft_burn->fee;
    expect(!changed_burn.verify(), "NFT burn signature binds fee");
    changed_burn = burn_tx;
    changed_burn.nft_burn->nft_id[0] ^= 0x01;
    expect(!changed_burn.verify(), "NFT burn signature binds NFT id");
    changed_burn = burn_tx;
    changed_burn.nft_burn->signature = sign_payload(
        changed_burn.nft_burn->serialize_for_signing(),
        make_key(0xa7)
    );
    expect(!changed_burn.verify(), "NFT burn rejects another signer");
    changed_burn = burn_tx;
    changed_burn.nft_burn->signature.clear();
    expect(!changed_burn.verify(), "NFT burn rejects an empty signature");

    auto nft_create_with_input = make_nft_creation_payload(nft_owner_key);
    sign_nft_regular_payload(nft_create_with_input, nft_owner_key);
    Transaction create_tx;
    create_tx.version = 2;
    create_tx.type = TransactionType::NFT_CREATE;
    create_tx.nft_create = nft_create_with_input;
    create_tx.inputs.push_back(make_input(0xa8));
    expect(!create_tx.verify(), "NFT create rejects an unsigned outer input");

    Transaction cadastral = make_cadastral_doc_transaction();
    expect(cadastral.verify(), "cadastral document without fee inputs has no ordinary witness");
    cadastral.inputs.push_back(make_input(0xa9));
    expect(!cadastral.verify(), "cadastral document rejects an unsigned fee input");

    const geocoin::bytes sidechain_key = make_key(0xaa);
    Transaction spend = make_sidechain_transaction(TransactionType::SPEND_DEPOSIT);
    spend.sidechain_owner = address_from_seed(0xaa);
    spend.outputs = {make_output(2 * geocoin::consensus::params::DUST_LIMIT)};
    spend.sidechain_amount = geocoin::consensus::params::DUST_LIMIT;
    spend.sidechain_unlock_height = 0;
    spend.sidechain_signature = sign_payload(
        geocoin::core::build_sidechain_payload_for_signing(spend),
        sidechain_key
    );
    expect(spend.verify(), "sidechain spend verifies with its owner signature");

    auto changed_spend = spend;
    ++changed_spend.sidechain_amount;
    expect(!changed_spend.verify(), "sidechain signature binds amount");
    changed_spend = spend;
    ++changed_spend.outputs.front().value;
    expect(!changed_spend.verify(), "sidechain signature binds payouts");
    changed_spend = spend;
    changed_spend.sidechain_signature.clear();
    expect(!changed_spend.verify(), "sidechain spend rejects an empty signature");
    changed_spend = spend;
    changed_spend.sidechain_signature = sign_payload(
        geocoin::core::build_sidechain_payload_for_signing(changed_spend),
        make_key(0xab)
    );
    expect(!changed_spend.verify(), "sidechain spend rejects another signer");
    changed_spend = spend;
    changed_spend.sidechain_owner = geocoin::Address();
    changed_spend.sidechain_signature = sign_payload(
        geocoin::core::build_sidechain_payload_for_signing(changed_spend),
        sidechain_key
    );
    expect(!changed_spend.verify(), "sidechain spend rejects an empty expected signer");

    const geocoin::bytes foundation_key = make_key(0xac);
    Transaction foundation = make_foundation_update_transaction();
    foundation.foundation_signatures.clear();
    expect(!foundation.verify(), "foundation update rejects an empty signature set early");
    foundation.foundation_signatures.push_back(sign_payload(
        geocoin::core::build_foundation_update_payload_for_signing(foundation),
        foundation_key
    ));
    expect(foundation.verify(), "foundation update accepts a well-formed recovered signature");
    auto changed_foundation = foundation;
    changed_foundation.foundation_new_address = address_from_seed(0xad);
    expect(!changed_foundation.verify(), "foundation signature binds the new configuration");
    changed_foundation = foundation;
    changed_foundation.foundation_signatures = {geocoin::Signature{0x01, 0x02}};
    expect(!changed_foundation.verify(), "foundation update rejects a malformed signature");

    const geocoin::bytes angle_key = make_key(0xae);
    const geocoin::Address angle_signer = address_from_seed(0xae);
    expect_throws(
        [&]() {
            (void)Transaction::create_angle_commitment(
                address_from_seed(0xaf), hash_from_seed(0xb0), angle_key, angle_signer, ""
            );
        },
        "angle constructor rejects an empty network id"
    );
    expect_throws(
        [&]() {
            (void)Transaction::create_angle_commitment(
                address_from_seed(0xaf), hash_from_seed(0xb0), angle_key, angle_signer,
                std::string(33, 'n')
            );
        },
        "angle constructor rejects an oversized network id"
    );
    expect_throws(
        [&]() {
            (void)Transaction::create_angle_commitment(
                address_from_seed(0xaf), hash_from_seed(0xb0), geocoin::bytes{0x01},
                geocoin::Address(), "degree"
            );
        },
        "angle constructor rejects an invalid private key"
    );
    expect_throws(
        [&]() {
            (void)Transaction::create_angle_commitment(
                address_from_seed(0xaf), hash_from_seed(0xb0), angle_key,
                address_from_seed(0xb1), "degree"
            );
        },
        "angle constructor rejects a mismatched signer address"
    );
    const Transaction constructed_angle = Transaction::create_angle_commitment(
        address_from_seed(0xaf), hash_from_seed(0xb0), angle_key, angle_signer, "degree"
    );
    expect(constructed_angle.verify(), "constructed angle commitment signature verifies");
    expect(
        constructed_angle.inputs.size() == 1 &&
        constructed_angle.inputs.front().prev_tx_hash ==
            constructed_angle.get_angle_commitment_message_hash(),
        "angle constructor stores its exact signed intent as the replay key"
    );

    const std::vector<geocoin::core::TxInput> multisig_inputs{make_input(0xb2)};
    const std::vector<geocoin::core::TxOutput> multisig_outputs{make_output()};
    const std::vector<geocoin::Hash256> commitments{hash_from_seed(0xb3), hash_from_seed(0xb4)};
    const Transaction multisig = Transaction::create_multisig_spend(
        multisig_inputs, multisig_outputs, commitments, 2, 3
    );
    expect(
        multisig.version == 2 && multisig.type == TransactionType::MULTISIG_SPEND &&
        multisig.inputs.size() == 1 && multisig.outputs.size() == 1 &&
        multisig.inputs.front().serialize() == multisig_inputs.front().serialize() &&
        multisig.outputs.front().serialize() == multisig_outputs.front().serialize() &&
        multisig.angle_commitment_hashes == commitments && multisig.m == 2 && multisig.n == 3,
        "multisig constructor retains the complete requested intent"
    );
    auto alternate_commitments = multisig;
    alternate_commitments.angle_commitment_hashes = {hash_from_seed(0xb5)};
    expect(
        alternate_commitments.get_hash_without_commitments() == multisig.get_hash_without_commitments(),
        "multisig signing hash deliberately ignores only commitment references"
    );

    Transaction hierarchical = make_multisig_spend_transaction(true);
    const geocoin::Hash256 hierarchical_hash = hierarchical.get_hierarchical_spend_hash();
    hierarchical.inputs.push_back(make_input(0xb6));
    expect(
        hierarchical.get_hierarchical_spend_hash() != hierarchical_hash,
        "hierarchical signing hash binds selected inputs"
    );
    hierarchical.inputs.pop_back();
    ++hierarchical.outputs.front().value;
    expect(
        hierarchical.get_hierarchical_spend_hash() != hierarchical_hash,
        "hierarchical signing hash binds every payout"
    );

    Transaction delegation = make_delegation_proof_transaction();
    const geocoin::Hash256 delegation_hash = delegation.get_delegation_proof_hash();
    delegation.angle_commitment_hashes = {hash_from_seed(0xb7)};
    expect(
        delegation.get_delegation_proof_hash() == delegation_hash,
        "delegation signing hash deliberately ignores only commitment references"
    );
    ++delegation.nonce;
    expect(
        delegation.get_delegation_proof_hash() != delegation_hash,
        "delegation signing hash binds delegation parameters"
    );

    Transaction revoke;
    revoke.version = 2;
    revoke.type = TransactionType::REVOKE_DELEGATION;
    revoke.revoked_delegation_proof_txid = hash_from_seed(0xb8);
    revoke.angle_commitment_hashes = {hash_from_seed(0xb9)};
    const geocoin::Hash256 revoke_hash = revoke.get_revoke_delegation_hash();
    revoke.angle_commitment_hashes = {hash_from_seed(0xba), hash_from_seed(0xbb)};
    expect(
        revoke.get_revoke_delegation_hash() == revoke_hash,
        "revocation signing hash deliberately ignores only commitment references"
    );
    revoke.revoked_delegation_proof_txid[0] ^= 0x01;
    expect(
        revoke.get_revoke_delegation_hash() != revoke_hash,
        "revocation signing hash binds the revoked delegation"
    );
}

void test_consensus_typed_transaction_guards()
{
    using geocoin::COIN;
    using geocoin::consensus::params::DUST_LIMIT;
    using geocoin::core::BatchCheckpointItem;
    using geocoin::core::Transaction;
    using geocoin::core::TransactionType;

    const geocoin::Address issuer = address_from_seed(0x40);
    const geocoin::Address owner = address_from_seed(0x41);
    const geocoin::Address recipient = address_from_seed(0x42);
    const std::vector<geocoin::Address> committee = sorted_addresses({issuer, owner, recipient});

    auto card_program = [&]() {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::CARD_PROGRAM_DEFINE;
        tx.inputs.push_back(make_input(0x50));
        tx.card_program_id = hash_from_seed(0x51);
        tx.card_issuer = issuer;
        tx.card_chip_vendor = "NXP";
        tx.card_chip_os = "JCOP";
        tx.card_applet_aid = "A000000001";
        tx.card_applet_version = 1;
        tx.card_applet_package_hash = hash_from_seed(0x52);
        tx.card_policy_id = 7;
        tx.card_scope_id = hash_from_seed(0x53);
        return tx;
    };

    {
        Transaction tx = card_program();
        tx.inputs.push_back(tx.inputs.front());
        expect_check_reject(tx, "bad-txns-inputs-duplicate",
                            "CARD_PROGRAM_DEFINE rejects duplicate inputs");
    }
    {
        Transaction tx = card_program();
        tx.card_program_id = geocoin::Hash256{};
        expect_check_reject(tx, "bad-card-tx", "CARD_PROGRAM_DEFINE rejects null program id");
    }
    {
        Transaction tx = card_program();
        tx.card_issuer = geocoin::Address();
        expect_check_reject(tx, "bad-card-tx", "CARD_PROGRAM_DEFINE rejects empty issuer");
    }
    {
        Transaction tx = card_program();
        tx.card_chip_vendor.clear();
        expect_check_reject(tx, "bad-card-program", "CARD_PROGRAM_DEFINE rejects empty chip vendor");
    }
    {
        Transaction tx = card_program();
        tx.card_chip_os = std::string("JCOP\n", 5);
        expect_check_reject(tx, "bad-card-program", "CARD_PROGRAM_DEFINE rejects non-printable chip OS");
    }
    {
        Transaction tx = card_program();
        tx.card_applet_aid.clear();
        expect_check_reject(tx, "bad-card-program", "CARD_PROGRAM_DEFINE rejects empty applet AID");
    }
    {
        Transaction tx = card_program();
        tx.card_applet_version = 0;
        expect_check_reject(tx, "bad-card-program", "CARD_PROGRAM_DEFINE rejects zero applet version");
    }
    {
        Transaction tx = card_program();
        tx.card_applet_package_hash = geocoin::Hash256{};
        expect_check_reject(tx, "bad-card-program", "CARD_PROGRAM_DEFINE rejects null package hash");
    }
    {
        Transaction tx = card_program();
        tx.card_scope_id = geocoin::Hash256{};
        expect_check_reject(tx, "bad-card-program", "CARD_PROGRAM_DEFINE rejects null scope id");
    }
    {
        Transaction tx = card_program();
        tx.card_uid_hash = hash_from_seed(0x54);
        expect_check_reject(tx, "bad-txns-unused-field", "CARD_PROGRAM_DEFINE rejects card instance hashes");
    }
    {
        Transaction tx = card_program();
        tx.card_status = 1;
        expect_check_reject(tx, "bad-txns-unused-field", "CARD_PROGRAM_DEFINE rejects non-zero status");
    }

    auto card_issue = [&]() {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::CARD_ISSUE;
        tx.inputs.push_back(make_input(0x55));
        tx.card_program_id = hash_from_seed(0x56);
        tx.card_issuer = issuer;
        tx.card_uid_hash = hash_from_seed(0x57);
        tx.card_pubkey_hash = hash_from_seed(0x58);
        tx.card_valid_from = 100;
        tx.card_valid_to = 200;
        tx.card_metadata_hash = hash_from_seed(0x59);
        return tx;
    };

    {
        Transaction tx = card_issue();
        tx.card_uid_hash = geocoin::Hash256{};
        expect_check_reject(tx, "bad-card-issue", "CARD_ISSUE rejects null UID hash");
    }
    {
        Transaction tx = card_issue();
        tx.card_pubkey_hash = geocoin::Hash256{};
        expect_check_reject(tx, "bad-card-issue", "CARD_ISSUE rejects null pubkey hash");
    }
    {
        Transaction tx = card_issue();
        tx.card_valid_from = 0;
        expect_check_reject(tx, "bad-card-issue", "CARD_ISSUE rejects zero valid_from");
    }
    {
        Transaction tx = card_issue();
        tx.card_valid_to = tx.card_valid_from;
        expect_check_reject(tx, "bad-card-issue", "CARD_ISSUE rejects inverted validity range");
    }
    {
        Transaction tx = card_issue();
        tx.card_status = 2;
        expect_check_reject(tx, "bad-card-issue", "CARD_ISSUE rejects non-active status");
    }
    {
        Transaction tx = card_issue();
        tx.card_metadata_hash = geocoin::Hash256{};
        expect_check_reject(tx, "bad-card-issue", "CARD_ISSUE rejects null metadata hash");
    }

    auto card_update = [&]() {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::CARD_UPDATE_STATUS;
        tx.inputs.push_back(make_input(0x5a));
        tx.card_program_id = hash_from_seed(0x5b);
        tx.card_issuer = issuer;
        tx.card_uid_hash = hash_from_seed(0x5c);
        tx.card_status = 1;
        return tx;
    };

    {
        Transaction tx = card_update();
        tx.card_uid_hash = geocoin::Hash256{};
        expect_check_reject(tx, "bad-card-update", "CARD_UPDATE_STATUS rejects null UID hash");
    }
    {
        Transaction tx = card_update();
        tx.card_status = 3;
        expect_check_reject(tx, "bad-card-update", "CARD_UPDATE_STATUS rejects unknown status");
    }

    auto foundation_update = [&]() {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::UPDATE_FOUNDATION_ADDRESS;
        tx.foundation_old_address = issuer;
        tx.foundation_new_address = recipient;
        tx.foundation_m = 1;
        tx.foundation_n = 2;
        tx.foundation_participants = {issuer, owner};
        tx.foundation_new_m = 1;
        tx.foundation_new_n = 1;
        tx.foundation_new_participants = {recipient};
        return tx;
    };

    {
        Transaction tx = foundation_update();
        tx.inputs.push_back(make_input(0x60));
        expect_check_reject(tx, "bad-foundation-update",
                            "UPDATE_FOUNDATION_ADDRESS rejects inputs");
    }
    {
        Transaction tx = foundation_update();
        tx.outputs.push_back(make_output());
        expect_check_reject(tx, "bad-foundation-update",
                            "UPDATE_FOUNDATION_ADDRESS rejects outputs");
    }
    {
        Transaction tx = foundation_update();
        tx.foundation_old_address = geocoin::Address();
        expect_check_reject(tx, "bad-foundation-update",
                            "UPDATE_FOUNDATION_ADDRESS rejects empty old address");
    }
    {
        Transaction tx = foundation_update();
        tx.foundation_m = 0;
        expect_check_reject(tx, "bad-foundation-update",
                            "UPDATE_FOUNDATION_ADDRESS rejects zero old quorum");
    }
    {
        Transaction tx = foundation_update();
        tx.foundation_new_m = 0;
        expect_check_reject(tx, "bad-foundation-update",
                            "UPDATE_FOUNDATION_ADDRESS rejects zero new quorum");
    }
    {
        Transaction tx = foundation_update();
        tx.foundation_n = 16;
        expect_check_reject(tx, "bad-foundation-update",
                            "UPDATE_FOUNDATION_ADDRESS rejects too large old n");
    }
    {
        Transaction tx = foundation_update();
        tx.foundation_participants.pop_back();
        expect_check_reject(tx, "bad-foundation-update",
                            "UPDATE_FOUNDATION_ADDRESS rejects old participant count mismatch");
    }
    {
        Transaction tx = foundation_update();
        tx.foundation_participants = {issuer, issuer};
        expect_check_reject(tx, "bad-foundation-update",
                            "UPDATE_FOUNDATION_ADDRESS rejects duplicate old participants");
    }
    {
        Transaction tx = foundation_update();
        tx.foundation_new_n = 2;
        tx.foundation_new_participants = {recipient, recipient};
        expect_check_reject(tx, "bad-foundation-update",
                            "UPDATE_FOUNDATION_ADDRESS rejects duplicate new participants");
    }

    {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::INTEREST_DISTRIBUTION;
        tx.inputs.push_back(make_input(0x61));
        expect_check_reject(tx, "bad-interest-distribution",
                            "INTEREST_DISTRIBUTION rejects inputs");
    }
    {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::INTEREST_DISTRIBUTION;
        expect_check_reject(tx, "bad-interest-distribution",
                            "INTEREST_DISTRIBUTION rejects empty outputs");
    }
    {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::INTEREST_DISTRIBUTION;
        tx.outputs.push_back(make_output(DUST_LIMIT - 1));
        expect_check_reject(tx, "bad-txns-dust",
                            "INTEREST_DISTRIBUTION still enforces dust on outputs");
    }

    auto batch_item = [&]() {
        BatchCheckpointItem item;
        item.sidechain_id = hash_from_seed(0x62);
        item.checkpoint = hash_from_seed(0x63);
        item.checkpoint_height = 10;
        item.fee = 1;
        item.version = 2;
        item.committee_id = hash_from_seed(0x64);
        item.committee_signatures.push_back(geocoin::Signature{0x01});
        item.proof = geocoin::bytes{0x02};
        return item;
    };
    auto batch_tx = [&](BatchCheckpointItem item) {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        tx.sidechain_batch_checkpoints.push_back(std::move(item));
        return tx;
    };

    {
        Transaction tx = batch_tx(batch_item());
        tx.inputs.push_back(make_input(0x65));
        expect_check_reject(tx, "bad-sidechain-batch", "batch checkpoint rejects inputs");
    }
    {
        Transaction tx = batch_tx(batch_item());
        tx.outputs.push_back(make_output());
        expect_check_reject(tx, "bad-sidechain-batch", "batch checkpoint rejects outputs");
    }
    {
        Transaction tx;
        tx.version = 2;
        tx.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        expect_check_reject(tx, "bad-sidechain-batch", "batch checkpoint rejects empty batch");
    }
    {
        auto item = batch_item();
        item.sidechain_id = geocoin::Hash256{};
        expect_check_reject(batch_tx(item), "bad-sidechain-batch",
                            "batch checkpoint rejects null sidechain id");
    }
    {
        auto item = batch_item();
        item.fee = 0;
        expect_check_reject(batch_tx(item), "bad-sidechain-batch",
                            "batch checkpoint rejects zero fee");
    }
    {
        auto item = batch_item();
        item.checkpoint_height = geocoin::consensus::params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT + 1;
        expect_check_reject(batch_tx(item), "bad-sidechain-batch",
                            "batch checkpoint rejects excessive checkpoint height");
    }
    {
        auto item = batch_item();
        item.version = 3;
        expect_check_reject(batch_tx(item), "bad-sidechain-batch",
                            "batch checkpoint rejects unsupported version");
    }
    {
        auto item = batch_item();
        item.proof.clear();
        expect_check_reject(batch_tx(item), "bad-sidechain-batch",
                            "batch checkpoint rejects empty proof");
    }
    {
        auto item = batch_item();
        item.proof.assign(geocoin::consensus::params::MAX_BATCH_CHECKPOINT_PROOF + 1, 0x03);
        expect_check_reject(batch_tx(item), "bad-sidechain-batch",
                            "batch checkpoint rejects oversized proof");
    }
    {
        auto item = batch_item();
        item.committee_signatures.clear();
        expect_check_reject(batch_tx(item), "bad-sidechain-batch",
                            "batch checkpoint rejects empty committee signatures");
    }
    {
        auto item = batch_item();
        item.committee_signatures.assign(
            geocoin::consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES + 1,
            geocoin::Signature{0x04}
        );
        expect_check_reject(batch_tx(item), "bad-sidechain-batch",
                            "batch checkpoint rejects too many committee signatures");
    }
    {
        Transaction tx = batch_tx(batch_item());
        tx.sidechain_batch_checkpoints.push_back(tx.sidechain_batch_checkpoints.front());
        expect_check_reject(tx, "bad-sidechain-batch",
                            "batch checkpoint rejects duplicate sidechain ids");
    }

    auto sidechain_base = [&](TransactionType type) {
        Transaction tx;
        tx.version = 2;
        tx.type = type;
        tx.sidechain_id = hash_from_seed(0x70);
        tx.sidechain_owner = owner;
        tx.sidechain_signature = geocoin::Signature{0x05};
        return tx;
    };

    {
        Transaction tx = sidechain_base(TransactionType::LOCK_DEPOSIT);
        tx.sidechain_id = geocoin::Hash256{};
        expect_check_reject(tx, "bad-deposit", "sidechain transactions reject null id");
    }
    {
        Transaction tx = sidechain_base(TransactionType::LOCK_DEPOSIT);
        tx.sidechain_owner = geocoin::Address();
        expect_check_reject(tx, "bad-deposit", "sidechain transactions reject empty owner");
    }
    {
        Transaction tx = sidechain_base(TransactionType::LOCK_DEPOSIT);
        tx.sidechain_signature.clear();
        expect_check_reject(tx, "bad-deposit", "LOCK_DEPOSIT rejects empty owner signature");
    }
    {
        Transaction tx = sidechain_base(TransactionType::LOCK_DEPOSIT);
        expect_check_reject(tx, "bad-deposit-lock", "LOCK_DEPOSIT rejects missing inputs");
    }
    {
        Transaction tx = sidechain_base(TransactionType::LOCK_DEPOSIT);
        tx.inputs.push_back(make_input(0x71));
        tx.sidechain_unlock_height = 10;
        expect_check_reject(tx, "bad-deposit-lock", "LOCK_DEPOSIT rejects zero amount");
    }
    {
        Transaction tx = sidechain_base(TransactionType::LOCK_DEPOSIT);
        tx.inputs.push_back(make_input(0x72));
        tx.sidechain_amount = 1;
        expect_check_reject(tx, "bad-deposit-lock", "LOCK_DEPOSIT rejects zero unlock height");
    }
    {
        Transaction tx = sidechain_base(TransactionType::LOCK_DEPOSIT);
        tx.inputs.push_back(make_input(0x73));
        tx.inputs.push_back(tx.inputs.front());
        tx.sidechain_amount = 1;
        tx.sidechain_unlock_height = 10;
        expect_check_reject(tx, "bad-txns-inputs-duplicate",
                            "LOCK_DEPOSIT rejects duplicate inputs");
    }

    {
        Transaction tx = sidechain_base(TransactionType::SPEND_DEPOSIT);
        tx.inputs.push_back(make_input(0x74));
        expect_check_reject(tx, "bad-deposit-spend", "SPEND_DEPOSIT rejects inputs");
    }
    {
        Transaction tx = sidechain_base(TransactionType::SPEND_DEPOSIT);
        expect_check_reject(tx, "bad-deposit-spend", "SPEND_DEPOSIT rejects empty outputs");
    }
    {
        Transaction tx = sidechain_base(TransactionType::SPEND_DEPOSIT);
        tx.outputs.push_back(make_output());
        expect_check_reject(tx, "bad-deposit-spend", "SPEND_DEPOSIT rejects zero amount");
    }
    {
        Transaction tx = sidechain_base(TransactionType::SPEND_DEPOSIT);
        tx.outputs.push_back(make_output());
        tx.sidechain_amount = 1;
        tx.sidechain_unlock_height = 1;
        expect_check_reject(tx, "bad-deposit-spend", "SPEND_DEPOSIT rejects non-zero unlock height");
    }

    {
        Transaction tx = sidechain_base(TransactionType::UNLOCK_DEPOSIT);
        tx.inputs.push_back(make_input(0x75));
        expect_check_reject(tx, "bad-deposit-unlock", "UNLOCK_DEPOSIT rejects inputs");
    }
    {
        Transaction tx = sidechain_base(TransactionType::UNLOCK_DEPOSIT);
        expect_check_reject(tx, "bad-deposit-unlock", "UNLOCK_DEPOSIT rejects empty outputs");
    }
    {
        Transaction tx = sidechain_base(TransactionType::UNLOCK_DEPOSIT);
        tx.outputs.push_back(make_output());
        tx.sidechain_amount = 1;
        expect_check_reject(tx, "bad-deposit-unlock", "UNLOCK_DEPOSIT rejects non-zero amount");
    }
    {
        Transaction tx = sidechain_base(TransactionType::UNLOCK_DEPOSIT);
        tx.outputs.push_back(make_output());
        tx.sidechain_unlock_height = 1;
        expect_check_reject(tx, "bad-deposit-unlock",
                            "UNLOCK_DEPOSIT rejects non-zero unlock height");
    }

    auto issue_sidechain = [&]() {
        Transaction tx = sidechain_base(TransactionType::ISSUE_SIDECHAIN);
        tx.sidechain_checkpoint = tx.sidechain_id;
        tx.sidechain_token_symbol = "SCD";
        tx.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        tx.sidechain_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
        tx.sidechain_fee_bps = 25;
        tx.sidechain_checkpoint_policy_id = 2;
        tx.sidechain_checkpoint_version = 2;
        tx.sidechain_committee_m = 2;
        tx.sidechain_committee_n = static_cast<uint8_t>(committee.size());
        tx.sidechain_committee_members = committee;
        tx.sidechain_committee_id = geocoin::consensus::compute_sidechain_committee_id(
            tx.sidechain_committee_m,
            tx.sidechain_committee_n,
            tx.sidechain_committee_members
        );
        return tx;
    };

    {
        Transaction tx = issue_sidechain();
        tx.inputs.push_back(make_input(0x76));
        expect_check_reject(tx, "bad-sidechain-issue", "ISSUE_SIDECHAIN rejects inputs");
    }
    {
        Transaction tx = issue_sidechain();
        tx.outputs.push_back(make_output());
        expect_check_reject(tx, "bad-sidechain-issue", "ISSUE_SIDECHAIN rejects outputs");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_amount = 1;
        expect_check_reject(tx, "bad-sidechain-issue",
                            "ISSUE_SIDECHAIN rejects non-zero amount");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_checkpoint = hash_from_seed(0x77);
        expect_check_reject(tx, "bad-sidechain-issue",
                            "ISSUE_SIDECHAIN requires checkpoint equal sidechain id");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_token_symbol.clear();
        expect_check_reject(tx, "bad-sidechain-issue", "ISSUE_SIDECHAIN rejects empty token");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_token_symbol = std::string("SC\n", 3);
        expect_check_reject(tx, "bad-sidechain-issue",
                            "ISSUE_SIDECHAIN rejects non-printable token");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER - 1;
        expect_check_reject(tx, "bad-sidechain-issue",
                            "ISSUE_SIDECHAIN rejects too small multiplier");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_fee_bps = geocoin::consensus::params::MAX_SIDECHAIN_FEE_BPS + 1;
        expect_check_reject(tx, "bad-sidechain-issue", "ISSUE_SIDECHAIN rejects high fee bps");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_checkpoint_policy_id = 1;
        expect_check_reject(tx, "bad-sidechain-issue",
                            "ISSUE_SIDECHAIN rejects unsupported checkpoint policy");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_committee_m = 0;
        expect_check_reject(tx, "bad-sidechain-issue",
                            "ISSUE_SIDECHAIN rejects zero committee quorum");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_committee_members.pop_back();
        expect_check_reject(tx, "bad-sidechain-issue",
                            "ISSUE_SIDECHAIN rejects committee count mismatch");
    }
    {
        Transaction tx = issue_sidechain();
        tx.sidechain_committee_members = {committee[0], committee[0], committee[2]};
        expect_check_reject(tx, "bad-sidechain-issue",
                            "ISSUE_SIDECHAIN rejects duplicate committee members");
    }

    auto committee_signed = [&](TransactionType type) {
        Transaction tx = sidechain_base(type);
        tx.sidechain_signature.clear();
        tx.sidechain_amount = 1;
        tx.sidechain_committee_id = hash_from_seed(0x78);
        tx.sidechain_committee_signatures.push_back(geocoin::Signature{0x06});
        return tx;
    };

    {
        Transaction tx = committee_signed(TransactionType::MINT_SIDECHAIN);
        tx.inputs.push_back(make_input(0x79));
        expect_check_reject(tx, "bad-sidechain-mint", "MINT_SIDECHAIN rejects inputs");
    }
    {
        Transaction tx = committee_signed(TransactionType::MINT_SIDECHAIN);
        tx.outputs.push_back(make_output());
        expect_check_reject(tx, "bad-sidechain-mint", "MINT_SIDECHAIN rejects outputs");
    }
    {
        Transaction tx = committee_signed(TransactionType::MINT_SIDECHAIN);
        tx.sidechain_amount = 0;
        expect_check_reject(tx, "bad-sidechain-mint", "MINT_SIDECHAIN rejects zero amount");
    }
    {
        Transaction tx = committee_signed(TransactionType::MINT_SIDECHAIN);
        tx.sidechain_committee_id = geocoin::Hash256{};
        expect_check_reject(tx, "bad-sidechain-mint", "MINT_SIDECHAIN requires committee id");
    }
    {
        Transaction tx = committee_signed(TransactionType::MINT_SIDECHAIN);
        tx.sidechain_committee_signatures.clear();
        expect_check_reject(tx, "bad-sidechain-mint",
                            "MINT_SIDECHAIN requires committee signatures");
    }

    {
        Transaction tx = committee_signed(TransactionType::REDEEM_SIDECHAIN);
        tx.inputs.push_back(make_input(0x7b));
        expect_check_reject(tx, "bad-sidechain-redeem", "REDEEM_SIDECHAIN rejects inputs");
    }
    {
        Transaction tx = committee_signed(TransactionType::REDEEM_SIDECHAIN);
        expect_check_reject(tx, "bad-sidechain-redeem", "REDEEM_SIDECHAIN rejects empty outputs");
    }
    {
        Transaction tx = committee_signed(TransactionType::REDEEM_SIDECHAIN);
        tx.outputs.push_back(make_output());
        tx.sidechain_amount = 0;
        expect_check_reject(tx, "bad-sidechain-redeem", "REDEEM_SIDECHAIN rejects zero amount");
    }
    {
        Transaction tx = committee_signed(TransactionType::REDEEM_SIDECHAIN);
        tx.outputs.push_back(make_output());
        tx.sidechain_committee_id = geocoin::Hash256{};
        expect_check_reject(tx, "bad-sidechain-redeem",
                            "REDEEM_SIDECHAIN requires committee id");
    }
    {
        Transaction tx = committee_signed(TransactionType::REDEEM_SIDECHAIN);
        tx.outputs.push_back(make_output());
        tx.sidechain_committee_signatures.clear();
        expect_check_reject(tx, "bad-sidechain-redeem",
                            "REDEEM_SIDECHAIN requires committee signatures");
    }

    auto slash_deposit = [&]() {
        Transaction tx = committee_signed(TransactionType::SLASH_DEPOSIT);
        tx.sidechain_amount = DUST_LIMIT;
        tx.sidechain_fraud_checkpoint_a = hash_from_seed(0x7c);
        tx.sidechain_fraud_checkpoint_b = hash_from_seed(0x7d);
        tx.sidechain_fraud_checkpoint_index_a = 0;
        tx.sidechain_fraud_checkpoint_index_b = 1;
        return tx;
    };

    {
        Transaction tx = slash_deposit();
        tx.inputs.push_back(make_input(0x7e));
        expect_check_reject(tx, "bad-deposit-slash", "SLASH_DEPOSIT rejects inputs");
    }
    {
        Transaction tx = slash_deposit();
        tx.sidechain_amount = 0;
        expect_check_reject(tx, "bad-deposit-slash", "SLASH_DEPOSIT rejects zero amount");
    }
    {
        Transaction tx = slash_deposit();
        tx.sidechain_committee_id = geocoin::Hash256{};
        expect_check_reject(tx, "bad-deposit-slash", "SLASH_DEPOSIT requires committee id");
    }
    {
        Transaction tx = slash_deposit();
        tx.sidechain_committee_signatures.clear();
        expect_check_reject(tx, "bad-deposit-slash",
                            "SLASH_DEPOSIT requires committee signatures");
    }
    {
        Transaction tx = slash_deposit();
        tx.sidechain_fraud_checkpoint_a = geocoin::Hash256{};
        expect_check_reject(tx, "bad-deposit-slash",
                            "SLASH_DEPOSIT requires fraud proof checkpoints");
    }
    {
        Transaction tx = slash_deposit();
        tx.sidechain_fraud_checkpoint_b = tx.sidechain_fraud_checkpoint_a;
        tx.sidechain_fraud_checkpoint_index_b = tx.sidechain_fraud_checkpoint_index_a;
        expect_check_reject(tx, "bad-deposit-slash",
                            "SLASH_DEPOSIT rejects duplicate fraud proof references");
    }
    {
        Transaction tx = slash_deposit();
        tx.outputs.push_back(make_output(DUST_LIMIT + 1));
        expect_check_reject(tx, "bad-deposit-slash",
                            "SLASH_DEPOSIT rejects outputs above slash amount");
    }

    {
        Transaction tx = committee_signed(TransactionType::SIDECHAIN_CHECKPOINT);
        expect_check_reject(tx, "legacy-sidechain-checkpoint-disabled",
                            "legacy SIDECHAIN_CHECKPOINT is disabled");
    }

    auto update_committee = [&]() {
        Transaction tx = sidechain_base(TransactionType::UPDATE_SIDECHAIN_COMMITTEE);
        tx.sidechain_signature.clear();
        tx.sidechain_committee_m = 2;
        tx.sidechain_committee_n = static_cast<uint8_t>(committee.size());
        tx.sidechain_committee_members = committee;
        tx.sidechain_committee_id = hash_from_seed(0x7f);
        tx.sidechain_committee_signatures.push_back(geocoin::Signature{0x07});
        return tx;
    };

    {
        Transaction tx = update_committee();
        tx.inputs.push_back(make_input(0x80));
        expect_check_reject(tx, "bad-sidechain-committee",
                            "UPDATE_SIDECHAIN_COMMITTEE rejects inputs");
    }
    {
        Transaction tx = update_committee();
        tx.outputs.push_back(make_output());
        expect_check_reject(tx, "bad-sidechain-committee",
                            "UPDATE_SIDECHAIN_COMMITTEE rejects outputs");
    }
    {
        Transaction tx = update_committee();
        tx.sidechain_amount = 1;
        expect_check_reject(tx, "bad-sidechain-committee",
                            "UPDATE_SIDECHAIN_COMMITTEE rejects non-zero amount");
    }
    {
        Transaction tx = update_committee();
        tx.sidechain_committee_m = 0;
        expect_check_reject(tx, "bad-sidechain-committee",
                            "UPDATE_SIDECHAIN_COMMITTEE rejects zero quorum");
    }
    {
        Transaction tx = update_committee();
        tx.sidechain_committee_members.pop_back();
        expect_check_reject(tx, "bad-sidechain-committee",
                            "UPDATE_SIDECHAIN_COMMITTEE rejects count mismatch");
    }
    {
        Transaction tx = update_committee();
        tx.sidechain_committee_members = {committee[0], committee[0], committee[2]};
        expect_check_reject(tx, "bad-sidechain-committee",
                            "UPDATE_SIDECHAIN_COMMITTEE rejects duplicate members");
    }
    {
        Transaction tx = update_committee();
        tx.sidechain_committee_id = geocoin::Hash256{};
        expect_check_reject(tx, "bad-sidechain-committee",
                            "UPDATE_SIDECHAIN_COMMITTEE requires committee id");
    }
    {
        Transaction tx = update_committee();
        tx.sidechain_committee_signatures.clear();
        expect_check_reject(tx, "bad-sidechain-committee",
                            "UPDATE_SIDECHAIN_COMMITTEE requires committee signatures");
    }

    {
        Transaction tx = sidechain_base(TransactionType::DECOMMISSION_SIDECHAIN);
        tx.inputs.push_back(make_input(0x81));
        expect_check_reject(tx, "bad-sidechain-decommission",
                            "DECOMMISSION_SIDECHAIN rejects inputs");
    }
    {
        Transaction tx = sidechain_base(TransactionType::DECOMMISSION_SIDECHAIN);
        tx.outputs.push_back(make_output());
        expect_check_reject(tx, "bad-sidechain-decommission",
                            "DECOMMISSION_SIDECHAIN rejects outputs");
    }
    {
        Transaction tx = sidechain_base(TransactionType::DECOMMISSION_SIDECHAIN);
        tx.sidechain_amount = 1;
        expect_check_reject(tx, "bad-sidechain-decommission",
                            "DECOMMISSION_SIDECHAIN rejects non-zero amount");
    }
    {
        Transaction tx = sidechain_base(TransactionType::DECOMMISSION_SIDECHAIN);
        tx.sidechain_checkpoint = hash_from_seed(0x82);
        expect_check_reject(tx, "bad-sidechain-decommission",
                            "DECOMMISSION_SIDECHAIN rejects non-null checkpoint");
    }
    {
        Transaction tx = sidechain_base(TransactionType::DECOMMISSION_SIDECHAIN);
        expect_check_reject(tx, "bad-sidechain-decommission",
                            "DECOMMISSION_SIDECHAIN requires committee id");
    }
    {
        Transaction tx = sidechain_base(TransactionType::DECOMMISSION_SIDECHAIN);
        tx.sidechain_committee_id = hash_from_seed(0x83);
        expect_check_reject(tx, "bad-sidechain-decommission",
                            "DECOMMISSION_SIDECHAIN requires committee signatures");
    }

    {
        Transaction tx = make_standard_tx();
        tx.outputs.front().value = -1;
        expect_check_reject(tx, "bad-txns-vout-negative",
                            "standard transaction rejects negative output");
    }
    {
        Transaction tx = make_standard_tx();
        tx.outputs.front().value = DUST_LIMIT - 1;
        expect_check_reject(tx, "bad-txns-dust", "standard transaction rejects dust output");
    }
    {
        Transaction tx = make_standard_tx();
        tx.outputs.front().value = 21000000LL * COIN + 1;
        expect_check_reject(tx, "bad-txns-txouttotal-toolarge",
                            "standard transaction rejects output above max money");
    }
    {
        Transaction tx = make_standard_tx();
        const geocoin::Address recipient = tx.outputs.front().address;
        tx.outputs.clear();
        tx.outputs.push_back({(std::numeric_limits<geocoin::Amount>::max)(), recipient, {}});
        tx.outputs.push_back({DUST_LIMIT, recipient, {}});
        expect_check_reject(tx, "bad-txns-txouttotal-toolarge",
                            "standard transaction rejects signed output total overflow");
    }
}

void test_delegation_limits_are_consensus_bounded()
{
    auto valid = make_delegation_proof_transaction();
    valid.inputs.push_back(make_input(0xd0));

    geocoin::consensus::ValidationState state;
    expect(geocoin::consensus::check_transaction(valid, state),
           "well-formed delegation limits pass consensus checks");

    {
        auto tx = valid;
        tx.max_total_delegated = -1;
        expect_check_reject(tx, "bad-delegation-proof",
                            "negative total delegation limit cannot disable enforcement");
    }
    {
        auto tx = valid;
        tx.max_per_transaction = -1;
        expect_check_reject(tx, "bad-delegation-proof",
                            "negative per-transaction delegation limit is rejected");
    }
    {
        auto tx = valid;
        tx.max_per_transaction = tx.max_total_delegated + 1;
        expect_check_reject(tx, "bad-delegation-proof",
                            "per-transaction delegation limit cannot exceed total limit");
    }
    {
        auto tx = valid;
        tx.max_percent_of_balance = 101;
        expect_check_reject(tx, "bad-delegation-proof",
                            "delegation percentage cannot exceed 100 percent");
    }
    {
        auto tx = valid;
        tx.valid_from = 200;
        tx.valid_until = 100;
        expect_check_reject(tx, "bad-delegation-proof",
                            "delegation validity interval cannot be inverted");
    }
}

} // namespace

int main()
{
    try {
        test_serialize_and_signing_guards();
        test_verify_input_guards();
        test_deserialize_witness_guards();
        test_deserialize_structured_roundtrips();
        test_deserialize_size_limit_guards();
        test_transaction_type_verify_guards();
        test_nft_pol_signature_verify_guards();
        test_hash_constructor_and_typed_signature_guards();
        test_consensus_typed_transaction_guards();
        test_delegation_limits_are_consensus_bounded();
        std::cout << "test_transaction_guard_units: ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "test_transaction_guard_units failed: " << ex.what() << "\n";
        return 1;
    }
}
