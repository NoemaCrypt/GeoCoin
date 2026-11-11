#include "geocoin/core/transaction.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/sidechain_fee.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace geocoin;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bytes make_seed(uint8_t v) {
    return bytes(32, v);
}

Address address_from_seed(const bytes& seed) {
    Address out;
    if (!crypto::derive_identity_address_from_private_key(seed, out)) {
        return Address();
    }
    return out;
}

Hash256 hash_from_string(const std::string& s) {
    const bytes data(s.begin(), s.end());
    return crypto::sha256(data);
}

void append_u64_le(bytes& out, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        out.push_back(static_cast<byte>((value >> (i * 8)) & 0xFFu));
    }
}

void append_u32_le(bytes& out, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<byte>((value >> (i * 8)) & 0xFFu));
    }
}

uint64_t read_u64_le(const bytes& data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8 && i < data.size(); ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

bool height_metadata_equals(state::Storage* storage, const std::string& key, Height expected) {
    if (!storage) {
        return false;
    }
    bytes data;
    return storage->get_metadata(key, data) && data.size() == 8 && read_u64_le(data) == expected;
}

Hash256 build_checkpoint_supply_leaf_v2_for_test(
    const Hash256& sidechain_id,
    Height checkpoint_height,
    Amount total_supply
) {
    static constexpr char kLeafDomain[] = "geocoin:sidechain:v2:supply_leaf";
    bytes data;
    data.insert(data.end(), kLeafDomain, kLeafDomain + sizeof(kLeafDomain) - 1);
    data.insert(data.end(), sidechain_id.begin(), sidechain_id.end());
    append_u64_le(data, checkpoint_height);
    append_u64_le(data, static_cast<uint64_t>(total_supply));
    return crypto::double_sha256(data);
}

Hash256 build_checkpoint_commitment_v2_for_test(
    const Hash256& sidechain_id,
    const Hash256& state_root,
    Height checkpoint_height,
    Amount total_supply
) {
    static constexpr char kCheckpointDomain[] = "geocoin:sidechain:v2:checkpoint_commitment";
    bytes data;
    data.insert(data.end(), kCheckpointDomain, kCheckpointDomain + sizeof(kCheckpointDomain) - 1);
    data.insert(data.end(), sidechain_id.begin(), sidechain_id.end());
    data.insert(data.end(), state_root.begin(), state_root.end());
    append_u64_le(data, checkpoint_height);
    append_u64_le(data, static_cast<uint64_t>(total_supply));
    return crypto::double_sha256(data);
}

struct CheckpointProofV2Fixture {
    Hash256 checkpoint{};
    bytes proof;
};

CheckpointProofV2Fixture build_checkpoint_proof_v2_for_test(
    const Hash256& sidechain_id,
    Height checkpoint_height,
    Amount total_supply,
    const std::string& salt
) {
    CheckpointProofV2Fixture out{};
    const Hash256 supply_leaf = build_checkpoint_supply_leaf_v2_for_test(sidechain_id, checkpoint_height, total_supply);
    std::vector<Hash256> leaves{
        hash_from_string(salt + ":leaf0"),
        hash_from_string(salt + ":leaf1"),
        supply_leaf,
        hash_from_string(salt + ":leaf3")
    };
    const size_t leaf_index = 2;
    const Hash256 state_root = core::compute_merkle_root(leaves);
    const std::vector<Hash256> path = core::compute_merkle_path(leaves, leaf_index);

    out.checkpoint = build_checkpoint_commitment_v2_for_test(
        sidechain_id,
        state_root,
        checkpoint_height,
        total_supply
    );

    bytes proof;
    proof.push_back('S');
    proof.push_back('C');
    proof.push_back('P');
    proof.push_back('2');
    proof.push_back(1); // proof-format version
    proof.push_back(static_cast<byte>(path.size()));
    proof.push_back(0); // reserved
    proof.push_back(0); // reserved
    proof.insert(proof.end(), state_root.begin(), state_root.end());
    append_u64_le(proof, checkpoint_height);
    append_u64_le(proof, static_cast<uint64_t>(total_supply));
    append_u32_le(proof, static_cast<uint32_t>(leaf_index));
    for (const auto& sibling : path) {
        proof.insert(proof.end(), sibling.begin(), sibling.end());
    }
    out.proof = std::move(proof);
    return out;
}

Signature sign_hash32(const Hash256& msg32, const bytes& priv_key) {
    return crypto::sign_recoverable_message(msg32, priv_key);
}

Signature sign_bytes_payload(const bytes& payload, const bytes& priv_key) {
    return sign_hash32(crypto::sha256(payload), priv_key);
}

Signature sign_sidechain_payload(const core::Transaction& tx, const bytes& priv_key) {
    return sign_bytes_payload(core::build_sidechain_payload_for_signing(tx), priv_key);
}

Signature sign_sidechain_checkpoint_payload(const core::Transaction& tx, const bytes& priv_key) {
    return sign_bytes_payload(core::build_sidechain_checkpoint_payload_for_signing(tx), priv_key);
}

Signature sign_sidechain_committee_update_payload(const core::Transaction& tx, const bytes& priv_key) {
    return sign_bytes_payload(core::build_sidechain_committee_update_payload_for_signing(tx), priv_key);
}

Signature sign_foundation_update_payload(const core::Transaction& tx, const bytes& priv_key) {
    return sign_bytes_payload(core::build_foundation_update_payload_for_signing(tx), priv_key);
}

Signature sign_sidechain_batch_item_payload(const core::BatchCheckpointItem& item, const bytes& priv_key) {
    return sign_bytes_payload(core::build_sidechain_batch_checkpoint_payload_for_signing(item), priv_key);
}

void set_single_input(core::Transaction& tx, const Hash256& prev_hash, uint32_t vout = 0) {
    tx.inputs.clear();
    core::TxInput in;
    in.prev_tx_hash = prev_hash;
    in.vout = vout;
    tx.inputs.push_back(in);
}

void set_single_output(core::Transaction& tx, Amount value, const Address& address) {
    tx.outputs.clear();
    core::TxOutput out;
    out.value = value;
    out.address = address;
    tx.outputs.push_back(out);
}

struct CommitteeMember {
    Address address;
    bytes private_key;
};

std::vector<CommitteeMember> make_sorted_committee(uint8_t start_seed, size_t n) {
    std::vector<CommitteeMember> members;
    members.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        CommitteeMember m;
        m.private_key = make_seed(static_cast<uint8_t>(start_seed + i));
        m.address = address_from_seed(m.private_key);
        members.push_back(m);
    }
    std::sort(members.begin(), members.end(), [](const CommitteeMember& a, const CommitteeMember& b) {
        return a.address.str() < b.address.str();
    });
    return members;
}

std::vector<Address> committee_addresses(const std::vector<CommitteeMember>& members) {
    std::vector<Address> out;
    out.reserve(members.size());
    for (const auto& m : members) {
        out.push_back(m.address);
    }
    return out;
}

void add_committee_signatures_for_sidechain_payload(
    core::Transaction& tx,
    const std::vector<CommitteeMember>& members,
    size_t required
) {
    tx.sidechain_committee_signatures.clear();
    const bytes payload = core::build_sidechain_payload_for_signing(tx);
    for (size_t i = 0; i < required && i < members.size(); ++i) {
        tx.sidechain_committee_signatures.push_back(sign_bytes_payload(payload, members[i].private_key));
    }
}

void add_committee_signatures_for_checkpoint_payload(
    core::Transaction& tx,
    const std::vector<CommitteeMember>& members,
    size_t required
) {
    tx.sidechain_committee_signatures.clear();
    const bytes payload = core::build_sidechain_checkpoint_payload_for_signing(tx);
    for (size_t i = 0; i < required && i < members.size(); ++i) {
        tx.sidechain_committee_signatures.push_back(sign_bytes_payload(payload, members[i].private_key));
    }
}

void add_committee_signatures_for_committee_update_payload(
    core::Transaction& tx,
    const std::vector<CommitteeMember>& members,
    size_t required
) {
    tx.sidechain_committee_signatures.clear();
    const bytes payload = core::build_sidechain_committee_update_payload_for_signing(tx);
    for (size_t i = 0; i < required && i < members.size(); ++i) {
        tx.sidechain_committee_signatures.push_back(sign_bytes_payload(payload, members[i].private_key));
    }
}

core::Transaction make_coinbase(const Address& addr, Amount value) {
    core::Transaction tx;
    tx.version = 2;
    tx.type = core::TransactionType::STANDARD;
    tx.timestamp = 1;

    core::TxInput in;
    in.prev_tx_hash = Hash256{};
    in.vout = 0xFFFFFFFF;
    // These funding fixtures are applied at height 1.
    in.script_sig = {1, 0, 0, 0, 0, 0, 0, 0};
    tx.inputs.push_back(in);

    core::TxOutput out;
    out.value = value;
    out.address = addr;
    tx.outputs.push_back(out);
    return tx;
}

std::shared_ptr<storage::BlockStorage> make_block_storage_with_height(Height height) {
    auto path = std::filesystem::temp_directory_path() /
        ("geocoin_test_all_tx_bs_" + std::to_string(height));
    std::filesystem::remove_all(path);
    auto storage_ptr = std::make_shared<storage::BlockStorage>(path.string());

    core::Block block;
    block.header.version = 1;
    block.header.timestamp = 1;
    block.header.bits = consensus::ProofOfWork::get_initial_bits();
    block.header.nonce = static_cast<uint32_t>(height + 1);
    block.header.prev_block_hash = Hash256{};
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();

    int32_t file = 0;
    uint32_t pos = 0;
    storage_ptr->save_block_with_pos(block, height, file, pos, false);
    return storage_ptr;
}

std::shared_ptr<storage::BlockStorage> make_temp_block_storage(const std::string& suffix) {
    auto path = std::filesystem::temp_directory_path() / ("geocoin_test_" + suffix);
    std::filesystem::remove_all(path);
    return std::make_shared<storage::BlockStorage>(path.string());
}

core::Block make_block_for_test(
    const Hash256& prev_hash,
    Height height,
    const std::vector<core::Transaction>& txs
) {
    core::Block block;
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = static_cast<uint32_t>(1000 + height);
    block.header.bits = consensus::ProofOfWork::get_initial_bits();
    block.header.geo_tag = 0;
    block.header.nonce = static_cast<uint64_t>(height);
    block.transactions = txs;
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    return block;
}

void check_roundtrip_consensus_and_verify(
    const core::Transaction& tx,
    const char* label,
    bool require_verify = true
) {
    bytes encoded = tx.serialize();
    size_t offset = 0;
    core::Transaction decoded;
    try {
        decoded = core::Transaction::deserialize(encoded, offset);
    } catch (const std::exception& e) {
        check(false, std::string(label) + ": deserialize exception: " + e.what());
        return;
    }

    check(offset == encoded.size(), std::string(label) + ": deserialize offset mismatch");
    check(decoded.get_hash() == tx.get_hash(), std::string(label) + ": hash mismatch after roundtrip");

    consensus::ValidationState state;
    if (!consensus::check_transaction(decoded, state)) {
        check(false, std::string(label) + ": consensus reject: " + state.get_reject_reason() +
                     " (" + state.get_debug_message() + ")");
        return;
    }

    if (require_verify) {
        check(decoded.verify(), std::string(label) + ": tx.verify failed");
    }
}

void test_unknown_transaction_type_rejected() {
    const bytes key = make_seed(11);
    const Address addr = address_from_seed(key);

    core::Transaction tx;
    tx.version = 2;
    tx.type = static_cast<core::TransactionType>(250);
    tx.timestamp = 99;
    set_single_input(tx, hash_from_string("unknown-type-prev"), 0);
    set_single_output(tx, COIN, addr);
    tx.sign_input(0, key);

    check(!tx.verify(), "unknown-type: tx.verify rejects unknown transaction type");

    consensus::ValidationState state;
    check(!consensus::check_transaction(tx, state), "unknown-type: consensus rejects unknown transaction type");
    check(state.get_reject_reason() == "bad-tx-type", "unknown-type: reject reason is stable");

    const bytes encoded = tx.serialize();
    size_t offset = 0;
    bool threw = false;
    try {
        (void)core::Transaction::deserialize(encoded, offset);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "unknown-type: deserialize rejects unknown transaction type");
}

void test_transaction_type_coverage() {
    const bytes key_a = make_seed(1);
    const bytes key_b = make_seed(2);
    const bytes key_c = make_seed(3);
    const Address addr_a = address_from_seed(key_a);
    const Address addr_b = address_from_seed(key_b);
    const Address addr_c = address_from_seed(key_c);
    check(!addr_a.str().empty() && !addr_b.str().empty() && !addr_c.str().empty(),
          "addresses generated");

    const Hash256 prev_1 = hash_from_string("prev-1");
    const Hash256 prev_2 = hash_from_string("prev-2");
    const Hash256 sidechain_id = hash_from_string("sidechain-coverage");
    const auto committee = make_sorted_committee(30, 3);
    const std::vector<Address> committee_members = committee_addresses(committee);
    const Hash256 committee_id = consensus::compute_sidechain_committee_id(2, 3, committee_members);

    std::set<int> covered;

    // STANDARD
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::STANDARD;
        tx.timestamp = 100;
        set_single_input(tx, prev_1, 0);
        set_single_output(tx, 2 * COIN, addr_a);
        tx.sign_input(0, key_a);
        check_roundtrip_consensus_and_verify(tx, "STANDARD");
        covered.insert(static_cast<int>(tx.type));
    }

    // ANGLE_COMMITMENT
    {
        core::Transaction tx = core::Transaction::create_angle_commitment(
            addr_a,
            hash_from_string("spending-hash"),
            key_a,
            addr_a,
            "degree"
        );
        check_roundtrip_consensus_and_verify(tx, "ANGLE_COMMITMENT");
        covered.insert(static_cast<int>(tx.type));
    }

    // MULTISIG_SPEND
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::MULTISIG_SPEND;
        tx.timestamp = 101;
        set_single_input(tx, prev_1, 1);
        set_single_output(tx, 2 * COIN, addr_b);
        tx.m = 2;
        tx.n = 3;
        tx.angle_commitment_hashes = {hash_from_string("ac-1"), hash_from_string("ac-2")};
        tx.spend_participants = {addr_a, addr_b, addr_c};
        check_roundtrip_consensus_and_verify(tx, "MULTISIG_SPEND");
        covered.insert(static_cast<int>(tx.type));
    }

    // DELEGATION_PROOF
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::DELEGATION_PROOF;
        tx.timestamp = 102;
        set_single_input(tx, prev_2, 0);
        tx.primary_multisig_address = addr_a;
        tx.delegated_multisig_address = addr_b;
        tx.primary_m = 1;
        tx.primary_n = 1;
        tx.primary_participants = {addr_a};
        tx.delegated_m = 1;
        tx.delegated_n = 1;
        tx.delegated_participants = {addr_b};
        tx.max_total_delegated = 5 * COIN;
        tx.max_per_transaction = COIN;
        tx.angle_commitment_hashes = {hash_from_string("deleg-proof-angle")};
        check_roundtrip_consensus_and_verify(tx, "DELEGATION_PROOF");
        covered.insert(static_cast<int>(tx.type));
    }

    // REVOKE_DELEGATION
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::REVOKE_DELEGATION;
        tx.timestamp = 103;
        set_single_input(tx, prev_2, 1);
        tx.revoked_delegation_proof_txid = hash_from_string("deleg-proof-txid");
        tx.angle_commitment_hashes = {hash_from_string("revoke-delegation-angle")};
        tx.sign_input(0, key_a);
        check_roundtrip_consensus_and_verify(tx, "REVOKE_DELEGATION");
        covered.insert(static_cast<int>(tx.type));
    }

    // LOCK_DEPOSIT
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::LOCK_DEPOSIT;
        tx.timestamp = 104;
        set_single_input(tx, prev_1, 2);
        set_single_output(tx, 5 * COIN, addr_a);
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = 10 * COIN;
        tx.sidechain_unlock_height = 50;
        tx.sidechain_checkpoint = Hash256{};
        tx.sidechain_signature = sign_sidechain_payload(tx, key_a);
        tx.sign_input(0, key_a);
        check_roundtrip_consensus_and_verify(tx, "LOCK_DEPOSIT");
        covered.insert(static_cast<int>(tx.type));
    }

    // SPEND_DEPOSIT
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::SPEND_DEPOSIT;
        tx.timestamp = 105;
        set_single_output(tx, COIN, addr_a);
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = COIN;
        tx.sidechain_signature = sign_sidechain_payload(tx, key_a);
        check_roundtrip_consensus_and_verify(tx, "SPEND_DEPOSIT");
        covered.insert(static_cast<int>(tx.type));
    }

    // TOP_UP_SIDECHAIN_DEPOSIT
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT;
        tx.timestamp = 105;
        set_single_input(tx, prev_2, 6);
        set_single_output(tx, COIN, addr_a);
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = COIN;
        tx.sidechain_signature = sign_sidechain_payload(tx, key_a);
        tx.sign_input(0, key_a);
        check_roundtrip_consensus_and_verify(tx, "TOP_UP_SIDECHAIN_DEPOSIT");
        covered.insert(static_cast<int>(tx.type));
    }

    // UNLOCK_DEPOSIT
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::UNLOCK_DEPOSIT;
        tx.timestamp = 106;
        set_single_output(tx, COIN, addr_a);
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = 0;
        tx.sidechain_unlock_height = 0;
        tx.sidechain_signature = sign_sidechain_payload(tx, key_a);
        check_roundtrip_consensus_and_verify(tx, "UNLOCK_DEPOSIT");
        covered.insert(static_cast<int>(tx.type));
    }

    // ISSUE_SIDECHAIN
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::ISSUE_SIDECHAIN;
        tx.timestamp = 107;
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = 0;
        tx.sidechain_unlock_height = 0;
        tx.sidechain_checkpoint = sidechain_id;
        tx.sidechain_token_symbol = "SCX";
        tx.sidechain_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        tx.sidechain_fee_bps = 25;
        tx.sidechain_max_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
        tx.sidechain_checkpoint_policy_id = 2;
        tx.sidechain_checkpoint_version = 2;
        tx.sidechain_committee_m = 2;
        tx.sidechain_committee_n = 3;
        tx.sidechain_committee_members = committee_members;
        tx.sidechain_committee_id = committee_id;
        tx.sidechain_signature = sign_sidechain_payload(tx, key_a);
        check_roundtrip_consensus_and_verify(tx, "ISSUE_SIDECHAIN");
        covered.insert(static_cast<int>(tx.type));
    }

    // MINT_SIDECHAIN
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::MINT_SIDECHAIN;
        tx.timestamp = 108;
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = 10 * COIN;
        tx.sidechain_committee_id = committee_id;
        tx.sidechain_committee_signatures.push_back(sign_hash32(hash_from_string("mint-sig"), key_a));
        check_roundtrip_consensus_and_verify(tx, "MINT_SIDECHAIN");
        covered.insert(static_cast<int>(tx.type));
    }

    // REGISTER_ACCOUNTING_SIDECHAIN
    {
        const Hash256 accounting_id = hash_from_string("accounting-sidechain-coverage");
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN;
        tx.timestamp = 108;
        tx.sidechain_id = accounting_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_checkpoint = accounting_id;
        tx.sidechain_namespace = "land-registry";
        tx.sidechain_schema_hash = hash_from_string("accounting-schema-v1");
        tx.sidechain_checkpoint_policy_id =
            consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID;
        tx.sidechain_checkpoint_version =
            consensus::params::ACCOUNTING_CHECKPOINT_VERSION;
        tx.sidechain_committee_m = 2;
        tx.sidechain_committee_n = 3;
        tx.sidechain_committee_members = committee_members;
        tx.sidechain_committee_id = committee_id;
        tx.sidechain_signature = sign_sidechain_payload(tx, key_a);
        check_roundtrip_consensus_and_verify(tx, "REGISTER_ACCOUNTING_SIDECHAIN");
        covered.insert(static_cast<int>(tx.type));
    }

    // REDEEM_SIDECHAIN
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::REDEEM_SIDECHAIN;
        tx.timestamp = 109;
        set_single_output(tx, COIN, addr_a);
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = COIN;
        tx.sidechain_committee_id = committee_id;
        tx.sidechain_committee_signatures.push_back(sign_hash32(hash_from_string("redeem-sig"), key_a));
        check_roundtrip_consensus_and_verify(tx, "REDEEM_SIDECHAIN");
        covered.insert(static_cast<int>(tx.type));
    }

    // UPDATE_FOUNDATION_ADDRESS
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::UPDATE_FOUNDATION_ADDRESS;
        tx.timestamp = 110;
        tx.foundation_old_address = addr_a;
        tx.foundation_new_address = addr_b;
        tx.foundation_m = 2;
        tx.foundation_n = 3;
        tx.foundation_participants = {addr_a, addr_b, addr_c};
        tx.foundation_new_m = 2;
        tx.foundation_new_n = 3;
        tx.foundation_new_participants = {addr_b, addr_c, addr_a};
        tx.foundation_signatures.push_back(sign_foundation_update_payload(tx, key_a));
        check_roundtrip_consensus_and_verify(tx, "UPDATE_FOUNDATION_ADDRESS");
        covered.insert(static_cast<int>(tx.type));
    }

    // SIDECHAIN_CHECKPOINT
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::SIDECHAIN_CHECKPOINT;
        tx.timestamp = 111;
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = consensus::params::MIN_TX_FEE;
        tx.sidechain_checkpoint = hash_from_string("checkpoint-coverage");
        tx.sidechain_checkpoint_height = 1;
        tx.sidechain_committee_id = committee_id;
        tx.sidechain_committee_signatures.push_back(sign_hash32(hash_from_string("cp-sig"), key_a));
        bytes encoded = tx.serialize();
        size_t offset = 0;
        core::Transaction decoded = core::Transaction::deserialize(encoded, offset);
        consensus::ValidationState state;
        check(!consensus::check_transaction(decoded, state), "SIDECHAIN_CHECKPOINT is disabled in production");
        check(state.get_reject_reason() == "legacy-sidechain-checkpoint-disabled",
              "SIDECHAIN_CHECKPOINT reject reason is stable");
        covered.insert(static_cast<int>(tx.type));
    }

    // UPDATE_SIDECHAIN_COMMITTEE
    {
        const auto new_committee = make_sorted_committee(60, 3);
        const std::vector<Address> new_members = committee_addresses(new_committee);
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE;
        tx.timestamp = 112;
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = 0;
        tx.sidechain_unlock_height = 0;
        tx.sidechain_committee_m = 2;
        tx.sidechain_committee_n = 3;
        tx.sidechain_committee_members = new_members;
        tx.sidechain_committee_id =
            consensus::compute_sidechain_committee_id(tx.sidechain_committee_m, tx.sidechain_committee_n, new_members);
        tx.sidechain_committee_signatures.push_back(sign_hash32(hash_from_string("upd-committee-sig"), key_a));
        check_roundtrip_consensus_and_verify(tx, "UPDATE_SIDECHAIN_COMMITTEE");
        covered.insert(static_cast<int>(tx.type));
    }

    // SLASH_DEPOSIT
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::SLASH_DEPOSIT;
        tx.timestamp = 113;
        set_single_output(tx, COIN, addr_b);
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = 2 * COIN;
        tx.sidechain_committee_id = committee_id;
        tx.sidechain_committee_signatures.push_back(sign_hash32(hash_from_string("slash-sig"), key_a));
        tx.sidechain_fraud_checkpoint_a = hash_from_string("fraud-a");
        tx.sidechain_fraud_checkpoint_b = hash_from_string("fraud-b");
        tx.sidechain_fraud_checkpoint_index_a = 0xFFFF;
        tx.sidechain_fraud_checkpoint_index_b = 0xFFFF;
        check_roundtrip_consensus_and_verify(tx, "SLASH_DEPOSIT");
        covered.insert(static_cast<int>(tx.type));
    }

    // SLASH_DEPOSIT (same txid, different batch indices allowed)
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::SLASH_DEPOSIT;
        tx.timestamp = 113;
        set_single_output(tx, COIN, addr_b);
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = 2 * COIN;
        tx.sidechain_committee_id = committee_id;
        tx.sidechain_committee_signatures.push_back(sign_hash32(hash_from_string("slash-sig-idx"), key_a));
        tx.sidechain_fraud_checkpoint_a = hash_from_string("fraud-batch-same-txid");
        tx.sidechain_fraud_checkpoint_b = tx.sidechain_fraud_checkpoint_a;
        tx.sidechain_fraud_checkpoint_index_a = 0;
        tx.sidechain_fraud_checkpoint_index_b = 1;
        check_roundtrip_consensus_and_verify(tx, "SLASH_DEPOSIT batch-item references");
    }

    // DECOMMISSION_SIDECHAIN
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::DECOMMISSION_SIDECHAIN;
        tx.timestamp = 114;
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = addr_a;
        tx.sidechain_amount = 0;
        tx.sidechain_unlock_height = 0;
        tx.sidechain_checkpoint = Hash256{};
        tx.sidechain_committee_id = committee_id;
        tx.sidechain_signature = sign_sidechain_payload(tx, key_a);
        tx.sidechain_committee_signatures.push_back(sign_sidechain_payload(tx, key_a));
        check_roundtrip_consensus_and_verify(tx, "DECOMMISSION_SIDECHAIN");
        covered.insert(static_cast<int>(tx.type));

        core::Transaction noncanonical = tx;
        noncanonical.sidechain_checkpoint = hash_from_string("decommission-noncanonical-checkpoint");
        noncanonical.sidechain_signature = sign_sidechain_payload(noncanonical, key_a);
        noncanonical.sidechain_committee_signatures = {sign_sidechain_payload(noncanonical, key_a)};
        consensus::ValidationState state;
        check(!consensus::check_transaction(noncanonical, state),
              "DECOMMISSION_SIDECHAIN rejects non-null checkpoint");
        check(state.get_reject_reason() == "bad-sidechain-decommission",
              "DECOMMISSION_SIDECHAIN non-null checkpoint reject reason is stable");
    }

    // BATCH_SIDECHAIN_CHECKPOINT
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        tx.timestamp = 114;
        core::BatchCheckpointItem item;
        item.sidechain_id = sidechain_id;
        item.checkpoint = hash_from_string("batch-checkpoint");
        item.checkpoint_height = 2;
        item.fee = consensus::params::MIN_TX_FEE;
        item.version = 2;
        item.committee_id = committee_id;
        item.proof.push_back(0x01);
        item.committee_signatures.push_back(sign_sidechain_batch_item_payload(item, key_a));
        tx.sidechain_batch_checkpoints.push_back(item);
        check_roundtrip_consensus_and_verify(tx, "BATCH_SIDECHAIN_CHECKPOINT");
        covered.insert(static_cast<int>(tx.type));
    }

    // CADASTRAL_DOC
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::CADASTRAL_DOC;
        tx.timestamp = 115;
        set_single_input(tx, prev_1, 3);
        set_single_output(tx, COIN, addr_a);
        tx.cadastral_doc_hash = hash_from_string("doc-hash");
        tx.cadastral_number = "77:01:0001001:1234";
        tx.cadastral_owner = addr_a;
        tx.cadastral_nft_id = hash_from_string("linked-parcel");
        tx.sign_input(0, key_a);
        check_roundtrip_consensus_and_verify(tx, "CADASTRAL_DOC");
        covered.insert(static_cast<int>(tx.type));
    }

    // NFT_CREATE
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::NFT_CREATE;
        tx.timestamp = 116;

        nft::NFTCreationTransaction create;
        create.nft_id = hash_from_string("nft-create-id");
        create.metadata.name = "NFT #1";
        create.metadata.description = "Coverage";
        create.metadata.image_uri = "ipfs://example";
        create.metadata.content_hash = "aabbcc";
        create.metadata.geometry.polygons.front().exterior = {
            {55750000000LL, 37610000000LL},
            {55750010000LL, 37610010000LL},
            {55749990000LL, 37610020000LL},
        };
        create.creator_address = addr_a;
        create.royalty_percent = 250;
        create.signature = sign_bytes_payload(create.serialize_for_signing(), key_a);
        tx.nft_create = create;

        set_single_input(tx, prev_1, 4);
        set_single_output(tx, COIN, addr_a);
        tx.sign_input(0, key_a);

        check_roundtrip_consensus_and_verify(tx, "NFT_CREATE");
        covered.insert(static_cast<int>(tx.type));
    }

    // NFT_TRANSFER
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::NFT_TRANSFER;
        tx.timestamp = 117;

        nft::NFTTransferTransaction transfer;
        transfer.nft_id = hash_from_string("nft-create-id");
        transfer.from = addr_a;
        transfer.to = addr_b;
        transfer.fee = consensus::params::MIN_TX_FEE;
        transfer.signature = sign_bytes_payload(transfer.serialize_for_signing(), key_a);
        tx.nft_transfer = transfer;

        set_single_input(tx, prev_1, 5);
        set_single_output(tx, COIN, addr_b);
        tx.sign_input(0, key_a);

        check_roundtrip_consensus_and_verify(tx, "NFT_TRANSFER");
        covered.insert(static_cast<int>(tx.type));
    }

    // NFT_BURN
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::NFT_BURN;
        tx.timestamp = 118;

        nft::NFTBurnTransaction burn;
        burn.nft_id = hash_from_string("nft-create-id");
        burn.owner = addr_b;
        burn.fee = consensus::params::MIN_TX_FEE;
        burn.signature = sign_bytes_payload(burn.serialize_for_signing(), key_b);
        tx.nft_burn = burn;

        set_single_input(tx, prev_1, 6);
        tx.sign_input(0, key_b);

        check_roundtrip_consensus_and_verify(tx, "NFT_BURN");
        covered.insert(static_cast<int>(tx.type));
    }

    // INTEREST_DISTRIBUTION
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::INTEREST_DISTRIBUTION;
        tx.timestamp = 119;

        set_single_output(tx, COIN, addr_a);

        check_roundtrip_consensus_and_verify(tx, "INTEREST_DISTRIBUTION");
        covered.insert(static_cast<int>(tx.type));
    }

    // CARD_PROGRAM_DEFINE
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::CARD_PROGRAM_DEFINE;
        tx.timestamp = 120;

        tx.card_program_id = hash_from_string("card-program-1");
        tx.card_issuer = addr_a;
        tx.card_chip_vendor = "NXP";
        tx.card_chip_os = "JCOP4";
        tx.card_applet_aid = "A000000001";
        tx.card_applet_version = 1;
        tx.card_applet_package_hash = hash_from_string("card-applet-package-v1");
        tx.card_policy_id = 1;
        tx.card_scope_id = hash_from_string("scope-cadastral");

        set_single_input(tx, prev_1, 7);
        set_single_output(tx, COIN, addr_a);
        tx.sign_input(0, key_a);

        check_roundtrip_consensus_and_verify(tx, "CARD_PROGRAM_DEFINE");
        covered.insert(static_cast<int>(tx.type));
    }

    // CARD_ISSUE
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::CARD_ISSUE;
        tx.timestamp = 121;

        tx.card_program_id = hash_from_string("card-program-1");
        tx.card_issuer = addr_a;
        tx.card_policy_id = 1;
        tx.card_scope_id = hash_from_string("scope-cadastral");
        tx.card_uid_hash = hash_from_string("card-uid-001");
        tx.card_pubkey_hash = hash_from_string("card-pubkey-001");
        tx.card_valid_from = 1710000000;
        tx.card_valid_to = 0;
        tx.card_status = 0;
        tx.card_metadata_hash = hash_from_string("card-metadata-001");

        set_single_input(tx, prev_1, 8);
        set_single_output(tx, COIN, addr_a);
        tx.sign_input(0, key_a);

        check_roundtrip_consensus_and_verify(tx, "CARD_ISSUE");
        covered.insert(static_cast<int>(tx.type));
    }

    // CARD_UPDATE_STATUS
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::CARD_UPDATE_STATUS;
        tx.timestamp = 122;

        tx.card_program_id = hash_from_string("card-program-1");
        tx.card_issuer = addr_a;
        tx.card_uid_hash = hash_from_string("card-uid-001");
        tx.card_status = 1; // suspended
        tx.card_metadata_hash = hash_from_string("card-status-suspend");

        set_single_input(tx, prev_1, 9);
        set_single_output(tx, COIN, addr_a);
        tx.sign_input(0, key_a);

        check_roundtrip_consensus_and_verify(tx, "CARD_UPDATE_STATUS");
        covered.insert(static_cast<int>(tx.type));
    }

    // CARD_UPDATE_POLICY
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::CARD_UPDATE_POLICY;
        tx.timestamp = 123;
        tx.card_program_id = hash_from_string("card-program-1");
        tx.card_issuer = addr_a;
        tx.card_policy_id = 2;
        tx.card_scope_id = hash_from_string("scope-cadastral");
        tx.card_corporate_policy.version = 1;
        tx.card_corporate_policy.mint = {1, {addr_a}};
        tx.card_corporate_policy.freeze = {1, {addr_a}};
        tx.card_corporate_policy.operations = {1, {addr_a}};
        tx.card_corporate_policy.recovery = {1, {addr_a}};
        tx.card_corporate_policy.compliance_policy_hash = hash_from_string("card-compliance-v1");
        tx.card_corporate_policy.max_active_cards = 100;

        set_single_input(tx, prev_1, 10);
        set_single_output(tx, COIN, addr_a);
        tx.sign_input(0, key_a);

        check_roundtrip_consensus_and_verify(tx, "CARD_UPDATE_POLICY");
        covered.insert(static_cast<int>(tx.type));
    }

    const std::set<int> expected = {
        static_cast<int>(core::TransactionType::STANDARD),
        static_cast<int>(core::TransactionType::ANGLE_COMMITMENT),
        static_cast<int>(core::TransactionType::MULTISIG_SPEND),
        static_cast<int>(core::TransactionType::DELEGATION_PROOF),
        static_cast<int>(core::TransactionType::REVOKE_DELEGATION),
        static_cast<int>(core::TransactionType::LOCK_DEPOSIT),
        static_cast<int>(core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT),
        static_cast<int>(core::TransactionType::SPEND_DEPOSIT),
        static_cast<int>(core::TransactionType::UNLOCK_DEPOSIT),
        static_cast<int>(core::TransactionType::ISSUE_SIDECHAIN),
        static_cast<int>(core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN),
        static_cast<int>(core::TransactionType::MINT_SIDECHAIN),
        static_cast<int>(core::TransactionType::REDEEM_SIDECHAIN),
        static_cast<int>(core::TransactionType::UPDATE_FOUNDATION_ADDRESS),
        static_cast<int>(core::TransactionType::SIDECHAIN_CHECKPOINT),
        static_cast<int>(core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE),
        static_cast<int>(core::TransactionType::SLASH_DEPOSIT),
        static_cast<int>(core::TransactionType::DECOMMISSION_SIDECHAIN),
        static_cast<int>(core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT),
        static_cast<int>(core::TransactionType::INTEREST_DISTRIBUTION),
        static_cast<int>(core::TransactionType::CARD_PROGRAM_DEFINE),
        static_cast<int>(core::TransactionType::CARD_ISSUE),
        static_cast<int>(core::TransactionType::CARD_UPDATE_STATUS),
        static_cast<int>(core::TransactionType::CARD_UPDATE_POLICY),
        static_cast<int>(core::TransactionType::CADASTRAL_DOC),
        static_cast<int>(core::TransactionType::NFT_CREATE),
        static_cast<int>(core::TransactionType::NFT_TRANSFER),
        static_cast<int>(core::TransactionType::NFT_BURN),
    };

    check(covered == expected, "all transaction types are covered");
}

void test_consensus_validation_guard_matrix() {
    const bytes key_a = make_seed(240);
    const Address addr_a = address_from_seed(key_a);
    const Address addr_b = address_from_seed(make_seed(241));
    const Hash256 prev = hash_from_string("consensus-guard-prev");
    check(!addr_a.str().empty() && !addr_b.str().empty(), "guard-matrix: addresses generated");

    auto expect_reject = [](const core::Transaction& tx,
                            const std::string& reason,
                            const std::string& label) {
        consensus::ValidationState state;
        check(!consensus::check_transaction(tx, state), label + " rejected");
        check(state.get_reject_reason() == reason,
              label + " reject reason is " + reason + " (got " + state.get_reject_reason() + ")");
    };

    auto standard_tx = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::STANDARD;
        tx.timestamp = 5200;
        set_single_input(tx, prev, 0);
        set_single_output(tx, COIN, addr_a);
        return tx;
    };

    {
        core::Transaction tx = standard_tx();
        tx.version = 1;
        expect_reject(tx, "bad-tx-version", "guard-matrix: bad tx version");
    }
    {
        core::Transaction tx = standard_tx();
        tx.witness_version = 0;
        expect_reject(tx, "bad-witness-version", "guard-matrix: bad witness version");
    }
    {
        core::Transaction tx = standard_tx();
        tx.inputs.resize(consensus::params::MAX_TX_INPUTS + 1);
        expect_reject(tx, "bad-txns-too-many-inputs", "guard-matrix: too many inputs");
    }
    {
        core::Transaction tx = standard_tx();
        tx.outputs.assign(consensus::params::MAX_TX_OUTPUTS + 1, core::TxOutput{0, addr_a, {}});
        expect_reject(tx, "bad-txns-too-many-outputs", "guard-matrix: too many outputs");
    }
    {
        core::Transaction tx = standard_tx();
        tx.inputs.push_back(tx.inputs.front());
        expect_reject(tx, "bad-txns-inputs-duplicate", "guard-matrix: duplicate standard input");
    }
    {
        core::Transaction tx = standard_tx();
        tx.outputs[0].value = 1;
        expect_reject(tx, "bad-txns-dust", "guard-matrix: dust output");
    }
    {
        core::Transaction tx = standard_tx();
        tx.outputs[0].value = 21000000LL * COIN + 1;
        expect_reject(tx, "bad-txns-txouttotal-toolarge", "guard-matrix: output total too large");
    }

    auto angle_tx = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::ANGLE_COMMITMENT;
        tx.commitment_network_id = "degree";
        set_single_input(tx, hash_from_string("guard-angle-key"), 0xFFFFFFFE);
        tx.multisig_address = addr_a;
        tx.spending_tx_hash = hash_from_string("guard-angle-spend");
        tx.signer_address = addr_b;
        tx.inputs[0].signature = bytes(10, 1);
        return tx;
    };
    {
        core::Transaction tx = angle_tx();
        tx.inputs.clear();
        expect_reject(tx, "bad-angle-commitment", "guard-matrix: angle missing input");
    }
    {
        core::Transaction tx = angle_tx();
        tx.inputs[0].vout = 0;
        expect_reject(tx, "bad-angle-commitment", "guard-matrix: angle bad marker");
    }
    {
        core::Transaction tx = angle_tx();
        set_single_output(tx, COIN, addr_a);
        expect_reject(tx, "bad-angle-commitment", "guard-matrix: angle unexpected output");
    }
    {
        core::Transaction tx = angle_tx();
        tx.multisig_address = Address();
        expect_reject(tx, "bad-angle-commitment", "guard-matrix: angle empty multisig address");
    }
    {
        core::Transaction tx = angle_tx();
        tx.spending_tx_hash = Hash256{};
        expect_reject(tx, "bad-angle-commitment", "guard-matrix: angle null spend hash");
    }
    {
        core::Transaction tx = angle_tx();
        tx.signer_address = Address();
        expect_reject(tx, "bad-angle-commitment", "guard-matrix: angle empty signer");
    }
    {
        core::Transaction tx = angle_tx();
        tx.inputs[0].signature = bytes{1};
        expect_reject(tx, "bad-angle-commitment", "guard-matrix: angle signature size");
    }

    auto multisig_tx = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::MULTISIG_SPEND;
        set_single_input(tx, prev, 1);
        set_single_output(tx, COIN, addr_a);
        tx.m = 2;
        tx.n = 3;
        tx.angle_commitment_hashes = {hash_from_string("guard-ms-a"), hash_from_string("guard-ms-b")};
        return tx;
    };
    {
        core::Transaction tx = multisig_tx();
        tx.inputs.clear();
        expect_reject(tx, "bad-multisig-spend", "guard-matrix: multisig missing input");
    }
    {
        core::Transaction tx = multisig_tx();
        tx.outputs.clear();
        expect_reject(tx, "bad-multisig-spend", "guard-matrix: multisig missing output");
    }
    {
        core::Transaction tx = multisig_tx();
        tx.angle_commitment_hashes.pop_back();
        expect_reject(tx, "bad-multisig-spend", "guard-matrix: multisig insufficient commitments");
    }
    {
        core::Transaction tx = multisig_tx();
        tx.m = 4;
        expect_reject(tx, "bad-multisig-spend", "guard-matrix: multisig bad m-n");
    }

    auto delegation_tx = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::DELEGATION_PROOF;
        set_single_input(tx, prev, 2);
        tx.primary_multisig_address = addr_a;
        tx.delegated_multisig_address = addr_b;
        tx.max_total_delegated = COIN;
        return tx;
    };
    {
        core::Transaction tx = delegation_tx();
        tx.inputs.clear();
        expect_reject(tx, "bad-delegation-proof", "guard-matrix: delegation missing input");
    }
    {
        core::Transaction tx = delegation_tx();
        set_single_output(tx, consensus::params::DUST_LIMIT - 1, addr_a);
        expect_reject(tx, "bad-delegation-proof", "guard-matrix: delegation dust change");
    }
    {
        core::Transaction tx = delegation_tx();
        tx.primary_multisig_address = Address();
        expect_reject(tx, "bad-delegation-proof", "guard-matrix: delegation empty primary");
    }
    {
        core::Transaction tx = delegation_tx();
        tx.delegated_multisig_address = Address();
        expect_reject(tx, "bad-delegation-proof", "guard-matrix: delegation empty delegated");
    }
    {
        core::Transaction tx = delegation_tx();
        tx.max_total_delegated = 0;
        expect_reject(tx, "bad-delegation-proof", "guard-matrix: delegation zero max");
    }

    auto revoke_tx = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::REVOKE_DELEGATION;
        set_single_input(tx, prev, 3);
        tx.revoked_delegation_proof_txid = hash_from_string("guard-revoke-proof");
        return tx;
    };
    {
        core::Transaction tx = revoke_tx();
        tx.inputs.clear();
        expect_reject(tx, "bad-revoke-delegation", "guard-matrix: revoke missing input");
    }
    {
        core::Transaction tx = revoke_tx();
        set_single_output(tx, COIN, addr_a);
        tx.outputs.push_back(core::TxOutput{COIN, addr_a, {}});
        expect_reject(tx, "bad-revoke-delegation", "guard-matrix: revoke multiple change outputs");
    }
    {
        core::Transaction tx = revoke_tx();
        tx.revoked_delegation_proof_txid = Hash256{};
        expect_reject(tx, "bad-revoke-delegation", "guard-matrix: revoke null proof");
    }

    nft::NFTCreationTransaction create_payload;
    create_payload.nft_id = hash_from_string("guard-nft-create");
    create_payload.creator_address = addr_a;
    create_payload.royalty_percent = 0;
    auto nft_create_tx = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::NFT_CREATE;
        tx.nft_create = create_payload;
        set_single_input(tx, prev, 4);
        set_single_output(tx, COIN, addr_a);
        return tx;
    };
    {
        core::Transaction tx = nft_create_tx();
        tx.nft_create.reset();
        expect_reject(tx, "bad-nft-create", "guard-matrix: nft create missing payload");
    }
    {
        core::Transaction tx = nft_create_tx();
        tx.inputs.clear();
        expect_reject(tx, "bad-nft-create", "guard-matrix: nft create missing input");
    }
    {
        core::Transaction tx = nft_create_tx();
        tx.outputs.clear();
        expect_reject(tx, "bad-nft-create", "guard-matrix: nft create missing output");
    }

    nft::NFTTransferTransaction transfer_payload;
    transfer_payload.nft_id = hash_from_string("guard-nft-transfer");
    transfer_payload.from = addr_a;
    transfer_payload.to = addr_b;
    auto nft_transfer_tx = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::NFT_TRANSFER;
        tx.nft_transfer = transfer_payload;
        set_single_input(tx, prev, 5);
        set_single_output(tx, COIN, addr_b);
        return tx;
    };
    {
        core::Transaction tx = nft_transfer_tx();
        tx.nft_transfer.reset();
        expect_reject(tx, "bad-nft-transfer", "guard-matrix: nft transfer missing payload");
    }
    {
        core::Transaction tx = nft_transfer_tx();
        tx.inputs.clear();
        expect_reject(tx, "bad-nft-transfer", "guard-matrix: nft transfer missing input");
    }
    {
        core::Transaction tx = nft_transfer_tx();
        tx.outputs.clear();
        expect_reject(tx, "bad-nft-transfer", "guard-matrix: nft transfer missing output");
    }

    nft::NFTBurnTransaction burn_payload;
    burn_payload.nft_id = hash_from_string("guard-nft-burn");
    burn_payload.owner = addr_a;
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::NFT_BURN;
        tx.nft_burn = burn_payload;
        set_single_input(tx, prev, 6);
        tx.nft_burn.reset();
        expect_reject(tx, "bad-nft-burn", "guard-matrix: nft burn missing payload");
    }
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::NFT_BURN;
        tx.nft_burn = burn_payload;
        expect_reject(tx, "bad-nft-burn", "guard-matrix: nft burn missing input");
    }

    auto cadastral_tx = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::CADASTRAL_DOC;
        tx.cadastral_doc_hash = hash_from_string("guard-cadastral-doc");
        tx.cadastral_nft_id = hash_from_string("guard-cadastral-parcel");
        tx.cadastral_owner = addr_a;
        set_single_input(tx, prev, 7);
        set_single_output(tx, COIN, addr_a);
        return tx;
    };
    {
        core::Transaction tx = cadastral_tx();
        tx.inputs.clear();
        expect_reject(tx, "bad-cadastral-doc", "guard-matrix: cadastral missing input");
    }
    {
        core::Transaction tx = cadastral_tx();
        tx.cadastral_doc_hash = Hash256{};
        expect_reject(tx, "bad-cadastral-doc", "guard-matrix: cadastral null hash");
    }
    {
        core::Transaction tx = cadastral_tx();
        tx.inputs.push_back(tx.inputs.front());
        expect_reject(tx, "bad-txns-inputs-duplicate", "guard-matrix: cadastral duplicate input");
    }

    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::INTEREST_DISTRIBUTION;
        set_single_input(tx, prev, 8);
        set_single_output(tx, COIN, addr_a);
        expect_reject(tx, "bad-interest-distribution", "guard-matrix: interest has input");
    }
    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::INTEREST_DISTRIBUTION;
        expect_reject(tx, "bad-interest-distribution", "guard-matrix: interest missing output");
    }
}

void test_consensus_block_and_header_guards() {
    const bytes key_a = make_seed(245);
    const Address addr_a = address_from_seed(key_a);
    const Address foundation_addr = address_from_seed(make_seed(246));
    const Hash256 prev = hash_from_string("block-guards-prev");
    check(!addr_a.str().empty() && !foundation_addr.str().empty(), "block-guards: addresses generated");

    auto expect_block_reject = [](const core::Block& block,
                                  const std::string& reason,
                                  const std::string& label,
                                  consensus::ValidationMode mode = consensus::ValidationMode::FULL) {
        consensus::ValidationState state;
        check(!consensus::check_block(block, state, mode), label + " rejected");
        check(state.get_reject_reason() == reason,
              label + " reason is " + reason + " (got " + state.get_reject_reason() + ")");
    };

    const Amount reward = consensus::ProofOfWork::calculate_block_reward(1);
    core::Transaction coinbase =
        core::Transaction::create_coinbase(1, addr_a, reward, 0, foundation_addr);
    {
        consensus::ValidationState height_state;
        check(consensus::check_coinbase_height(coinbase, 1, height_state),
              "block-guards: matching coinbase height accepted");

        consensus::ValidationState wrong_height_state;
        check(!consensus::check_coinbase_height(coinbase, 2, wrong_height_state),
              "block-guards: reused coinbase at another height rejected");
        check(wrong_height_state.get_reject_reason() == "bad-cb-height",
              "block-guards: reused coinbase height reason");

        core::Transaction short_height = coinbase;
        short_height.inputs[0].script_sig.resize(7);
        consensus::ValidationState short_height_state;
        check(!consensus::check_coinbase_height(short_height, 1, short_height_state),
              "block-guards: truncated coinbase height rejected");
    }
    core::Block good = make_block_for_test(prev, 1, {coinbase});
    {
        consensus::ValidationState state;
        check(consensus::check_block(good, state, consensus::ValidationMode::HEADER_ONLY),
              "block-guards: basic block accepted");
    }

    {
        core::Block block = good;
        block.transactions.clear();
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        expect_block_reject(block, "bad-blk-length", "block-guards: empty block");
    }

    {
        core::Transaction malformed;
        malformed.version = 2;
        malformed.type = core::TransactionType::NFT_CREATE;
        core::Block block;
        block.header = good.header;
        block.transactions = {coinbase, malformed};
        expect_block_reject(
            block,
            "bad-blk-serialization",
            "block-guards: malformed typed payload"
        );
    }

    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::STANDARD;
        tx.timestamp = 6100;
        set_single_input(tx, hash_from_string("block-guards-standard-prev"), 0);
        set_single_output(tx, COIN, addr_a);
        core::Block block = make_block_for_test(prev, 1, {tx});
        expect_block_reject(block, "bad-cb-missing", "block-guards: missing coinbase");
    }

    {
        core::Transaction second_coinbase =
            core::Transaction::create_coinbase(2, addr_a, reward, 0, foundation_addr);
        core::Block block = make_block_for_test(prev, 1, {coinbase, second_coinbase});
        expect_block_reject(block, "bad-cb-multiple", "block-guards: multiple coinbase");
    }

    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::STANDARD;
        tx.timestamp = 6101;
        set_single_input(tx, hash_from_string("block-guards-duplicate-prev"), 0);
        set_single_output(tx, COIN, addr_a);
        core::Block block = make_block_for_test(prev, 1, {coinbase, tx, tx});
        expect_block_reject(
            block,
            "bad-txns-duplicate",
            "block-guards: duplicate txid"
        );
        check(!block.verify(), "block-guards: duplicate txid fails Block::verify");
    }

    {
        core::Block block = good;
        block.header.merkle_root = hash_from_string("block-guards-bad-merkle");
        expect_block_reject(block, "bad-txnmrklroot", "block-guards: bad merkle root",
                            consensus::ValidationMode::HEADER_ONLY);
    }

    {
        core::Block block = good;
        block.header.witness_commitment = hash_from_string("block-guards-bad-witness");
        expect_block_reject(block, "bad-witness-commitment", "block-guards: bad witness commitment",
                            consensus::ValidationMode::HEADER_ONLY);
    }

    {
        core::Transaction cadastral;
        cadastral.version = 2;
        cadastral.type = core::TransactionType::CADASTRAL_DOC;
        cadastral.timestamp = 6101;
        cadastral.cadastral_doc_hash = hash_from_string("block-guards-cadastral");
        cadastral.cadastral_nft_id = hash_from_string("block-guards-parcel");
        cadastral.cadastral_owner = addr_a;
        set_single_input(cadastral, hash_from_string("block-guards-cadastral-prev"), 0);
        set_single_output(cadastral, COIN, addr_a);
        cadastral.sign_input(0, key_a);

        core::Block block = make_block_for_test(prev, 1, {coinbase, cadastral});
        block.header.geo_tag = 41;
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        consensus::ValidationState state;
        check(consensus::check_block(block, state, consensus::ValidationMode::HEADER_ONLY),
              "block-guards: cadastral document location follows its NFT, not the containing block");
    }

    {
        core::BlockHeader header = good.header;
        const auto now = std::chrono::system_clock::now();
        const auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()
        ).count();
        header.timestamp = static_cast<Timestamp>(now_ts + consensus::params::MAX_FUTURE_BLOCK_TIME + 1);
        consensus::ValidationState state;
        check(!consensus::check_block_header(header, state), "block-guards: future header rejected");
        check(state.get_reject_reason() == "time-too-new", "block-guards: future header reason");
    }

    {
        consensus::ValidationState state;
        check(!consensus::check_difficulty(
                  consensus::ProofOfWork::get_initial_bits() + 1,
                  nullptr,
                  good.header.timestamp,
                  state),
              "block-guards: bad genesis difficulty rejected");
        check(state.get_reject_reason() == "bad-diffbits", "block-guards: genesis difficulty reason");
    }

    {
        chain::BlockIndex prev_index(good.header);
        prev_index.height = 1;
        prev_index.hash = good.get_hash();
        core::BlockHeader header = good.header;
        header.prev_block_hash = hash_from_string("block-guards-wrong-prev");
        consensus::ValidationState state;
        check(!consensus::contextual_check_block_header(header, state, &prev_index),
              "block-guards: wrong prev hash rejected");
        check(state.get_reject_reason() == "bad-prevblk", "block-guards: wrong prev reason");

        header.prev_block_hash = prev_index.get_block_hash();
        header.timestamp = prev_index.get_median_time_past();
        consensus::ValidationState time_state;
        check(!consensus::contextual_check_block_header(header, time_state, &prev_index),
              "block-guards: old contextual timestamp rejected");
        check(time_state.get_reject_reason() == "time-too-old",
              "block-guards: old contextual timestamp reason");
    }

    {
        chain::BlockIndex max_height_prev(good.header);
        max_height_prev.height = (std::numeric_limits<Height>::max)();
        max_height_prev.hash = good.get_hash();
        core::BlockHeader header = good.header;
        header.prev_block_hash = max_height_prev.get_block_hash();
        consensus::ValidationState state;
        check(!consensus::contextual_check_block_header(header, state, &max_height_prev),
              "block-guards: height overflow rejected");
        check(state.get_reject_reason() == "bad-height-overflow",
              "block-guards: height overflow reason");
    }
}

void test_card_consensus_validation_guards() {
    const bytes key_a = make_seed(210);
    const Address addr_a = address_from_seed(key_a);
    const Hash256 prev = hash_from_string("card-consensus-guards-prev");
    check(!addr_a.str().empty(), "card-guards: address generated");

    auto make_card_program = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::CARD_PROGRAM_DEFINE;
        tx.timestamp = 5000;
        tx.card_program_id = hash_from_string("card-guards-program");
        tx.card_issuer = addr_a;
        tx.card_chip_vendor = "NXP";
        tx.card_chip_os = "JCOP4";
        tx.card_applet_aid = "A000000001";
        tx.card_applet_version = 1;
        tx.card_applet_package_hash = hash_from_string("card-guards-applet");
        tx.card_policy_id = 1;
        tx.card_scope_id = hash_from_string("card-guards-scope");
        set_single_input(tx, prev, 0);
        set_single_output(tx, COIN, addr_a);
        tx.sign_input(0, key_a);
        return tx;
    };

    {
        core::Transaction tx = make_card_program();
        tx.card_issuer = Address("GEO-invalid-address");
        consensus::ValidationState state;
        check(!consensus::check_transaction(tx, state), "card-guards: invalid issuer rejected");
    }

    {
        core::Transaction tx = make_card_program();
        tx.card_chip_vendor = std::string("NXP\n", 4);
        consensus::ValidationState state;
        check(!consensus::check_transaction(tx, state), "card-guards: non-printable vendor rejected");
    }

    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::CARD_ISSUE;
        tx.timestamp = 5001;
        tx.card_program_id = hash_from_string("card-guards-program");
        tx.card_issuer = addr_a;
        tx.card_policy_id = 1;
        tx.card_scope_id = hash_from_string("card-guards-scope");
        tx.card_uid_hash = hash_from_string("card-guards-uid");
        tx.card_pubkey_hash = hash_from_string("card-guards-pubkey");
        tx.card_valid_from = 1710000100;
        tx.card_valid_to = 1710000000; // меньше valid_from
        tx.card_status = 0;
        tx.card_metadata_hash = hash_from_string("card-guards-meta");
        set_single_input(tx, prev, 1);
        set_single_output(tx, COIN, addr_a);
        tx.sign_input(0, key_a);
        consensus::ValidationState state;
        check(!consensus::check_transaction(tx, state), "card-guards: invalid validity range rejected");
    }
}

void test_sidechain_consensus_validation_guards() {
    const bytes owner_key = make_seed(220);
    const Address owner_addr = address_from_seed(owner_key);
    check(!owner_addr.str().empty(), "sidechain-guards: owner address generated");

    const Hash256 sidechain_id = hash_from_string("sidechain-guards-id");
    const auto committee = make_sorted_committee(230, 3);
    const std::vector<Address> committee_members = committee_addresses(committee);
    const Hash256 committee_id = consensus::compute_sidechain_committee_id(2, 3, committee_members);

    auto make_issue = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::ISSUE_SIDECHAIN;
        tx.timestamp = 5100;
        tx.sidechain_id = sidechain_id;
        tx.sidechain_owner = owner_addr;
        tx.sidechain_amount = 0;
        tx.sidechain_unlock_height = 0;
        tx.sidechain_checkpoint = sidechain_id;
        tx.sidechain_token_symbol = "SCD";
        tx.sidechain_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        tx.sidechain_max_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
        tx.sidechain_fee_bps = 25;
        tx.sidechain_checkpoint_policy_id = 2;
        tx.sidechain_checkpoint_version = 2;
        tx.sidechain_committee_m = 2;
        tx.sidechain_committee_n = 3;
        tx.sidechain_committee_members = committee_members;
        tx.sidechain_committee_id = committee_id;
        tx.sidechain_signature = sign_sidechain_payload(tx, owner_key);
        return tx;
    };

    {
        core::Transaction tx = make_issue();
        tx.sidechain_token_symbol = std::string("SC\n", 3);
        tx.sidechain_signature = sign_sidechain_payload(tx, owner_key);
        consensus::ValidationState state;
        check(!consensus::check_transaction(tx, state), "sidechain-guards: non-printable token symbol rejected");
    }

    {
        core::Transaction tx = make_issue();
        tx.sidechain_owner = Address("GEO-invalid-owner");
        tx.sidechain_signature = sign_sidechain_payload(tx, owner_key);
        consensus::ValidationState state;
        check(!consensus::check_transaction(tx, state), "sidechain-guards: invalid owner rejected");
    }

    {
        core::Transaction tx = make_issue();
        tx.sidechain_committee_members = {
            Address("GEO-invalid-committee"),
            committee_members[1],
            committee_members[2]
        };
        std::sort(tx.sidechain_committee_members.begin(), tx.sidechain_committee_members.end(),
                  [](const Address& a, const Address& b) { return a.str() < b.str(); });
        tx.sidechain_signature = sign_sidechain_payload(tx, owner_key);
        consensus::ValidationState state;
        check(!consensus::check_transaction(tx, state), "sidechain-guards: invalid committee address rejected");
    }

    {
        core::Transaction tx = make_issue();
        tx.sidechain_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER - 1;
        tx.sidechain_signature = sign_sidechain_payload(tx, owner_key);
        consensus::ValidationState state;
        check(!consensus::check_transaction(tx, state), "sidechain-guards: too small multiplier rejected");
    }

    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        tx.timestamp = 5101;

        core::BatchCheckpointItem item{};
        item.sidechain_id = sidechain_id;
        item.checkpoint = hash_from_string("sidechain-guards-checkpoint");
        item.checkpoint_height = consensus::params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT + 1;
        item.fee = 1;
        item.committee_id = hash_from_string("sidechain-guards-committee");
        item.version = 2;
        item.proof = bytes{0x01};
        item.committee_signatures.push_back(bytes{0x02});
        tx.sidechain_batch_checkpoints.push_back(item);

        consensus::ValidationState state;
        check(!consensus::check_transaction(tx, state), "sidechain-guards: too large checkpoint_height rejected");
    }
}

void test_coinbase_interest_pool_activation() {
    const bytes miner_key = make_seed(200);
    const bytes foundation_key = make_seed(201);
    const Address miner_addr = address_from_seed(miner_key);
    const Address foundation_addr = address_from_seed(foundation_key);

    check(!miner_addr.str().empty(), "coinbase-pool: miner address generated");
    check(!foundation_addr.str().empty(), "coinbase-pool: foundation address generated");

    const Amount reward = consensus::ProofOfWork::get_initial_reward();
    const Amount fees = 1234;
    const Amount total = reward + fees;
    const Amount foundation_cut = consensus::calculate_foundation_reward_cut(total);
    const Amount expected_miner = total - foundation_cut;

    const Height activation = consensus::params::INTEREST_POOL_ACTIVATION_HEIGHT;
    const Height pre_activation = (activation > 0) ? (activation - 1) : 0;

    core::Transaction cb_pre = core::Transaction::create_coinbase(
        pre_activation,
        miner_addr,
        reward,
        fees,
        foundation_addr
    );

    check(cb_pre.outputs.size() == 2, "coinbase-pool: pre-activation has miner+foundation outputs");
    if (cb_pre.outputs.size() == 2) {
        check(cb_pre.outputs[0].address == miner_addr, "coinbase-pool: pre-activation miner address");
        check(cb_pre.outputs[0].value == expected_miner, "coinbase-pool: pre-activation miner amount");
        check(cb_pre.outputs[1].address == foundation_addr, "coinbase-pool: pre-activation foundation address");
        check(cb_pre.outputs[1].value == foundation_cut, "coinbase-pool: pre-activation foundation amount");
    }

    consensus::ValidationState pre_state;
    check(consensus::check_coinbase_outputs(
            cb_pre,
            pre_activation,
            reward,
            fees,
            foundation_addr,
            pre_state),
          "coinbase-pool: pre-activation coinbase accepted by consensus");

    core::Transaction cb_post = core::Transaction::create_coinbase(
        activation,
        miner_addr,
        reward,
        fees,
        foundation_addr
    );

    check(cb_post.outputs.size() == 1, "coinbase-pool: post-activation has only miner output");
    if (cb_post.outputs.size() == 1) {
        check(cb_post.outputs[0].address == miner_addr, "coinbase-pool: post-activation miner address");
        check(cb_post.outputs[0].value == expected_miner, "coinbase-pool: post-activation miner amount");
    }

    consensus::ValidationState post_state;
    check(consensus::check_coinbase_outputs(
            cb_post,
            activation,
            reward,
            fees,
            foundation_addr,
            post_state),
          "coinbase-pool: post-activation coinbase accepted by consensus");

    core::Transaction cb_bad = cb_post;
    core::TxOutput extra_foundation;
    extra_foundation.value = foundation_cut;
    extra_foundation.address = foundation_addr;
    cb_bad.outputs.push_back(extra_foundation);

    consensus::ValidationState bad_state;
    check(!consensus::check_coinbase_outputs(
              cb_bad,
              activation,
              reward,
              fees,
              foundation_addr,
              bad_state),
          "coinbase-pool: post-activation rejects foundation output in coinbase");
}

void test_consensus_contextual_and_commitment_guards() {
    const bytes key_a = make_seed(205);
    const bytes key_b = make_seed(206);
    const Address addr_a = address_from_seed(key_a);
    const Address addr_b = address_from_seed(key_b);
    const Hash256 prev = hash_from_string("contextual-guards-prev");

    consensus::ValidationState timestamp_state;
    check(consensus::get_median_time_past(nullptr) == 0, "contextual-guards: null MTP is zero");
    check(consensus::check_timestamp_against_prev(1000, nullptr, timestamp_state),
          "contextual-guards: null prev timestamp check is accepted");

    auto base_standard_tx = [&]() {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::STANDARD;
        tx.timestamp = 6000;
        set_single_input(tx, prev, 0);
        set_single_output(tx, COIN, addr_a);
        return tx;
    };

    auto expect_contextual_reject = [&](const core::Transaction& tx,
                                        consensus::ContextualTxLookup lookup,
                                        const std::string& reason,
                                        const std::string& label) {
        lookup.signatures_verified = true;
        consensus::ValidationState state;
        check(!consensus::contextual_check_transaction(tx, state, lookup), label + " rejected");
        check(state.get_reject_reason() == reason,
              label + " reason is " + reason + " (got " + state.get_reject_reason() + ")");
    };

    {
        core::Transaction tx = base_standard_tx();
        const Hash256 txid = tx.get_hash();
        consensus::ContextualTxLookup lookup;
        lookup.get_transaction_by_txid = [txid, tx](const Hash256& hash, core::Transaction& out) {
            if (hash != txid) {
                return false;
            }
            out = tx;
            return true;
        };
        expect_contextual_reject(tx, lookup, "bad-txns-duplicate",
                                 "contextual-guards: duplicate txid");
    }

    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::ANGLE_COMMITMENT;
        tx.timestamp = 6001;
        set_single_input(tx, prev, 1);
        tx.spending_tx_hash = hash_from_string("contextual-guards-angle-spending");

        consensus::ContextualTxLookup duplicate_lookup;
        duplicate_lookup.get_transaction_by_txid = [](const Hash256&, core::Transaction&) { return false; };
        duplicate_lookup.has_angle_commitment_key = [prev](const Hash256& key) { return key == prev; };
        expect_contextual_reject(tx, duplicate_lookup, "bad-angle-commitment-duplicate",
                                 "contextual-guards: duplicate angle commitment");

        consensus::ContextualTxLookup spent_lookup;
        spent_lookup.get_transaction_by_txid = [](const Hash256&, core::Transaction&) { return false; };
        spent_lookup.has_angle_commitment_key = [](const Hash256&) { return false; };
        const Hash256 spending_tx_hash = tx.spending_tx_hash;
        spent_lookup.has_multisig_spend_key =
            [spending_tx_hash](const Hash256& key) { return key == spending_tx_hash; };
        expect_contextual_reject(tx, spent_lookup, "bad-angle-commitment-spent",
                                 "contextual-guards: angle for spent multisig");
    }

    {
        core::Transaction tx = base_standard_tx();
        tx.type = core::TransactionType::MULTISIG_SPEND;
        tx.m = 2;
        tx.n = 3;
        tx.spend_participants = {addr_a, addr_b, address_from_seed(make_seed(207))};
        tx.angle_commitment_hashes = {
            hash_from_string("contextual-guards-angle-1"),
            hash_from_string("contextual-guards-angle-2")
        };
        const Hash256 spending_key = tx.get_hash_without_commitments();

        consensus::ContextualTxLookup lookup;
        lookup.get_transaction_by_txid = [](const Hash256&, core::Transaction&) { return false; };
        lookup.has_multisig_spend_key = [spending_key](const Hash256& key) { return key == spending_key; };
        expect_contextual_reject(tx, lookup, "bad-multisig-spend-duplicate",
                                 "contextual-guards: duplicate multisig spend");
    }

    {
        core::Transaction tx = base_standard_tx();
        tx.type = core::TransactionType::MULTISIG_SPEND;
        tx.m = 1;
        tx.n = 2;
        tx.delegation_proof_txid = hash_from_string("contextual-guards-delegation-proof");
        tx.angle_commitment_hashes = {hash_from_string("contextual-guards-hier-angle")};
        const Hash256 hierarchical_key = tx.get_hierarchical_spend_hash();

        consensus::ContextualTxLookup lookup;
        lookup.get_transaction_by_txid = [](const Hash256&, core::Transaction&) { return false; };
        lookup.has_hierarchical_spend_key =
            [hierarchical_key](const Hash256& key) { return key == hierarchical_key; };
        expect_contextual_reject(tx, lookup, "bad-hierarchical-spend-duplicate",
                                 "contextual-guards: duplicate hierarchical spend");
    }

    core::Transaction delegation_tx;
    delegation_tx.version = 2;
    delegation_tx.type = core::TransactionType::DELEGATION_PROOF;
    delegation_tx.timestamp = 6002;
    set_single_input(delegation_tx, prev, 2);
    delegation_tx.primary_multisig_address = addr_a;
    delegation_tx.delegated_multisig_address = addr_b;
    delegation_tx.primary_m = 1;
    delegation_tx.primary_n = 2;
    delegation_tx.delegated_m = 1;
    delegation_tx.delegated_n = 2;
    delegation_tx.valid_until = 7000;

    {
        const Hash256 delegation_key = delegation_tx.get_delegation_proof_hash();
        consensus::ContextualTxLookup lookup;
        lookup.get_transaction_by_txid = [](const Hash256&, core::Transaction&) { return false; };
        lookup.has_delegation_proof_key =
            [delegation_key](const Hash256& key) { return key == delegation_key; };
        expect_contextual_reject(delegation_tx, lookup, "bad-delegation-proof-duplicate",
                                 "contextual-guards: duplicate delegation proof");
    }

    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::REVOKE_DELEGATION;
        tx.timestamp = 6003;
        set_single_input(tx, prev, 3);
        tx.revoked_delegation_proof_txid = hash_from_string("contextual-guards-revoked-delegation");
        const Hash256 delegation_txid = tx.revoked_delegation_proof_txid;

        consensus::ContextualTxLookup missing_lookup;
        missing_lookup.get_transaction_by_txid = [](const Hash256&, core::Transaction&) { return false; };
        expect_contextual_reject(tx, missing_lookup, "bad-revoke-delegation-notfound",
                                 "contextual-guards: missing delegation revoke");

        consensus::ContextualTxLookup wrong_type_lookup;
        wrong_type_lookup.get_transaction_by_txid = [delegation_txid](const Hash256& hash, core::Transaction& out) {
            if (hash != delegation_txid) {
                return false;
            }
            out.type = core::TransactionType::STANDARD;
            return true;
        };
        expect_contextual_reject(tx, wrong_type_lookup, "bad-revoke-delegation-type",
                                 "contextual-guards: revoke wrong transaction type");

        consensus::ContextualTxLookup duplicate_lookup;
        duplicate_lookup.get_transaction_by_txid =
            [delegation_txid, delegation_tx](const Hash256& hash, core::Transaction& out) {
                if (hash != delegation_txid) {
                    return false;
                }
                out = delegation_tx;
                return true;
            };
        duplicate_lookup.has_revoke_delegation_key =
            [delegation_txid](const Hash256& key) { return key == delegation_txid; };
        expect_contextual_reject(tx, duplicate_lookup, "bad-revoke-delegation-duplicate",
                                 "contextual-guards: duplicate revoke delegation");
    }

    {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::NFT_CREATE;
        tx.timestamp = 6004;
        nft::NFTCreationTransaction create;
        create.nft_id = hash_from_string("contextual-guards-nft-id");
        create.creator_address = addr_a;
        tx.nft_create = create;
        set_single_input(tx, prev, 4);
        set_single_output(tx, COIN, addr_a);
        const Hash256 nft_id = create.nft_id;

        consensus::ContextualTxLookup lookup;
        lookup.has_nft_id_key = [nft_id](const Hash256& key) { return key == nft_id; };
        expect_contextual_reject(tx, lookup, "bad-nft-create-duplicate",
                                 "contextual-guards: duplicate nft id");
    }

    {
        const Amount reward = consensus::ProofOfWork::get_initial_reward();
        const Amount fees = 123;
        core::Transaction genesis =
            core::Transaction::create_coinbase(0, addr_a, reward, fees, addr_b);

        consensus::ValidationState ok_state;
        check(consensus::check_coinbase_outputs(genesis, 0, reward, fees, addr_b, ok_state),
              "contextual-guards: genesis coinbase accepted");

        core::Transaction bad_outputs = genesis;
        core::TxOutput extra;
        extra.value = 1;
        extra.address = addr_a;
        bad_outputs.outputs.push_back(extra);
        consensus::ValidationState outputs_state;
        check(!consensus::check_coinbase_outputs(bad_outputs, 0, reward, fees, addr_b, outputs_state),
              "contextual-guards: genesis extra output rejected");
        check(outputs_state.get_reject_reason() == "bad-cb-genesis-outputs",
              "contextual-guards: genesis extra output reason");

        core::Transaction bad_addr = genesis;
        bad_addr.outputs[0].address = addr_a;
        consensus::ValidationState addr_state;
        check(!consensus::check_coinbase_outputs(bad_addr, 0, reward, fees, addr_b, addr_state),
              "contextual-guards: genesis wrong address rejected");
        check(addr_state.get_reject_reason() == "bad-cb-genesis-addr",
              "contextual-guards: genesis wrong address reason");

        core::Transaction bad_amount = genesis;
        bad_amount.outputs[0].value += 1;
        consensus::ValidationState amount_state;
        check(!consensus::check_coinbase_outputs(bad_amount, 0, reward, fees, addr_b, amount_state),
              "contextual-guards: genesis wrong amount rejected");
        check(amount_state.get_reject_reason() == "bad-cb-genesis-amount",
              "contextual-guards: genesis wrong amount reason");

        const Height post_height = consensus::params::INTEREST_POOL_ACTIVATION_HEIGHT;
        const Amount post_reward = consensus::ProofOfWork::calculate_block_reward(post_height);
        core::Transaction post_pool =
            core::Transaction::create_coinbase(post_height, addr_a, post_reward, fees, addr_b);
        post_pool.outputs[0].value += 1;
        consensus::ValidationState post_state;
        check(!consensus::check_coinbase_outputs(post_pool, post_height, post_reward, fees, addr_b, post_state),
              "contextual-guards: post-pool wrong miner amount rejected");
        check(post_state.get_reject_reason() == "bad-cb-miner-amount",
              "contextual-guards: post-pool wrong miner amount reason");
    }
}

void test_sidechain_state_flow_with_batch() {
    state::ChainState st(std::make_unique<state::MemoryStorage>());
    const Height spend_height_base = consensus::params::COINBASE_MATURITY + 1;

    const bytes owner_key = make_seed(90);
    const Address owner_addr = address_from_seed(owner_key);
    check(!owner_addr.str().empty(), "owner address generated for sidechain flow");

    const auto committee_old = make_sorted_committee(100, 3);
    const auto committee_old_members = committee_addresses(committee_old);
    const Hash256 committee_old_id = consensus::compute_sidechain_committee_id(2, 3, committee_old_members);

    const Hash256 sidechain_id = hash_from_string("sidechain-flow");

    core::Transaction coinbase = make_coinbase(owner_addr, 10000 * COIN);
    check(st.apply_transaction(coinbase, 1, true), "flow: apply coinbase");
    st.set_height(spend_height_base - 1);

    core::Transaction lock;
    lock.version = 2;
    lock.type = core::TransactionType::LOCK_DEPOSIT;
    lock.timestamp = 2;
    set_single_input(lock, coinbase.get_hash(), 0);
    set_single_output(lock, 6000 * COIN, owner_addr);
    lock.sidechain_id = sidechain_id;
    lock.sidechain_owner = owner_addr;
    lock.sidechain_amount = 3900 * COIN;
    lock.sidechain_unlock_height = 10;
    lock.sidechain_signature = sign_sidechain_payload(lock, owner_key);
    lock.sign_input(0, owner_key);
    check(st.validate_transaction(lock), "flow: lock validate");
    check(st.apply_transaction(lock, spend_height_base, false), "flow: lock apply");

    core::Transaction issue;
    issue.version = 2;
    issue.type = core::TransactionType::ISSUE_SIDECHAIN;
    issue.timestamp = 3;
    issue.sidechain_id = sidechain_id;
    issue.sidechain_owner = owner_addr;
    issue.sidechain_amount = 0;
    issue.sidechain_unlock_height = 0;
    issue.sidechain_checkpoint = sidechain_id;
    issue.sidechain_token_symbol = "SDF";
    issue.sidechain_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_fee_bps = 25;
    issue.sidechain_max_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
    issue.sidechain_checkpoint_policy_id = 2;
    issue.sidechain_checkpoint_version = 2;
    issue.sidechain_committee_m = 2;
    issue.sidechain_committee_n = 3;
    issue.sidechain_committee_members = committee_old_members;
    issue.sidechain_committee_id = committee_old_id;
    issue.sidechain_signature = sign_sidechain_payload(issue, owner_key);
    check(st.validate_transaction(issue), "flow: issue validate");
    check(st.apply_transaction(issue, spend_height_base + 1, false), "flow: issue apply");

    core::Transaction mint;
    mint.version = 2;
    mint.type = core::TransactionType::MINT_SIDECHAIN;
    mint.timestamp = 4;
    mint.sidechain_id = sidechain_id;
    mint.sidechain_owner = owner_addr;
    mint.sidechain_amount = 1000 * COIN;
    mint.sidechain_committee_id = committee_old_id;
    add_committee_signatures_for_sidechain_payload(mint, committee_old, 2);
    check(st.validate_transaction(mint), "flow: mint validate");
    check(st.apply_transaction(mint, spend_height_base + 2, false), "flow: mint apply");

    const CheckpointProofV2Fixture first_fixture =
        build_checkpoint_proof_v2_for_test(sidechain_id, 1, 1000 * COIN, "flow-checkpoint-1");
    core::Transaction checkpoint_batch;
    checkpoint_batch.version = 2;
    checkpoint_batch.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    checkpoint_batch.timestamp = 5;
    core::BatchCheckpointItem cp_item;
    cp_item.sidechain_id = sidechain_id;
    cp_item.checkpoint = first_fixture.checkpoint;
    cp_item.checkpoint_height = 1;
    cp_item.fee = consensus::calculate_sidechain_checkpoint_min_fee(spend_height_base + 3, 0);
    cp_item.version = 2;
    cp_item.committee_id = committee_old_id;
    cp_item.proof = first_fixture.proof;
    cp_item.committee_signatures.push_back(sign_sidechain_batch_item_payload(cp_item, committee_old[0].private_key));
    cp_item.committee_signatures.push_back(sign_sidechain_batch_item_payload(cp_item, committee_old[1].private_key));
    checkpoint_batch.sidechain_batch_checkpoints.push_back(cp_item);
    check(st.validate_transaction(checkpoint_batch), "flow: checkpoint(batch v2) validate");
    check(st.apply_transaction(checkpoint_batch, spend_height_base + 3, false), "flow: checkpoint(batch v2) apply");

    core::Transaction rejected_spend;
    rejected_spend.version = 2;
    rejected_spend.type = core::TransactionType::SPEND_DEPOSIT;
    rejected_spend.timestamp = 6;
    rejected_spend.sidechain_id = sidechain_id;
    rejected_spend.sidechain_owner = owner_addr;
    rejected_spend.sidechain_amount = 10 * COIN;
    set_single_output(rejected_spend, 10 * COIN, owner_addr);
    rejected_spend.sidechain_signature = sign_sidechain_payload(rejected_spend, owner_key);
    check(!st.validate_transaction(rejected_spend), "flow: spend rejected after sidechain issue");

    core::Transaction redeem;
    redeem.version = 2;
    redeem.type = core::TransactionType::REDEEM_SIDECHAIN;
    redeem.timestamp = 7;
    redeem.sidechain_id = sidechain_id;
    redeem.sidechain_owner = owner_addr;
    redeem.sidechain_amount = 100 * COIN;
    set_single_output(redeem, 100 * COIN, owner_addr);
    redeem.sidechain_committee_id = committee_old_id;
    add_committee_signatures_for_sidechain_payload(redeem, committee_old, 2);
    check(st.validate_transaction(redeem), "flow: redeem validate");
    check(st.apply_transaction(redeem, spend_height_base + 5, false), "flow: redeem apply");

    core::Transaction update_committee;
    update_committee.version = 2;
    update_committee.type = core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE;
    update_committee.timestamp = 8;
    update_committee.sidechain_id = sidechain_id;
    update_committee.sidechain_owner = owner_addr;
    update_committee.sidechain_amount = 0;
    update_committee.sidechain_unlock_height = 0;
    update_committee.sidechain_committee_m = 2;
    update_committee.sidechain_committee_n = 3;
    update_committee.sidechain_committee_members = committee_old_members;
    update_committee.sidechain_committee_id = committee_old_id;
    add_committee_signatures_for_committee_update_payload(update_committee, committee_old, 2);
    check(st.validate_transaction(update_committee), "flow: update committee validate (same committee)");
    check(st.apply_transaction(update_committee, spend_height_base + 6, false), "flow: update committee apply (same committee)");

    // Продвигаем высоту до 9 обычной транзакцией, чтобы batch checkpoint прошел интервал.
    core::Transaction tick9;
    tick9.version = 2;
    tick9.type = core::TransactionType::STANDARD;
    tick9.timestamp = 9;
    set_single_input(tick9, lock.get_hash(), 0);
    set_single_output(tick9, 5999 * COIN, owner_addr);
    tick9.sign_input(0, owner_key);
    check(st.validate_transaction(tick9), "flow: tick9 validate");
    check(st.apply_transaction(tick9, spend_height_base + 7, false), "flow: tick9 apply");

    core::Transaction batch;
    batch.version = 2;
    batch.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    batch.timestamp = 10;
    core::BatchCheckpointItem item;
    item.sidechain_id = sidechain_id;
    const CheckpointProofV2Fixture second_fixture =
        build_checkpoint_proof_v2_for_test(sidechain_id, 2, 900 * COIN, "flow-checkpoint-2");
    item.checkpoint = second_fixture.checkpoint;
    item.checkpoint_height = 2;
    item.fee = consensus::calculate_sidechain_checkpoint_min_fee(spend_height_base + 8, spend_height_base + 3);
    item.version = 2;
    item.committee_id = committee_old_id;
    item.proof = second_fixture.proof;
    item.committee_signatures.push_back(sign_sidechain_batch_item_payload(item, committee_old[0].private_key));
    item.committee_signatures.push_back(sign_sidechain_batch_item_payload(item, committee_old[1].private_key));
    batch.sidechain_batch_checkpoints.push_back(item);

    st.set_block_storage(make_block_storage_with_height(spend_height_base + 7));

    state::SidechainDeposit dep_before_batch;
    state::SidechainRegistryEntry reg_before_batch;
    check(st.get_sidechain_deposit(sidechain_id, dep_before_batch), "flow: deposit before batch exists");
    check(st.get_sidechain_registry(sidechain_id, reg_before_batch), "flow: registry before batch exists");
    check(item.committee_id == reg_before_batch.committee_id, "flow: batch committee_id matches registry");
    check(item.checkpoint_height > dep_before_batch.last_sidechain_checkpoint_height,
          "flow: batch checkpoint height is monotonic");
    check(item.fee >= consensus::calculate_sidechain_checkpoint_min_fee(spend_height_base + 8, dep_before_batch.last_checkpoint_height),
          "flow: batch fee satisfies minimum");

    check(st.validate_transaction(batch), "flow: batch validate");
    check(st.apply_transaction(batch, spend_height_base + 8, false), "flow: batch apply");

    // Еще один шаг до высоты 11.
    core::Transaction tick11;
    tick11.version = 2;
    tick11.type = core::TransactionType::STANDARD;
    tick11.timestamp = 11;
    set_single_input(tick11, tick9.get_hash(), 0);
    set_single_output(tick11, 5998 * COIN, owner_addr);
    tick11.sign_input(0, owner_key);
    check(st.validate_transaction(tick11), "flow: tick11 validate");
    check(st.apply_transaction(tick11, spend_height_base + 9, false), "flow: tick11 apply");

    state::SidechainDeposit dep;
    check(st.get_sidechain_deposit(sidechain_id, dep), "flow: deposit still exists");

    core::Transaction redeem_remaining;
    redeem_remaining.version = 2;
    redeem_remaining.type = core::TransactionType::REDEEM_SIDECHAIN;
    redeem_remaining.timestamp = 12;
    redeem_remaining.sidechain_id = sidechain_id;
    redeem_remaining.sidechain_owner = owner_addr;
    redeem_remaining.sidechain_amount = 900 * COIN;
    set_single_output(redeem_remaining, 900 * COIN, owner_addr);
    redeem_remaining.sidechain_committee_id = committee_old_id;
    add_committee_signatures_for_sidechain_payload(redeem_remaining, committee_old, 2);
    check(st.validate_transaction(redeem_remaining), "flow: redeem remaining validate");
    check(st.apply_transaction(redeem_remaining, spend_height_base + 10, false), "flow: redeem remaining apply");

    core::Transaction decommission;
    decommission.version = 2;
    decommission.type = core::TransactionType::DECOMMISSION_SIDECHAIN;
    decommission.timestamp = 13;
    decommission.sidechain_id = sidechain_id;
    decommission.sidechain_owner = owner_addr;
    decommission.sidechain_amount = 0;
    decommission.sidechain_unlock_height = 0;
    decommission.sidechain_checkpoint = Hash256{};
    decommission.sidechain_committee_id = committee_old_id;
    decommission.sidechain_signature = sign_sidechain_payload(decommission, owner_key);
    add_committee_signatures_for_sidechain_payload(decommission, committee_old, 2);
    check(st.validate_transaction(decommission), "flow: decommission validate");
    const Height decommission_height = spend_height_base + 11;
    check(st.apply_transaction(decommission, decommission_height, false), "flow: decommission apply");
    state::SidechainRegistryEntry reg_after_decommission;
    check(st.get_sidechain_registry(sidechain_id, reg_after_decommission) &&
              reg_after_decommission.status == state::SidechainStatus::DECOMMISSIONING &&
              reg_after_decommission.decommission_height == decommission_height,
          "flow: registry remains slashable during decommission challenge");

    check(st.get_sidechain_deposit(sidechain_id, dep), "flow: deposit still exists after decommission");

    core::Transaction unlock;
    unlock.version = 2;
    unlock.type = core::TransactionType::UNLOCK_DEPOSIT;
    unlock.timestamp = 14;
    unlock.sidechain_id = sidechain_id;
    unlock.sidechain_owner = owner_addr;
    unlock.sidechain_amount = 0;
    unlock.sidechain_unlock_height = 0;
    set_single_output(unlock, dep.amount, owner_addr);
    unlock.sidechain_signature = sign_sidechain_payload(unlock, owner_key);
    check(!st.validate_transaction(unlock), "flow: unlock rejected during decommission challenge");
    const Height challenge_end = decommission_height +
        consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS;
    st.set_height(challenge_end - 1);
    check(st.validate_transaction(unlock), "flow: unlock validate");
    check(st.apply_transaction(unlock, challenge_end, false), "flow: unlock apply after challenge");
    check(!st.get_sidechain_deposit(sidechain_id, dep), "flow: deposit removed after unlock");
    check(!st.get_sidechain_registry(sidechain_id, reg_after_decommission),
          "flow: registry removed together with deposit after challenge");
}

void test_sidechain_replay_and_spend_guards() {
    state::ChainState st(std::make_unique<state::MemoryStorage>());
    const Height lock_height = consensus::params::COINBASE_MATURITY + 1;

    const bytes owner_key = make_seed(180);
    const Address owner_addr = address_from_seed(owner_key);
    const auto committee = make_sorted_committee(190, 3);
    const auto committee_members = committee_addresses(committee);
    const Hash256 committee_id = consensus::compute_sidechain_committee_id(2, 3, committee_members);
    const Hash256 sidechain_id = hash_from_string("sidechain-replay-guards");

    core::Transaction coinbase = make_coinbase(owner_addr, 10000 * COIN);
    check(st.apply_transaction(coinbase, 1, true), "guards: apply coinbase");
    st.set_height(lock_height - 1);

    core::Transaction lock;
    lock.version = 2;
    lock.type = core::TransactionType::LOCK_DEPOSIT;
    lock.timestamp = 2;
    set_single_input(lock, coinbase.get_hash(), 0);
    set_single_output(lock, 6000 * COIN, owner_addr);
    lock.sidechain_id = sidechain_id;
    lock.sidechain_owner = owner_addr;
    lock.sidechain_amount = 3900 * COIN;
    lock.sidechain_unlock_height = lock_height + 100;
    lock.sidechain_signature = sign_sidechain_payload(lock, owner_key);
    lock.sign_input(0, owner_key);
    check(st.validate_transaction(lock), "guards: lock validate");
    check(st.apply_transaction(lock, lock_height, false), "guards: lock apply");
    st.set_height(lock_height);

    core::Transaction premature_spend;
    premature_spend.version = 2;
    premature_spend.type = core::TransactionType::SPEND_DEPOSIT;
    premature_spend.timestamp = 3;
    premature_spend.sidechain_id = sidechain_id;
    premature_spend.sidechain_owner = owner_addr;
    premature_spend.sidechain_amount = 10 * COIN;
    set_single_output(premature_spend, 10 * COIN, owner_addr);
    premature_spend.sidechain_signature = sign_sidechain_payload(premature_spend, owner_key);
    check(!st.validate_transaction(premature_spend), "guards: SPEND_DEPOSIT before unlock is rejected");

    core::Transaction issue;
    issue.version = 2;
    issue.type = core::TransactionType::ISSUE_SIDECHAIN;
    issue.timestamp = 4;
    issue.sidechain_id = sidechain_id;
    issue.sidechain_owner = owner_addr;
    issue.sidechain_amount = 0;
    issue.sidechain_unlock_height = 0;
    issue.sidechain_checkpoint = sidechain_id;
    issue.sidechain_token_symbol = "GRD";
    issue.sidechain_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_fee_bps = 25;
    issue.sidechain_max_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
    issue.sidechain_checkpoint_policy_id = 2;
    issue.sidechain_checkpoint_version = 2;
    issue.sidechain_committee_m = 2;
    issue.sidechain_committee_n = 3;
    issue.sidechain_committee_members = committee_members;
    issue.sidechain_committee_id = committee_id;
    issue.sidechain_signature = sign_sidechain_payload(issue, owner_key);
    check(st.validate_transaction(issue), "guards: issue validate");
    check(st.apply_transaction(issue, lock_height + 1, false), "guards: issue apply");
    st.set_height(lock_height + 1);

    core::Transaction registered_spend = premature_spend;
    registered_spend.timestamp = 5;
    registered_spend.sidechain_signature = sign_sidechain_payload(registered_spend, owner_key);
    check(!st.validate_transaction(registered_spend), "guards: SPEND_DEPOSIT after sidechain issue is rejected");

    core::Transaction mint;
    mint.version = 2;
    mint.type = core::TransactionType::MINT_SIDECHAIN;
    mint.timestamp = 6;
    mint.sidechain_id = sidechain_id;
    mint.sidechain_owner = owner_addr;
    mint.sidechain_amount = 1000 * COIN;
    mint.sidechain_committee_id = committee_id;
    add_committee_signatures_for_sidechain_payload(mint, committee, 2);
    check(st.validate_transaction(mint), "guards: mint validate");
    check(st.apply_transaction(mint, lock_height + 2, false), "guards: mint apply");
    st.set_height(lock_height + 2);

    core::Transaction redeem;
    redeem.version = 2;
    redeem.type = core::TransactionType::REDEEM_SIDECHAIN;
    redeem.timestamp = 7;
    redeem.sidechain_id = sidechain_id;
    redeem.sidechain_owner = owner_addr;
    redeem.sidechain_amount = 100 * COIN;
    set_single_output(redeem, 100 * COIN, owner_addr);
    redeem.sidechain_committee_id = committee_id;
    add_committee_signatures_for_sidechain_payload(redeem, committee, 2);
    check(st.validate_transaction(redeem), "guards: redeem validate");
    check(st.apply_transaction(redeem, lock_height + 3, false), "guards: redeem apply");
    st.set_height(lock_height + 3);

    check(!st.validate_transaction(redeem), "guards: confirmed no-input sidechain tx replay is rejected");
    check(!st.apply_transaction(redeem, lock_height + 4, false), "guards: replay apply is rejected");
}

void test_sidechain_batch_v2_proof_validation() {
    state::ChainState st(std::make_unique<state::MemoryStorage>());
    const Height spend_height_base = consensus::params::COINBASE_MATURITY + 1;

    const bytes owner_key = make_seed(140);
    const Address owner_addr = address_from_seed(owner_key);
    check(!owner_addr.str().empty(), "v2: owner address generated");

    const auto committee = make_sorted_committee(150, 3);
    const std::vector<Address> committee_members = committee_addresses(committee);
    const Hash256 committee_id = consensus::compute_sidechain_committee_id(2, 3, committee_members);
    const Hash256 sidechain_id = hash_from_string("sidechain-v2-proof");

    core::Transaction coinbase = make_coinbase(owner_addr, 10000 * COIN);
    check(st.apply_transaction(coinbase, 1, true), "v2: apply coinbase");
    st.set_height(spend_height_base - 1);

    core::Transaction lock;
    lock.version = 2;
    lock.type = core::TransactionType::LOCK_DEPOSIT;
    lock.timestamp = 2;
    set_single_input(lock, coinbase.get_hash(), 0);
    set_single_output(lock, 6000 * COIN, owner_addr);
    lock.sidechain_id = sidechain_id;
    lock.sidechain_owner = owner_addr;
    lock.sidechain_amount = 3900 * COIN;
    lock.sidechain_unlock_height = 20;
    lock.sidechain_signature = sign_sidechain_payload(lock, owner_key);
    lock.sign_input(0, owner_key);
    check(st.validate_transaction(lock), "v2: lock validate");
    check(st.apply_transaction(lock, spend_height_base, false), "v2: lock apply");

    core::Transaction issue;
    issue.version = 2;
    issue.type = core::TransactionType::ISSUE_SIDECHAIN;
    issue.timestamp = 3;
    issue.sidechain_id = sidechain_id;
    issue.sidechain_owner = owner_addr;
    issue.sidechain_amount = 0;
    issue.sidechain_unlock_height = 0;
    issue.sidechain_checkpoint = sidechain_id;
    issue.sidechain_token_symbol = "SV2";
    issue.sidechain_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_fee_bps = 25;
    issue.sidechain_max_multiplier = consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
    issue.sidechain_checkpoint_policy_id = 2;
    issue.sidechain_checkpoint_version = 2;
    issue.sidechain_committee_m = 2;
    issue.sidechain_committee_n = 3;
    issue.sidechain_committee_members = committee_members;
    issue.sidechain_committee_id = committee_id;
    issue.sidechain_signature = sign_sidechain_payload(issue, owner_key);
    check(st.validate_transaction(issue), "v2: issue validate");
    check(st.apply_transaction(issue, spend_height_base + 1, false), "v2: issue apply");

    const Height checkpoint_height = 1;
    const Amount total_supply = 0;
    const Amount fee = consensus::calculate_sidechain_checkpoint_min_fee(spend_height_base + 2, 0);
    const CheckpointProofV2Fixture fixture =
        build_checkpoint_proof_v2_for_test(sidechain_id, checkpoint_height, total_supply, "v2-proof");

    core::Transaction bad_batch;
    bad_batch.version = 2;
    bad_batch.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    bad_batch.timestamp = 4;
    core::BatchCheckpointItem bad_item;
    bad_item.sidechain_id = sidechain_id;
    bad_item.checkpoint = fixture.checkpoint;
    bad_item.checkpoint_height = checkpoint_height;
    bad_item.fee = fee;
    bad_item.version = 2;
    bad_item.committee_id = committee_id;
    bad_item.proof = fixture.proof;
    if (!bad_item.proof.empty()) {
        bad_item.proof.back() ^= 0x01; // ломаем merkle-path, подписи считаются уже на испорченном proof
    }
    bad_item.committee_signatures.push_back(sign_sidechain_batch_item_payload(bad_item, committee[0].private_key));
    bad_item.committee_signatures.push_back(sign_sidechain_batch_item_payload(bad_item, committee[1].private_key));
    bad_batch.sidechain_batch_checkpoints.push_back(bad_item);
    check(!st.validate_transaction(bad_batch), "v2: reject tampered proof");

    core::Transaction good_batch;
    good_batch.version = 2;
    good_batch.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    good_batch.timestamp = 4;
    core::BatchCheckpointItem good_item;
    good_item.sidechain_id = sidechain_id;
    good_item.checkpoint = fixture.checkpoint;
    good_item.checkpoint_height = checkpoint_height;
    good_item.fee = fee;
    good_item.version = 2;
    good_item.committee_id = committee_id;
    good_item.proof = fixture.proof;
    good_item.committee_signatures.push_back(sign_sidechain_batch_item_payload(good_item, committee[0].private_key));
    good_item.committee_signatures.push_back(sign_sidechain_batch_item_payload(good_item, committee[1].private_key));
    good_batch.sidechain_batch_checkpoints.push_back(good_item);
    check(st.validate_transaction(good_batch), "v2: validate good proof");
    check(st.apply_transaction(good_batch, spend_height_base + 2, false), "v2: apply good proof");
}

void test_witness_prune_restore_integrity() {
    auto storage_ptr = make_temp_block_storage("witness_prune_restore");
    check(static_cast<bool>(storage_ptr), "witness: storage created");
    if (!storage_ptr) {
        return;
    }

    const bytes key = make_seed(151);
    const Address addr = address_from_seed(key);
    check(!addr.str().empty(), "witness: address generated");

    core::Transaction tx;
    tx.version = 2;
    tx.type = core::TransactionType::STANDARD;
    tx.timestamp = 200;
    set_single_input(tx, hash_from_string("witness-prev"), 0);
    set_single_output(tx, COIN, addr);
    tx.sign_input(0, key);
    check(tx.verify(), "witness: source tx verifies before prune");

    core::Block block = make_block_for_test(hash_from_string("witness-prev-block"), 1, {tx});
    int32_t file = 0;
    uint32_t pos = 0;
    check(storage_ptr->save_block_with_pos(block, 1, file, pos, false), "witness: save block");
    check(storage_ptr->set_active_block(1, block.get_hash()), "witness: set active block");
    check(storage_ptr->set_active_tip(block.get_hash(), 1), "witness: set active tip");

    check(storage_ptr->prune_witness_at_height(1), "witness: prune");

    core::Block pruned;
    check(storage_ptr->get_block(block.get_hash(), pruned, false), "witness: read pruned block");
    bool has_material_witness = false;
    for (const auto& ptx : pruned.transactions) {
        for (const auto& w : ptx.witnesses) {
            if (!w.signature.empty() || !w.public_key.empty()) {
                has_material_witness = true;
                break;
            }
        }
        if (has_material_witness) {
            break;
        }
    }
    check(!has_material_witness, "witness: pruned block has no material witness");

    bytes payload;
    check(storage_ptr->get_witness_payload(block.get_hash(), payload), "witness: payload persisted");
    check(!payload.empty(), "witness: payload is not empty");

    bytes tampered = payload;
    tampered.back() ^= 0x01u;
    check(!storage_ptr->restore_witness_from_payload(block.get_hash(), tampered),
          "witness: reject tampered payload");

    check(storage_ptr->restore_witness_from_payload(block.get_hash(), payload),
          "witness: restore original payload");
    core::Block restored;
    check(storage_ptr->get_block(block.get_hash(), restored, false), "witness: read restored block");
    check(!restored.transactions.empty(), "witness: restored tx exists");
    if (!restored.transactions.empty()) {
        check(restored.transactions[0].verify(), "witness: restored tx.verify");
    }
}

void test_witness_pubkey_refcount_on_reorg() {
    auto chain_storage = std::make_unique<state::MemoryStorage>();
    state::ChainState st(std::move(chain_storage));

    const bytes miner_key = make_seed(160);
    const Address miner_addr = address_from_seed(miner_key);
    const Address foundation_addr = st.get_foundation_address();
    check(!miner_addr.str().empty(), "refcount: miner address generated");

    Hash256 prev_hash = hash_from_string("refcount-chain-start");
    const Height maturity = consensus::params::COINBASE_MATURITY;
    std::vector<core::Block> chain_blocks;
    chain_blocks.reserve(static_cast<size_t>(maturity + 2));

    Hash256 first_coinbase_hash{};
    Amount first_coinbase_value = 0;

    for (Height h = 1; h <= maturity; ++h) {
        const Amount reward = consensus::ProofOfWork::calculate_block_reward(h);
        core::Transaction coinbase = core::Transaction::create_coinbase(h, miner_addr, reward, 0, foundation_addr);
        if (h == 1) {
            first_coinbase_hash = coinbase.get_hash();
            first_coinbase_value = coinbase.outputs.empty() ? 0 : coinbase.outputs[0].value;
        }
        core::Block b = make_block_for_test(prev_hash, h, {coinbase});
        check(st.apply_block(b, true), "refcount: apply maturity block");
        prev_hash = b.get_hash();
        chain_blocks.push_back(std::move(b));
    }

    check(height_metadata_equals(st.get_storage(), "chainstate_height", maturity),
          "refcount: canonical chainstate height metadata persisted by apply_block");
    check(!st.get_storage()->has_metadata("height"),
          "refcount: apply_block writes only the canonical height record");
    check(first_coinbase_value > 2000, "refcount: first coinbase captured");

    core::Transaction coinbase_h1_spend;
    coinbase_h1_spend.version = 2;
    coinbase_h1_spend.type = core::TransactionType::STANDARD;
    coinbase_h1_spend.timestamp = 2000;
    set_single_input(coinbase_h1_spend, first_coinbase_hash, 0);
    set_single_output(coinbase_h1_spend, first_coinbase_value - 1000, miner_addr);
    coinbase_h1_spend.sign_input(0, miner_key);
    const Hash256 pubkey_hash = coinbase_h1_spend.witnesses[0].public_key_hash;

    {
        const Height h = maturity + 1;
        const Amount reward = consensus::ProofOfWork::calculate_block_reward(h);
        core::Transaction coinbase =
            core::Transaction::create_coinbase(h, miner_addr, reward, 1000, foundation_addr);
        core::Block b = make_block_for_test(prev_hash, h, {coinbase, coinbase_h1_spend});
        check(st.apply_block(b, true), "refcount: apply block with first spend");
        prev_hash = b.get_hash();
        chain_blocks.push_back(std::move(b));
    }

    core::Transaction second_spend;
    second_spend.version = 2;
    second_spend.type = core::TransactionType::STANDARD;
    second_spend.timestamp = 2001;
    set_single_input(second_spend, coinbase_h1_spend.get_hash(), 0);
    set_single_output(second_spend, first_coinbase_value - 2000, miner_addr);
    second_spend.sign_input(0, miner_key);

    core::Block block_with_second_spend;
    {
        const Height h = maturity + 2;
        const Amount reward = consensus::ProofOfWork::calculate_block_reward(h);
        core::Transaction coinbase =
            core::Transaction::create_coinbase(h, miner_addr, reward, 1000, foundation_addr);
        block_with_second_spend = make_block_for_test(prev_hash, h, {coinbase, second_spend});
        check(st.apply_block(block_with_second_spend, true), "refcount: apply block with second spend");
    }

    bytes cached_pubkey;
    check(st.get_witness_pubkey(pubkey_hash, cached_pubkey), "refcount: key present after two spends");

    check(st.revert_block(block_with_second_spend), "refcount: revert block with second spend");
    cached_pubkey.clear();
    check(st.get_witness_pubkey(pubkey_hash, cached_pubkey), "refcount: key still present after first revert");

    check(st.revert_block(chain_blocks.back()), "refcount: revert block with first spend");
    cached_pubkey.clear();
    check(!st.get_witness_pubkey(pubkey_hash, cached_pubkey), "refcount: key removed after last reference");
}

} // namespace

int main() {
    test_unknown_transaction_type_rejected();
    test_transaction_type_coverage();
    test_consensus_validation_guard_matrix();
    test_consensus_block_and_header_guards();
    test_card_consensus_validation_guards();
    test_sidechain_consensus_validation_guards();
    test_coinbase_interest_pool_activation();
    test_consensus_contextual_and_commitment_guards();
    test_sidechain_state_flow_with_batch();
    test_sidechain_replay_and_spend_guards();
    test_sidechain_batch_v2_proof_validation();
    test_witness_prune_restore_integrity();
    test_witness_pubkey_refcount_on_reorg();

    if (failures == 0) {
        std::cout << "All transaction tests passed\n";
        return 0;
    }

    std::cerr << failures << " transaction tests failed\n";
    return 1;
}
