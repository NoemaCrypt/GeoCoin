#include "geocoin/state/state.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_throws(const std::function<void()>& fn, const char* message)
{
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 19);
    }
    return hash;
}

std::string hash_to_hex(const geocoin::Hash256& hash)
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : hash) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

void append_u16(geocoin::bytes& data, uint16_t value)
{
    data.push_back(static_cast<uint8_t>(value & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void append_u32(geocoin::bytes& data, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        data.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void append_u64(geocoin::bytes& data, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void append_hash(geocoin::bytes& data, const geocoin::Hash256& hash)
{
    data.insert(data.end(), hash.begin(), hash.end());
}

void append_string(geocoin::bytes& data, const std::string& value)
{
    append_u32(data, static_cast<uint32_t>(value.size()));
    data.insert(data.end(), value.begin(), value.end());
}

geocoin::bytes serialize_foundation_config_for_test(const geocoin::state::FoundationConfig& config)
{
    geocoin::bytes data;
    append_string(data, config.address.str());
    data.push_back(config.m);
    data.push_back(config.n);
    append_u32(data, static_cast<uint32_t>(config.participants.size()));
    for (const auto& participant : config.participants) {
        append_string(data, participant.str());
    }
    return data;
}

geocoin::bytes serialize_card_program_for_test(const geocoin::state::CardProgramEntry& entry)
{
    geocoin::bytes data;
    append_string(data, entry.issuer.str());
    append_string(data, entry.chip_vendor);
    append_string(data, entry.chip_os);
    append_string(data, entry.applet_aid);
    append_u32(data, entry.applet_version);
    append_hash(data, entry.applet_package_hash);
    append_u32(data, entry.policy_id);
    append_hash(data, entry.scope_id);
    append_u64(data, static_cast<uint64_t>(entry.created_height));
    data.push_back(0); // Explicit current corporate-policy version.
    return data;
}

geocoin::bytes serialize_card_instance_for_test(const geocoin::state::CardInstanceEntry& entry)
{
    geocoin::bytes data;
    append_hash(data, entry.program_id);
    append_string(data, entry.issuer.str());
    append_hash(data, entry.uid_hash);
    append_hash(data, entry.pubkey_hash);
    append_u64(data, entry.valid_from);
    append_u64(data, entry.valid_to);
    data.push_back(entry.status);
    append_hash(data, entry.metadata_hash);
    append_u64(data, static_cast<uint64_t>(entry.issued_height));
    append_u64(data, static_cast<uint64_t>(entry.updated_height));
    return data;
}

geocoin::bytes serialize_sidechain_deposit_for_test(const geocoin::state::SidechainDeposit& deposit)
{
    geocoin::bytes data;
    append_string(data, deposit.owner.str());
    append_u64(data, static_cast<uint64_t>(deposit.amount));
    append_u64(data, static_cast<uint64_t>(deposit.unlock_height));
    append_hash(data, deposit.checkpoint);
    append_u64(data, static_cast<uint64_t>(deposit.last_checkpoint_height));
    append_u64(data, static_cast<uint64_t>(deposit.last_sidechain_checkpoint_height));
    return data;
}

geocoin::bytes serialize_sidechain_registry_for_test(
    const geocoin::state::SidechainRegistryEntry& entry
)
{
    geocoin::bytes data;
    append_string(data, entry.owner.str());
    append_u64(data, static_cast<uint64_t>(entry.issuance_limit));
    append_u64(data, static_cast<uint64_t>(entry.total_issued));
    append_u32(data, entry.multiplier);
    append_u32(data, entry.fee_bps);
    append_u32(data, entry.max_multiplier);
    data.push_back(entry.checkpoint_policy_id);
    data.push_back(entry.checkpoint_version);
    append_string(data, entry.token_symbol);
    data.push_back(entry.committee_m);
    data.push_back(entry.committee_n);
    append_u32(data, static_cast<uint32_t>(entry.committee_members.size()));
    for (const auto& member : entry.committee_members) {
        append_string(data, member.str());
    }
    append_hash(data, entry.committee_id);
    data.push_back(0xA3);
    data.push_back(1);
    data.push_back(static_cast<uint8_t>(entry.kind));
    data.push_back(static_cast<uint8_t>(entry.status));
    append_u64(data, static_cast<uint64_t>(entry.decommission_height));
    append_u32(data, static_cast<uint32_t>(entry.accounting_namespace.size()));
    data.insert(
        data.end(),
        entry.accounting_namespace.begin(),
        entry.accounting_namespace.end()
    );
    append_hash(data, entry.accounting_schema_hash);
    return data;
}

geocoin::state::UTXO make_utxo(uint8_t seed, uint32_t vout)
{
    geocoin::state::UTXO utxo;
    utxo.tx_hash = hash_from_seed(seed);
    utxo.vout = vout;
    utxo.value = static_cast<geocoin::Amount>((seed + 1) * geocoin::COIN);
    utxo.address = geocoin::Address("GEOunit" + std::to_string(seed));
    utxo.height = 100 + seed;
    utxo.is_coinbase = (seed % 2) == 0;
    return utxo;
}

geocoin::state::TxUndo make_full_tx_undo()
{
    geocoin::state::TxUndo undo;
    undo.spent_utxos = {make_utxo(1, 0), make_utxo(2, 3)};

    undo.nft_undo.kind = 3;
    undo.nft_undo.nft_id = hash_from_seed(0x10);
    undo.nft_undo.prev_owner = "GEOnft-owner";
    undo.nft_undo.prev_metadata = {1, 2, 3, 4, 5};

    undo.deposit_undo.kind = 2;
    undo.deposit_undo.sidechain_id = hash_from_seed(0x20);
    undo.deposit_undo.prev_owner = "GEOdeposit-owner";
    undo.deposit_undo.prev_amount = 7 * geocoin::COIN;
    undo.deposit_undo.prev_unlock_height = 250;
    undo.deposit_undo.prev_checkpoint = hash_from_seed(0x21);
    undo.deposit_undo.prev_last_checkpoint_height = 251;
    undo.deposit_undo.prev_last_sidechain_checkpoint_height = 252;

    geocoin::state::TxUndo::DepositUndo empty_deposit;
    geocoin::state::TxUndo::DepositUndo batch_deposit;
    batch_deposit.kind = 3;
    batch_deposit.sidechain_id = hash_from_seed(0x22);
    batch_deposit.prev_owner = "GEObatch-owner";
    batch_deposit.prev_amount = 9 * geocoin::COIN;
    batch_deposit.prev_unlock_height = 350;
    batch_deposit.prev_checkpoint = hash_from_seed(0x23);
    batch_deposit.prev_last_checkpoint_height = 351;
    batch_deposit.prev_last_sidechain_checkpoint_height = 352;
    undo.deposit_undo_batch = {empty_deposit, batch_deposit};

    undo.sidechain_undo.kind = 2;
    undo.sidechain_undo.sidechain_id = hash_from_seed(0x30);
    undo.sidechain_undo.prev_owner = "GEOside-owner";
    undo.sidechain_undo.prev_limit = 100 * geocoin::COIN;
    undo.sidechain_undo.prev_issued = 12 * geocoin::COIN;
    undo.sidechain_undo.prev_kind = geocoin::core::SidechainKind::ACCOUNTING;
    undo.sidechain_undo.prev_status = geocoin::state::SidechainStatus::DECOMMISSIONING;
    undo.sidechain_undo.prev_decommission_height = 333;
    undo.sidechain_undo.prev_multiplier = 2;
    undo.sidechain_undo.prev_fee_bps = 25;
    undo.sidechain_undo.prev_max_multiplier = 10;
    undo.sidechain_undo.prev_symbol = "TST";
    undo.sidechain_undo.prev_accounting_namespace = "unit-registry";
    undo.sidechain_undo.prev_accounting_schema_hash = hash_from_seed(0x32);
    undo.sidechain_undo.prev_committee_m = 2;
    undo.sidechain_undo.prev_committee_n = 3;
    undo.sidechain_undo.prev_committee_members = {"GEOmember1", "GEOmember2", "GEOmember3"};
    undo.sidechain_undo.prev_committee_id = hash_from_seed(0x31);
    undo.sidechain_undo.prev_checkpoint_policy_id = 7;
    undo.sidechain_undo.prev_checkpoint_version = 9;

    undo.foundation_undo.kind = 1;
    undo.foundation_undo.prev_address = "GEOfoundation";
    undo.foundation_undo.prev_m = 2;
    undo.foundation_undo.prev_n = 4;
    undo.foundation_undo.prev_participants = {"GEOf1", "GEOf2", "GEOf3", "GEOf4"};

    undo.card_undo.kind = 3;
    undo.card_undo.program_id = hash_from_seed(0x40);
    undo.card_undo.card_uid_hash = hash_from_seed(0x41);
    undo.card_undo.card_pubkey_hash = hash_from_seed(0x42);
    undo.card_undo.prev_issuer = "GEOissuer";
    undo.card_undo.prev_valid_from = 1000;
    undo.card_undo.prev_valid_to = 2000;
    undo.card_undo.prev_status = 2;
    undo.card_undo.prev_metadata_hash = hash_from_seed(0x43);
    undo.card_undo.prev_issued_height = 55;
    undo.card_undo.prev_updated_height = 66;

    undo.added_witness_pubkeys = {hash_from_seed(0x50), hash_from_seed(0x51)};
    return undo;
}

void assert_utxo_equal(const geocoin::state::UTXO& a, const geocoin::state::UTXO& b)
{
    expect(a.tx_hash == b.tx_hash, "utxo tx hash mismatch");
    expect(a.vout == b.vout, "utxo vout mismatch");
    expect(a.value == b.value, "utxo value mismatch");
    expect(a.address == b.address, "utxo address mismatch");
    expect(a.height == b.height, "utxo height mismatch");
    expect(a.is_coinbase == b.is_coinbase, "utxo coinbase flag mismatch");
}

void assert_tx_undo_equal(const geocoin::state::TxUndo& a, const geocoin::state::TxUndo& b)
{
    expect(a.spent_utxos.size() == b.spent_utxos.size(), "spent utxo count mismatch");
    for (size_t i = 0; i < a.spent_utxos.size(); ++i) {
        assert_utxo_equal(a.spent_utxos[i], b.spent_utxos[i]);
    }

    expect(a.nft_undo.kind == b.nft_undo.kind, "nft undo kind mismatch");
    expect(a.nft_undo.nft_id == b.nft_undo.nft_id, "nft undo id mismatch");
    expect(a.nft_undo.prev_owner == b.nft_undo.prev_owner, "nft undo owner mismatch");
    expect(a.nft_undo.prev_metadata == b.nft_undo.prev_metadata, "nft undo metadata mismatch");

    expect(a.deposit_undo.kind == b.deposit_undo.kind, "deposit undo kind mismatch");
    expect(a.deposit_undo.sidechain_id == b.deposit_undo.sidechain_id, "deposit undo id mismatch");
    expect(a.deposit_undo.prev_owner == b.deposit_undo.prev_owner, "deposit undo owner mismatch");
    expect(a.deposit_undo.prev_amount == b.deposit_undo.prev_amount, "deposit undo amount mismatch");
    expect(a.deposit_undo.prev_unlock_height == b.deposit_undo.prev_unlock_height, "deposit undo unlock mismatch");
    expect(a.deposit_undo.prev_checkpoint == b.deposit_undo.prev_checkpoint, "deposit undo checkpoint mismatch");
    expect(a.deposit_undo.prev_last_checkpoint_height == b.deposit_undo.prev_last_checkpoint_height,
           "deposit undo checkpoint height mismatch");
    expect(a.deposit_undo.prev_last_sidechain_checkpoint_height == b.deposit_undo.prev_last_sidechain_checkpoint_height,
           "deposit undo sidechain checkpoint height mismatch");

    expect(a.deposit_undo_batch.size() == b.deposit_undo_batch.size(), "deposit undo batch count mismatch");
    expect(a.deposit_undo_batch[0].kind == b.deposit_undo_batch[0].kind, "empty deposit batch kind mismatch");
    expect(a.deposit_undo_batch[1].sidechain_id == b.deposit_undo_batch[1].sidechain_id,
           "deposit batch id mismatch");
    expect(a.deposit_undo_batch[1].prev_owner == b.deposit_undo_batch[1].prev_owner,
           "deposit batch owner mismatch");
    expect(a.deposit_undo_batch[1].prev_last_sidechain_checkpoint_height ==
               b.deposit_undo_batch[1].prev_last_sidechain_checkpoint_height,
           "deposit batch sidechain checkpoint height mismatch");

    expect(a.sidechain_undo.kind == b.sidechain_undo.kind, "sidechain undo kind mismatch");
    expect(a.sidechain_undo.sidechain_id == b.sidechain_undo.sidechain_id, "sidechain undo id mismatch");
    expect(a.sidechain_undo.prev_owner == b.sidechain_undo.prev_owner, "sidechain undo owner mismatch");
    expect(a.sidechain_undo.prev_limit == b.sidechain_undo.prev_limit, "sidechain undo limit mismatch");
    expect(a.sidechain_undo.prev_issued == b.sidechain_undo.prev_issued, "sidechain undo issued mismatch");
    expect(a.sidechain_undo.prev_kind == b.sidechain_undo.prev_kind,
           "sidechain undo kind metadata mismatch");
    expect(a.sidechain_undo.prev_status == b.sidechain_undo.prev_status,
           "sidechain undo status mismatch");
    expect(a.sidechain_undo.prev_decommission_height == b.sidechain_undo.prev_decommission_height,
           "sidechain undo decommission height mismatch");
    expect(a.sidechain_undo.prev_multiplier == b.sidechain_undo.prev_multiplier, "sidechain undo multiplier mismatch");
    expect(a.sidechain_undo.prev_fee_bps == b.sidechain_undo.prev_fee_bps, "sidechain undo fee mismatch");
    expect(a.sidechain_undo.prev_max_multiplier == b.sidechain_undo.prev_max_multiplier,
           "sidechain undo max multiplier mismatch");
    expect(a.sidechain_undo.prev_symbol == b.sidechain_undo.prev_symbol, "sidechain undo symbol mismatch");
    expect(a.sidechain_undo.prev_accounting_namespace == b.sidechain_undo.prev_accounting_namespace,
           "sidechain undo accounting namespace mismatch");
    expect(a.sidechain_undo.prev_accounting_schema_hash == b.sidechain_undo.prev_accounting_schema_hash,
           "sidechain undo accounting schema mismatch");
    expect(a.sidechain_undo.prev_committee_members == b.sidechain_undo.prev_committee_members,
           "sidechain undo committee members mismatch");
    expect(a.sidechain_undo.prev_committee_id == b.sidechain_undo.prev_committee_id,
           "sidechain undo committee id mismatch");
    expect(a.sidechain_undo.prev_checkpoint_policy_id == b.sidechain_undo.prev_checkpoint_policy_id,
           "sidechain undo checkpoint policy mismatch");
    expect(a.sidechain_undo.prev_checkpoint_version == b.sidechain_undo.prev_checkpoint_version,
           "sidechain undo checkpoint version mismatch");

    expect(a.foundation_undo.kind == b.foundation_undo.kind, "foundation undo kind mismatch");
    expect(a.foundation_undo.prev_address == b.foundation_undo.prev_address, "foundation address mismatch");
    expect(a.foundation_undo.prev_participants == b.foundation_undo.prev_participants,
           "foundation participants mismatch");

    expect(a.card_undo.kind == b.card_undo.kind, "card undo kind mismatch");
    expect(a.card_undo.card_uid_hash == b.card_undo.card_uid_hash, "card uid mismatch");
    expect(a.card_undo.prev_issuer == b.card_undo.prev_issuer, "card issuer mismatch");
    expect(a.card_undo.prev_metadata_hash == b.card_undo.prev_metadata_hash, "card metadata hash mismatch");

    expect(a.added_witness_pubkeys == b.added_witness_pubkeys, "witness pubkey cache undo mismatch");
}

void test_utxo_roundtrip_and_errors()
{
    const auto utxo = make_utxo(3, 9);
    const auto serialized = utxo.serialize();
    const auto decoded = geocoin::state::UTXO::deserialize(serialized);
    assert_utxo_equal(utxo, decoded);
    expect(utxo.get_key().find(":9") != std::string::npos, "utxo key contains vout");
    const auto high_vout = make_utxo(3, 10);
    expect(high_vout.get_key().find(":10") != std::string::npos, "utxo key prints vout in decimal");
    expect(high_vout.get_key().find(":a") == std::string::npos, "utxo key does not print vout in hex");
    expect(make_utxo(3, 26).get_key().find(":1a") == std::string::npos,
           "utxo key does not print vout 26 as hex");
    expect(make_utxo(3, 26).get_key().find(":26") != std::string::npos,
           "utxo key prints vout 26 in decimal");

    geocoin::state::UTXO too_long_address = utxo;
    too_long_address.address = geocoin::Address(std::string(256, 'x'));
    expect_throws([&]() { (void)too_long_address.serialize(); }, "oversized UTXO address must throw");

    expect_throws([]() { (void)geocoin::state::UTXO::deserialize({1, 2, 3}); },
                  "truncated UTXO must throw");

    auto trailing = serialized;
    trailing.push_back(0);
    expect_throws([&]() { (void)geocoin::state::UTXO::deserialize(trailing); },
                  "UTXO trailing data must throw");

    auto negative_value = serialized;
    std::fill(negative_value.begin() + 36, negative_value.begin() + 44, 0xff);
    expect_throws([&]() { (void)geocoin::state::UTXO::deserialize(negative_value); },
                  "negative serialized UTXO value must throw");
    auto negative_utxo = utxo;
    negative_utxo.value = -1;
    expect_throws([&]() { (void)negative_utxo.serialize(); },
                  "negative in-memory UTXO value must not be persisted");

    auto oversized_address = serialized;
    oversized_address[44] = static_cast<uint8_t>(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1);
    expect_throws([&]() { (void)geocoin::state::UTXO::deserialize(oversized_address); },
                  "oversized serialized UTXO address must throw");

    auto zero_value = utxo;
    zero_value.value = 0;
    zero_value.address = geocoin::Address("");
    const auto zero_roundtrip = geocoin::state::UTXO::deserialize(zero_value.serialize());
    expect(zero_roundtrip.value == 0 && zero_roundtrip.address.str().empty(),
           "zero-value script-only UTXO remains compatible");

    auto invalid_coinbase = serialized;
    invalid_coinbase.back() = 2;
    expect_throws([&]() { (void)geocoin::state::UTXO::deserialize(invalid_coinbase); },
                  "non-canonical serialized UTXO coinbase flag must throw");

    expect_throws([&]() {
        auto storage = std::make_unique<geocoin::state::MemoryStorage>();
        expect(storage->put_metadata(utxo.get_key(), negative_value),
               "corrupt UTXO test record stores");
        geocoin::state::ChainState state(std::move(storage));
        (void)state;
    }, "ChainState must fail closed instead of loading a partial corrupt UTXO snapshot");
}

void test_tx_undo_roundtrip_and_legacy_deposit()
{
    const auto undo = make_full_tx_undo();
    const auto decoded = geocoin::state::TxUndo::deserialize(undo.serialize());
    assert_tx_undo_equal(undo, decoded);

    geocoin::bytes minimal;
    append_u32(minimal, 0);
    expect_throws([&] { (void)geocoin::state::TxUndo::deserialize(minimal); }, "unsupported minimal TxUndo is rejected");

    geocoin::bytes legacy_deposit;
    append_u32(legacy_deposit, 0);
    legacy_deposit.push_back(0); // nft kind
    legacy_deposit.push_back(2); // deposit kind
    append_hash(legacy_deposit, hash_from_seed(0x60));
    append_string(legacy_deposit, "GEOlegacy-deposit");
    append_u64(legacy_deposit, static_cast<uint64_t>(5 * geocoin::COIN));
    append_u64(legacy_deposit, 500);
    append_hash(legacy_deposit, hash_from_seed(0x61));
    legacy_deposit.push_back(0xBC);
    append_u32(legacy_deposit, 0);

    expect_throws([&] { (void)geocoin::state::TxUndo::deserialize(legacy_deposit); }, "missing checkpoint heights are rejected");

    geocoin::bytes invalid_amount = legacy_deposit;
    const size_t amount_offset = 4 + 1 + 1 + 32 + 4 + std::string("GEOlegacy-deposit").size();
    std::fill(invalid_amount.begin() + static_cast<std::ptrdiff_t>(amount_offset),
              invalid_amount.begin() + static_cast<std::ptrdiff_t>(amount_offset + 8),
              0xff);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(invalid_amount); },
                  "out-of-range persisted deposit amount must throw");

    auto negative_undo = undo;
    negative_undo.deposit_undo.prev_amount = -1;
    expect_throws([&]() { (void)negative_undo.serialize(); },
                  "negative in-memory undo amount must not be persisted");
}

void test_tx_undo_error_paths()
{
    geocoin::bytes invalid_count;
    append_u32(invalid_count, 3);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(invalid_count); },
                  "invalid TxUndo UTXO count must throw");

    geocoin::bytes empty_utxo;
    append_u32(empty_utxo, 1);
    append_u16(empty_utxo, 0);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(empty_utxo); },
                  "empty TxUndo UTXO payload must throw");

    geocoin::bytes invalid_nft;
    append_u32(invalid_nft, 0);
    invalid_nft.push_back(4);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(invalid_nft); },
                  "invalid NFT undo kind must throw");

    geocoin::bytes invalid_deposit;
    append_u32(invalid_deposit, 0);
    invalid_deposit.push_back(0);
    invalid_deposit.push_back(4);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(invalid_deposit); },
                  "invalid deposit undo kind must throw");

    geocoin::bytes invalid_batch_count;
    append_u32(invalid_batch_count, 0);
    invalid_batch_count.push_back(0);
    invalid_batch_count.push_back(0);
    invalid_batch_count.push_back(0xBC);
    append_u32(invalid_batch_count, 1);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(invalid_batch_count); },
                  "invalid deposit batch count must throw");

    geocoin::bytes invalid_sidechain;
    append_u32(invalid_sidechain, 0);
    invalid_sidechain.push_back(0);
    invalid_sidechain.push_back(0);
    invalid_sidechain.push_back(3);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(invalid_sidechain); },
                  "invalid sidechain undo kind must throw");

    geocoin::bytes invalid_foundation;
    append_u32(invalid_foundation, 0);
    invalid_foundation.push_back(0);
    invalid_foundation.push_back(0);
    invalid_foundation.push_back(0);
    invalid_foundation.push_back(2);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(invalid_foundation); },
                  "invalid foundation undo kind must throw");

    geocoin::bytes invalid_card;
    append_u32(invalid_card, 0);
    invalid_card.push_back(0);
    invalid_card.push_back(0);
    invalid_card.push_back(0);
    invalid_card.push_back(0);
    invalid_card.push_back(0xC4);
    invalid_card.push_back(4);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(invalid_card); },
                  "invalid card undo kind must throw");

    geocoin::bytes invalid_witness_count;
    append_u32(invalid_witness_count, 0);
    invalid_witness_count.push_back(0);
    invalid_witness_count.push_back(0);
    invalid_witness_count.push_back(0);
    invalid_witness_count.push_back(0);
    invalid_witness_count.push_back(0xD1);
    append_u32(invalid_witness_count, 1);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(invalid_witness_count); },
                  "invalid witness pubkey count must throw");

    geocoin::bytes sidechain_policy_without_undo;
    append_u32(sidechain_policy_without_undo, 0);
    sidechain_policy_without_undo.push_back(0);
    sidechain_policy_without_undo.push_back(0);
    sidechain_policy_without_undo.push_back(0);
    sidechain_policy_without_undo.push_back(0);
    sidechain_policy_without_undo.push_back(0xD2);
    sidechain_policy_without_undo.push_back(7);
    sidechain_policy_without_undo.push_back(9);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(sidechain_policy_without_undo); },
                  "sidechain policy tag without sidechain undo must throw");

    geocoin::bytes trailing;
    append_u32(trailing, 0);
    trailing.push_back(0);
    trailing.push_back(0);
    trailing.push_back(0);
    trailing.push_back(0);
    trailing.push_back(0xaa);
    expect_throws([&]() { (void)geocoin::state::TxUndo::deserialize(trailing); },
                  "TxUndo trailing data must throw");
}

void test_block_undo_roundtrip_legacy_and_errors()
{
    geocoin::state::BlockUndo block_undo;
    block_undo.height = 777;
    block_undo.block_hash = hash_from_seed(0x70);
    block_undo.tx_undos = {make_full_tx_undo()};
    block_undo.interest_pool_delta = 123456;

    const auto decoded = geocoin::state::BlockUndo::deserialize(block_undo.serialize());
    expect(decoded.height == block_undo.height, "BlockUndo height mismatch");
    expect(decoded.block_hash == block_undo.block_hash, "BlockUndo hash mismatch");
    expect(decoded.interest_pool_delta == block_undo.interest_pool_delta,
           "BlockUndo interest pool delta mismatch");
    expect(decoded.tx_undos.size() == 1, "BlockUndo tx undo count mismatch");
    assert_tx_undo_equal(block_undo.tx_undos[0], decoded.tx_undos[0]);

    block_undo.interest_pool_delta = -123456;
    const auto negative_delta = geocoin::state::BlockUndo::deserialize(block_undo.serialize());
    expect(negative_delta.interest_pool_delta == block_undo.interest_pool_delta,
           "negative BlockUndo interest delta has a portable signed roundtrip");

    geocoin::bytes legacy;
    append_u64(legacy, 5);
    append_hash(legacy, hash_from_seed(0x71));
    append_u32(legacy, 0);
    expect_throws([&] { (void)geocoin::state::BlockUndo::deserialize(legacy); }, "missing interest delta is rejected");

    expect_throws([]() { (void)geocoin::state::BlockUndo::deserialize({1, 2, 3}); },
                  "truncated BlockUndo must throw");

    geocoin::bytes invalid_count;
    append_u64(invalid_count, 9);
    append_hash(invalid_count, hash_from_seed(0x72));
    append_u32(invalid_count, 4);
    expect_throws([&]() { (void)geocoin::state::BlockUndo::deserialize(invalid_count); },
                  "invalid BlockUndo tx count must throw");

    geocoin::bytes empty_payload;
    append_u64(empty_payload, 9);
    append_hash(empty_payload, hash_from_seed(0x73));
    append_u32(empty_payload, 1);
    append_u32(empty_payload, 0);
    expect_throws([&]() { (void)geocoin::state::BlockUndo::deserialize(empty_payload); },
                  "empty BlockUndo tx payload must throw");

    geocoin::bytes trailing;
    append_u64(trailing, 9);
    append_hash(trailing, hash_from_seed(0x74));
    append_u32(trailing, 0);
    trailing.push_back(0xaa);
    expect_throws([&]() { (void)geocoin::state::BlockUndo::deserialize(trailing); },
                  "BlockUndo trailing data must throw");
}

void test_noncanonical_utxo_keys_are_rejected()
{
    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    const auto output = make_utxo(3, 10);
    expect(storage->put_metadata("idx_epoch", geocoin::bytes(32, 'a')), "seed valid epoch");
    expect(storage->put_metadata("utxo:" + hash_to_hex(output.tx_hash) + ":a", output.serialize()), "seed invalid outpoint key");
    expect_throws([&] { geocoin::state::ChainState state(std::move(storage)); },
                  "loader rejects noncanonical keys instead of converting them");
}

void test_chainstate_persistence_metadata_loads()
{
    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* storage_ptr = storage.get();
    const std::string valid_epoch = "0123456789abcdef0123456789abcdef";
    expect(storage_ptr->put_metadata(
               "idx_epoch",
               geocoin::bytes(valid_epoch.begin(), valid_epoch.end())
           ),
           "valid state index epoch stored");

    const std::vector<geocoin::Address> foundation_participants = {
        geocoin::test::address_from_seed(0xb0),
        geocoin::test::address_from_seed(0xb1),
        geocoin::test::address_from_seed(0xb2),
    };
    const auto multisig = geocoin::wallet::create_multisig_address(2, 3, foundation_participants);
    geocoin::state::FoundationConfig foundation;
    foundation.address = geocoin::Address(multisig.address);
    foundation.m = 2;
    foundation.n = 3;
    foundation.participants = foundation_participants;
    expect(storage_ptr->put_metadata("foundation_cfg", serialize_foundation_config_for_test(foundation)),
           "valid foundation config metadata stored");

    geocoin::bytes issuer_key(32);
    for (size_t i = 0; i < issuer_key.size(); ++i) {
        issuer_key[i] = static_cast<geocoin::byte>(0x41 + i);
    }
    geocoin::Address issuer;
    expect(geocoin::crypto::compute_address_from_private_key(issuer_key, issuer),
           "card persistence issuer address derives from test key");
    const geocoin::Hash256 program_id = hash_from_seed(0x90);
    const geocoin::Hash256 uid_hash = hash_from_seed(0x91);
    const geocoin::Hash256 pubkey_hash = hash_from_seed(0x92);
    const geocoin::Hash256 scope_id = hash_from_seed(0x93);

    geocoin::state::CardProgramEntry program;
    program.issuer = issuer;
    program.chip_vendor = "NXP";
    program.chip_os = "JCOP4";
    program.applet_aid = "A000000001";
    program.applet_version = 7;
    program.applet_package_hash = hash_from_seed(0x94);
    program.policy_id = 11;
    program.scope_id = scope_id;
    program.created_height = 42;
    geocoin::bytes projection_height;
    append_u64(projection_height, 44);
    expect(storage_ptr->put_metadata("chainstate_height", projection_height), "projection height includes card updates");
    expect(storage_ptr->put_metadata("chainstate_utxo_count", geocoin::bytes(8, 0)), "empty projection UTXO count");
    expect(storage_ptr->put_metadata("card:prog:" + hash_to_hex(program_id),
                                     serialize_card_program_for_test(program)),
           "valid card program metadata stored");

    geocoin::state::CardInstanceEntry card;
    card.program_id = program_id;
    card.issuer = issuer;
    card.uid_hash = uid_hash;
    card.pubkey_hash = pubkey_hash;
    card.valid_from = 100;
    card.valid_to = 0;
    card.status = 0;
    card.metadata_hash = hash_from_seed(0x95);
    card.issued_height = 43;
    card.updated_height = 44;
    expect(storage_ptr->put_metadata("card:uid:" + hash_to_hex(uid_hash),
                                     serialize_card_instance_for_test(card)),
           "valid card instance metadata stored");

    expect(storage_ptr->put_metadata("card:prog:not-a-valid-id", {1, 2, 3}), "invalid card program key stored");
    expect(storage_ptr->put_metadata("card:uid:not-a-valid-id", {1, 2, 3}), "invalid card uid key stored");
    expect(storage_ptr->put_metadata("card:prog:" + hash_to_hex(hash_from_seed(0x96)), {1, 2}),
           "truncated card program metadata stored");
    expect(storage_ptr->put_metadata("card:uid:" + hash_to_hex(hash_from_seed(0x97)), {1, 2}),
           "truncated card instance metadata stored");

    geocoin::state::ChainState state(std::move(storage));
    expect(!state.has_derived_state_snapshot(),
           "malformed card records invalidate the persisted derived snapshot");
    const auto loaded_foundation = state.get_foundation_config();
    expect(loaded_foundation.address == foundation.address, "foundation config address loads from storage");
    expect(loaded_foundation.m == 2 && loaded_foundation.n == 3, "foundation M-of-N loads from storage");
    expect(loaded_foundation.participants == foundation.participants,
           "foundation participants load from storage");

    const auto coinbase = geocoin::core::Transaction::create_coinbase(
        1, issuer, 10 * geocoin::consensus::params::DUST_LIMIT);
    expect(state.apply_transaction(coinbase, 1, true), "issuer funding UTXO applies");

    geocoin::core::TxInput input;
    input.prev_tx_hash = coinbase.get_hash();
    input.vout = 0;

    geocoin::core::TxOutput output;
    output.value = geocoin::consensus::params::DUST_LIMIT;
    output.address = issuer;

    geocoin::core::Transaction update;
    update.type = geocoin::core::TransactionType::CARD_UPDATE_STATUS;
    update.inputs.push_back(input);
    update.outputs.push_back(output);
    update.card_program_id = program_id;
    update.card_issuer = issuer;
    update.card_uid_hash = uid_hash;
    update.card_status = 1;
    update.card_metadata_hash = hash_from_seed(0x98);
    update.sign_input(0, issuer_key);
    expect(state.validate_transaction(update), "card update validates against loaded card instance");

    geocoin::core::Transaction duplicate_pubkey_issue;
    duplicate_pubkey_issue.type = geocoin::core::TransactionType::CARD_ISSUE;
    duplicate_pubkey_issue.inputs.push_back(input);
    duplicate_pubkey_issue.outputs.push_back(output);
    duplicate_pubkey_issue.card_program_id = program_id;
    duplicate_pubkey_issue.card_issuer = issuer;
    duplicate_pubkey_issue.card_policy_id = program.policy_id;
    duplicate_pubkey_issue.card_scope_id = scope_id;
    duplicate_pubkey_issue.card_uid_hash = hash_from_seed(0x99);
    duplicate_pubkey_issue.card_pubkey_hash = pubkey_hash;
    duplicate_pubkey_issue.card_valid_from = 100;
    duplicate_pubkey_issue.card_valid_to = 0;
    duplicate_pubkey_issue.card_status = 0;
    duplicate_pubkey_issue.card_metadata_hash = hash_from_seed(0x9a);
    duplicate_pubkey_issue.sign_input(0, issuer_key);
    expect(!state.validate_transaction(duplicate_pubkey_issue),
           "loaded card pubkey index rejects duplicate pubkey hash");

    geocoin::core::Transaction issue = duplicate_pubkey_issue;
    issue.card_uid_hash = hash_from_seed(0x9b);
    issue.card_pubkey_hash = hash_from_seed(0x9c);
    issue.sign_input(0, issuer_key);
    expect(state.validate_transaction(issue), "card issue validates against loaded card program");
}

void test_sidechain_metadata_loads_and_lazy_fallback()
{
    const geocoin::Hash256 deposit_id = hash_from_seed(0xa0);
    const geocoin::Hash256 registry_id = deposit_id;
    const geocoin::Hash256 accounting_registry_id = hash_from_seed(0xa6);
    const geocoin::Hash256 bad_deposit_id = hash_from_seed(0xa2);
    const geocoin::Hash256 bad_registry_id = hash_from_seed(0xa3);

    geocoin::state::SidechainDeposit deposit;
    deposit.owner = geocoin::test::address_from_seed(0xa0);
    deposit.amount = 77 * geocoin::COIN;
    deposit.unlock_height = 555;
    deposit.checkpoint = hash_from_seed(0xa4);
    deposit.last_checkpoint_height = 556;
    deposit.last_sidechain_checkpoint_height = 557;

    geocoin::state::SidechainRegistryEntry registry;
    registry.owner = deposit.owner;
    registry.issuance_limit = 1000 * geocoin::COIN;
    registry.total_issued = 123 * geocoin::COIN;
    registry.multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
    registry.fee_bps = 25;
    registry.max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 8;
    registry.token_symbol = "GST";
    registry.committee_m = 2;
    registry.committee_n = 3;
    registry.committee_members = {
        geocoin::test::address_from_seed(0xa1),
        geocoin::test::address_from_seed(0xa2),
        geocoin::test::address_from_seed(0xa3),
    };
    std::sort(registry.committee_members.begin(), registry.committee_members.end(),
              [](const auto& a, const auto& b) { return a.str() < b.str(); });
    registry.committee_id = geocoin::consensus::compute_sidechain_committee_id(2, 3, registry.committee_members);
    registry.checkpoint_policy_id = 2;
    registry.checkpoint_version = 2;

    geocoin::state::SidechainRegistryEntry accounting_registry;
    accounting_registry.kind = geocoin::core::SidechainKind::ACCOUNTING;
    accounting_registry.owner = deposit.owner;
    accounting_registry.accounting_namespace = "cadastre";
    accounting_registry.accounting_schema_hash = hash_from_seed(0xa7);
    accounting_registry.committee_m = 1;
    accounting_registry.committee_n = 1;
    accounting_registry.committee_members = {
        geocoin::test::address_from_seed(0xa4)
    };
    accounting_registry.committee_id = geocoin::consensus::compute_sidechain_committee_id(1, 1, accounting_registry.committee_members);
    accounting_registry.checkpoint_policy_id =
        geocoin::consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID;
    accounting_registry.checkpoint_version =
        geocoin::consensus::params::ACCOUNTING_CHECKPOINT_VERSION;

    {
        auto storage = std::make_unique<geocoin::state::MemoryStorage>();
        auto* storage_ptr = storage.get();
        const std::string valid_epoch = "0123456789abcdef0123456789abcdef";
        expect(storage_ptr->put_metadata(
                   "idx_epoch",
                   geocoin::bytes(valid_epoch.begin(), valid_epoch.end())
               ),
               "valid sidechain test index epoch stored");
        const std::string deposit_hex = hash_to_hex(deposit_id);
        const std::string registry_hex = hash_to_hex(registry_id);
        const std::string accounting_registry_hex = hash_to_hex(accounting_registry_id);
        geocoin::bytes height; append_u64(height, 556);
        expect(storage_ptr->put_metadata("chainstate_height", height), "projection height includes checkpoint");
        expect(storage_ptr->put_metadata("chainstate_utxo_count", geocoin::bytes(8, 0)), "empty projection UTXO count");
        expect(storage_ptr->put_metadata("scd:" + accounting_registry_hex, serialize_sidechain_deposit_for_test(deposit)),
               "accounting registry has a matching deposit");
        expect(storage_ptr->put_metadata("scd:" + deposit_hex, serialize_sidechain_deposit_for_test(deposit)),
               "valid sidechain deposit metadata stored");
        expect(storage_ptr->put_metadata("sc:reg:" + registry_hex, serialize_sidechain_registry_for_test(registry)),
               "valid sidechain registry metadata stored");
        expect(storage_ptr->put_metadata(
                   "sc:reg:" + accounting_registry_hex,
                   serialize_sidechain_registry_for_test(accounting_registry)
               ),
               "accounting sidechain registry metadata stored");
        expect(storage_ptr->put_metadata("scd:not-a-valid-id", {1, 2, 3}),
               "invalid sidechain deposit key stored");
        expect(storage_ptr->put_metadata("sc:reg:not-a-valid-id", {1, 2, 3}),
               "invalid sidechain registry key stored");
        expect(storage_ptr->put_metadata("scd:" + hash_to_hex(bad_deposit_id), {1, 2}),
               "truncated sidechain deposit metadata stored");
        expect(storage_ptr->put_metadata("sc:reg:" + hash_to_hex(bad_registry_id), {1, 2}),
               "truncated sidechain registry metadata stored");

        geocoin::state::ChainState state(std::move(storage));
        expect(!state.has_derived_state_snapshot(),
               "malformed sidechain records invalidate the persisted derived snapshot");
        geocoin::state::SidechainDeposit loaded_deposit;
        expect(state.get_sidechain_deposit(deposit_id, loaded_deposit),
               "sidechain deposit loads from startup metadata");
        expect(loaded_deposit.owner == deposit.owner, "loaded sidechain deposit owner matches");
        expect(loaded_deposit.amount == deposit.amount, "loaded sidechain deposit amount matches");
        expect(loaded_deposit.last_sidechain_checkpoint_height == deposit.last_sidechain_checkpoint_height,
               "loaded sidechain deposit checkpoint metadata matches");
        bool deposit_corrupt = false;
        try { state.get_sidechain_deposit(bad_deposit_id, loaded_deposit); }
        catch (const geocoin::storage::ReadError& error) { deposit_corrupt = error.status() == geocoin::storage::ReadStatus::CORRUPT; }
        expect(deposit_corrupt, "corrupt sidechain deposit reports a read error");

        geocoin::state::SidechainRegistryEntry loaded_registry;
        expect(state.get_sidechain_registry(registry_id, loaded_registry),
               "sidechain registry loads from startup metadata");
        expect(loaded_registry.owner == registry.owner, "loaded sidechain registry owner matches");
        expect(loaded_registry.token_symbol == registry.token_symbol,
               "loaded sidechain registry symbol matches");
        expect(loaded_registry.committee_members == registry.committee_members,
               "loaded sidechain registry committee matches");
        expect(state.get_sidechain_registry_by_id_hex(registry_hex, loaded_registry),
               "sidechain registry lookup by hex id succeeds");
        expect(!state.get_sidechain_registry_by_id_hex(std::string(64, 'z'), loaded_registry),
               "sidechain registry lookup rejects non-hex id");
        bool registry_corrupt = false;
        try { state.get_sidechain_registry(bad_registry_id, loaded_registry); }
        catch (const geocoin::storage::ReadError& error) { registry_corrupt = error.status() == geocoin::storage::ReadStatus::CORRUPT; }
        expect(registry_corrupt, "corrupt sidechain registry reports a read error");
        expect(state.get_sidechain_registry(accounting_registry_id, loaded_registry),
               "accounting sidechain registry loads from extension metadata");
        expect(loaded_registry.kind == geocoin::core::SidechainKind::ACCOUNTING &&
                   loaded_registry.accounting_namespace ==
                       accounting_registry.accounting_namespace &&
                   loaded_registry.accounting_schema_hash ==
                       accounting_registry.accounting_schema_hash,
               "accounting registry kind, namespace and schema survive persistence");
    }

    {
        auto storage = std::make_unique<geocoin::state::MemoryStorage>();
        auto* storage_ptr = storage.get();
        geocoin::state::ChainState state(std::move(storage));
        expect(state.set_height(556), "lazy projection height includes checkpoint");
        expect(storage_ptr->put_metadata("scd:" + hash_to_hex(deposit_id),
                                         serialize_sidechain_deposit_for_test(deposit)),
               "lazy sidechain deposit metadata stored");
        expect(storage_ptr->put_metadata("sc:reg:" + hash_to_hex(registry_id),
                                         serialize_sidechain_registry_for_test(registry)),
               "lazy sidechain registry metadata stored");

        geocoin::state::SidechainDeposit lazy_deposit;
        expect(state.get_sidechain_deposit(deposit_id, lazy_deposit),
               "sidechain deposit lazy storage fallback loads metadata");
        expect(lazy_deposit.checkpoint == deposit.checkpoint,
               "lazy sidechain deposit checkpoint matches");

        geocoin::state::SidechainRegistryEntry lazy_registry;
        expect(state.get_sidechain_registry(registry_id, lazy_registry),
               "sidechain registry lazy storage fallback loads metadata");
        expect(lazy_registry.committee_id == registry.committee_id,
               "lazy sidechain registry committee id matches");
    }
}

} // namespace

int main()
{
    try {
        test_utxo_roundtrip_and_errors();
        test_noncanonical_utxo_keys_are_rejected();
        test_tx_undo_roundtrip_and_legacy_deposit();
        test_tx_undo_error_paths();
        test_block_undo_roundtrip_legacy_and_errors();
        test_chainstate_persistence_metadata_loads();
        test_sidechain_metadata_loads_and_lazy_fallback();
        std::cout << "State serialization unit tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "State serialization unit tests failed: " << ex.what() << "\n";
        return 1;
    }
}
