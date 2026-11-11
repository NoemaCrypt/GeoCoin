#include "test_utils.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/sidechain_fee.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/wallet.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace geocoin::state {

struct ChainStateTestAccess {
    // Component tests exercise mutation guards without a signing fixture.
    // Production apply_transaction() always validates the transaction context.
    static bool apply_sidechain_effects(ChainState& state, const core::Transaction& tx, Height height) {
        return state.apply_sidechain_transaction_(tx, height);
    }
    static bool validate_sidechain_state(const ChainState& state, const core::Transaction& tx) {
        return state.validate_sidechain_transaction_(tx);
    }
    static bool apply_interest_effects(ChainState& state, const core::Transaction& tx, Height height) {
        return state.apply_interest_distribution_(tx, height);
    }
    static bool apply_nft_effects(ChainState& state, const core::Transaction& tx, Height height) {
        return state.apply_nft_transaction_(tx, height);
    }
    static bool apply_angle_effects(ChainState& state, const core::Transaction& tx, Height height) {
        return state.apply_angle_commitment_(tx, height);
    }
    static bool apply_delegation_effects(ChainState& state, const core::Transaction& tx, Height height) {
        return state.apply_delegation_proof_(tx, height);
    }
    static bool apply_revoke_effects(ChainState& state, const core::Transaction& tx, Height height) {
        return state.apply_revoke_delegation_(tx, height);
    }
    static bool apply_multisig_effects(ChainState& state, const core::Transaction& tx, Height height) {
        Amount input_total = 0;
        return state.apply_standard_utxo_effects_(tx, height, false, nullptr, &input_total) &&
               state.apply_multisig_spend_effects_(tx, input_total);
    }

    static std::unique_lock<std::recursive_mutex> lock_state(ChainState& state)
    {
        return std::unique_lock<std::recursive_mutex>(state.mutex_);
    }

    static bool derived_state_valid(const ChainState& state)
    {
        std::lock_guard<std::recursive_mutex> lock(state.mutex_);
        return state.derived_state_storage_valid_;
    }

    static bool revert_transaction(ChainState& state, const core::Transaction& tx, const TxUndo& tx_undo)
    {
        return state.revert_transaction(tx, tx_undo);
    }

    static void register_witness_pubkeys_from_tx(ChainState& state, const core::Transaction& tx)
    {
        state.register_witness_pubkeys_from_tx(tx);
    }

    static void set_foundation_config(ChainState& state, FoundationConfig config)
    {
        state.foundation_config_ = std::move(config);
        state.persist_foundation_config_to_storage();
    }

    static void set_interest_pool_balance(ChainState& state, Amount amount)
    {
        state.interest_pool_balance_ = amount;
        state.persist_interest_pool_balance_to_storage();
    }

    static bool build_interest_distribution_outputs(
        const ChainState& state,
        Height distribution_height,
        std::vector<core::TxOutput>& outputs,
        Amount& distributed_total
    )
    {
        return state.build_interest_distribution_outputs_(distribution_height, outputs, distributed_total);
    }

    static void add_utxo(ChainState& state, const UTXO& utxo)
    {
        state.add_utxo(utxo);
    }

    static bool remove_utxo(ChainState& state, const Hash256& tx_hash, uint32_t vout)
    {
        return state.remove_utxo(tx_hash, vout);
    }

    static void inject_utxo_unchecked(ChainState& state, const UTXO& utxo)
    {
        state.adopt_utxo_memory_(utxo);
    }

    static std::map<std::string, bytes> utxo_snapshot(const ChainState& state)
    {
        std::lock_guard<std::recursive_mutex> lock(state.mutex_);
        std::map<std::string, bytes> snapshot;
        for (const auto& [key, utxo] : state.utxo_set_) {
            (void)key;
            snapshot.emplace(utxo.get_key(), utxo.serialize());
        }
        return snapshot;
    }

    static void cache_witness_payload(ChainState& state, const Hash256& block_hash, const bytes& payload)
    {
        state.cache_witness_payload(block_hash, payload);
    }

    static void rebuild_witness_pubkey_cache_from_active_chain(ChainState& state)
    {
        state.rebuild_witness_pubkey_cache_from_active_chain();
    }

    static bool has_card_program(const ChainState& state, const Hash256& program_id)
    {
        return state.card_programs_.count(ChainState::hash256_to_hex(program_id)) > 0;
    }

    static bool has_card_instance(const ChainState& state, const Hash256& uid_hash)
    {
        return state.card_instances_.count(ChainState::hash256_to_hex(uid_hash)) > 0;
    }

    static bool card_pubkey_maps_to(
        const ChainState& state,
        const Hash256& pubkey_hash,
        const Hash256& uid_hash
    ) {
        auto it = state.card_pubkey_to_uid_.find(ChainState::hash256_to_hex(pubkey_hash));
        return it != state.card_pubkey_to_uid_.end() &&
               it->second == ChainState::hash256_to_hex(uid_hash);
    }
};

} // namespace geocoin::state

namespace {

using StateAccess = geocoin::state::ChainStateTestAccess;

class FailingDeleteStorage final : public geocoin::state::MemoryStorage {
public:
    bool fail_deletes{false};

    bool delete_metadata(const std::string& key) override
    {
        if (fail_deletes) {
            return false;
        }
        return MemoryStorage::delete_metadata(key);
    }
};

class FailingCommitStorage final : public geocoin::state::MemoryStorage {
public:
    bool fail_next_commit{false};

    bool commit_batch() override
    {
        if (fail_next_commit) {
            fail_next_commit = false;
            MemoryStorage::abort_batch();
            return false;
        }
        return MemoryStorage::commit_batch();
    }
};

class SelectiveDeleteFailureStorage final : public geocoin::state::MemoryStorage {
public:
    std::string failed_key;

    bool delete_metadata(const std::string& key) override
    {
        if (key == failed_key) {
            return false;
        }
        return MemoryStorage::delete_metadata(key);
    }
};

class SelectiveNftWriteFailureStorage final : public geocoin::state::MemoryStorage {
public:
    std::string failed_put_key;
    std::string secondary_failed_put_key;
    std::string failed_put_fragment;
    std::string failed_delete_key;
    bool fail_transaction_put{false};
    bool fail_transaction_delete{false};
    bool throw_on_put_failure{false};
    bool throw_on_secondary_put_failure{false};
    bool throw_on_delete_failure{false};
    bool throw_on_transaction_failure{false};
    bool throw_on_transaction_delete_failure{false};
    size_t remaining_put_failures{1};
    size_t remaining_secondary_put_failures{1};
    size_t remaining_delete_failures{1};
    size_t remaining_transaction_failures{1};
    size_t remaining_transaction_delete_failures{1};

    bool put_metadata(const std::string& key, const geocoin::bytes& value) override
    {
        if (!secondary_failed_put_key.empty() && key == secondary_failed_put_key &&
            remaining_secondary_put_failures > 0) {
            --remaining_secondary_put_failures;
            if (throw_on_secondary_put_failure) {
                throw std::runtime_error("injected metadata restoration exception");
            }
            return false;
        }
        if ((!failed_put_key.empty() && key == failed_put_key) ||
            (!failed_put_fragment.empty() &&
             key.find(failed_put_fragment) != std::string::npos)) {
            if (remaining_put_failures == 0) {
                return MemoryStorage::put_metadata(key, value);
            }
            --remaining_put_failures;
            if (throw_on_put_failure) {
                throw std::runtime_error("injected metadata put exception");
            }
            return false;
        }
        return MemoryStorage::put_metadata(key, value);
    }

    bool delete_metadata(const std::string& key) override
    {
        if (!failed_delete_key.empty() && key == failed_delete_key &&
            remaining_delete_failures > 0) {
            --remaining_delete_failures;
            if (throw_on_delete_failure) {
                throw std::runtime_error("injected metadata delete exception");
            }
            return false;
        }
        return MemoryStorage::delete_metadata(key);
    }

    bool put_transaction(
        const geocoin::Hash256& hash,
        const geocoin::core::Transaction& tx
    ) override {
        if (fail_transaction_put && remaining_transaction_failures > 0) {
            --remaining_transaction_failures;
            if (throw_on_transaction_failure) {
                throw std::runtime_error("injected transaction put exception");
            }
            return false;
        }
        return MemoryStorage::put_transaction(hash, tx);
    }

    bool delete_transaction(const geocoin::Hash256& hash) override
    {
        if (fail_transaction_delete && remaining_transaction_delete_failures > 0) {
            --remaining_transaction_delete_failures;
            if (throw_on_transaction_delete_failure) {
                throw std::runtime_error("injected transaction delete exception");
            }
            return false;
        }
        return MemoryStorage::delete_transaction(hash);
    }
};

class DeferredDurabilityStorage final : public geocoin::state::MemoryStorage {
public:
    bool durable{false};

    bool last_batch_commit_was_durable() const override
    {
        return durable;
    }
};

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 29);
    }
    return hash;
}

std::string hash_to_hex(const geocoin::Hash256& hash)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const auto byte : hash) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

geocoin::bytes encode_u64_le(uint64_t value)
{
    geocoin::bytes out(8, 0);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<geocoin::byte>((value >> (i * 8)) & 0xFFu);
    }
    return out;
}

uint64_t amount_to_u64(geocoin::Amount value)
{
    expect(value >= 0, "test amount must be non-negative");
    return static_cast<uint64_t>(value);
}

void append_u32_le(geocoin::bytes& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<geocoin::byte>((value >> (i * 8)) & 0xFFu));
    }
}

void append_u64_le(geocoin::bytes& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<geocoin::byte>((value >> (i * 8)) & 0xFFu));
    }
}

void append_hash(geocoin::bytes& out, const geocoin::Hash256& hash)
{
    out.insert(out.end(), hash.begin(), hash.end());
}

void append_string(geocoin::bytes& out, const std::string& value)
{
    append_u32_le(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

geocoin::bytes bytes_from_string(const std::string& value)
{
    return geocoin::bytes(value.begin(), value.end());
}

geocoin::bytes serialize_test_deposit(
    const geocoin::Address& owner,
    geocoin::Amount amount,
    geocoin::Height unlock_height,
    const geocoin::Hash256& checkpoint,
    geocoin::Height last_checkpoint_height,
    geocoin::Height last_sidechain_checkpoint_height
) {
    geocoin::bytes out;
    append_string(out, owner.str());
    append_u64_le(out, amount_to_u64(amount));
    append_u64_le(out, static_cast<uint64_t>(unlock_height));
    append_hash(out, checkpoint);
    append_u64_le(out, static_cast<uint64_t>(last_checkpoint_height));
    append_u64_le(out, static_cast<uint64_t>(last_sidechain_checkpoint_height));
    return out;
}

geocoin::Address make_multisig_address(uint8_t m, uint8_t n, const std::vector<geocoin::Address>& participants)
{
    return geocoin::Address(geocoin::wallet::create_multisig_address(m, n, participants).address);
}

geocoin::bytes serialize_test_registry(
    const geocoin::Address& owner,
    geocoin::Amount issuance_limit,
    geocoin::Amount total_issued,
    uint32_t multiplier,
    uint32_t fee_bps,
    uint32_t max_multiplier,
    const std::string& symbol,
    uint8_t m,
    uint8_t n,
    const std::vector<geocoin::Address>& members,
    uint8_t policy_id = 2,
    uint8_t version = 2
) {
    geocoin::bytes out;
    append_string(out, owner.str());
    append_u64_le(out, amount_to_u64(issuance_limit));
    append_u64_le(out, amount_to_u64(total_issued));
    append_u32_le(out, multiplier);
    append_u32_le(out, fee_bps);
    append_u32_le(out, max_multiplier);
    out.push_back(policy_id);
    out.push_back(version);
    append_string(out, symbol);
    out.push_back(m);
    out.push_back(n);
    append_u32_le(out, static_cast<uint32_t>(members.size()));
    for (const auto& member : members) {
        append_string(out, member.str());
    }
    append_hash(out, geocoin::consensus::compute_sidechain_committee_id(m, n, members));
    out.insert(out.end(), {0xA3, 1, 0, 0});
    append_u64_le(out, 0);
    append_string(out, "");
    append_hash(out, {});
    return out;
}

geocoin::bytes serialize_test_card_program(
    const geocoin::Address& issuer,
    const geocoin::Hash256& package_hash,
    uint32_t policy_id,
    const geocoin::Hash256& scope_id,
    geocoin::Height created_height
) {
    geocoin::bytes out;
    append_string(out, issuer.str());
    append_string(out, "vendor");
    append_string(out, "os");
    append_string(out, "aid");
    append_u32_le(out, 7);
    append_hash(out, package_hash);
    append_u32_le(out, policy_id);
    append_hash(out, scope_id);
    append_u64_le(out, static_cast<uint64_t>(created_height));
    out.push_back(0); // Explicit current corporate policy version.
    return out;
}

geocoin::bytes serialize_test_card_instance(
    const geocoin::Hash256& program_id,
    const geocoin::Address& issuer,
    const geocoin::Hash256& uid_hash,
    const geocoin::Hash256& pubkey_hash,
    uint8_t status
) {
    geocoin::bytes out;
    append_hash(out, program_id);
    append_string(out, issuer.str());
    append_hash(out, uid_hash);
    append_hash(out, pubkey_hash);
    append_u64_le(out, 11);
    append_u64_le(out, 99);
    out.push_back(status);
    append_hash(out, hash_from_seed(0x7a));
    append_u64_le(out, 12);
    append_u64_le(out, 13);
    return out;
}

geocoin::bytes serialize_test_foundation_config(
    const geocoin::Address& address,
    uint8_t m,
    uint8_t n,
    const std::vector<geocoin::Address>& participants
) {
    geocoin::bytes out;
    append_string(out, address.str());
    out.push_back(m);
    out.push_back(n);
    append_u32_le(out, static_cast<uint32_t>(participants.size()));
    for (const auto& participant : participants) {
        append_string(out, participant.str());
    }
    return out;
}

struct CheckpointProofFixture {
    geocoin::bytes proof;
    geocoin::Hash256 state_root{};
    geocoin::Hash256 checkpoint{};
};

geocoin::Hash256 hash_pair(const geocoin::Hash256& left, const geocoin::Hash256& right)
{
    geocoin::bytes data;
    append_hash(data, left);
    append_hash(data, right);
    return geocoin::crypto::double_sha256(data);
}

geocoin::Hash256 build_test_checkpoint_supply_leaf_v2(
    const geocoin::Hash256& sidechain_id,
    geocoin::Height checkpoint_height,
    uint64_t total_supply
)
{
    static constexpr char kLeafDomain[] = "geocoin:sidechain:v2:supply_leaf";
    geocoin::bytes data;
    data.insert(data.end(), kLeafDomain, kLeafDomain + sizeof(kLeafDomain) - 1);
    append_hash(data, sidechain_id);
    append_u64_le(data, static_cast<uint64_t>(checkpoint_height));
    append_u64_le(data, total_supply);
    return geocoin::crypto::double_sha256(data);
}

geocoin::Hash256 build_test_checkpoint_commitment_v2(
    const geocoin::Hash256& sidechain_id,
    const geocoin::Hash256& state_root,
    geocoin::Height checkpoint_height,
    uint64_t total_supply
)
{
    static constexpr char kCheckpointDomain[] = "geocoin:sidechain:v2:checkpoint_commitment";
    geocoin::bytes data;
    data.insert(data.end(), kCheckpointDomain, kCheckpointDomain + sizeof(kCheckpointDomain) - 1);
    append_hash(data, sidechain_id);
    append_hash(data, state_root);
    append_u64_le(data, static_cast<uint64_t>(checkpoint_height));
    append_u64_le(data, total_supply);
    return geocoin::crypto::double_sha256(data);
}

CheckpointProofFixture make_checkpoint_proof_v2(
    const geocoin::Hash256& sidechain_id,
    geocoin::Height checkpoint_height,
    uint64_t total_supply,
    uint32_t leaf_index = 0
)
{
    const geocoin::Hash256 leaf =
        build_test_checkpoint_supply_leaf_v2(sidechain_id, checkpoint_height, total_supply);
    const geocoin::Hash256 sibling = hash_from_seed(0xbb);

    CheckpointProofFixture fixture;
    fixture.state_root = (leaf_index == 0) ? hash_pair(leaf, sibling) : hash_pair(sibling, leaf);
    fixture.checkpoint = build_test_checkpoint_commitment_v2(
        sidechain_id,
        fixture.state_root,
        checkpoint_height,
        total_supply
    );

    fixture.proof = {'S', 'C', 'P', '2', 1, 1, 0, 0};
    append_hash(fixture.proof, fixture.state_root);
    append_u64_le(fixture.proof, static_cast<uint64_t>(checkpoint_height));
    append_u64_le(fixture.proof, total_supply);
    append_u32_le(fixture.proof, leaf_index);
    append_hash(fixture.proof, sibling);
    return fixture;
}

CheckpointProofFixture make_checkpoint_proof_v2_single_leaf(
    const geocoin::Hash256& sidechain_id,
    geocoin::Height checkpoint_height,
    uint64_t total_supply
)
{
    const geocoin::Hash256 leaf =
        build_test_checkpoint_supply_leaf_v2(sidechain_id, checkpoint_height, total_supply);

    CheckpointProofFixture fixture;
    fixture.state_root = leaf;
    fixture.checkpoint = build_test_checkpoint_commitment_v2(
        sidechain_id,
        fixture.state_root,
        checkpoint_height,
        total_supply
    );

    fixture.proof = {'S', 'C', 'P', '2', 1, 0, 0, 0};
    append_hash(fixture.proof, fixture.state_root);
    append_u64_le(fixture.proof, static_cast<uint64_t>(checkpoint_height));
    append_u64_le(fixture.proof, total_supply);
    append_u32_le(fixture.proof, 0);
    return fixture;
}

CheckpointProofFixture make_accounting_checkpoint_proof_v1(
    const geocoin::Hash256& sidechain_id,
    const geocoin::Hash256& state_root,
    geocoin::Height checkpoint_height,
    const geocoin::Hash256& schema_hash
)
{
    static constexpr char kCheckpointDomain[] =
        "geocoin:sidechain:accounting:v1:checkpoint_commitment";
    geocoin::bytes commitment_data;
    commitment_data.insert(
        commitment_data.end(),
        kCheckpointDomain,
        kCheckpointDomain + sizeof(kCheckpointDomain) - 1
    );
    append_hash(commitment_data, sidechain_id);
    append_hash(commitment_data, state_root);
    append_u64_le(commitment_data, static_cast<uint64_t>(checkpoint_height));
    append_hash(commitment_data, schema_hash);

    CheckpointProofFixture fixture;
    fixture.state_root = state_root;
    fixture.checkpoint = geocoin::crypto::double_sha256(commitment_data);
    fixture.proof = {'S', 'C', 'A', '1', 1, 0, 0, 0};
    append_hash(fixture.proof, state_root);
    append_u64_le(fixture.proof, static_cast<uint64_t>(checkpoint_height));
    append_hash(fixture.proof, schema_hash);
    return fixture;
}

geocoin::Signature sign_bytes_payload(const geocoin::bytes& payload, const geocoin::bytes& private_key)
{
    return geocoin::crypto::sign_recoverable_message(geocoin::crypto::sha256(payload), private_key);
}

geocoin::Signature sign_sidechain_payload(
    const geocoin::core::Transaction& tx,
    const geocoin::bytes& private_key
)
{
    return sign_bytes_payload(geocoin::core::build_sidechain_payload_for_signing(tx), private_key);
}

geocoin::Hash256 owner_committee_id(const geocoin::Address& owner)
{
    return geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner});
}

void authorize_decommission(
    geocoin::core::Transaction& tx,
    const geocoin::bytes& owner_key,
    const geocoin::bytes& committee_key,
    const geocoin::Hash256& committee_id
)
{
    tx.sidechain_committee_id = committee_id;
    tx.sidechain_signature = sign_sidechain_payload(tx, owner_key);
    tx.sidechain_committee_signatures = {sign_sidechain_payload(tx, committee_key)};
}

geocoin::bytes make_witness_pubkey(uint8_t seed)
{
    geocoin::bytes pubkey(geocoin::consensus::params::WITNESS_PUBKEY_SIZE);
    for (size_t i = 0; i < pubkey.size(); ++i) {
        pubkey[i] = static_cast<geocoin::byte>(seed + i);
    }
    return pubkey;
}

geocoin::bytes private_key_from_seed(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<geocoin::byte>(seed + i);
    }
    return key;
}

geocoin::Address address_from_seed(uint8_t seed)
{
    geocoin::bytes key = private_key_from_seed(seed);
    geocoin::Address address;
    expect(geocoin::crypto::compute_address_from_private_key(key, address),
           "test address derives from private key");
    return address;
}

std::filesystem::path make_temp_dir(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path()
        / (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

class TempDirectoryGuard {
public:
    explicit TempDirectoryGuard(std::filesystem::path path)
        : path_(std::move(path))
    {
    }

    ~TempDirectoryGuard()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

geocoin::bytes serialize_test_sidechain_deposit(const geocoin::state::SidechainDeposit& deposit)
{
    geocoin::bytes out;
    append_string(out, deposit.owner.str());
    append_u64_le(out, static_cast<uint64_t>(deposit.amount));
    append_u64_le(out, static_cast<uint64_t>(deposit.unlock_height));
    append_hash(out, deposit.checkpoint);
    append_u64_le(out, static_cast<uint64_t>(deposit.last_checkpoint_height));
    append_u64_le(out, static_cast<uint64_t>(deposit.last_sidechain_checkpoint_height));
    return out;
}

geocoin::bytes serialize_test_sidechain_registry(const geocoin::state::SidechainRegistryEntry& entry)
{
    geocoin::bytes out;
    append_string(out, entry.owner.str());
    append_u64_le(out, static_cast<uint64_t>(entry.issuance_limit));
    append_u64_le(out, static_cast<uint64_t>(entry.total_issued));
    append_u32_le(out, entry.multiplier);
    append_u32_le(out, entry.fee_bps);
    append_u32_le(out, entry.max_multiplier);
    out.push_back(entry.checkpoint_policy_id);
    out.push_back(entry.checkpoint_version);
    append_string(out, entry.token_symbol);
    out.push_back(entry.committee_m);
    out.push_back(entry.committee_n);
    append_u32_le(out, static_cast<uint32_t>(entry.committee_members.size()));
    for (const auto& member : entry.committee_members) {
        append_string(out, member.str());
    }
    append_hash(out, entry.committee_id);
    out.insert(out.end(), {0xA3, 1, static_cast<uint8_t>(entry.kind), static_cast<uint8_t>(entry.status)});
    append_u64_le(out, entry.decommission_height);
    append_string(out, entry.accounting_namespace);
    append_hash(out, entry.accounting_schema_hash);
    return out;
}

geocoin::core::Block make_block(
    geocoin::Height height,
    const geocoin::Hash256& prev_hash,
    std::vector<geocoin::core::Transaction> transactions
)
{
    geocoin::core::Block block;
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = static_cast<uint32_t>(1000 + height);
    block.header.bits = 0x1d0fffff;
    block.header.nonce = height + transactions.size();
    block.transactions = std::move(transactions);
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    return block;
}

geocoin::nft::NFTMetadata make_nft_metadata()
{
    geocoin::nft::NFTMetadata metadata;
    metadata.name = "Restored NFT";
    metadata.description = "Revert coverage";
    metadata.image_uri = "ipfs://restored";
    metadata.content_hash = std::string(64, 'b');
    metadata.geometry.polygons.front().exterior = {{0LL, 0LL}, {0LL, 10000000LL}, {10000000LL, 10000000LL}, {10000000LL, 0LL}};
    metadata.created_at = 1700000000;
    metadata.block_height = 7;
    metadata.block_hash = hash_from_seed(0x44);
    metadata.location_name = "Restore place";
    metadata.tags = {"restore"};
    metadata.creator = "creator";
    return metadata;
}

geocoin::state::TxUndo::DepositUndo make_deposit_undo(
    uint8_t kind,
    const geocoin::Hash256& sidechain_id,
    const geocoin::Address& owner,
    geocoin::Amount amount,
    geocoin::Height unlock_height
)
{
    geocoin::state::TxUndo::DepositUndo undo;
    undo.kind = kind;
    undo.sidechain_id = sidechain_id;
    undo.prev_owner = owner.str();
    undo.prev_amount = amount;
    undo.prev_unlock_height = unlock_height;
    undo.prev_checkpoint = hash_from_seed(0x45);
    undo.prev_last_checkpoint_height = 11;
    undo.prev_last_sidechain_checkpoint_height = 22;
    return undo;
}

geocoin::state::BlockUndo make_single_tx_block_undo(
    const geocoin::core::Block& block,
    geocoin::state::TxUndo tx_undo,
    geocoin::Height height
)
{
    geocoin::state::BlockUndo undo;
    undo.height = height;
    undo.block_hash = block.get_hash();
    undo.tx_undos.push_back(std::move(tx_undo));
    return undo;
}

void run_custom_revert(
    geocoin::state::ChainState& state,
    const geocoin::core::Block& block,
    const geocoin::state::BlockUndo& undo,
    geocoin::Height height
)
{
    state.set_height(height);
    state.set_tip_hash(block.get_hash());
    expect(state.save_block_undo(undo), "custom undo saves");
    expect(state.revert_block(block), "custom block reverts");
}

void restore_sidechain_registry_with_revert(
    geocoin::state::ChainState& state,
    const geocoin::Hash256& sidechain_id,
    const geocoin::Address& owner,
    geocoin::Amount issuance_limit,
    geocoin::Amount total_issued,
    uint8_t checkpoint_policy_id = 2,
    uint8_t checkpoint_version = 2
)
{
    geocoin::core::Transaction tx;
    tx.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
    tx.sidechain_id = sidechain_id;

    geocoin::state::TxUndo tx_undo;
    tx_undo.sidechain_undo.kind = 2;
    tx_undo.sidechain_undo.sidechain_id = sidechain_id;
    tx_undo.sidechain_undo.prev_owner = owner.str();
    tx_undo.sidechain_undo.prev_limit = issuance_limit;
    tx_undo.sidechain_undo.prev_issued = total_issued;
    tx_undo.sidechain_undo.prev_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    tx_undo.sidechain_undo.prev_fee_bps = 25;
    tx_undo.sidechain_undo.prev_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
    tx_undo.sidechain_undo.prev_symbol = "TST";
    tx_undo.sidechain_undo.prev_committee_m = 1;
    tx_undo.sidechain_undo.prev_committee_n = 1;
    tx_undo.sidechain_undo.prev_committee_members = {owner.str()};
    tx_undo.sidechain_undo.prev_committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, std::vector<geocoin::Address>{owner});
    tx_undo.sidechain_undo.prev_checkpoint_policy_id = checkpoint_policy_id;
    tx_undo.sidechain_undo.prev_checkpoint_version = checkpoint_version;

    const auto block = make_block(70, hash_from_seed(0x66), {tx});
    run_custom_revert(state, block, make_single_tx_block_undo(block, tx_undo, 70), 70);
}

void restore_deposit_with_revert(
    geocoin::state::ChainState& state,
    const geocoin::Hash256& sidechain_id,
    const geocoin::Address& owner,
    geocoin::Amount amount,
    geocoin::Height unlock_height
)
{
    geocoin::core::Transaction tx;
    tx.type = geocoin::core::TransactionType::SPEND_DEPOSIT;
    tx.sidechain_id = sidechain_id;

    geocoin::state::TxUndo tx_undo;
    tx_undo.deposit_undo = make_deposit_undo(2, sidechain_id, owner, amount, unlock_height);

    const auto block = make_block(50, hash_from_seed(0x51), {tx});
    run_custom_revert(state, block, make_single_tx_block_undo(block, tx_undo, 50), 50);
}

void test_custom_revert_restores_consensus_state()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    geocoin::state::Storage* storage = state.get_storage();
    expect(storage != nullptr, "chainstate storage exists");

    const geocoin::Hash256 sidechain_id = hash_from_seed(0x10);
    const geocoin::Hash256 batch_sidechain_id = hash_from_seed(0x11);
    const geocoin::nft::NFTMetadata restored_nft_metadata = make_nft_metadata();
    const geocoin::Hash256 nft_id = restored_nft_metadata.calculate_hash();
    const geocoin::Hash256 card_program_delete = hash_from_seed(0x13);
    const geocoin::Hash256 card_uid_delete = hash_from_seed(0x14);
    const geocoin::Hash256 card_uid_restore = hash_from_seed(0x15);
    const geocoin::Hash256 card_pubkey_restore = hash_from_seed(0x16);
    const geocoin::Address owner("GEOowner");

    expect(storage->put_metadata("card:prog:" + hash_to_hex(card_program_delete), {9}),
           "seed card program metadata");
    expect(storage->put_metadata("card:uid:" + hash_to_hex(card_uid_delete), {8}),
           "seed card instance metadata");

    geocoin::core::Transaction sidechain_tx;
    sidechain_tx.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
    sidechain_tx.sidechain_id = sidechain_id;

    geocoin::state::TxUndo sidechain_undo;
    sidechain_undo.deposit_undo = make_deposit_undo(2, sidechain_id, owner, 100 * geocoin::COIN, 5);
    sidechain_undo.sidechain_undo.kind = 2;
    sidechain_undo.sidechain_undo.sidechain_id = sidechain_id;
    sidechain_undo.sidechain_undo.prev_owner = owner.str();
    sidechain_undo.sidechain_undo.prev_limit = 1000 * geocoin::COIN;
    sidechain_undo.sidechain_undo.prev_issued = 25 * geocoin::COIN;
    sidechain_undo.sidechain_undo.prev_multiplier = 3;
    sidechain_undo.sidechain_undo.prev_fee_bps = 12;
    sidechain_undo.sidechain_undo.prev_max_multiplier = 9;
    sidechain_undo.sidechain_undo.prev_symbol = "TST";
    sidechain_undo.sidechain_undo.prev_committee_m = 1;
    sidechain_undo.sidechain_undo.prev_committee_n = 2;
    sidechain_undo.sidechain_undo.prev_committee_members = {"GEOmemberA", "GEOmemberB"};
    sidechain_undo.sidechain_undo.prev_committee_id = hash_from_seed(0x17);
    sidechain_undo.sidechain_undo.prev_checkpoint_policy_id = 7;
    sidechain_undo.sidechain_undo.prev_checkpoint_version = 9;

    geocoin::core::Transaction batch_tx;
    batch_tx.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    geocoin::state::TxUndo batch_undo;
    batch_undo.deposit_undo_batch.push_back(
        make_deposit_undo(2, batch_sidechain_id, geocoin::Address("GEObatch"), 77 * geocoin::COIN, 6)
    );

    geocoin::core::Transaction foundation_tx;
    foundation_tx.type = geocoin::core::TransactionType::UPDATE_FOUNDATION_ADDRESS;
    const std::vector<geocoin::Address> previous_foundation_members{
        address_from_seed(181), address_from_seed(182), address_from_seed(183)};
    const geocoin::Address previous_foundation_address(
        geocoin::multisig::create_multisig_address(2, 3, previous_foundation_members).address);
    foundation_tx.foundation_old_address = previous_foundation_address;
    foundation_tx.foundation_m = 2;
    foundation_tx.foundation_n = 3;
    foundation_tx.foundation_participants = previous_foundation_members;
    const auto current_foundation = state.get_foundation_config();
    foundation_tx.foundation_new_address = current_foundation.address;
    foundation_tx.foundation_new_m = current_foundation.m;
    foundation_tx.foundation_new_n = current_foundation.n;
    foundation_tx.foundation_new_participants = current_foundation.participants;
    geocoin::state::TxUndo foundation_undo;
    foundation_undo.foundation_undo.kind = 1;
    foundation_undo.foundation_undo.prev_address = previous_foundation_address.str();
    foundation_undo.foundation_undo.prev_m = 2;
    foundation_undo.foundation_undo.prev_n = 3;
    for (const auto& member : previous_foundation_members) {
        foundation_undo.foundation_undo.prev_participants.push_back(member.str());
    }

    geocoin::core::Transaction card_delete_program_tx;
    card_delete_program_tx.type = geocoin::core::TransactionType::CARD_PROGRAM_DEFINE;
    geocoin::state::TxUndo card_delete_program_undo;
    card_delete_program_undo.card_undo.kind = 1;
    card_delete_program_undo.card_undo.program_id = card_program_delete;

    geocoin::core::Transaction card_delete_instance_tx;
    card_delete_instance_tx.type = geocoin::core::TransactionType::CARD_ISSUE;
    geocoin::state::TxUndo card_delete_instance_undo;
    card_delete_instance_undo.card_undo.kind = 2;
    card_delete_instance_undo.card_undo.card_uid_hash = card_uid_delete;
    card_delete_instance_undo.card_undo.card_pubkey_hash = hash_from_seed(0x18);

    geocoin::core::Transaction card_restore_tx;
    card_restore_tx.type = geocoin::core::TransactionType::CARD_UPDATE_STATUS;
    geocoin::state::TxUndo card_restore_undo;
    card_restore_undo.card_undo.kind = 3;
    card_restore_undo.card_undo.program_id = hash_from_seed(0x19);
    card_restore_undo.card_undo.card_uid_hash = card_uid_restore;
    card_restore_undo.card_undo.card_pubkey_hash = card_pubkey_restore;
    card_restore_undo.card_undo.prev_issuer = "GEOissuer";
    card_restore_undo.card_undo.prev_valid_from = 100;
    card_restore_undo.card_undo.prev_valid_to = 200;
    card_restore_undo.card_undo.prev_status = 1;
    card_restore_undo.card_undo.prev_metadata_hash = hash_from_seed(0x1a);
    card_restore_undo.card_undo.prev_issued_height = 30;
    card_restore_undo.card_undo.prev_updated_height = 40;

    geocoin::core::Transaction nft_burn_tx;
    nft_burn_tx.type = geocoin::core::TransactionType::NFT_BURN;
    geocoin::nft::NFTBurnTransaction burn_payload;
    burn_payload.nft_id = nft_id;
    burn_payload.owner = geocoin::Address("GEOnft-owner");
    nft_burn_tx.nft_burn = burn_payload;
    geocoin::state::TxUndo nft_undo;
    nft_undo.nft_undo.kind = 3;
    nft_undo.nft_undo.nft_id = nft_id;
    nft_undo.nft_undo.prev_owner = "GEOnft-owner";
    nft_undo.nft_undo.prev_metadata = restored_nft_metadata.serialize();

    const auto block = make_block(
        80,
        hash_from_seed(0x60),
        {
            sidechain_tx,
            batch_tx,
            foundation_tx,
            card_delete_program_tx,
            card_delete_instance_tx,
            card_restore_tx,
            nft_burn_tx
        }
    );

    geocoin::state::BlockUndo undo;
    undo.height = 80;
    undo.block_hash = block.get_hash();
    undo.tx_undos = {
        sidechain_undo,
        batch_undo,
        foundation_undo,
        card_delete_program_undo,
        card_delete_instance_undo,
        card_restore_undo,
        nft_undo
    };

    run_custom_revert(state, block, undo, 80);

    geocoin::state::SidechainDeposit deposit;
    expect(state.get_sidechain_deposit(sidechain_id, deposit), "sidechain deposit restored");
    expect(deposit.owner == owner && deposit.amount == 100 * geocoin::COIN,
           "sidechain deposit values restored");
    expect(deposit.last_checkpoint_height == 11 && deposit.last_sidechain_checkpoint_height == 22,
           "sidechain deposit checkpoint heights restored");

    geocoin::state::SidechainRegistryEntry registry_entry;
    expect(state.get_sidechain_registry(sidechain_id, registry_entry), "sidechain registry restored");
    expect(registry_entry.owner == owner && registry_entry.total_issued == 25 * geocoin::COIN,
           "sidechain registry values restored");
    expect(registry_entry.checkpoint_policy_id == 7 && registry_entry.checkpoint_version == 9,
           "sidechain registry checkpoint policy restored");

    geocoin::state::SidechainDeposit batch_deposit;
    expect(state.get_sidechain_deposit(batch_sidechain_id, batch_deposit), "batch deposit restored");
    expect(batch_deposit.amount == 77 * geocoin::COIN, "batch deposit amount restored");

    const auto foundation = state.get_foundation_config();
    expect(foundation.address == previous_foundation_address,
           "foundation address restored");
    expect(foundation.m == 2 && foundation.n == 3 && foundation.participants.size() == 3,
           "foundation multisig parameters restored");

    expect(!storage->has_metadata("card:prog:" + hash_to_hex(card_program_delete)),
           "card program delete undo removes program metadata");
    expect(!storage->has_metadata("card:uid:" + hash_to_hex(card_uid_delete)),
           "card issue undo removes card metadata");
    expect(storage->has_metadata("card:uid:" + hash_to_hex(card_uid_restore)),
           "card update undo restores card metadata");

    expect(state.nft_exists(nft_id), "NFT burn undo restores NFT");
    expect(state.get_nft_owner(nft_id) == geocoin::Address("GEOnft-owner"), "NFT owner restored");
    expect(state.get_nft_metadata(nft_id).name == "Restored NFT", "NFT metadata restored");
    expect(state.get_height() == 79, "revert decrements chain height");
    expect(state.get_tip_hash() == block.header.prev_block_hash, "revert moves tip to previous hash");
}

void test_sidechain_spend_and_unlock_apply_paths()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());

    const geocoin::Hash256 spend_id = hash_from_seed(0x70);
    const geocoin::Address owner("GEOspend-owner");
    restore_deposit_with_revert(state, spend_id, owner, 50 * geocoin::COIN, 3);

    geocoin::core::Transaction spend;
    spend.type = geocoin::core::TransactionType::SPEND_DEPOSIT;
    spend.sidechain_id = spend_id;
    spend.sidechain_owner = owner;
    spend.sidechain_amount = 20 * geocoin::COIN;
    geocoin::core::TxOutput spend_output;
    spend_output.value = spend.sidechain_amount;
    spend_output.address = geocoin::Address("GEOspend-recipient");
    spend.outputs.push_back(spend_output);
    expect(StateAccess::apply_sidechain_effects(state, spend, 10), "SPEND_DEPOSIT applies");

    geocoin::state::UTXO spend_utxo;
    expect(state.get_utxo(spend.get_hash(), 0, spend_utxo), "SPEND_DEPOSIT creates output UTXO");
    expect(spend_utxo.value == spend.sidechain_amount, "SPEND_DEPOSIT UTXO value matches");
    geocoin::state::SidechainDeposit spend_deposit;
    expect(state.get_sidechain_deposit(spend_id, spend_deposit), "SPEND_DEPOSIT keeps deposit");
    expect(spend_deposit.amount == 30 * geocoin::COIN, "SPEND_DEPOSIT reduces deposit");

    const geocoin::Hash256 unlock_id = hash_from_seed(0x71);
    restore_deposit_with_revert(state, unlock_id, geocoin::Address("GEOunlock-owner"), 33 * geocoin::COIN, 4);

    geocoin::core::Transaction unlock;
    unlock.type = geocoin::core::TransactionType::UNLOCK_DEPOSIT;
    unlock.sidechain_id = unlock_id;
    unlock.sidechain_owner = geocoin::Address("GEOunlock-owner");
    geocoin::core::TxOutput unlock_output;
    unlock_output.value = 33 * geocoin::COIN;
    unlock_output.address = geocoin::Address("GEOunlock-recipient");
    unlock.outputs.push_back(unlock_output);
    expect(StateAccess::apply_sidechain_effects(state, unlock, 10), "UNLOCK_DEPOSIT applies");
    expect(state.get_utxo(unlock.get_hash(), 0, spend_utxo), "UNLOCK_DEPOSIT creates output UTXO");
    expect(!state.has_sidechain_deposit_key(unlock_id), "UNLOCK_DEPOSIT removes deposit");
}

void test_sidechain_deposit_apply_guard_paths()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::Address owner("GEOdeposit-owner");
    const geocoin::Address other("GEOdeposit-other");
    const geocoin::Address recipient("GEOdeposit-recipient");

    const auto funding = geocoin::core::Transaction::create_coinbase(1, owner, 1000 * geocoin::COIN);
    expect(state.apply_transaction(funding, 1, true), "deposit guard funding applies");

    geocoin::core::Transaction lock_missing_input;
    lock_missing_input.type = geocoin::core::TransactionType::LOCK_DEPOSIT;
    lock_missing_input.sidechain_id = hash_from_seed(0xe0);
    lock_missing_input.sidechain_owner = owner;
    lock_missing_input.sidechain_amount = 10 * geocoin::COIN;
    lock_missing_input.inputs.push_back({hash_from_seed(0xe1), 0, {}, 0xffffffff, {}, {}});
    expect(!StateAccess::apply_sidechain_effects(state, lock_missing_input, geocoin::consensus::params::COINBASE_MATURITY + 2),
           "LOCK_DEPOSIT rejects missing input UTXO");

    geocoin::core::Transaction immature_lock;
    immature_lock.type = geocoin::core::TransactionType::LOCK_DEPOSIT;
    immature_lock.sidechain_id = hash_from_seed(0xe2);
    immature_lock.sidechain_owner = owner;
    immature_lock.sidechain_amount = 10 * geocoin::COIN;
    immature_lock.inputs.push_back({funding.get_hash(), 0, {}, 0xffffffff, {}, {}});
    expect(!StateAccess::apply_sidechain_effects(state, immature_lock, 1),
           "LOCK_DEPOSIT rejects immature coinbase input");

    const auto future_funding = geocoin::core::Transaction::create_coinbase(
        500,
        owner,
        20 * geocoin::COIN
    );
    expect(state.apply_transaction(future_funding, 500, true),
           "future-height deposit guard funding applies to test fixture");
    geocoin::core::Transaction future_lock = immature_lock;
    future_lock.sidechain_id = hash_from_seed(0xef);
    future_lock.inputs[0].prev_tx_hash = future_funding.get_hash();
    expect(!StateAccess::apply_sidechain_effects(state, future_lock, 1),
           "LOCK_DEPOSIT rejects future-height UTXO without unsigned underflow");

    geocoin::core::Transaction lock;
    lock.type = geocoin::core::TransactionType::LOCK_DEPOSIT;
    lock.sidechain_id = hash_from_seed(0xe3);
    lock.sidechain_owner = owner;
    lock.sidechain_amount = 200 * geocoin::COIN;
    lock.sidechain_unlock_height = geocoin::consensus::params::COINBASE_MATURITY + 10;
    lock.inputs.push_back({funding.get_hash(), 0, {}, 0xffffffff, {}, {}});
    lock.outputs.push_back({700 * geocoin::COIN, recipient, {}});
    expect(StateAccess::apply_sidechain_effects(state, lock, geocoin::consensus::params::COINBASE_MATURITY + 2),
           "LOCK_DEPOSIT applies after coinbase maturity");
    geocoin::state::SidechainDeposit locked_deposit;
    expect(state.get_sidechain_deposit(lock.sidechain_id, locked_deposit),
           "LOCK_DEPOSIT creates sidechain deposit");
    expect(locked_deposit.amount == lock.sidechain_amount && locked_deposit.owner == owner,
           "LOCK_DEPOSIT stores owner and amount");

    geocoin::core::Transaction duplicate_lock = lock;
    duplicate_lock.inputs.clear();
    duplicate_lock.outputs.clear();
    duplicate_lock.inputs.push_back({hash_from_seed(0xe4), 0, {}, 0xffffffff, {}, {}});
    expect(!StateAccess::apply_sidechain_effects(state, duplicate_lock, geocoin::consensus::params::COINBASE_MATURITY + 3),
           "LOCK_DEPOSIT rejects duplicate sidechain id before input lookup");

    const auto second_funding = geocoin::core::Transaction::create_coinbase(2, owner, 50 * geocoin::COIN);
    expect(state.apply_transaction(second_funding, 2, true), "second deposit guard funding applies");
    geocoin::core::Transaction insufficient_lock;
    insufficient_lock.type = geocoin::core::TransactionType::LOCK_DEPOSIT;
    insufficient_lock.sidechain_id = hash_from_seed(0xe5);
    insufficient_lock.sidechain_owner = owner;
    insufficient_lock.sidechain_amount = 40 * geocoin::COIN;
    insufficient_lock.inputs.push_back({second_funding.get_hash(), 0, {}, 0xffffffff, {}, {}});
    insufficient_lock.outputs.push_back({20 * geocoin::COIN, recipient, {}});
    expect(!StateAccess::apply_sidechain_effects(state, insufficient_lock, geocoin::consensus::params::COINBASE_MATURITY + 3),
           "LOCK_DEPOSIT rejects output plus locked amount above input");

    geocoin::core::Transaction spend_missing;
    spend_missing.type = geocoin::core::TransactionType::SPEND_DEPOSIT;
    spend_missing.sidechain_id = hash_from_seed(0xe6);
    spend_missing.sidechain_owner = owner;
    spend_missing.sidechain_amount = 1;
    spend_missing.outputs.push_back({1, recipient, {}});
    expect(!StateAccess::apply_sidechain_effects(state, spend_missing, lock.sidechain_unlock_height + 1),
           "SPEND_DEPOSIT rejects missing deposit");

    geocoin::core::Transaction spend_wrong_owner = spend_missing;
    spend_wrong_owner.sidechain_id = lock.sidechain_id;
    spend_wrong_owner.sidechain_owner = other;
    expect(!StateAccess::apply_sidechain_effects(state, spend_wrong_owner, lock.sidechain_unlock_height + 1),
           "SPEND_DEPOSIT rejects owner mismatch");

    geocoin::core::Transaction spend_premature = spend_missing;
    spend_premature.sidechain_id = lock.sidechain_id;
    spend_premature.sidechain_owner = owner;
    expect(!StateAccess::apply_sidechain_effects(state, spend_premature, lock.sidechain_unlock_height - 1),
           "SPEND_DEPOSIT rejects premature spend before unlock height");

    geocoin::core::Transaction spend_bad_output = spend_missing;
    spend_bad_output.sidechain_id = lock.sidechain_id;
    spend_bad_output.sidechain_owner = owner;
    spend_bad_output.sidechain_amount = 10;
    spend_bad_output.outputs[0].value = 9;
    expect(!StateAccess::apply_sidechain_effects(state, spend_bad_output, lock.sidechain_unlock_height + 1),
           "SPEND_DEPOSIT rejects output total mismatch");

    geocoin::core::Transaction spend_too_much = spend_missing;
    spend_too_much.sidechain_id = lock.sidechain_id;
    spend_too_much.sidechain_owner = owner;
    spend_too_much.sidechain_amount = lock.sidechain_amount + 1;
    spend_too_much.outputs[0].value = spend_too_much.sidechain_amount;
    expect(!StateAccess::apply_sidechain_effects(state, spend_too_much, lock.sidechain_unlock_height + 1),
           "SPEND_DEPOSIT rejects amount above deposit");

    const geocoin::Hash256 registered_spend_id = hash_from_seed(0xe7);
    restore_deposit_with_revert(state, registered_spend_id, owner, 10 * geocoin::COIN, 1);
    restore_sidechain_registry_with_revert(state, registered_spend_id, owner, 10 * geocoin::COIN, 0);
    geocoin::core::Transaction spend_registered = spend_missing;
    spend_registered.sidechain_id = registered_spend_id;
    spend_registered.sidechain_owner = owner;
    spend_registered.sidechain_amount = 1;
    spend_registered.outputs[0].value = 1;
    expect(!StateAccess::apply_sidechain_effects(state, spend_registered, 10),
           "SPEND_DEPOSIT rejects registered sidechain deposits");

    geocoin::core::Transaction unlock_missing;
    unlock_missing.type = geocoin::core::TransactionType::UNLOCK_DEPOSIT;
    unlock_missing.sidechain_id = hash_from_seed(0xe8);
    unlock_missing.sidechain_owner = owner;
    unlock_missing.outputs.push_back({1, recipient, {}});
    expect(!StateAccess::apply_sidechain_effects(state, unlock_missing, 10),
           "UNLOCK_DEPOSIT rejects missing deposit");

    geocoin::core::Transaction unlock_wrong_owner = unlock_missing;
    unlock_wrong_owner.sidechain_id = lock.sidechain_id;
    unlock_wrong_owner.sidechain_owner = other;
    unlock_wrong_owner.outputs[0].value = lock.sidechain_amount;
    expect(!StateAccess::apply_sidechain_effects(state, unlock_wrong_owner, lock.sidechain_unlock_height + 1),
           "UNLOCK_DEPOSIT rejects owner mismatch");

    geocoin::core::Transaction unlock_premature = unlock_wrong_owner;
    unlock_premature.sidechain_owner = owner;
    expect(!StateAccess::apply_sidechain_effects(state, unlock_premature, lock.sidechain_unlock_height - 1),
           "UNLOCK_DEPOSIT rejects premature unlock");

    geocoin::core::Transaction unlock_bad_total = unlock_premature;
    unlock_bad_total.outputs[0].value = lock.sidechain_amount - 1;
    expect(!StateAccess::apply_sidechain_effects(state, unlock_bad_total, lock.sidechain_unlock_height + 1),
           "UNLOCK_DEPOSIT rejects output total mismatch");

    const geocoin::Hash256 registered_unlock_id = hash_from_seed(0xe9);
    restore_deposit_with_revert(state, registered_unlock_id, owner, 10 * geocoin::COIN, 1);
    restore_sidechain_registry_with_revert(state, registered_unlock_id, owner, 10 * geocoin::COIN, 0);
    geocoin::core::Transaction unlock_registered = unlock_missing;
    unlock_registered.sidechain_id = registered_unlock_id;
    unlock_registered.sidechain_owner = owner;
    unlock_registered.outputs[0].value = 10 * geocoin::COIN;
    expect(!StateAccess::apply_sidechain_effects(state, unlock_registered, 10),
           "UNLOCK_DEPOSIT rejects registered sidechain deposits");
}

void test_sidechain_deposit_top_up_lifecycle()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::bytes owner_key = private_key_from_seed(0xd1);
    const geocoin::Address owner = address_from_seed(0xd1);
    const geocoin::Hash256 sidechain_id = hash_from_seed(0xd2);
    const geocoin::Amount initial_deposit = 100 * geocoin::COIN;

    restore_deposit_with_revert(state, sidechain_id, owner, initial_deposit, 0);
    restore_sidechain_registry_with_revert(
        state,
        sidechain_id,
        owner,
        1000 * geocoin::COIN,
        0
    );
    const auto funding = geocoin::core::Transaction::create_coinbase(
        1,
        owner,
        100 * geocoin::COIN
    );
    const auto later_funding = geocoin::core::Transaction::create_coinbase(
        2,
        owner,
        100 * geocoin::COIN
    );
    expect(state.apply_transaction(funding, 1, true), "top-up funding applies");
    expect(state.apply_transaction(later_funding, 2, true), "second top-up funding applies");

    const geocoin::Height mature_height = geocoin::consensus::params::COINBASE_MATURITY + 1;
    state.set_height(mature_height - 1);
    geocoin::core::Transaction top_up;
    top_up.type = geocoin::core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT;
    top_up.sidechain_id = sidechain_id;
    top_up.sidechain_owner = owner;
    top_up.sidechain_amount = funding.outputs.front().value / 2;
    top_up.inputs.push_back({funding.get_hash(), 0, {}, 0xffffffff, {}, {}});
    top_up.outputs.push_back({funding.outputs.front().value - top_up.sidechain_amount, owner, {}});
    top_up.sidechain_signature = sign_sidechain_payload(top_up, owner_key);
    top_up.sign_input(0, owner_key);

    expect(top_up.verify(), "TOP_UP_SIDECHAIN_DEPOSIT cryptographic signatures verify");
    geocoin::state::UTXO funding_utxo;
    expect(state.get_utxo(funding.get_hash(), 0, funding_utxo) && funding_utxo.address == owner,
           "TOP_UP_SIDECHAIN_DEPOSIT funding UTXO belongs to owner");
    expect(state.get_transaction(top_up.get_hash()) == nullptr,
           "TOP_UP_SIDECHAIN_DEPOSIT is not already indexed");
    expect(top_up.witnesses.size() == 1 && !top_up.witnesses.front().public_key.empty(),
           "TOP_UP_SIDECHAIN_DEPOSIT carries a full witness");
    geocoin::Address witness_owner;
    expect(geocoin::crypto::address_from_public_key(
               top_up.witnesses.front().public_key,
               witness_owner
           ) && witness_owner == owner,
           "TOP_UP_SIDECHAIN_DEPOSIT witness derives deposit owner");
    geocoin::state::SidechainDeposit before_top_up;
    expect(state.get_sidechain_deposit(sidechain_id, before_top_up) &&
               before_top_up.owner == owner,
           "TOP_UP_SIDECHAIN_DEPOSIT target deposit belongs to owner");
    expect(state.validate_transaction(top_up), "TOP_UP_SIDECHAIN_DEPOSIT validates");
    expect(state.apply_transaction(top_up, mature_height, false),
           "TOP_UP_SIDECHAIN_DEPOSIT applies");
    geocoin::state::SidechainDeposit deposit;
    expect(state.get_sidechain_deposit(sidechain_id, deposit) &&
               deposit.amount == initial_deposit + top_up.sidechain_amount,
           "TOP_UP_SIDECHAIN_DEPOSIT increases deposit atomically");
    geocoin::state::SidechainRegistryEntry registry;
    expect(state.get_sidechain_registry(sidechain_id, registry) &&
               registry.issuance_limit ==
                   1000 * geocoin::COIN +
                       top_up.sidechain_amount *
                           static_cast<geocoin::Amount>(
                               geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER),
           "TOP_UP_SIDECHAIN_DEPOSIT increases financial issuance limit");
    expect(state.has_utxo(top_up.get_hash(), 0),
           "TOP_UP_SIDECHAIN_DEPOSIT creates change output");

    state.set_height(mature_height);
    geocoin::core::Transaction decommission;
    decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
    decommission.sidechain_id = sidechain_id;
    decommission.sidechain_owner = owner;
    authorize_decommission(decommission, owner_key, owner_key, owner_committee_id(owner));
    expect(state.validate_transaction(decommission), "top-up fixture decommission validates");
    expect(state.apply_transaction(decommission, mature_height + 1, false),
           "top-up fixture decommission applies");

    geocoin::core::Transaction late_top_up = top_up;
    late_top_up.inputs.front().prev_tx_hash = later_funding.get_hash();
    late_top_up.sidechain_signature = sign_sidechain_payload(late_top_up, owner_key);
    late_top_up.sign_input(0, owner_key);
    state.set_height(mature_height + 1);
    expect(!state.validate_transaction(late_top_up),
           "TOP_UP_SIDECHAIN_DEPOSIT is blocked once decommission starts");
}

void test_sidechain_apply_edge_guard_paths()
{
    const geocoin::Address owner = address_from_seed(0xa0);
    const geocoin::Address other = address_from_seed(0xa1);
    const geocoin::Address recipient = address_from_seed(0xa2);

    auto issue_tx = [&](const geocoin::Hash256& id) {
        geocoin::core::Transaction tx;
        tx.type = geocoin::core::TransactionType::ISSUE_SIDECHAIN;
        tx.sidechain_id = id;
        tx.sidechain_owner = owner;
        tx.sidechain_checkpoint = id;
        tx.sidechain_token_symbol = "EDG";
        tx.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        tx.sidechain_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
        tx.sidechain_fee_bps = 25;
        tx.sidechain_checkpoint_policy_id = 2;
        tx.sidechain_checkpoint_version = 2;
        tx.sidechain_committee_m = 1;
        tx.sidechain_committee_n = 1;
        tx.sidechain_committee_members = {owner};
        tx.sidechain_committee_id = geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner});
        return tx;
    };

    auto restore_sidechain = [&](geocoin::state::ChainState& state,
                                 const geocoin::Hash256& id,
                                 geocoin::Amount deposit_amount = 1000 * geocoin::COIN,
                                 geocoin::Amount issued = 10 * geocoin::COIN,
                                 geocoin::Amount limit = 100 * geocoin::COIN,
                                 uint8_t policy_id = 2,
                                 uint8_t version = 2) {
        restore_deposit_with_revert(state, id, owner, deposit_amount, 0);
        restore_sidechain_registry_with_revert(state, id, owner, limit, issued, policy_id, version);
    };

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x01);
        restore_sidechain(state, id);
        expect(!StateAccess::apply_sidechain_effects(state, issue_tx(id), 100),
               "ISSUE_SIDECHAIN rejects duplicate registry");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        expect(!StateAccess::apply_sidechain_effects(state, issue_tx(hash_from_seed(0x02)), 100),
               "ISSUE_SIDECHAIN rejects missing deposit");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x03);
        restore_deposit_with_revert(state, id, owner, 1000 * geocoin::COIN, 0);
        auto tx = issue_tx(id);
        tx.sidechain_owner = other;
        expect(!StateAccess::apply_sidechain_effects(state, tx, 100), "ISSUE_SIDECHAIN rejects owner mismatch");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x04);
        const geocoin::Address invalid_owner("GEOinvalid-owner");
        restore_deposit_with_revert(state, id, invalid_owner, 1000 * geocoin::COIN, 0);
        auto tx = issue_tx(id);
        tx.sidechain_owner = invalid_owner;
        expect(!StateAccess::apply_sidechain_effects(state, tx, 100), "ISSUE_SIDECHAIN rejects invalid owner address");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x05);
        restore_deposit_with_revert(state, id, owner, 0, 0);
        expect(!StateAccess::apply_sidechain_effects(state, issue_tx(id), 100),
               "ISSUE_SIDECHAIN rejects deposit below registration fee");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x06);
        restore_deposit_with_revert(state, id, owner, 1000 * geocoin::COIN, 0);
        auto tx = issue_tx(id);
        tx.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER - 1;
        expect(!StateAccess::apply_sidechain_effects(state, tx, 100), "ISSUE_SIDECHAIN rejects low multiplier");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x07);
        restore_deposit_with_revert(state, id, owner, 1000 * geocoin::COIN, 0);
        auto tx = issue_tx(id);
        tx.sidechain_token_symbol = "BAD\n";
        expect(!StateAccess::apply_sidechain_effects(state, tx, 100), "ISSUE_SIDECHAIN rejects non-printable symbol");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x08);
        restore_deposit_with_revert(state, id, owner, 1000 * geocoin::COIN, 0);
        auto tx = issue_tx(id);
        tx.sidechain_checkpoint = hash_from_seed(0x80);
        expect(!StateAccess::apply_sidechain_effects(state, tx, 100), "ISSUE_SIDECHAIN requires checkpoint equal id");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x09);
        restore_deposit_with_revert(state, id, owner, 1000 * geocoin::COIN, 0);
        auto tx = issue_tx(id);
        tx.sidechain_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER - 1;
        expect(!StateAccess::apply_sidechain_effects(state, tx, 100), "ISSUE_SIDECHAIN rejects bad max multiplier");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x0a);
        restore_deposit_with_revert(state, id, owner, 1000 * geocoin::COIN, 0);
        auto tx = issue_tx(id);
        tx.sidechain_fee_bps = geocoin::consensus::params::MAX_SIDECHAIN_FEE_BPS + 1;
        expect(!StateAccess::apply_sidechain_effects(state, tx, 100), "ISSUE_SIDECHAIN rejects high fee bps");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x0b);
        restore_deposit_with_revert(state, id, owner, 1000 * geocoin::COIN, 0);
        auto tx = issue_tx(id);
        tx.sidechain_checkpoint_policy_id = 1;
        expect(!StateAccess::apply_sidechain_effects(state, tx, 100), "ISSUE_SIDECHAIN rejects unsupported checkpoint policy");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x0c);
        restore_deposit_with_revert(state, id, owner, std::numeric_limits<geocoin::Amount>::max(), 0);
        auto tx = issue_tx(id);
        tx.sidechain_fee_bps = 0;
        tx.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        tx.sidechain_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        expect(!StateAccess::apply_sidechain_effects(state, tx, 100), "ISSUE_SIDECHAIN rejects issuance limit overflow");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction mint;
        mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
        mint.sidechain_id = hash_from_seed(0x10);
        mint.sidechain_amount = 1;
        expect(!StateAccess::apply_sidechain_effects(state, mint, 100), "MINT_SIDECHAIN rejects missing registry");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x11);
        restore_sidechain(state, id, 1000 * geocoin::COIN, 0, 10 * geocoin::COIN, 1, 1);
        geocoin::core::Transaction mint;
        mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
        mint.sidechain_id = id;
        mint.sidechain_amount = 1;
        expect(!StateAccess::apply_sidechain_effects(state, mint, 100), "MINT_SIDECHAIN rejects unsupported registry policy");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x12);
        restore_sidechain(state, id);
        geocoin::core::Transaction mint;
        mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
        mint.sidechain_id = id;
        mint.sidechain_amount = 0;
        expect(!StateAccess::apply_sidechain_effects(state, mint, 100), "MINT_SIDECHAIN rejects zero amount");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x13);
        restore_sidechain(state, id, 1000 * geocoin::COIN, 9 * geocoin::COIN, 10 * geocoin::COIN);
        geocoin::core::Transaction mint;
        mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
        mint.sidechain_id = id;
        mint.sidechain_amount = 2 * geocoin::COIN;
        expect(!StateAccess::apply_sidechain_effects(state, mint, 100), "MINT_SIDECHAIN rejects issuance overflow");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x14);
        restore_sidechain_registry_with_revert(state, id, owner, 100 * geocoin::COIN, 0);
        geocoin::core::Transaction mint;
        mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
        mint.sidechain_id = id;
        mint.sidechain_amount = 1;
        expect(!StateAccess::apply_sidechain_effects(state, mint, 100), "MINT_SIDECHAIN rejects missing deposit");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x15);
        restore_sidechain(state, id, 0, 0, 100 * geocoin::COIN);
        geocoin::core::Transaction mint;
        mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
        mint.sidechain_id = id;
        mint.sidechain_amount = geocoin::COIN;
        expect(!StateAccess::apply_sidechain_effects(state, mint, 100), "MINT_SIDECHAIN rejects deposit below fee");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x16);
        restore_sidechain(state, id, 1000 * geocoin::COIN, 0, 100 * geocoin::COIN);
        geocoin::core::Transaction mint;
        mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
        mint.sidechain_id = id;
        mint.sidechain_amount = geocoin::COIN;
        expect(!StateAccess::apply_sidechain_effects(state, mint, 100), "MINT_SIDECHAIN rejects missing committee signatures");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction redeem;
        redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
        redeem.sidechain_id = hash_from_seed(0x20);
        redeem.sidechain_amount = 1;
        expect(!StateAccess::apply_sidechain_effects(state, redeem, 100), "REDEEM_SIDECHAIN rejects missing registry");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x21);
        restore_sidechain(state, id, 1000 * geocoin::COIN, 10 * geocoin::COIN, 100 * geocoin::COIN, 1, 1);
        geocoin::core::Transaction redeem;
        redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
        redeem.sidechain_id = id;
        redeem.sidechain_amount = 1;
        expect(!StateAccess::apply_sidechain_effects(state, redeem, 100), "REDEEM_SIDECHAIN rejects unsupported registry policy");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x22);
        restore_sidechain(state, id, 1000 * geocoin::COIN, 10 * geocoin::COIN);
        geocoin::core::Transaction redeem;
        redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
        redeem.sidechain_id = id;
        redeem.sidechain_amount = 0;
        expect(!StateAccess::apply_sidechain_effects(state, redeem, 100), "REDEEM_SIDECHAIN rejects zero amount");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x23);
        restore_sidechain(state, id, 1000 * geocoin::COIN, 10 * geocoin::COIN);
        geocoin::core::Transaction redeem;
        redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
        redeem.sidechain_id = id;
        redeem.sidechain_amount = 11 * geocoin::COIN;
        expect(!StateAccess::apply_sidechain_effects(state, redeem, 100), "REDEEM_SIDECHAIN rejects amount above issued supply");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x24);
        restore_sidechain_registry_with_revert(state, id, owner, 100 * geocoin::COIN, 10 * geocoin::COIN);
        geocoin::core::Transaction redeem;
        redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
        redeem.sidechain_id = id;
        redeem.sidechain_amount = geocoin::COIN;
        expect(!StateAccess::apply_sidechain_effects(state, redeem, 100), "REDEEM_SIDECHAIN rejects missing deposit");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x25);
        restore_sidechain(state, id, 1, 10 * geocoin::COIN);
        geocoin::core::Transaction redeem;
        redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
        redeem.sidechain_id = id;
        redeem.sidechain_amount = geocoin::COIN;
        expect(!StateAccess::apply_sidechain_effects(state, redeem, 100), "REDEEM_SIDECHAIN rejects deposit below payout plus fee");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x26);
        restore_sidechain(state, id, 1000 * geocoin::COIN, 10 * geocoin::COIN);
        geocoin::core::Transaction redeem;
        redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
        redeem.sidechain_id = id;
        redeem.sidechain_amount = geocoin::COIN;
        redeem.outputs.push_back({geocoin::COIN - 1, recipient, {}});
        expect(!StateAccess::apply_sidechain_effects(state, redeem, 100), "REDEEM_SIDECHAIN rejects payout total mismatch");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction slash;
        slash.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
        slash.sidechain_id = hash_from_seed(0x30);
        slash.sidechain_amount = 1;
        expect(!StateAccess::apply_sidechain_effects(state, slash, 100), "SLASH_DEPOSIT rejects missing registry");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x31);
        restore_sidechain(state, id, 1000 * geocoin::COIN, 0, 100 * geocoin::COIN, 1, 1);
        geocoin::core::Transaction slash;
        slash.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
        slash.sidechain_id = id;
        slash.sidechain_amount = 1;
        expect(!StateAccess::apply_sidechain_effects(state, slash, 100), "SLASH_DEPOSIT rejects unsupported registry policy");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x32);
        restore_sidechain(state, id);
        geocoin::core::Transaction slash;
        slash.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
        slash.sidechain_id = id;
        slash.sidechain_amount = 0;
        expect(!StateAccess::apply_sidechain_effects(state, slash, 100), "SLASH_DEPOSIT rejects zero amount");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x33);
        restore_sidechain_registry_with_revert(state, id, owner, 100 * geocoin::COIN, 0);
        geocoin::core::Transaction slash;
        slash.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
        slash.sidechain_id = id;
        slash.sidechain_amount = 1;
        expect(!StateAccess::apply_sidechain_effects(state, slash, 100), "SLASH_DEPOSIT rejects missing deposit");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x34);
        restore_sidechain(state, id, 1);
        geocoin::core::Transaction slash;
        slash.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
        slash.sidechain_id = id;
        slash.sidechain_amount = 2;
        expect(!StateAccess::apply_sidechain_effects(state, slash, 100), "SLASH_DEPOSIT rejects amount above deposit");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x35);
        restore_sidechain(state, id);
        geocoin::core::Transaction slash;
        slash.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
        slash.sidechain_id = id;
        slash.sidechain_amount = 1;
        slash.sidechain_fraud_checkpoint_a = hash_from_seed(0x36);
        slash.sidechain_fraud_checkpoint_b = hash_from_seed(0x37);
        expect(!StateAccess::apply_sidechain_effects(state, slash, 100), "SLASH_DEPOSIT rejects unresolved fraud proof");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction batch;
        batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        expect(!StateAccess::apply_sidechain_effects(state, batch, 100), "BATCH_SIDECHAIN_CHECKPOINT rejects empty batch");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction batch;
        batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        geocoin::core::BatchCheckpointItem item;
        item.sidechain_id = hash_from_seed(0x40);
        item.checkpoint = hash_from_seed(0x41);
        item.checkpoint_height = 1;
        item.fee = 1;
        item.version = 2;
        batch.sidechain_batch_checkpoints = {item, item};
        expect(!StateAccess::apply_sidechain_effects(state, batch, 100), "BATCH_SIDECHAIN_CHECKPOINT rejects duplicate ids");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction batch;
        batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        geocoin::core::BatchCheckpointItem item;
        item.sidechain_id = hash_from_seed(0x42);
        item.checkpoint_height = 0;
        item.fee = 1;
        batch.sidechain_batch_checkpoints = {item};
        expect(!StateAccess::apply_sidechain_effects(state, batch, 100), "BATCH_SIDECHAIN_CHECKPOINT rejects invalid item basics");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction batch;
        batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        geocoin::core::BatchCheckpointItem item;
        item.sidechain_id = hash_from_seed(0x43);
        item.checkpoint = hash_from_seed(0x44);
        item.checkpoint_height = 1;
        item.fee = 1;
        item.version = 2;
        batch.sidechain_batch_checkpoints = {item};
        expect(!StateAccess::apply_sidechain_effects(state, batch, 100), "BATCH_SIDECHAIN_CHECKPOINT rejects missing registry");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x45);
        restore_sidechain(state, id, 1000 * geocoin::COIN, 0, 100 * geocoin::COIN, 1, 1);
        geocoin::core::Transaction batch;
        batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        geocoin::core::BatchCheckpointItem item;
        item.sidechain_id = id;
        item.checkpoint = hash_from_seed(0x46);
        item.checkpoint_height = 1;
        item.fee = 1;
        item.version = 2;
        batch.sidechain_batch_checkpoints = {item};
        expect(!StateAccess::apply_sidechain_effects(state, batch, 100), "BATCH_SIDECHAIN_CHECKPOINT rejects unsupported registry policy");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x47);
        restore_sidechain(state, id);
        geocoin::core::Transaction batch;
        batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        geocoin::core::BatchCheckpointItem item;
        item.sidechain_id = id;
        item.checkpoint = hash_from_seed(0x48);
        item.checkpoint_height = 1;
        item.fee = 1;
        item.version = 1;
        batch.sidechain_batch_checkpoints = {item};
        expect(!StateAccess::apply_sidechain_effects(state, batch, 100), "BATCH_SIDECHAIN_CHECKPOINT rejects version mismatch");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x49);
        restore_sidechain_registry_with_revert(state, id, owner, 100 * geocoin::COIN, 0);
        geocoin::core::Transaction batch;
        batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        geocoin::core::BatchCheckpointItem item;
        item.sidechain_id = id;
        item.checkpoint = hash_from_seed(0x4a);
        item.checkpoint_height = 1;
        item.fee = 1;
        item.version = 2;
        item.proof = {1};
        batch.sidechain_batch_checkpoints = {item};
        expect(!StateAccess::apply_sidechain_effects(state, batch, 100), "BATCH_SIDECHAIN_CHECKPOINT rejects missing deposit");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction decommission;
        decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
        decommission.sidechain_id = hash_from_seed(0x50);
        decommission.sidechain_owner = owner;
        decommission.inputs.push_back({hash_from_seed(0x51), 0, {}, 0xffffffff, {}, {}});
        expect(!StateAccess::apply_sidechain_effects(state, decommission, 100), "DECOMMISSION_SIDECHAIN rejects inputs");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction decommission;
        decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
        decommission.sidechain_id = hash_from_seed(0x52);
        decommission.sidechain_owner = owner;
        decommission.sidechain_amount = 1;
        expect(!StateAccess::apply_sidechain_effects(state, decommission, 100), "DECOMMISSION_SIDECHAIN rejects non-zero amount");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction decommission;
        decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
        decommission.sidechain_id = hash_from_seed(0x53);
        decommission.sidechain_owner = owner;
        decommission.sidechain_checkpoint = hash_from_seed(0x54);
        expect(!StateAccess::apply_sidechain_effects(state, decommission, 100), "DECOMMISSION_SIDECHAIN rejects checkpoint");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction decommission;
        decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
        decommission.sidechain_id = hash_from_seed(0x55);
        decommission.sidechain_owner = owner;
        expect(!StateAccess::apply_sidechain_effects(state, decommission, 100), "DECOMMISSION_SIDECHAIN rejects missing registry");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x56);
        restore_sidechain_registry_with_revert(state, id, owner, 100 * geocoin::COIN, geocoin::COIN);
        geocoin::core::Transaction decommission;
        decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
        decommission.sidechain_id = id;
        decommission.sidechain_owner = owner;
        expect(!StateAccess::apply_sidechain_effects(state, decommission, 100), "DECOMMISSION_SIDECHAIN rejects issued supply");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x5c);
        restore_deposit_with_revert(state, id, owner, 0, 0);
        restore_sidechain_registry_with_revert(state, id, owner, 100 * geocoin::COIN, geocoin::COIN);
        geocoin::core::Transaction decommission;
        decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
        decommission.sidechain_id = id;
        decommission.sidechain_owner = owner;
        authorize_decommission(
            decommission,
            private_key_from_seed(0xa0),
            private_key_from_seed(0xa0),
            owner_committee_id(owner)
        );
        expect(StateAccess::apply_sidechain_effects(state, decommission, 100),
               "DECOMMISSION_SIDECHAIN allows issued supply after deposit is exhausted");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x57);
        restore_sidechain_registry_with_revert(state, id, owner, 100 * geocoin::COIN, 0);
        geocoin::core::Transaction decommission;
        decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
        decommission.sidechain_id = id;
        decommission.sidechain_owner = owner;
        expect(!StateAccess::apply_sidechain_effects(state, decommission, 100), "DECOMMISSION_SIDECHAIN rejects missing deposit");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto id = hash_from_seed(0x58);
        restore_sidechain(state, id, 100 * geocoin::COIN, 0);
        geocoin::core::Transaction decommission;
        decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
        decommission.sidechain_id = id;
        decommission.sidechain_owner = other;
        expect(!StateAccess::apply_sidechain_effects(state, decommission, 100), "DECOMMISSION_SIDECHAIN rejects owner mismatch");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction interest;
        interest.type = geocoin::core::TransactionType::INTEREST_DISTRIBUTION;
        interest.outputs.push_back({0, recipient, {}});
        expect(!StateAccess::apply_interest_effects(state, interest, 100), "INTEREST_DISTRIBUTION rejects non-positive output");
    }
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::state::ChainStateTestAccess::set_interest_pool_balance(state, 1);
        geocoin::core::Transaction interest;
        interest.type = geocoin::core::TransactionType::INTEREST_DISTRIBUTION;
        interest.outputs.push_back({2, recipient, {}});
        expect(!StateAccess::apply_interest_effects(state, interest, 100), "INTEREST_DISTRIBUTION rejects amount above pool");
    }
}

void test_sidechain_registry_apply_paths()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::bytes owner_key = private_key_from_seed(0xa3);
    const geocoin::bytes next_committee_key = private_key_from_seed(0xa4);
    const geocoin::Address owner = address_from_seed(0xa3);
    const geocoin::Address next_committee_member = address_from_seed(0xa4);
    const geocoin::Address recipient = address_from_seed(0xa5);
    const geocoin::Hash256 sidechain_id = hash_from_seed(0xa6);
    const geocoin::Amount starting_deposit = 2000 * geocoin::COIN;
    const geocoin::Amount minted_amount = 10 * geocoin::COIN;
    const uint32_t fee_bps = 25;

    restore_deposit_with_revert(state, sidechain_id, owner, starting_deposit, 0);
    state.set_height(100);

    geocoin::core::Transaction issue;
    issue.type = geocoin::core::TransactionType::ISSUE_SIDECHAIN;
    issue.sidechain_id = sidechain_id;
    issue.sidechain_owner = owner;
    issue.sidechain_checkpoint = sidechain_id;
    issue.sidechain_token_symbol = "APL";
    issue.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2;
    issue.sidechain_fee_bps = fee_bps;
    issue.sidechain_checkpoint_policy_id = 2;
    issue.sidechain_checkpoint_version = 2;
    issue.sidechain_committee_m = 1;
    issue.sidechain_committee_n = 1;
    issue.sidechain_committee_members = {owner};
    issue.sidechain_committee_id = geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner});

    issue.sidechain_signature = sign_sidechain_payload(issue, owner_key);
    expect(state.validate_transaction(issue), "ISSUE_SIDECHAIN apply lifecycle validates");
    expect(StateAccess::apply_sidechain_effects(state, issue, 101), "ISSUE_SIDECHAIN applies");

    geocoin::state::SidechainRegistryEntry registry;
    expect(state.get_sidechain_registry(sidechain_id, registry), "ISSUE_SIDECHAIN creates registry");
    expect(registry.owner == owner && registry.token_symbol == "APL", "ISSUE_SIDECHAIN stores registry metadata");
    expect(registry.total_issued == 0, "ISSUE_SIDECHAIN starts with zero issued supply");

    geocoin::state::SidechainDeposit deposit;
    expect(state.get_sidechain_deposit(sidechain_id, deposit), "ISSUE_SIDECHAIN keeps deposit");
    const geocoin::Amount issue_fee = geocoin::consensus::calc_sidechain_fee(0, fee_bps);
    expect(deposit.amount == starting_deposit - issue_fee, "ISSUE_SIDECHAIN deducts registration fee");

    geocoin::core::Transaction mint;
    mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
    mint.sidechain_owner = owner;
    mint.sidechain_id = sidechain_id;
    mint.sidechain_amount = minted_amount;
    mint.sidechain_committee_id = issue.sidechain_committee_id;
    mint.sidechain_committee_signatures.push_back(sign_sidechain_payload(mint, owner_key));
    expect(state.validate_transaction(mint), "MINT_SIDECHAIN apply lifecycle validates");
    expect(StateAccess::apply_sidechain_effects(state, mint, 102), "MINT_SIDECHAIN applies");
    expect(state.get_sidechain_registry(sidechain_id, registry), "MINT_SIDECHAIN registry remains readable");
    expect(registry.total_issued == minted_amount, "MINT_SIDECHAIN increments issued supply");
    expect(state.get_sidechain_deposit(sidechain_id, deposit), "MINT_SIDECHAIN deposit remains readable");
    const geocoin::Amount mint_fee = geocoin::consensus::calc_sidechain_fee(minted_amount, fee_bps);
    expect(deposit.amount == starting_deposit - issue_fee - mint_fee, "MINT_SIDECHAIN deducts fee");

    const auto checkpoint_proof = make_checkpoint_proof_v2(sidechain_id, 23, amount_to_u64(minted_amount));
    geocoin::core::BatchCheckpointItem item;
    item.sidechain_id = sidechain_id;
    item.checkpoint = checkpoint_proof.checkpoint;
    item.checkpoint_height = 23;
    item.fee = geocoin::consensus::calculate_sidechain_checkpoint_min_fee(state.get_height() + 1, 0);
    item.version = 2;
    item.proof = checkpoint_proof.proof;
    item.committee_id = issue.sidechain_committee_id;
    item.committee_signatures.push_back(
        sign_bytes_payload(geocoin::core::build_sidechain_batch_checkpoint_payload_for_signing(item), owner_key)
    );

    geocoin::core::Transaction batch;
    batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    batch.sidechain_batch_checkpoints = {item};
    expect(state.validate_transaction(batch), "BATCH_SIDECHAIN_CHECKPOINT apply lifecycle validates");
    expect(StateAccess::apply_sidechain_effects(state, batch, state.get_height() + 1), "BATCH_SIDECHAIN_CHECKPOINT applies");
    expect(state.get_sidechain_deposit(sidechain_id, deposit), "BATCH_SIDECHAIN_CHECKPOINT keeps deposit");
    expect(deposit.checkpoint == item.checkpoint &&
               deposit.last_sidechain_checkpoint_height == item.checkpoint_height,
           "BATCH_SIDECHAIN_CHECKPOINT updates checkpoint metadata");
    expect(deposit.amount == starting_deposit - issue_fee - mint_fee - item.fee,
           "BATCH_SIDECHAIN_CHECKPOINT deducts checkpoint fee");

    geocoin::core::Transaction redeem;
    redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
    redeem.sidechain_owner = owner;
    redeem.sidechain_id = sidechain_id;
    redeem.sidechain_amount = minted_amount;
    redeem.outputs.push_back({minted_amount, recipient, {}});
    redeem.sidechain_committee_id = issue.sidechain_committee_id;
    redeem.sidechain_committee_signatures.push_back(sign_sidechain_payload(redeem, owner_key));
    expect(state.validate_transaction(redeem), "REDEEM_SIDECHAIN apply lifecycle validates");
    expect(StateAccess::apply_sidechain_effects(state, redeem, 103), "REDEEM_SIDECHAIN applies");
    expect(state.get_sidechain_registry(sidechain_id, registry), "REDEEM_SIDECHAIN registry remains readable");
    expect(registry.total_issued == 0, "REDEEM_SIDECHAIN decrements issued supply");
    geocoin::state::UTXO redeem_utxo;
    expect(state.get_utxo(redeem.get_hash(), 0, redeem_utxo), "REDEEM_SIDECHAIN creates payout UTXO");
    expect(redeem_utxo.value == minted_amount && redeem_utxo.address == recipient,
           "REDEEM_SIDECHAIN payout UTXO matches output");

    geocoin::core::Transaction update_committee;
    update_committee.type = geocoin::core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE;
    update_committee.sidechain_owner = owner;
    update_committee.sidechain_id = sidechain_id;
    update_committee.sidechain_committee_m = 1;
    update_committee.sidechain_committee_n = 1;
    update_committee.sidechain_committee_members = {next_committee_member};
    update_committee.sidechain_committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, {next_committee_member});
    update_committee.sidechain_committee_signatures.push_back(
        sign_bytes_payload(
            geocoin::core::build_sidechain_committee_update_payload_for_signing(update_committee),
            owner_key
        )
    );
    expect(state.validate_transaction(update_committee), "UPDATE_SIDECHAIN_COMMITTEE apply lifecycle validates");
    expect(StateAccess::apply_sidechain_effects(state, update_committee, 104), "UPDATE_SIDECHAIN_COMMITTEE applies");
    expect(state.get_sidechain_registry(sidechain_id, registry), "UPDATE_SIDECHAIN_COMMITTEE registry remains readable");
    expect(registry.committee_id == update_committee.sidechain_committee_id &&
               registry.committee_members == std::vector<geocoin::Address>{next_committee_member},
           "UPDATE_SIDECHAIN_COMMITTEE stores new committee");

    geocoin::core::Transaction mint_with_new_committee;
    mint_with_new_committee.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
    mint_with_new_committee.sidechain_owner = owner;
    mint_with_new_committee.sidechain_id = sidechain_id;
    mint_with_new_committee.sidechain_amount = geocoin::COIN;
    mint_with_new_committee.sidechain_committee_id = update_committee.sidechain_committee_id;
    mint_with_new_committee.sidechain_committee_signatures.push_back(
        sign_sidechain_payload(mint_with_new_committee, next_committee_key)
    );
    expect(state.validate_transaction(mint_with_new_committee),
           "updated sidechain committee can authorize later mint");

    geocoin::core::Transaction decommission;
    decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
    decommission.sidechain_id = sidechain_id;
    decommission.sidechain_owner = owner;
    authorize_decommission(
        decommission,
        owner_key,
        next_committee_key,
        update_committee.sidechain_committee_id
    );
    expect(state.validate_transaction(decommission), "DECOMMISSION_SIDECHAIN apply lifecycle validates");
    expect(StateAccess::apply_sidechain_effects(state, decommission, 105), "DECOMMISSION_SIDECHAIN applies");
    expect(state.get_sidechain_registry(sidechain_id, registry),
           "DECOMMISSION_SIDECHAIN keeps registry during challenge period");
    expect(registry.status == geocoin::state::SidechainStatus::DECOMMISSIONING &&
               registry.decommission_height == 105,
           "DECOMMISSION_SIDECHAIN records challenge state and height");
    expect(state.get_sidechain_deposit(sidechain_id, deposit), "DECOMMISSION_SIDECHAIN leaves deposit record");
}

void test_accounting_sidechain_lifecycle()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::bytes owner_key = private_key_from_seed(0xb1);
    const geocoin::Address owner = address_from_seed(0xb1);
    const geocoin::Hash256 sidechain_id = hash_from_seed(0xb2);
    const geocoin::Hash256 schema_hash = hash_from_seed(0xb3);
    const geocoin::Amount starting_deposit = 500 * geocoin::COIN;

    restore_deposit_with_revert(state, sidechain_id, owner, starting_deposit, 0);
    state.set_height(100);

    geocoin::core::Transaction registration;
    registration.type = geocoin::core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN;
    registration.sidechain_id = sidechain_id;
    registration.sidechain_owner = owner;
    registration.sidechain_checkpoint = sidechain_id;
    registration.sidechain_namespace = "land-registry";
    registration.sidechain_schema_hash = schema_hash;
    registration.sidechain_checkpoint_policy_id =
        geocoin::consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID;
    registration.sidechain_checkpoint_version =
        geocoin::consensus::params::ACCOUNTING_CHECKPOINT_VERSION;
    registration.sidechain_committee_m = 1;
    registration.sidechain_committee_n = 1;
    registration.sidechain_committee_members = {owner};
    registration.sidechain_committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner});
    registration.sidechain_signature = sign_sidechain_payload(registration, owner_key);

    expect(state.validate_transaction(registration),
           "REGISTER_ACCOUNTING_SIDECHAIN validates");
    expect(state.apply_transaction(registration, 101, false),
           "REGISTER_ACCOUNTING_SIDECHAIN applies");

    geocoin::state::SidechainRegistryEntry registry;
    expect(state.get_sidechain_registry(sidechain_id, registry),
           "accounting registry is created");
    expect(registry.kind == geocoin::core::SidechainKind::ACCOUNTING &&
               registry.status == geocoin::state::SidechainStatus::ACTIVE &&
               registry.accounting_namespace == "land-registry" &&
               registry.accounting_schema_hash == schema_hash,
           "accounting registry stores kind, namespace and schema");
    expect(registry.issuance_limit == 0 && registry.total_issued == 0 &&
               registry.token_symbol.empty(),
           "accounting registry has no token economics");

    const geocoin::Hash256 other_id = hash_from_seed(0xb4);
    restore_deposit_with_revert(state, other_id, owner, starting_deposit, 0);
    geocoin::core::Transaction duplicate_namespace = registration;
    duplicate_namespace.sidechain_id = other_id;
    duplicate_namespace.sidechain_checkpoint = other_id;
    duplicate_namespace.sidechain_signature = sign_sidechain_payload(duplicate_namespace, owner_key);
    expect(!state.validate_transaction(duplicate_namespace),
           "accounting namespace stays unique until UNLOCK");

    geocoin::core::Transaction forbidden_mint;
    forbidden_mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
    forbidden_mint.sidechain_id = sidechain_id;
    forbidden_mint.sidechain_amount = geocoin::COIN;
    forbidden_mint.sidechain_committee_id = registry.committee_id;
    forbidden_mint.sidechain_committee_signatures.push_back(
        sign_sidechain_payload(forbidden_mint, owner_key)
    );
    expect(!state.validate_transaction(forbidden_mint),
           "accounting sidechain rejects token minting");

    geocoin::state::SidechainDeposit deposit_before_checkpoint;
    expect(state.get_sidechain_deposit(sidechain_id, deposit_before_checkpoint),
           "accounting deposit remains after registration");
    const geocoin::Height checkpoint_height =
        deposit_before_checkpoint.last_sidechain_checkpoint_height + 1;
    const auto proof = make_accounting_checkpoint_proof_v1(
        sidechain_id,
        hash_from_seed(0xb4),
        checkpoint_height,
        schema_hash
    );
    geocoin::core::BatchCheckpointItem item;
    item.sidechain_id = sidechain_id;
    item.checkpoint = proof.checkpoint;
    item.checkpoint_height = checkpoint_height;
    item.fee = geocoin::consensus::calculate_sidechain_checkpoint_min_fee(
        102,
        deposit_before_checkpoint.last_checkpoint_height
    );
    item.version = geocoin::consensus::params::ACCOUNTING_CHECKPOINT_VERSION;
    item.committee_id = registry.committee_id;
    item.proof = proof.proof;
    item.committee_signatures.push_back(sign_bytes_payload(
        geocoin::core::build_sidechain_batch_checkpoint_payload_for_signing(item),
        owner_key
    ));

    geocoin::core::Transaction batch;
    batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    batch.sidechain_batch_checkpoints = {item};
    state.set_height(101);
    expect(state.validate_transaction(batch), "accounting checkpoint validates");

    geocoin::core::Transaction bad_schema_batch = batch;
    auto& bad_item = bad_schema_batch.sidechain_batch_checkpoints.front();
    bad_item.proof.back() ^= 0x01;
    bad_item.committee_signatures.clear();
    bad_item.committee_signatures.push_back(sign_bytes_payload(
        geocoin::core::build_sidechain_batch_checkpoint_payload_for_signing(bad_item),
        owner_key
    ));
    expect(!state.validate_transaction(bad_schema_batch),
           "accounting checkpoint rejects proof for another schema");

    expect(state.apply_transaction(batch, 102, false), "accounting checkpoint applies");
    geocoin::state::SidechainDeposit deposit;
    expect(state.get_sidechain_deposit(sidechain_id, deposit) &&
               deposit.checkpoint == item.checkpoint &&
               deposit.last_sidechain_checkpoint_height == checkpoint_height,
           "accounting checkpoint updates deposit metadata");
}

void test_special_apply_and_revert_index_paths()
{
    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* storage_ptr = storage.get();
    geocoin::state::ChainState state(std::move(storage));

    geocoin::core::Transaction angle;
    angle.type = geocoin::core::TransactionType::ANGLE_COMMITMENT;
    angle.inputs.push_back({hash_from_seed(0xd0), 0, {}, 0xffffffff, {}, {}});
    expect(StateAccess::apply_angle_effects(state, angle, 20), "ANGLE_COMMITMENT apply path succeeds");
    expect(state.has_angle_commitment_key(angle.inputs[0].prev_tx_hash),
           "ANGLE_COMMITMENT anti-replay key is indexed");
    expect(state.get_angle_store()->has_commitment(angle.get_hash()),
           "ANGLE_COMMITMENT is present in the in-memory store");
    expect(storage_ptr->has_transaction(angle.get_hash()), "ANGLE_COMMITMENT stores transaction");

    geocoin::state::TxUndo empty_undo;
    auto revert_tx = [&](const geocoin::core::Transaction& tx) {
        return geocoin::state::ChainStateTestAccess::revert_transaction(state, tx, empty_undo);
    };

    expect(revert_tx(angle), "ANGLE_COMMITMENT revert path succeeds");
    expect(!state.has_angle_commitment_key(angle.inputs[0].prev_tx_hash),
           "ANGLE_COMMITMENT anti-replay key is removed on revert");
    expect(!state.get_angle_store()->has_commitment(angle.get_hash()),
           "ANGLE_COMMITMENT is removed from the in-memory store on revert");

    geocoin::core::Transaction delegation;
    delegation.type = geocoin::core::TransactionType::DELEGATION_PROOF;
    delegation.primary_multisig_address = address_from_seed(0xd1);
    delegation.delegated_multisig_address = address_from_seed(0xd2);
    delegation.nonce = 42;
    delegation.max_per_transaction = 10 * geocoin::COIN;
    delegation.max_total_delegated = 30 * geocoin::COIN;
    expect(StateAccess::apply_delegation_effects(state, delegation, 21), "DELEGATION_PROOF apply path succeeds");
    expect(state.has_delegation_proof_key(delegation.get_delegation_proof_hash()),
           "DELEGATION_PROOF anti-replay key is indexed");
    const geocoin::Hash256 delegation_txid = delegation.get_hash();

    geocoin::core::Transaction revoke;
    revoke.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
    revoke.revoked_delegation_proof_txid = delegation_txid;
    expect(StateAccess::apply_revoke_effects(state, revoke, 22), "REVOKE_DELEGATION apply path succeeds");
    expect(state.has_revoke_delegation_key(delegation_txid), "REVOKE_DELEGATION key is indexed");
    expect(state.is_delegation_revoked(delegation_txid), "delegation is marked revoked");
    expect(revert_tx(revoke), "REVOKE_DELEGATION revert path succeeds");
    expect(!state.has_revoke_delegation_key(delegation_txid), "REVOKE_DELEGATION key is removed on revert");

    geocoin::core::Transaction multisig;
    multisig.type = geocoin::core::TransactionType::MULTISIG_SPEND;
    multisig.version = 2;
    multisig.m = 1;
    multisig.n = 1;
    multisig.spend_participants = {address_from_seed(0xd3)};
    multisig.outputs.push_back({5 * geocoin::COIN, address_from_seed(0xd4), {}});
    geocoin::state::UTXO multisig_funding;
    multisig_funding.tx_hash = hash_from_seed(0xd5);
    multisig_funding.value = 5 * geocoin::COIN;
    multisig_funding.address = address_from_seed(0xd3);
    multisig_funding.height = 1;
    StateAccess::add_utxo(state, multisig_funding);
    multisig.inputs = {{multisig_funding.tx_hash, multisig_funding.vout}};
    expect(StateAccess::apply_multisig_effects(state, multisig, 23), "standard MULTISIG_SPEND apply path succeeds");
    expect(state.has_multisig_spend_key(multisig.get_hash_without_commitments()),
           "standard MULTISIG_SPEND key is indexed");
    expect(state.has_utxo(multisig.get_hash(), 0), "standard MULTISIG_SPEND creates output");
    geocoin::state::TxUndo multisig_undo;
    multisig_undo.spent_utxos = {multisig_funding};
    expect(StateAccess::revert_transaction(state, multisig, multisig_undo),
           "standard MULTISIG_SPEND revert path succeeds");
    expect(state.has_utxo(multisig_funding.tx_hash, multisig_funding.vout),
           "standard MULTISIG_SPEND revert restores funding");
    expect(!state.has_multisig_spend_key(multisig.get_hash_without_commitments()),
           "standard MULTISIG_SPEND key is removed on revert");

    geocoin::core::Transaction hierarchical = multisig;
    hierarchical.spend_participants.clear();
    hierarchical.delegation_proof_txid = delegation_txid;
    hierarchical.outputs.clear();
    hierarchical.inputs.clear();
    // Index-only fixture: no funds are created or spent. Real signed spends,
    // including fee accounting and undo, are covered by test_consensus_authorization.
    expect(StateAccess::apply_multisig_effects(state, hierarchical, 24), "hierarchical MULTISIG_SPEND apply path succeeds");
    expect(state.has_hierarchical_spend_key(hierarchical.get_hierarchical_spend_hash()),
           "hierarchical MULTISIG_SPEND key is indexed");
    expect(revert_tx(hierarchical), "hierarchical MULTISIG_SPEND revert path succeeds");
    expect(!state.has_hierarchical_spend_key(hierarchical.get_hierarchical_spend_hash()),
           "hierarchical MULTISIG_SPEND key is removed on revert");

    expect(revert_tx(delegation), "DELEGATION_PROOF revert path succeeds");
    expect(!state.has_delegation_proof_key(delegation.get_delegation_proof_hash()),
           "DELEGATION_PROOF anti-replay key is removed on revert");
}

void test_card_apply_lifecycle_and_validation_guards()
{
    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* storage_ptr = storage.get();
    geocoin::state::ChainState state(std::move(storage));

    const geocoin::bytes issuer_key = private_key_from_seed(0xc0);
    const geocoin::Address issuer = address_from_seed(0xc0);
    const geocoin::Address recipient = address_from_seed(0xc1);
    const geocoin::Height mature_height = geocoin::consensus::params::COINBASE_MATURITY + 5;

    auto owner_output = [&](const geocoin::core::Transaction& tx,
                            const geocoin::Address& address,
                            geocoin::Amount& value_out) {
        for (uint32_t i = 0; i < tx.outputs.size(); ++i) {
            if (tx.outputs[i].address == address) {
                value_out = tx.outputs[i].value;
                return i;
            }
        }
        throw std::runtime_error("expected owner output");
    };

    auto sign_single_input = [&](geocoin::core::Transaction& tx) {
        tx.sign_input(0, issuer_key);
    };

    auto add_spend = [&](geocoin::core::Transaction& tx,
                         const geocoin::core::Transaction& prev_tx,
                         uint32_t vout,
                         geocoin::Amount value) {
        geocoin::core::TxInput input;
        input.prev_tx_hash = prev_tx.get_hash();
        input.vout = vout;
        tx.inputs.push_back(input);
        tx.outputs.push_back({value - geocoin::consensus::params::MIN_TX_FEE, issuer, {}});
    };

    const auto funding = geocoin::core::Transaction::create_coinbase(1, issuer, 100 * geocoin::COIN);
    expect(state.apply_transaction(funding, 1, true), "card funding coinbase applies");
    geocoin::Amount funding_value = 0;
    const uint32_t funding_vout = owner_output(funding, issuer, funding_value);

    const geocoin::Hash256 program_id = hash_from_seed(0xc2);
    const geocoin::Hash256 scope_id = hash_from_seed(0xc3);
    const geocoin::Hash256 uid_hash = hash_from_seed(0xc4);
    const geocoin::Hash256 pubkey_hash = hash_from_seed(0xc5);

    geocoin::core::Transaction missing_program;
    missing_program.version = 2;
    missing_program.type = geocoin::core::TransactionType::CARD_ISSUE;
    add_spend(missing_program, funding, funding_vout, funding_value);
    missing_program.card_program_id = program_id;
    missing_program.card_issuer = issuer;
    missing_program.card_policy_id = 7;
    missing_program.card_scope_id = scope_id;
    missing_program.card_uid_hash = uid_hash;
    missing_program.card_pubkey_hash = pubkey_hash;
    missing_program.card_valid_from = 100;
    missing_program.card_status = 0;
    missing_program.card_metadata_hash = hash_from_seed(0xc6);
    sign_single_input(missing_program);
    expect(!state.validate_transaction(missing_program), "CARD_ISSUE rejects missing program");

    geocoin::core::Transaction missing_card_input = missing_program;
    missing_card_input.inputs[0].prev_tx_hash = hash_from_seed(0xec);
    sign_single_input(missing_card_input);
    expect(!state.validate_transaction(missing_card_input), "CARD_* rejects missing issuer input UTXO");

    const auto other_funding = geocoin::core::Transaction::create_coinbase(2, recipient, 50 * geocoin::COIN);
    expect(state.apply_transaction(other_funding, 2, true), "card non-issuer funding applies");
    geocoin::Amount other_funding_value = 0;
    const uint32_t other_funding_vout = owner_output(other_funding, recipient, other_funding_value);

    geocoin::core::Transaction no_issuer_input = missing_program;
    no_issuer_input.inputs.clear();
    no_issuer_input.outputs.clear();
    add_spend(no_issuer_input, other_funding, other_funding_vout, other_funding_value);
    sign_single_input(no_issuer_input);
    expect(!state.validate_transaction(no_issuer_input), "CARD_* rejects inputs not owned by issuer");

    geocoin::core::Transaction program;
    program.version = 2;
    program.type = geocoin::core::TransactionType::CARD_PROGRAM_DEFINE;
    add_spend(program, funding, funding_vout, funding_value);
    program.card_program_id = program_id;
    program.card_issuer = issuer;
    program.card_chip_vendor = "NXP";
    program.card_chip_os = "JCOP4";
    program.card_applet_aid = "A000000001";
    program.card_applet_version = 1;
    program.card_applet_package_hash = hash_from_seed(0xc7);
    program.card_policy_id = 7;
    program.card_scope_id = scope_id;
    sign_single_input(program);

    geocoin::core::Transaction forged_program = program;
    forged_program.card_program_id = hash_from_seed(0xce);
    forged_program.card_applet_package_hash = hash_from_seed(0xcf);
    forged_program.sign_input(0, private_key_from_seed(0xc1));
    expect(!state.validate_transaction(forged_program),
           "CARD_PROGRAM_DEFINE rejects a witness not owned by the issuer UTXO");

    expect(state.validate_transaction(program), "CARD_PROGRAM_DEFINE validates");
    expect(state.apply_transaction(program, mature_height, false), "CARD_PROGRAM_DEFINE applies");
    expect(storage_ptr->has_metadata("card:prog:" + hash_to_hex(program_id)),
           "CARD_PROGRAM_DEFINE persists program metadata");
    expect(!state.validate_transaction(program), "CARD_PROGRAM_DEFINE duplicate is rejected");

    geocoin::core::Transaction wrong_program_issuer = missing_program;
    wrong_program_issuer.inputs.clear();
    wrong_program_issuer.outputs.clear();
    add_spend(wrong_program_issuer, other_funding, other_funding_vout, other_funding_value);
    wrong_program_issuer.card_issuer = recipient;
    sign_single_input(wrong_program_issuer);
    expect(!state.validate_transaction(wrong_program_issuer), "CARD_* rejects issuer mismatch with program");

    const auto second_program_funding =
        geocoin::core::Transaction::create_coinbase(3, issuer, 50 * geocoin::COIN);
    expect(state.apply_transaction(second_program_funding, 3, true), "card second program funding applies");
    geocoin::Amount second_program_funding_value = 0;
    const uint32_t second_program_funding_vout =
        owner_output(second_program_funding, issuer, second_program_funding_value);
    const geocoin::Hash256 second_program_id = hash_from_seed(0xcc);
    geocoin::core::Transaction second_program = program;
    second_program.inputs.clear();
    second_program.outputs.clear();
    add_spend(second_program, second_program_funding, second_program_funding_vout, second_program_funding_value);
    second_program.card_program_id = second_program_id;
    second_program.card_applet_package_hash = hash_from_seed(0xcd);
    sign_single_input(second_program);
    expect(state.validate_transaction(second_program), "second CARD_PROGRAM_DEFINE validates");
    expect(state.apply_transaction(second_program, mature_height, false), "second CARD_PROGRAM_DEFINE applies");

    geocoin::Amount program_change = 0;
    const uint32_t program_change_vout = owner_output(program, issuer, program_change);

    geocoin::core::Transaction issue = missing_program;
    issue.inputs.clear();
    issue.outputs.clear();
    add_spend(issue, program, program_change_vout, program_change);
    sign_single_input(issue);
    expect(state.validate_transaction(issue), "CARD_ISSUE validates against stored program");

    geocoin::core::Transaction issue_bad_scope = issue;
    issue_bad_scope.card_scope_id = hash_from_seed(0xc8);
    sign_single_input(issue_bad_scope);
    expect(!state.validate_transaction(issue_bad_scope), "CARD_ISSUE rejects policy/scope mismatch");

    expect(state.apply_transaction(issue, mature_height + 1, false), "CARD_ISSUE applies");
    expect(storage_ptr->has_metadata("card:uid:" + hash_to_hex(uid_hash)),
           "CARD_ISSUE persists card metadata");
    expect(!state.validate_transaction(issue), "CARD_ISSUE duplicate uid is rejected");

    geocoin::core::Transaction duplicate_pubkey = issue;
    duplicate_pubkey.inputs.clear();
    duplicate_pubkey.outputs.clear();
    geocoin::Amount issue_change = 0;
    const uint32_t issue_change_vout = owner_output(issue, issuer, issue_change);
    add_spend(duplicate_pubkey, issue, issue_change_vout, issue_change);
    duplicate_pubkey.card_uid_hash = hash_from_seed(0xc9);
    sign_single_input(duplicate_pubkey);
    expect(!state.validate_transaction(duplicate_pubkey), "CARD_ISSUE duplicate pubkey is rejected");

    geocoin::core::Transaction update;
    update.version = 2;
    update.type = geocoin::core::TransactionType::CARD_UPDATE_STATUS;
    add_spend(update, issue, issue_change_vout, issue_change);
    update.card_program_id = program_id;
    update.card_issuer = issuer;
    update.card_uid_hash = uid_hash;
    update.card_status = 1;
    update.card_metadata_hash = hash_from_seed(0xca);
    sign_single_input(update);
    expect(state.validate_transaction(update), "CARD_UPDATE_STATUS validates");

    geocoin::core::Transaction update_missing_card = update;
    update_missing_card.card_uid_hash = hash_from_seed(0xce);
    sign_single_input(update_missing_card);
    expect(!state.validate_transaction(update_missing_card), "CARD_UPDATE_STATUS rejects missing card uid");

    geocoin::core::Transaction update_wrong_program = update;
    update_wrong_program.card_program_id = second_program_id;
    sign_single_input(update_wrong_program);
    expect(!state.validate_transaction(update_wrong_program), "CARD_UPDATE_STATUS rejects program mismatch");

    geocoin::core::Transaction update_noop = update;
    update_noop.card_status = 0;
    sign_single_input(update_noop);
    expect(!state.validate_transaction(update_noop), "CARD_UPDATE_STATUS rejects no-op transition");

    geocoin::core::Transaction update_bad_active_transition = update;
    update_bad_active_transition.card_status = 3;
    sign_single_input(update_bad_active_transition);
    expect(!state.validate_transaction(update_bad_active_transition),
           "CARD_UPDATE_STATUS rejects invalid active-card transition");

    geocoin::core::Transaction update_wrong_pubkey = update;
    update_wrong_pubkey.card_pubkey_hash = hash_from_seed(0xcb);
    geocoin::test::expect_throws([&] { sign_single_input(update_wrong_pubkey); },
                               "CARD_UPDATE_STATUS refuses to sign an inactive pubkey field");
    expect(!state.validate_transaction(update_wrong_pubkey), "CARD_UPDATE_STATUS rejects an inactive pubkey field");

    expect(state.apply_transaction(update, mature_height + 2, false), "CARD_UPDATE_STATUS applies");
    expect(storage_ptr->has_metadata("card:uid:" + hash_to_hex(uid_hash)),
           "CARD_UPDATE_STATUS keeps card metadata");

    geocoin::Amount update_change = 0;
    const uint32_t update_change_vout = owner_output(update, issuer, update_change);
    geocoin::core::Transaction update_reactivate;
    update_reactivate.version = 2;
    update_reactivate.type = geocoin::core::TransactionType::CARD_UPDATE_STATUS;
    add_spend(update_reactivate, update, update_change_vout, update_change);
    update_reactivate.card_program_id = program_id;
    update_reactivate.card_issuer = issuer;
    update_reactivate.card_uid_hash = uid_hash;
    update_reactivate.card_status = 0;
    sign_single_input(update_reactivate);
    expect(state.validate_transaction(update_reactivate), "CARD_UPDATE_STATUS reactivation validates from suspended");

    geocoin::core::Transaction update_bad_suspended_transition = update_reactivate;
    update_bad_suspended_transition.card_status = 3;
    sign_single_input(update_bad_suspended_transition);
    expect(!state.validate_transaction(update_bad_suspended_transition),
           "CARD_UPDATE_STATUS rejects invalid suspended-card transition");

    geocoin::core::Transaction update_revoke = update_reactivate;
    update_revoke.card_status = 2;
    sign_single_input(update_revoke);
    expect(state.validate_transaction(update_revoke), "CARD_UPDATE_STATUS revoke validates from suspended");
    expect(state.apply_transaction(update_revoke, mature_height + 3, false), "CARD_UPDATE_STATUS revoke applies");

    geocoin::Amount revoke_change = 0;
    const uint32_t revoke_change_vout = owner_output(update_revoke, issuer, revoke_change);
    geocoin::core::Transaction update_revoked_card = update_reactivate;
    update_revoked_card.inputs.clear();
    update_revoked_card.outputs.clear();
    add_spend(update_revoked_card, update_revoke, revoke_change_vout, revoke_change);
    update_revoked_card.card_status = 0;
    sign_single_input(update_revoked_card);
    expect(!state.validate_transaction(update_revoked_card),
           "CARD_UPDATE_STATUS rejects changes to revoked card");
}

void test_card_corporate_policy_authorities_and_recovery()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::Height spend_height = geocoin::consensus::params::COINBASE_MATURITY + 10;

    const geocoin::bytes key_a = private_key_from_seed(0xd1);
    const geocoin::bytes key_b = private_key_from_seed(0xd2);
    const geocoin::bytes key_c = private_key_from_seed(0xd3);
    const geocoin::Address address_a = address_from_seed(0xd1);
    const geocoin::Address address_b = address_from_seed(0xd2);
    const geocoin::Address address_c = address_from_seed(0xd3);

    auto fund = [&](geocoin::Height height, const geocoin::Address& address) {
        const auto coinbase = geocoin::core::Transaction::create_coinbase(height, address, 10 * geocoin::COIN);
        expect(state.apply_transaction(coinbase, height, true), "corporate card funding applies");
        return coinbase;
    };

    auto add_input = [](geocoin::core::Transaction& tx,
                        const geocoin::core::Transaction& funding,
                        const geocoin::Address& address) {
        for (uint32_t vout = 0; vout < funding.outputs.size(); ++vout) {
            if (funding.outputs[vout].address == address) {
                tx.inputs.push_back({funding.get_hash(), vout, {}, 0xffffffff, {}, {}});
                tx.outputs.push_back({
                    funding.outputs[vout].value - geocoin::consensus::params::MIN_TX_FEE,
                    address,
                    {}
                });
                return;
            }
        }
        throw std::runtime_error("corporate card funding output is missing");
    };

    auto sign_inputs = [](geocoin::core::Transaction& tx, const std::vector<geocoin::bytes>& keys) {
        for (size_t index = 0; index < keys.size(); ++index) {
            tx.sign_input(index, keys[index]);
        }
    };

    auto make_policy = [&](uint8_t mint_quorum, std::vector<geocoin::Address> mint_members,
                           uint8_t freeze_quorum, std::vector<geocoin::Address> freeze_members,
                           uint8_t operations_quorum, std::vector<geocoin::Address> operations_members,
                           uint8_t recovery_quorum, std::vector<geocoin::Address> recovery_members) {
        geocoin::core::CardCorporatePolicy policy;
        policy.version = 1;
        auto sort_members = [](std::vector<geocoin::Address>& members) {
            std::sort(members.begin(), members.end(), [](const auto& left, const auto& right) {
                return left.str() < right.str();
            });
        };
        sort_members(mint_members);
        sort_members(freeze_members);
        sort_members(operations_members);
        sort_members(recovery_members);
        policy.mint = {mint_quorum, std::move(mint_members)};
        policy.freeze = {freeze_quorum, std::move(freeze_members)};
        policy.operations = {operations_quorum, std::move(operations_members)};
        policy.recovery = {recovery_quorum, std::move(recovery_members)};
        policy.compliance_policy_hash = hash_from_seed(0xd4);
        policy.max_active_cards = 1;
        return policy;
    };

    const auto initial_policy = make_policy(
        2, {address_a, address_b},
        2, {address_b, address_c},
        1, {address_a},
        2, {address_a, address_b}
    );
    const geocoin::Hash256 program_id = hash_from_seed(0xd5);
    const geocoin::Hash256 scope_id = hash_from_seed(0xd6);

    const auto recovery_fund_a = fund(1, address_a);
    const auto recovery_fund_b = fund(2, address_b);
    geocoin::core::Transaction program;
    program.version = 2;
    program.type = geocoin::core::TransactionType::CARD_PROGRAM_DEFINE;
    add_input(program, recovery_fund_a, address_a);
    add_input(program, recovery_fund_b, address_b);
    program.card_program_id = program_id;
    program.card_issuer = address_a;
    program.card_chip_vendor = "NXP";
    program.card_chip_os = "JCOP4";
    program.card_applet_aid = "A000000001";
    program.card_applet_version = 1;
    program.card_applet_package_hash = hash_from_seed(0xd7);
    program.card_policy_id = 101;
    program.card_scope_id = scope_id;
    program.card_corporate_policy = initial_policy;
    sign_inputs(program, {key_a, key_b});
    expect(program.verify(), "corporate card program transaction signatures verify");
    expect(state.validate_transaction(program), "recovery quorum defines corporate card program");
    expect(state.apply_transaction(program, spend_height, false), "corporate card program applies");

    const auto mint_fund_a = fund(3, address_a);
    const auto mint_fund_b = fund(4, address_b);
    geocoin::core::Transaction issue;
    issue.version = 2;
    issue.type = geocoin::core::TransactionType::CARD_ISSUE;
    add_input(issue, mint_fund_a, address_a);
    add_input(issue, mint_fund_b, address_b);
    issue.card_program_id = program_id;
    issue.card_issuer = address_a;
    issue.card_policy_id = 101;
    issue.card_scope_id = scope_id;
    issue.card_uid_hash = hash_from_seed(0xd8);
    issue.card_pubkey_hash = hash_from_seed(0xd9);
    issue.card_valid_from = 100;
    issue.card_metadata_hash = hash_from_seed(0xda);
    sign_inputs(issue, {key_a, key_b});
    expect(state.validate_transaction(issue), "mint quorum issues card");
    expect(state.apply_transaction(issue, spend_height + 1, false), "corporate card issue applies");

    const auto excess_fund_a = fund(5, address_a);
    const auto excess_fund_b = fund(6, address_b);
    geocoin::core::Transaction excess_issue = issue;
    excess_issue.inputs.clear();
    excess_issue.outputs.clear();
    excess_issue.witnesses.clear();
    add_input(excess_issue, excess_fund_a, address_a);
    add_input(excess_issue, excess_fund_b, address_b);
    excess_issue.card_uid_hash = hash_from_seed(0xdb);
    excess_issue.card_pubkey_hash = hash_from_seed(0xdc);
    sign_inputs(excess_issue, {key_a, key_b});
    expect(!state.validate_transaction(excess_issue), "max active card policy limit is enforced");

    const auto freeze_fund_b = fund(7, address_b);
    const auto freeze_fund_c = fund(8, address_c);
    geocoin::core::Transaction suspend;
    suspend.version = 2;
    suspend.type = geocoin::core::TransactionType::CARD_UPDATE_STATUS;
    add_input(suspend, freeze_fund_b, address_b);
    add_input(suspend, freeze_fund_c, address_c);
    suspend.card_program_id = program_id;
    suspend.card_issuer = address_a;
    suspend.card_uid_hash = issue.card_uid_hash;
    suspend.card_status = 1;
    sign_inputs(suspend, {key_b, key_c});
    expect(state.validate_transaction(suspend), "freeze quorum suspends card");
    expect(state.apply_transaction(suspend, spend_height + 2, false), "corporate card suspension applies");

    const auto rotate_fund_a = fund(9, address_a);
    const auto rotate_fund_b = fund(10, address_b);
    geocoin::core::Transaction rotate;
    rotate.version = 2;
    rotate.type = geocoin::core::TransactionType::CARD_UPDATE_POLICY;
    add_input(rotate, rotate_fund_a, address_a);
    add_input(rotate, rotate_fund_b, address_b);
    rotate.card_program_id = program_id;
    rotate.card_issuer = address_a;
    rotate.card_policy_id = 102;
    rotate.card_scope_id = scope_id;
    rotate.card_corporate_policy = make_policy(
        1, {address_c},
        1, {address_c},
        1, {address_c},
        1, {address_c}
    );
    sign_inputs(rotate, {key_a, key_b});
    expect(state.validate_transaction(rotate), "recovery quorum rotates corporate policy");
    expect(state.apply_transaction(rotate, spend_height + 3, false), "corporate policy rotation applies");

    geocoin::state::TxUndo policy_undo;
    policy_undo.card_undo.kind = 4;
    policy_undo.card_undo.program_id = program_id;
    policy_undo.card_undo.prev_program.corporate_policy = initial_policy;
    const auto decoded_policy_undo = geocoin::state::TxUndo::deserialize(policy_undo.serialize());
    expect(decoded_policy_undo.card_undo.kind == 4, "policy update undo kind roundtrips");
    expect(decoded_policy_undo.card_undo.prev_program.corporate_policy.version == 1,
           "policy update undo corporate policy roundtrips");

    const auto operations_fund_c = fund(11, address_c);
    geocoin::core::Transaction reactivate = suspend;
    reactivate.inputs.clear();
    reactivate.outputs.clear();
    reactivate.witnesses.clear();
    add_input(reactivate, operations_fund_c, address_c);
    reactivate.card_status = 0;
    sign_inputs(reactivate, {key_c});
    expect(state.validate_transaction(reactivate), "operations authority reactivates card after recovery rotation");
}

void test_state_accessor_fee_and_undo_paths()
{
    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* storage_ptr = storage.get();
    geocoin::state::ChainState state(std::move(storage));
    const geocoin::Address owner("GEOaccess-owner");
    const geocoin::Address recipient("GEOaccess-recipient");
    auto owner_utxo_from_tx = [&](const geocoin::core::Transaction& tx, geocoin::Height height) {
        geocoin::state::UTXO out{};
        bool found = false;
        for (uint32_t i = 0; i < tx.outputs.size(); ++i) {
            if (tx.outputs[i].address == owner) {
                out.tx_hash = tx.get_hash();
                out.vout = i;
                out.value = tx.outputs[i].value;
                out.address = owner;
                out.height = height;
                out.is_coinbase = true;
                found = true;
                break;
            }
        }
        expect(found, "coinbase contains owner output");
        return out;
    };

    const auto funding = geocoin::core::Transaction::create_coinbase(5, owner, 1000);
    expect(state.apply_transaction(funding, 5, true), "coinbase funding applies");
    const auto input_utxo = owner_utxo_from_tx(funding, 5);
    expect(storage_ptr->has_metadata(input_utxo.get_key()), "add_utxo persists UTXO metadata");
    expect(state.get_balance(owner) == input_utxo.value, "get_balance sees added UTXO");
    expect(state.get_total_supply() >= input_utxo.value, "get_total_supply includes UTXO set");
    expect(state.get_utxos_for_address(owner).size() == 1, "get_utxos_for_address returns added UTXO");
    expect(state.has_utxo(input_utxo.tx_hash, input_utxo.vout), "has_utxo finds added UTXO");
    geocoin::state::UTXO copied_utxo;
    expect(state.get_utxo(input_utxo.tx_hash, input_utxo.vout, copied_utxo), "copy get_utxo finds added UTXO");
    expect(copied_utxo.value == input_utxo.value, "copy get_utxo preserves value");

    geocoin::core::Transaction stored_tx;
    stored_tx.timestamp = 11;
    stored_tx.outputs.push_back({1, owner, {}});
    const auto stored_txid = stored_tx.get_hash();
    expect(storage_ptr->put_transaction(stored_txid, stored_tx), "storage stores fallback transaction");
    const auto loaded_tx = state.get_transaction(stored_txid);
    expect(loaded_tx != nullptr && loaded_tx->get_hash() == stored_txid,
           "get_transaction falls back to storage when active index is empty");
    expect(state.get_transaction(hash_from_seed(0x81)) == nullptr,
           "get_transaction returns null for missing storage transaction");

    geocoin::core::Transaction stored_checkpoint;
    stored_checkpoint.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    stored_checkpoint.timestamp = 12;
    const auto stored_checkpoint_txid = stored_checkpoint.get_hash();
    expect(storage_ptr->put_transaction(stored_checkpoint_txid, stored_checkpoint),
           "storage stores fraud-proof checkpoint transaction");
    const auto fraud_checkpoint = state.get_transaction_for_fraud_proof(stored_checkpoint_txid);
    expect(fraud_checkpoint != nullptr && fraud_checkpoint->get_hash() == stored_checkpoint_txid,
           "fraud-proof lookup reads a stored checkpoint transaction");
    expect(state.get_transaction_for_fraud_proof(stored_txid) == nullptr,
           "fraud-proof lookup rejects non-checkpoint transactions");

    geocoin::core::Transaction coinbase =
        geocoin::core::Transaction::create_coinbase(10, owner, 50 * geocoin::COIN);
    expect(state.calculate_transaction_fee(coinbase) == 0, "coinbase fee is zero");

    geocoin::core::Transaction standard;
    geocoin::core::TxInput standard_input;
    standard_input.prev_tx_hash = input_utxo.tx_hash;
    standard_input.vout = input_utxo.vout;
    standard.inputs.push_back(standard_input);
    standard.outputs.push_back({input_utxo.value - 100, recipient, {}});
    expect(state.calculate_transaction_fee(standard) == 100, "standard fee is input minus output");

    geocoin::core::Transaction missing_input = standard;
    missing_input.inputs[0].prev_tx_hash = hash_from_seed(0x82);
    expect(state.calculate_transaction_fee(missing_input) == 0, "missing input fee returns zero");

    geocoin::core::Transaction over_output = standard;
    over_output.outputs[0].value = input_utxo.value + 1;
    expect(state.calculate_transaction_fee(over_output) == 0, "over-output transaction fee returns zero");

    const auto lock_funding = geocoin::core::Transaction::create_coinbase(6, owner, 1000);
    expect(state.apply_transaction(lock_funding, 6, true), "lock funding applies");
    const auto lock_utxo = owner_utxo_from_tx(lock_funding, 6);
    geocoin::core::Transaction lock_deposit;
    lock_deposit.type = geocoin::core::TransactionType::LOCK_DEPOSIT;
    geocoin::core::TxInput lock_input;
    lock_input.prev_tx_hash = lock_utxo.tx_hash;
    lock_input.vout = lock_utxo.vout;
    lock_deposit.inputs.push_back(lock_input);
    lock_deposit.outputs.push_back({lock_utxo.value / 2, owner, {}});
    lock_deposit.sidechain_amount = lock_utxo.value / 4;
    const geocoin::Amount expected_lock_fee = lock_utxo.value - lock_deposit.outputs[0].value - lock_deposit.sidechain_amount;
    expect(state.calculate_transaction_fee(lock_deposit) == expected_lock_fee,
           "LOCK_DEPOSIT fee excludes locked sidechain amount");
    lock_deposit.sidechain_amount = lock_utxo.value;
    expect(state.calculate_transaction_fee(lock_deposit) == 0,
           "LOCK_DEPOSIT fee returns zero when output plus locked amount consumes input");

    geocoin::core::Transaction checkpoint;
    checkpoint.type = geocoin::core::TransactionType::SIDECHAIN_CHECKPOINT;
    checkpoint.sidechain_amount = 77;
    expect(state.calculate_transaction_fee(checkpoint) == 77, "checkpoint fee uses sidechain amount");

    geocoin::core::Transaction batch;
    batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    geocoin::core::BatchCheckpointItem item_a;
    item_a.fee = 3;
    geocoin::core::BatchCheckpointItem item_b;
    item_b.fee = 4;
    batch.sidechain_batch_checkpoints = {item_a, item_b};
    expect(state.calculate_transaction_fee(batch) == 7, "batch checkpoint fee sums item fees");

    geocoin::core::Transaction mint_missing_registry;
    mint_missing_registry.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
    mint_missing_registry.sidechain_id = hash_from_seed(0x84);
    mint_missing_registry.sidechain_amount = 100;
    expect(state.calculate_transaction_fee(mint_missing_registry) == 0,
           "mint fee is zero when sidechain registry is missing");

    geocoin::core::Transaction interest;
    interest.type = geocoin::core::TransactionType::INTEREST_DISTRIBUTION;
    expect(state.calculate_transaction_fee(interest) == 0, "interest distribution fee is zero");

    geocoin::state::BlockUndo cached_undo;
    cached_undo.height = 9;
    cached_undo.block_hash = hash_from_seed(0x85);
    expect(state.save_block_undo(cached_undo), "save_block_undo stores undo");
    geocoin::state::BlockUndo loaded_undo;
    expect(state.load_block_undo(cached_undo.block_hash, loaded_undo), "load_block_undo reads cached undo");
    expect(loaded_undo.height == cached_undo.height, "cached undo height roundtrips");

    geocoin::state::BlockUndo stored_undo;
    stored_undo.height = 10;
    stored_undo.block_hash = hash_from_seed(0x86);
    expect(storage_ptr->put_metadata("undo:" + hash_to_hex(stored_undo.block_hash), stored_undo.serialize()),
           "test stores serialized undo directly");
    expect(state.load_block_undo(stored_undo.block_hash, loaded_undo), "load_block_undo reads storage undo");
    expect(loaded_undo.height == stored_undo.height, "storage undo height roundtrips");

    const auto corrupt_hash = hash_from_seed(0x87);
    expect(storage_ptr->put_metadata("undo:" + hash_to_hex(corrupt_hash), {1, 2, 3}),
           "test stores corrupt undo directly");
    expect(!state.load_block_undo(corrupt_hash, loaded_undo), "load_block_undo rejects corrupt storage undo");
    expect(!state.load_block_undo(hash_from_seed(0x88), loaded_undo), "load_block_undo reports missing undo");

    state.clear();
    expect(state.get_total_supply() == 0, "clear removes UTXO supply");
    expect(state.get_witness_pubkey_stats().total_keys == 0, "clear resets witness stats");
}

void test_utxo_delete_failure_is_fail_closed()
{
    auto storage = std::make_unique<FailingDeleteStorage>();
    auto* storage_ptr = storage.get();
    geocoin::state::ChainState state(std::move(storage));

    geocoin::state::UTXO utxo;
    utxo.tx_hash = hash_from_seed(0xd1);
    utxo.vout = 7;
    utxo.value = 100;
    utxo.address = geocoin::Address("GEOdelete-fail-closed");
    utxo.height = 1;
    geocoin::state::ChainStateTestAccess::add_utxo(state, utxo);

    storage_ptr->fail_deletes = true;
    expect(!geocoin::state::ChainStateTestAccess::remove_utxo(state, utxo.tx_hash, utxo.vout),
           "remove_utxo reports persistent deletion failure");
    expect(state.has_utxo(utxo.tx_hash, utxo.vout),
           "failed persistent deletion leaves the in-memory UTXO intact");

    storage_ptr->fail_deletes = false;
    expect(geocoin::state::ChainStateTestAccess::remove_utxo(state, utxo.tx_hash, utxo.vout),
           "remove_utxo succeeds after storage recovery");
    expect(!state.has_utxo(utxo.tx_hash, utxo.vout),
           "successful persistent deletion removes the in-memory UTXO");
}

void test_persistence_loads_sidechain_and_interest_metadata()
{
    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* storage_ptr = storage.get();
    expect(storage_ptr->put_metadata("chainstate_height", encode_u64_le(457)), "projection height includes checkpoint");
    expect(storage_ptr->put_metadata("chainstate_utxo_count", encode_u64_le(0)), "empty projection UTXO count");
    expect(storage_ptr->put_metadata("idx_epoch", geocoin::bytes(32, '1')), "projection index epoch");

    const geocoin::Hash256 deposit_id = hash_from_seed(0xa1);
    const geocoin::Address owner = address_from_seed(0xa2);
    geocoin::state::SidechainDeposit deposit;
    deposit.owner = owner;
    deposit.amount = 123 * geocoin::COIN;
    deposit.unlock_height = 456;
    deposit.checkpoint = hash_from_seed(0xa3);
    deposit.last_checkpoint_height = 457;
    deposit.last_sidechain_checkpoint_height = 458;
    expect(storage_ptr->put_metadata(
               "scd:" + hash_to_hex(deposit_id),
               serialize_test_sidechain_deposit(deposit)
           ),
           "test stores sidechain deposit metadata");

    const geocoin::Hash256 bad_deposit_id = hash_from_seed(0xa4);
    expect(storage_ptr->put_metadata("scd:" + hash_to_hex(bad_deposit_id), {1, 2, 3}),
           "test stores corrupt sidechain deposit metadata");
    expect(storage_ptr->put_metadata("scd:not-a-valid-id", serialize_test_sidechain_deposit(deposit)),
           "test stores ignored sidechain deposit key");

    const geocoin::Hash256 registry_id = deposit_id;
    geocoin::state::SidechainRegistryEntry registry;
    registry.owner = owner;
    registry.issuance_limit = 1000 * geocoin::COIN;
    registry.total_issued = 250 * geocoin::COIN;
    registry.multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 3;
    registry.fee_bps = 25;
    registry.max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 9;
    registry.token_symbol = "PST";
    registry.committee_m = 1;
    registry.committee_n = 2;
    registry.committee_members = {owner, address_from_seed(0xb2)};
    std::sort(registry.committee_members.begin(), registry.committee_members.end(),
              [](const auto& a, const auto& b) { return a.str() < b.str(); });
    registry.committee_id = geocoin::consensus::compute_sidechain_committee_id(
        registry.committee_m,
        registry.committee_n,
        registry.committee_members
    );
    registry.checkpoint_policy_id = 2;
    registry.checkpoint_version = 2;
    expect(storage_ptr->put_metadata(
               "sc:reg:" + hash_to_hex(registry_id),
               serialize_test_sidechain_registry(registry)
           ),
           "test stores sidechain registry metadata");

    const geocoin::Hash256 bad_registry_id = hash_from_seed(0xb3);
    expect(storage_ptr->put_metadata("sc:reg:" + hash_to_hex(bad_registry_id), {9}),
           "test stores corrupt sidechain registry metadata");
    expect(storage_ptr->put_metadata("sc:reg:" + std::string(64, 'z'), serialize_test_sidechain_registry(registry)),
           "test stores ignored non-hex registry key");

    const geocoin::Amount interest_balance = 77 * geocoin::COIN;
    expect(storage_ptr->put_metadata("interest_pool_balance", encode_u64_le(interest_balance)),
           "test stores interest pool metadata");
    expect(storage_ptr->put_metadata("foundation_cfg", {1, 2, 3}),
           "test stores ignored corrupt foundation config");

    geocoin::state::ChainState state(std::move(storage));

    geocoin::state::SidechainDeposit loaded_deposit;
    expect(state.has_sidechain_deposit_key(deposit_id), "loaded deposit key is indexed");
    expect(state.get_sidechain_deposit(deposit_id, loaded_deposit), "loaded deposit is readable");
    expect(loaded_deposit.owner == deposit.owner, "loaded deposit owner matches");
    expect(loaded_deposit.amount == deposit.amount, "loaded deposit amount matches");
    expect(loaded_deposit.unlock_height == deposit.unlock_height, "loaded deposit unlock matches");
    expect(loaded_deposit.checkpoint == deposit.checkpoint, "loaded deposit checkpoint matches");
    expect(loaded_deposit.last_checkpoint_height == deposit.last_checkpoint_height,
           "loaded deposit checkpoint height matches");
    expect(loaded_deposit.last_sidechain_checkpoint_height == deposit.last_sidechain_checkpoint_height,
           "loaded deposit sidechain checkpoint height matches");
    bool deposit_corrupt = false;
    try { state.get_sidechain_deposit(bad_deposit_id, loaded_deposit); }
    catch (const geocoin::storage::ReadError& error) { deposit_corrupt = error.status() == geocoin::storage::ReadStatus::CORRUPT; }
    expect(deposit_corrupt, "corrupt deposit metadata reports a read error");

    geocoin::state::SidechainRegistryEntry loaded_registry;
    expect(state.has_sidechain_registry_key(registry_id), "loaded registry key is indexed");
    expect(state.get_sidechain_registry(registry_id, loaded_registry), "loaded registry is readable");
    expect(loaded_registry.owner == registry.owner, "loaded registry owner matches");
    expect(loaded_registry.issuance_limit == registry.issuance_limit, "loaded registry limit matches");
    expect(loaded_registry.total_issued == registry.total_issued, "loaded registry issued matches");
    expect(loaded_registry.multiplier == registry.multiplier, "loaded registry multiplier matches");
    expect(loaded_registry.fee_bps == registry.fee_bps, "loaded registry fee matches");
    expect(loaded_registry.max_multiplier == registry.max_multiplier, "loaded registry max multiplier matches");
    expect(loaded_registry.token_symbol == registry.token_symbol, "loaded registry symbol matches");
    expect(loaded_registry.committee_members == registry.committee_members,
           "loaded registry committee members match");
    expect(loaded_registry.committee_id == registry.committee_id, "loaded registry committee id matches");
    expect(loaded_registry.checkpoint_policy_id == registry.checkpoint_policy_id,
           "loaded registry checkpoint policy matches");
    expect(loaded_registry.checkpoint_version == registry.checkpoint_version,
           "loaded registry checkpoint version matches");
    expect(state.get_sidechain_registry_by_id_hex(hash_to_hex(registry_id), loaded_registry),
           "registry is readable by id hex");
    expect(!state.get_sidechain_registry_by_id_hex("bad", loaded_registry),
           "registry lookup rejects invalid hex");
    bool registry_corrupt = false;
    try { state.get_sidechain_registry(bad_registry_id, loaded_registry); }
    catch (const geocoin::storage::ReadError& error) { registry_corrupt = error.status() == geocoin::storage::ReadStatus::CORRUPT; }
    expect(registry_corrupt, "corrupt registry metadata reports a read error");

    expect(state.get_interest_pool_balance() == interest_balance, "interest pool balance loads from storage");
    expect(!state.get_foundation_address().str().empty(), "corrupt foundation config falls back to defaults");
}

void test_journal_recovery_and_query_indexes()
{
    {
        auto storage = std::make_unique<geocoin::state::MemoryStorage>();
        auto* storage_ptr = storage.get();

        geocoin::bytes journal;
        journal.push_back(1);
        journal.push_back(2);
        append_u64_le(journal, 77);
        append_hash(journal, hash_from_seed(0x84));
        expect(storage_ptr->put_metadata("chainstate_journal", journal), "seed valid journal");
        expect(storage_ptr->put_metadata("chainstate_height", encode_u64_le(77)), "seed chainstate height");
        expect(storage_ptr->put_metadata("chainstate_tip_hash", geocoin::bytes(32, 1)), "seed tip hash");
        expect(storage_ptr->put_metadata("height", encode_u64_le(77)), "seed legacy height");
        expect(storage_ptr->put_metadata("foundation_cfg", {1, 2, 3}), "seed foundation cfg");
        expect(storage_ptr->put_metadata("interest_pool_balance", encode_u64_le(123)), "seed interest pool");
        expect(storage_ptr->put_metadata("nft:" + hash_to_hex(hash_from_seed(0x85)), {1}), "seed nft metadata");
        expect(storage_ptr->put_metadata("scd:" + hash_to_hex(hash_from_seed(0x86)), {1}), "seed sidechain deposit");
        expect(storage_ptr->put_metadata("sc:reg:" + hash_to_hex(hash_from_seed(0x87)), {1}), "seed sidechain registry");
        expect(storage_ptr->put_metadata("card:prog:" + hash_to_hex(hash_from_seed(0x88)), {1}), "seed card program");
        expect(storage_ptr->put_metadata("card:uid:" + hash_to_hex(hash_from_seed(0x89)), {1}), "seed card uid");
        expect(storage_ptr->put_metadata("atx:" + hash_to_hex(hash_from_seed(0x8a)), encode_u64_le(9)), "seed atx");
        expect(storage_ptr->put_metadata("wpk:" + hash_to_hex(hash_from_seed(0x8b)), {1}), "seed witness key");
        expect(storage_ptr->put_metadata("wpkref:" + hash_to_hex(hash_from_seed(0x8b)), encode_u64_le(1)), "seed witness ref");
        expect(storage_ptr->put_metadata("wpk_stats", geocoin::bytes(24, 1)), "seed witness stats");
        geocoin::state::UTXO stale_utxo;
        stale_utxo.tx_hash = hash_from_seed(0x8d);
        stale_utxo.vout = 3;
        stale_utxo.value = 1234;
        stale_utxo.address = address_from_seed(0x8e);
        stale_utxo.height = 77;
        expect(storage_ptr->put_metadata(stale_utxo.get_key(), stale_utxo.serialize()),
               "seed stale UTXO from interrupted snapshot");
        const std::string stale_undo_key = "undo:" + hash_to_hex(hash_from_seed(0x8f));
        expect(storage_ptr->put_metadata(stale_undo_key, {1, 2, 3}),
               "seed stale undo metadata");
        const std::string stale_epoch_index =
            "idx:stale-epoch:ac:" + hash_to_hex(hash_from_seed(0x90));
        expect(storage_ptr->put_metadata(stale_epoch_index, {1}),
               "seed stale epoch index metadata");
        expect(storage_ptr->put_metadata("custom:survives", {9}), "seed unrelated metadata");

        geocoin::state::ChainState recovered(std::move(storage));
        expect(storage_ptr->has_metadata("chainstate_journal"), "opening retains recovery journal");
        expect(storage_ptr->has_metadata(stale_utxo.get_key()), "opening retains UTXO snapshot");
        expect(!recovered.has_derived_state_snapshot(), "pending recovery cannot serve as valid state");
        expect(recovered.prepare_rebuild(), "explicit rebuild begins after caller verifies history");
        expect(recovered.finish_rebuild(), "explicit empty test rebuild finalizes");
        expect(recovered.get_height() == 0, "explicit rebuild resets state height");
        expect(!storage_ptr->has_metadata("chainstate_journal"), "journal is cleared after recovery");
        expect(!storage_ptr->has_metadata("chainstate_height"), "recovery purges chainstate height");
        expect(!storage_ptr->has_metadata("chainstate_tip_hash"), "recovery purges tip hash");
        expect(!storage_ptr->has_metadata("height"), "recovery purges legacy height");
        expect(!storage_ptr->has_metadata("foundation_cfg"), "recovery purges foundation config");
        expect(!storage_ptr->has_metadata("interest_pool_balance"), "recovery purges interest pool");
        expect(!storage_ptr->has_metadata("wpk_stats"), "recovery purges witness stats");
        expect(!storage_ptr->has_metadata(stale_utxo.get_key()),
               "recovery purges stale persistent UTXO entries");
        expect(!recovered.has_utxo(stale_utxo.tx_hash, stale_utxo.vout),
               "recovery cannot reload a stale UTXO into memory");
        expect(!storage_ptr->has_metadata(stale_undo_key),
               "recovery purges stale undo metadata");
        expect(!storage_ptr->has_metadata(stale_epoch_index),
               "recovery purges stale epoch index metadata");
        expect(storage_ptr->has_metadata("custom:survives"), "recovery preserves unrelated metadata");
        expect(storage_ptr->has_metadata("idx_epoch"), "recovery creates a fresh index epoch");
    }

    {
        TempDirectoryGuard dir(make_temp_dir("geocoin-chainstate-split-pending"));
        auto storage = geocoin::state::StorageFactory::create_split(
            geocoin::state::StorageFactory::Type::ROCKSDB,
            (dir.path() / "chainstate").string(),
            (dir.path() / "addresses").string()
        );
        auto* storage_ptr = storage.get();
        expect(storage_ptr != nullptr, "split storage opens for pending-marker recovery");

        geocoin::state::UTXO stale_utxo;
        stale_utxo.tx_hash = hash_from_seed(0x91);
        stale_utxo.vout = 1;
        stale_utxo.value = 4321;
        stale_utxo.address = address_from_seed(0x92);
        stale_utxo.height = 44;
        expect(storage_ptr->put_metadata(stale_utxo.get_key(), stale_utxo.serialize()),
               "seed UTXO behind an incomplete split commit");
        expect(storage_ptr->put_metadata("chainstate_height", encode_u64_le(44)),
               "seed height behind an incomplete split commit");
        expect(storage_ptr->put_metadata("chainstate_tip_hash", geocoin::bytes(32, 2)),
               "seed tip behind an incomplete split commit");
        expect(storage_ptr->put_metadata("split_commit_pending", {1}),
               "seed split-commit pending on chaindata");

        geocoin::state::ChainState recovered(std::move(storage));
        expect(recovered.get_height() == 44, "opening preserves incomplete split snapshot position");
        expect(!recovered.has_derived_state_snapshot(), "split snapshot is unavailable until verified recovery");
        expect(storage_ptr->has_metadata(stale_utxo.get_key()), "opening preserves incomplete split UTXOs");
        expect(recovered.prepare_rebuild() && recovered.finish_rebuild(), "explicit split-state rebuild");
        expect(recovered.get_height() == 0, "explicit split-state rebuild resets height");
        expect(!storage_ptr->has_incomplete_split_commit(),
               "split-commit pending is cleared after recovery");
        expect(!storage_ptr->has_metadata("split_commit_pending"),
               "split-commit pending metadata is removed");
        expect(!storage_ptr->has_metadata("chainstate_height"),
               "split-commit pending recovery purges height");
        expect(!storage_ptr->has_metadata(stale_utxo.get_key()),
               "split-commit pending recovery purges stale UTXO");
        expect(!recovered.has_utxo(stale_utxo.tx_hash, stale_utxo.vout),
               "split-commit pending recovery does not reload a stale UTXO");
    }

    {
        auto storage = std::make_unique<geocoin::state::MemoryStorage>();
        auto* storage_ptr = storage.get();
        expect(storage_ptr->put_metadata("chainstate_journal", {1, 2, 3}), "seed corrupt journal");
        expect(storage_ptr->put_metadata("scd:" + hash_to_hex(hash_from_seed(0x8c)), {1}),
               "seed purge candidate for corrupt journal");

        geocoin::state::ChainState recovered(std::move(storage));
        expect(recovered.get_height() == 0, "corrupt journal recovery resets height");
        expect(storage_ptr->has_metadata("chainstate_journal"), "corrupt journal is retained until explicit rebuild");
        expect(!recovered.has_derived_state_snapshot(), "corrupt journal prevents using the snapshot");
        expect(recovered.prepare_rebuild() && recovered.finish_rebuild(), "explicit rebuild resolves corrupt journal");
        expect(!storage_ptr->has_metadata("scd:" + hash_to_hex(hash_from_seed(0x8c))),
               "corrupt journal recovery purges rebuild metadata");
    }

    {
        auto storage = std::make_unique<SelectiveDeleteFailureStorage>();
        auto* storage_ptr = storage.get();
        geocoin::bytes journal;
        journal.push_back(1);
        journal.push_back(1);
        append_u64_le(journal, 88);
        append_hash(journal, hash_from_seed(0x8d));
        expect(storage_ptr->put_metadata("chainstate_journal", journal),
               "seed journal for purge-failure recovery");
        expect(storage_ptr->put_metadata("chainstate_height", encode_u64_le(88)),
               "seed undeletable recovery metadata");
        storage_ptr->failed_key = "chainstate_height";

        geocoin::state::ChainState recovered(std::move(storage));
        expect(storage_ptr->has_metadata("chainstate_journal"),
               "partial recovery purge retains the durable journal");
        expect(!geocoin::state::ChainStateTestAccess::derived_state_valid(recovered),
               "partial recovery purge marks derived state invalid");
        expect(recovered.get_height() == 88, "opening retains unverified snapshot position");
        expect(!recovered.prepare_rebuild(), "explicit rebuild reports a failed purge");
        expect(storage_ptr->has_metadata("chainstate_height") && storage_ptr->has_metadata("chainstate_journal"),
               "failed purge preserves source position and journal");
    }

    geocoin::Hash256 key = hash_from_seed(0x90);
    geocoin::state::ChainState no_storage_state;
    expect(!no_storage_state.has_angle_commitment_key(key), "angle index misses without storage");
    expect(!no_storage_state.has_multisig_spend_key(key), "multisig index misses without storage");
    expect(!no_storage_state.has_hierarchical_spend_key(key), "hierarchical index misses without storage");
    expect(!no_storage_state.has_delegation_proof_key(key), "delegation index misses without storage");
    expect(!no_storage_state.has_nft_id_key(key), "nft index misses without storage");
    expect(!no_storage_state.has_revoke_delegation_key(key), "revoke index misses without storage");
    expect(!no_storage_state.has_sidechain_deposit_key(key), "sidechain deposit misses without storage");
    expect(!no_storage_state.has_sidechain_registry_key(key), "sidechain registry misses without storage");
    expect(!no_storage_state.has_witness_pubkey(key), "witness pubkey misses without storage");
    geocoin::bytes missing_pubkey;
    expect(!no_storage_state.get_witness_pubkey(key, missing_pubkey), "witness get misses without storage");

    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* storage_ptr = storage.get();
    const std::string epoch = "0123456789abcdef0123456789abcdef";
    const std::string key_hex = hash_to_hex(key);
    expect(storage_ptr->put_metadata("idx_epoch", geocoin::bytes(epoch.begin(), epoch.end())), "seed idx epoch");
    expect(storage_ptr->put_metadata("idx:" + epoch + ":ac:" + key_hex, {1}), "seed angle index");
    expect(storage_ptr->put_metadata("idx:" + epoch + ":ms:" + key_hex, {1}), "seed multisig index");
    expect(storage_ptr->put_metadata("idx:" + epoch + ":hs:" + key_hex, {1}), "seed hierarchical index");
    expect(storage_ptr->put_metadata("idx:" + epoch + ":dp:" + key_hex, {1}), "seed delegation index");
    expect(storage_ptr->put_metadata("idx:" + epoch + ":nft:" + key_hex, {1}), "seed nft index");
    expect(storage_ptr->put_metadata("idx:" + epoch + ":rd:" + key_hex, {1}), "seed revoke index");

    geocoin::state::ChainState indexed(std::move(storage));
    expect(indexed.has_angle_commitment_key(key), "angle index is read from storage");
    expect(indexed.has_multisig_spend_key(key), "multisig index is read from storage");
    expect(indexed.has_hierarchical_spend_key(key), "hierarchical index is read from storage");
    expect(indexed.has_delegation_proof_key(key), "delegation index is read from storage");
    expect(indexed.has_nft_id_key(key), "nft index is read from storage");
    expect(!indexed.nft_exists(key),
           "a reserved NFT id can persist after its owner and metadata were removed");
    expect(indexed.has_revoke_delegation_key(key), "revoke index is read from storage");

    indexed.reset_index_epoch();
    expect(!indexed.has_angle_commitment_key(key), "reset_index_epoch invalidates old angle index key");
    expect(!indexed.has_multisig_spend_key(key), "reset_index_epoch invalidates old multisig index key");

    const geocoin::bytes pubkey_a = make_witness_pubkey(0x91);
    const geocoin::Hash256 pubkey_hash_a = geocoin::crypto::sha256(pubkey_a);
    const std::string pubkey_hex_a = hash_to_hex(pubkey_hash_a);
    expect(storage_ptr->put_metadata("wpk:" + pubkey_hex_a, pubkey_a), "seed witness pubkey after construction");
    expect(storage_ptr->put_metadata("wpkref:" + pubkey_hex_a, encode_u64_le(2)), "seed witness ref after construction");
    expect(indexed.has_witness_pubkey(pubkey_hash_a), "has_witness_pubkey loads uncached storage key");

    const geocoin::bytes pubkey_b = make_witness_pubkey(0x92);
    const geocoin::Hash256 pubkey_hash_b = geocoin::crypto::sha256(pubkey_b);
    const std::string pubkey_hex_b = hash_to_hex(pubkey_hash_b);
    expect(storage_ptr->put_metadata("wpk:" + pubkey_hex_b, pubkey_b), "seed second witness pubkey");
    expect(storage_ptr->put_metadata("wpkref:" + pubkey_hex_b, encode_u64_le(1)), "seed second witness ref");
    geocoin::bytes loaded_pubkey;
    expect(indexed.get_witness_pubkey(pubkey_hash_b, loaded_pubkey), "get_witness_pubkey loads storage miss");
    expect(loaded_pubkey == pubkey_b, "loaded witness pubkey bytes match");
    expect(indexed.get_witness_pubkey(pubkey_hash_b, loaded_pubkey), "get_witness_pubkey hits cache on second read");

    const geocoin::Hash256 missing_payload_hash = hash_from_seed(0x93);
    expect(storage_ptr->put_metadata("wpkref:" + hash_to_hex(missing_payload_hash), encode_u64_le(1)),
           "seed witness ref without pubkey payload");
    bool missing_payload_corrupt = false;
    try { indexed.get_witness_pubkey(missing_payload_hash, loaded_pubkey); }
    catch (const geocoin::storage::ReadError& error) {
        missing_payload_corrupt = error.status() == geocoin::storage::ReadStatus::CORRUPT;
    }
    expect(missing_payload_corrupt, "get_witness_pubkey distinguishes missing payload from an unknown key");

    const geocoin::Hash256 mismatched_hash = hash_from_seed(0x94);
    expect(storage_ptr->put_metadata("wpk:" + hash_to_hex(mismatched_hash), make_witness_pubkey(0x95)),
           "seed mismatched witness payload");
    expect(storage_ptr->put_metadata("wpkref:" + hash_to_hex(mismatched_hash), encode_u64_le(1)),
           "seed mismatched witness ref");
    bool mismatched_payload_corrupt = false;
    try { indexed.has_witness_pubkey(mismatched_hash); }
    catch (const geocoin::storage::ReadError& error) {
        mismatched_payload_corrupt = error.status() == geocoin::storage::ReadStatus::CORRUPT;
    }
    expect(mismatched_payload_corrupt, "has_witness_pubkey reports a stored hash mismatch as corruption");

    const auto stats = indexed.get_witness_pubkey_stats();
    expect(stats.total_keys >= 2, "witness stats count loaded keys");
    expect(stats.cache_hits >= 1, "witness stats track cache hits");
    expect(stats.cache_misses == 1, "witness stats track successful cold reads and exclude corrupt records");
}

geocoin::core::Transaction make_block_coinbase(
    geocoin::Height height,
    const geocoin::state::ChainState& state
)
{
    const geocoin::Amount reward =
        geocoin::consensus::ProofOfWork::calculate_block_reward(height);
    return geocoin::core::Transaction::create_coinbase(
        height,
        geocoin::Address("GEOapply-block-miner"),
        reward,
        0,
        state.get_foundation_address()
    );
}

geocoin::core::Block make_next_rewarded_block(
    const geocoin::state::ChainState& state,
    const geocoin::Address& miner,
    std::vector<geocoin::core::Transaction> transactions,
    std::optional<geocoin::Amount> package_fees = std::nullopt
)
{
    const geocoin::Height height = state.get_height() + 1;
    geocoin::Amount fees = package_fees.value_or(0);
    if (!package_fees) {
        for (const auto& tx : transactions) {
            fees += state.calculate_transaction_fee(tx);
        }
    }

    std::vector<geocoin::core::Transaction> block_transactions;
    block_transactions.reserve(transactions.size() + 1);
    block_transactions.push_back(geocoin::core::Transaction::create_coinbase(
        height,
        miner,
        geocoin::consensus::ProofOfWork::calculate_block_reward(height),
        fees,
        state.get_foundation_address()
    ));
    block_transactions.insert(
        block_transactions.end(),
        std::make_move_iterator(transactions.begin()),
        std::make_move_iterator(transactions.end())
    );
    return make_block(height, state.get_tip_hash(), std::move(block_transactions));
}

void test_deferred_durability_retains_recovery_journal()
{
    auto storage = std::make_unique<DeferredDurabilityStorage>();
    auto* storage_ptr = storage.get();
    geocoin::state::ChainState state(std::move(storage));
    state.set_height(0);
    state.set_tip_hash(hash_from_seed(0xcf));

    const auto first = make_block(
        1,
        state.get_tip_hash(),
        {make_block_coinbase(1, state)}
    );
    expect(state.apply_block(first, true), "async chainstate batch applies");
    expect(storage_ptr->has_metadata("chainstate_journal"),
           "recovery journal remains while split storage is not crash-durable");

    geocoin::core::Transaction missing_input;
    missing_input.version = 2;
    missing_input.type = geocoin::core::TransactionType::STANDARD;
    missing_input.timestamp = 1'700'000'010;
    missing_input.inputs.push_back({hash_from_seed(0xca), 0, {}, 0xffffffff, {}, {}});
    missing_input.outputs.push_back({
        geocoin::consensus::params::DUST_LIMIT,
        geocoin::Address("GEOdeferred-durability-invalid"),
        {}
    });
    const auto rejected = make_block(
        2,
        state.get_tip_hash(),
        {make_block_coinbase(2, state), missing_input}
    );
    expect(!state.apply_block(rejected, true),
           "candidate after async commit is rejected on missing input");
    expect(storage_ptr->has_metadata("chainstate_journal"),
           "failed next candidate cannot clear an older durability debt");

    storage_ptr->durable = true;
    const auto second = make_block(
        2,
        state.get_tip_hash(),
        {make_block_coinbase(2, state)}
    );
    expect(state.apply_block(second, true), "durable chainstate batch applies");
    expect(!storage_ptr->has_metadata("chainstate_journal"),
           "recovery journal clears after a crash-durable split commit");
}

#ifndef _WIN32
[[noreturn]] void run_chainstate_crash_child(const std::string& path, bool stop_during_ibd)
{
    auto fail = [](int code) -> void { ::_exit(code); };
    geocoin::state::StorageOptions options;
    options.profile = geocoin::state::StorageProfile::FAST_SYNC;
    options.disable_wal_during_ibd = true;
    auto storage = geocoin::state::StorageFactory::create_split(
        geocoin::state::StorageFactory::Type::ROCKSDB,
        (std::filesystem::path(path) / "chainstate").string(),
        (std::filesystem::path(path) / "addresses").string(),
        options
    );
    if (!storage) {
        fail(21);
    }

    auto* state = new geocoin::state::ChainState(std::move(storage));
    geocoin::state::ChainStateTestAccess::set_foundation_config(*state, state->get_foundation_config());
    geocoin::state::ChainStateTestAccess::set_interest_pool_balance(*state, 0);
    const auto initial_tip = hash_from_seed(0xce);
    state->set_height(0);
    state->set_tip_hash(initial_tip);
    state->set_ibd_flush_interval(10000);
    state->set_ibd_mode(true);

    auto first_coinbase = make_block_coinbase(1, *state);
    first_coinbase.timestamp = 1'700'000'001;
    const auto first = make_block(1, initial_tip, {first_coinbase});
    if (!state->apply_block(first, true)) {
        fail(22);
    }
    if (stop_during_ibd) {
        ::_exit(0);
    }

    // Leaving IBD must preserve both the earlier and the next state commit.
    state->set_ibd_mode(false);
    auto second_coinbase = make_block_coinbase(2, *state);
    second_coinbase.timestamp = 1'700'000'002;
    const auto second = make_block(2, first.get_hash(), {second_coinbase});
    if (!state->apply_block(second, true)) {
        fail(23);
    }
    ::_exit(0);
}

void spawn_chainstate_crash_child(
    const std::string& self_path,
    const std::filesystem::path& path,
    const char* mode
)
{
    const pid_t pid = ::fork();
    expect(pid >= 0, "fork for chainstate crash test succeeds");
    if (pid == 0) {
        ::execl(
            self_path.c_str(),
            self_path.c_str(),
            mode,
            path.c_str(),
            static_cast<char*>(nullptr)
        );
        ::_exit(127);
    }

    int status = 0;
    expect(::waitpid(pid, &status, 0) == pid, "chainstate crash child is reaped");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "chainstate crash child completed its writes before abrupt exit");
}

std::unique_ptr<geocoin::state::Storage> open_crash_test_storage(
    const std::filesystem::path& path
)
{
    geocoin::state::StorageOptions options;
    options.profile = geocoin::state::StorageProfile::FAST_SYNC;
    options.disable_wal_during_ibd = true;
    return geocoin::state::StorageFactory::create_split(
        geocoin::state::StorageFactory::Type::ROCKSDB,
        (path / "chainstate").string(),
        (path / "addresses").string(),
        options
    );
}

void test_split_chainstate_survives_abrupt_exit(const std::string& self_path)
{
    const auto ibd_path = make_temp_dir("geocoin-chainstate-crash-ibd");
    spawn_chainstate_crash_child(
        self_path,
        ibd_path,
        "--chainstate-crash-ibd-child"
    );
    {
        auto storage = open_crash_test_storage(ibd_path);
        expect(storage != nullptr, "IBD split chainstate reopens after abrupt exit");
        expect(!storage->has_metadata("chainstate_journal") && !storage->has_incomplete_split_commit(),
               "completed IBD split commit needs neither a journal nor split recovery");
        geocoin::state::ChainState recovered(std::move(storage));
        expect(recovered.get_height() == 1 && recovered.has_derived_state_snapshot(),
               "completed IBD snapshot survives without rebuilding");
        geocoin::state::ChainState fixture;
        auto coinbase = make_block_coinbase(1, fixture);
        coinbase.timestamp = 1'700'000'001;
        const auto first = make_block(1, hash_from_seed(0xce), {coinbase});
        expect(recovered.get_tip_hash() == first.get_hash(),
               "completed IBD commit preserves the expected tip");
        geocoin::state::UTXO utxo;
        expect(recovered.get_utxo(coinbase.get_hash(), 0, utxo) &&
               recovered.get_storage()->has_metadata(utxo.get_key()),
               "completed IBD commit preserves its UTXO in memory and RocksDB");
        geocoin::state::BlockUndo undo;
        expect(recovered.load_block_undo(first.get_hash(), undo) &&
               undo.height == 1 && undo.block_hash == first.get_hash(),
               "completed IBD commit preserves matching undo data");
    }
    std::filesystem::remove_all(ibd_path);

    const auto durable_path = make_temp_dir("geocoin-chainstate-crash-durable");
    spawn_chainstate_crash_child(
        self_path,
        durable_path,
        "--chainstate-crash-durable-child"
    );

    {
        auto storage = open_crash_test_storage(durable_path);
        expect(storage != nullptr, "durable split chainstate reopens after abrupt exit");
        expect(!storage->has_metadata("chainstate_journal"),
               "post-IBD durable commit clears the recovery journal");
        geocoin::state::ChainState recovered(std::move(storage));
        expect(recovered.get_height() == 2,
               "post-IBD durable commit preserves the chain height");

        geocoin::state::ChainState fixture;
        const auto initial_tip = hash_from_seed(0xce);
        auto first_coinbase = make_block_coinbase(1, fixture);
        first_coinbase.timestamp = 1'700'000'001;
        const auto first = make_block(1, initial_tip, {first_coinbase});
        auto second_coinbase = make_block_coinbase(2, fixture);
        second_coinbase.timestamp = 1'700'000'002;
        const auto second = make_block(2, first.get_hash(), {second_coinbase});
        expect(recovered.get_tip_hash() == second.get_hash(),
               "post-IBD durable commit preserves the expected tip");
        expect(recovered.has_utxo(first_coinbase.get_hash(), 0),
               "post-IBD sync preserves the earlier address state");
        expect(recovered.has_utxo(second_coinbase.get_hash(), 0),
               "post-IBD sync preserves the current address state");
    }
    std::filesystem::remove_all(durable_path);
}
#endif

void test_apply_block_guard_paths()
{
    const geocoin::Hash256 tip_a = hash_from_seed(0xd1);
    const geocoin::Hash256 tip_b = hash_from_seed(0xd2);

    {
        geocoin::state::ChainState no_storage;
        no_storage.set_height(0);
        no_storage.set_tip_hash(tip_a);
        const auto block = make_block(1, tip_a, {});
        expect(!no_storage.apply_block(block, true),
               "apply_block rejects non-genesis without persistent storage");
    }

    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    state.set_height(0);
    state.set_tip_hash(tip_a);

    auto invalid = make_block(1, tip_a, {make_block_coinbase(1, state)});
    invalid.header.merkle_root = hash_from_seed(0xd3);
    expect(!state.apply_block(invalid, false), "apply_block rejects invalid untrusted block");

    const auto coinbase = make_block_coinbase(1, state);
    auto bad_reward_coinbase = geocoin::core::Transaction::create_coinbase(
        1,
        geocoin::Address("GEObad-reward-miner"),
        geocoin::consensus::ProofOfWork::calculate_block_reward(1) + 1,
        0,
        state.get_foundation_address()
    );
    const auto bad_reward_block = make_block(1, tip_a, {bad_reward_coinbase});
    expect(!state.apply_block(bad_reward_block, true),
           "apply_block rejects excessive coinbase reward");

    const auto wrong_prev = make_block(1, tip_b, {coinbase});
    expect(!state.apply_block(wrong_prev, true), "apply_block rejects prev-hash mismatch");

    geocoin::core::Transaction interest;
    interest.version = 2;
    interest.type = geocoin::core::TransactionType::INTEREST_DISTRIBUTION;
    interest.outputs.push_back({
        geocoin::consensus::params::DUST_LIMIT,
        geocoin::Address("GEOinterest-too-early"),
        {}
    });
    const auto premature_interest = make_block(1, tip_a, {coinbase, interest});
    expect(!state.apply_block(premature_interest, true),
           "apply_block rejects interest distribution outside scheduled height");

    geocoin::core::Transaction duplicate_tx;
    duplicate_tx.version = 2;
    duplicate_tx.type = geocoin::core::TransactionType::STANDARD;
    const auto duplicate_block = make_block(1, tip_a, {coinbase, duplicate_tx, duplicate_tx});
    expect(!state.apply_block(duplicate_block, true),
           "apply_block rejects duplicate non-coinbase txids in one block");

    geocoin::core::Transaction active_tx;
    active_tx.version = 2;
    active_tx.type = geocoin::core::TransactionType::STANDARD;
    active_tx.timestamp = 2222;
    active_tx.outputs.push_back({
        geocoin::consensus::params::DUST_LIMIT,
        geocoin::Address("GEOalready-active-output"),
        {}
    });
    expect(state.get_storage()->put_transaction(active_tx.get_hash(), active_tx),
           "test seeds active transaction in backing storage");
    const auto already_active_block = make_block(1, tip_a, {coinbase, active_tx});
    expect(!state.apply_block(already_active_block, true),
           "apply_block rejects txid that already exists in active storage");

    geocoin::core::Transaction missing_input_tx;
    missing_input_tx.version = 2;
    missing_input_tx.type = geocoin::core::TransactionType::STANDARD;
    missing_input_tx.timestamp = 3333;
    geocoin::core::TxInput missing_input;
    missing_input.prev_tx_hash = hash_from_seed(0xd4);
    missing_input.vout = 0;
    missing_input_tx.inputs.push_back(missing_input);
    missing_input_tx.outputs.push_back({
        geocoin::consensus::params::DUST_LIMIT,
        geocoin::Address("GEOmissing-input-output"),
        {}
    });
    const auto missing_input_block = make_block(1, tip_a, {coinbase, missing_input_tx});
    expect(!state.apply_block(missing_input_block, true),
           "apply_block rolls back when a non-coinbase input UTXO is missing");

    const geocoin::bytes commitment_key = private_key_from_seed(0xd5);
    const geocoin::Address commitment_signer = address_from_seed(0xd5);
    const auto multisig = geocoin::wallet::create_multisig_address(
        1,
        1,
        {commitment_signer}
    );
    const auto mempool_commitment = geocoin::core::Transaction::create_angle_commitment(
        geocoin::Address(multisig.address),
        hash_from_seed(0xd6),
        commitment_key,
        commitment_signer,
        "degree"
    );
    state.add_angle_commitment_to_store(mempool_commitment);
    expect(state.get_angle_store()->has_commitment(mempool_commitment.get_hash()),
           "mempool commitment fixture is present before candidate block apply");

    const auto commitment_then_invalid = make_block(
        1,
        tip_a,
        {coinbase, mempool_commitment, missing_input_tx}
    );
    expect(!state.apply_block(commitment_then_invalid, true),
           "candidate block with a later invalid transaction is rejected");
    expect(state.get_angle_store()->has_commitment(mempool_commitment.get_hash()),
           "failed candidate rollback preserves a pre-existing mempool commitment");
    expect(!state.has_angle_commitment_key(mempool_commitment.inputs[0].prev_tx_hash),
           "failed candidate rollback still removes its chain anti-replay key");

    const auto funding = geocoin::core::Transaction::create_coinbase(
        99,
        geocoin::Address("GEOoverspend-funding"),
        10 * geocoin::COIN
    );
    expect(state.apply_transaction(funding, 99, true),
           "overspend test funding coinbase applies");
    geocoin::core::Transaction overspend;
    overspend.version = 2;
    overspend.type = geocoin::core::TransactionType::STANDARD;
    overspend.timestamp = 4444;
    geocoin::core::TxInput overspend_input;
    overspend_input.prev_tx_hash = funding.get_hash();
    overspend_input.vout = 0;
    overspend.inputs.push_back(overspend_input);
    overspend.outputs.push_back({
        10 * geocoin::COIN + 1,
        geocoin::Address("GEOoverspend-output"),
        {}
    });
    const auto overspend_block = make_block(1, tip_a, {coinbase, overspend});
    expect(!state.apply_block(overspend_block, true),
           "apply_block rolls back when transaction application fails after undo capture");
    expect(state.has_utxo(funding.get_hash(), 0),
           "failed current transaction preserves its funding UTXO");
    expect(!state.has_utxo(overspend.get_hash(), 0),
           "failed current transaction does not leave an output UTXO");

    expect(state.get_height() == 0, "failed apply_block guards leave height unchanged");
    expect(state.get_tip_hash() == tip_a, "failed apply_block guards leave tip unchanged");
}

void test_apply_block_dependency_rollback_restores_full_utxo_state()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::bytes owner_key = private_key_from_seed(0x21);
    const geocoin::Address owner = address_from_seed(0x21);
    const geocoin::Address recipient = address_from_seed(0x22);
    const geocoin::Height next_height = geocoin::consensus::params::COINBASE_MATURITY + 2;

    const auto funding = geocoin::core::Transaction::create_coinbase(
        1,
        owner,
        5 * geocoin::COIN
    );
    expect(state.apply_transaction(funding, 1, true), "dependency rollback funding applies");
    state.set_height(next_height - 1);
    state.set_tip_hash(hash_from_seed(0xe1));

    const auto before = geocoin::state::ChainStateTestAccess::utxo_snapshot(state);
    expect(state.has_utxo(funding.get_hash(), 0),
           "dependency rollback fixture contains funding UTXO");

    auto make_spend = [&](const geocoin::Hash256& prev_txid,
                          geocoin::Amount value,
                          const geocoin::Address& destination,
                          uint64_t timestamp) {
        geocoin::core::Transaction tx;
        tx.version = 2;
        tx.type = geocoin::core::TransactionType::STANDARD;
        tx.timestamp = timestamp;
        tx.inputs.push_back({prev_txid, 0, {}, 0xffffffff, {}, {}});
        tx.outputs.push_back({value, destination, {}});
        tx.sign_input(0, owner_key);
        return tx;
    };

    // A creates X; B spends X. C is contextually invalid and forces emergency rollback.
    const auto tx_a = make_spend(funding.get_hash(), 5 * geocoin::COIN, owner, 5001);
    const auto tx_b = make_spend(tx_a.get_hash(), 5 * geocoin::COIN, recipient, 5002);

    geocoin::core::Transaction invalid_c;
    invalid_c.version = 2;
    invalid_c.type = geocoin::core::TransactionType::STANDARD;
    invalid_c.timestamp = 5003;
    invalid_c.inputs.push_back({hash_from_seed(0xe2), 0, {}, 0xffffffff, {}, {}});
    invalid_c.outputs.push_back({geocoin::consensus::params::DUST_LIMIT, recipient, {}});

    const auto coinbase = make_block_coinbase(next_height, state);
    const auto block = make_block(
        next_height,
        state.get_tip_hash(),
        {coinbase, tx_a, tx_b, invalid_c}
    );
    expect(!state.apply_block(block, true), "A->B->invalid C block is rejected");

    const auto after = geocoin::state::ChainStateTestAccess::utxo_snapshot(state);
    expect(after == before, "failed dependent block restores the complete UTXO set byte-for-byte");
    expect(state.has_utxo(funding.get_hash(), 0), "original funding UTXO is restored");
    expect(!state.has_utxo(tx_a.get_hash(), 0), "A output is absent after rollback");
    expect(!state.has_utxo(tx_b.get_hash(), 0), "B output is absent after rollback");
    expect(!state.has_utxo(coinbase.get_hash(), 0), "failed block coinbase is absent after rollback");
    expect(state.get_height() == next_height - 1, "failed dependent block preserves height");
    expect(state.get_tip_hash() == hash_from_seed(0xe1), "failed dependent block preserves tip");
}

void test_revert_block_guard_paths_and_snapshot_probe()
{
    {
        auto storage = std::make_unique<geocoin::state::MemoryStorage>();
        auto* storage_ptr = storage.get();
        expect(storage_ptr->put_metadata("idx_epoch", geocoin::bytes(32, 'A')),
               "invalid uppercase index epoch is seeded");
        geocoin::state::ChainState state(std::move(storage));
        expect(!state.has_derived_state_snapshot(),
               "invalid anti-replay epoch invalidates the derived snapshot");

        geocoin::bytes replacement_epoch;
        expect(storage_ptr->get_metadata("idx_epoch", replacement_epoch) &&
                   replacement_epoch == geocoin::bytes(32, 'A'),
               "invalid epoch is preserved for diagnosis until explicit rebuild");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        expect(state.has_derived_state_snapshot(), "height-zero state is treated as having derived snapshot");
        state.set_height(1);
        expect(!state.has_derived_state_snapshot(), "nonzero empty state reports missing derived snapshot");

        const auto coinbase = geocoin::core::Transaction::create_coinbase(
            1,
            geocoin::Address("GEOsnapshot-miner"),
            1000
        );
        expect(state.apply_transaction(coinbase, 1, true), "snapshot probe funding applies");
        expect(state.has_derived_state_snapshot(), "nonzero state with UTXO reports derived snapshot");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto block = make_block(5, hash_from_seed(0x68), {});
        state.set_tip_hash(hash_from_seed(0x69));
        expect(!state.revert_block(block), "revert_block rejects non-tip block");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto block = make_block(5, hash_from_seed(0x6f), {});
        auto state_lock = geocoin::state::ChainStateTestAccess::lock_state(state);
        std::promise<void> worker_started;
        auto started = worker_started.get_future();
        auto revert = std::async(std::launch::async, [&]() {
            worker_started.set_value();
            return state.revert_block(block);
        });
        started.wait();
        expect(
            revert.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
            "revert_block waits for the ChainState mutex"
        );
        state_lock.unlock();
        expect(!revert.get(), "serialized non-tip revert is rejected after acquiring the mutex");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto block = make_block(6, hash_from_seed(0x6a), {});
        state.set_tip_hash(block.get_hash());
        expect(!state.revert_block(block), "revert_block rejects missing undo data");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::core::Transaction tx;
        tx.version = 2;
        tx.type = geocoin::core::TransactionType::STANDARD;
        const auto block = make_block(7, hash_from_seed(0x6b), {tx});
        geocoin::state::BlockUndo undo;
        undo.height = 7;
        undo.block_hash = block.get_hash();
        expect(state.save_block_undo(undo), "mismatched undo saves");
        state.set_tip_hash(block.get_hash());
        expect(!state.revert_block(block), "revert_block rejects undo tx count mismatch");
    }

    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        const auto coinbase = geocoin::core::Transaction::create_coinbase(
            8,
            address_from_seed(0x6c),
            1000
        );
        expect(state.apply_transaction(coinbase, 8, true),
               "interest-underflow fixture coinbase applies");
        const auto block = make_block(8, hash_from_seed(0x6c), {coinbase});
        geocoin::state::BlockUndo undo;
        undo.height = 8;
        undo.block_hash = block.get_hash();
        undo.tx_undos.push_back({});
        undo.interest_pool_delta = 1;
        expect(state.save_block_undo(undo), "interest underflow undo saves");
        state.set_height(8);
        state.set_tip_hash(block.get_hash());
        expect(!state.revert_block(block), "revert_block rejects interest pool underflow");
        expect(state.has_utxo(coinbase.get_hash(), 0),
               "interest pool preflight preserves transaction UTXOs");
        expect(geocoin::state::ChainStateTestAccess::derived_state_valid(state),
               "interest pool underflow is rejected before mutating derived state");
    }

    {
        auto storage = std::make_unique<FailingCommitStorage>();
        auto* storage_ptr = storage.get();
        geocoin::state::ChainState state(std::move(storage));
        const auto block = make_block(9, hash_from_seed(0x6d), {});
        geocoin::state::BlockUndo undo;
        undo.height = 9;
        undo.block_hash = block.get_hash();
        expect(state.save_block_undo(undo), "commit-failure undo saves");
        state.set_height(9);
        state.set_tip_hash(block.get_hash());
        storage_ptr->fail_next_commit = true;
        expect(!state.revert_block(block), "revert_block surfaces storage commit failure");
        expect(!geocoin::state::ChainStateTestAccess::derived_state_valid(state),
               "failed revert commit poisons derived state until rebuild");
        expect(!state.revert_block(block), "poisoned state rejects another revert");
        expect(!state.apply_transaction(
                   geocoin::core::Transaction::create_coinbase(
                       9,
                       geocoin::Address("GEOpoisoned-state"),
                       1000),
                   9,
                   true),
               "poisoned state rejects further transaction mutations");
    }
}

void test_apply_block_auto_undo_for_nft_lifecycle()
{
    auto chain_state =
        std::make_shared<geocoin::state::ChainState>(std::make_unique<geocoin::state::MemoryStorage>());
    chain_state->set_activation_manifest(geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));

    geocoin::wallet::Wallet miner_wallet;
    geocoin::wallet::Wallet recipient_wallet;
    expect(miner_wallet.initialize(private_key_from_seed(0x21)), "miner wallet initializes");
    expect(recipient_wallet.initialize(private_key_from_seed(0x31)), "recipient wallet initializes");

    const geocoin::Address miner = miner_wallet.get_address();
    const geocoin::Address recipient = recipient_wallet.get_address();
    const geocoin::Amount fee = geocoin::consensus::params::MIN_TX_FEE;

    const auto funding = geocoin::core::Transaction::create_coinbase(
        1,
        miner,
        1000 * geocoin::COIN,
        0,
        chain_state->get_foundation_address()
    );
    expect(chain_state->apply_transaction(funding, 1, true), "NFT lifecycle funding coinbase applies");
    chain_state->set_height(geocoin::consensus::params::COINBASE_MATURITY + 1);
    chain_state->set_tip_hash(hash_from_seed(0x32));

    auto make_metadata = [&](const std::string& name, geocoin::Height height) {
        geocoin::nft::NFTMetadata metadata = make_nft_metadata();
        metadata.name = name;
        metadata.description = "apply_block auto undo coverage";
        metadata.content_hash = std::string(64, static_cast<char>('a' + (name.size() % 6)));
        metadata.block_height = height;
        metadata.block_hash = hash_from_seed(static_cast<uint8_t>(0x33 + name.size()));
        metadata.location_name = name + " place";
        metadata.tags = {"apply-block", "undo"};
        metadata.creator = miner.str();
        return metadata;
    };
    auto make_cadastral_metadata = [&](const std::string& number, geocoin::Height height) {
        geocoin::nft::NFTMetadata metadata = make_metadata("CadastralTransfer", height);
        metadata.geometry.polygons.front().exterior = {
            {2000000000LL, 2000000000LL},
            {2000000000LL, 2010000000LL},
            {2010000000LL, 2010000000LL},
            {2010000000LL, 2000000000LL}
        };
        geocoin::nft::NFTMetadata::CadastralData cadastral;
        cadastral.cadastral_number = number;
        cadastral.owner_name = "Test owner";
        cadastral.land_category = "test";
        cadastral.permitted_use = "coverage";
        cadastral.ownership_document_hash = hash_from_seed(0x88);
        cadastral.technical_plan_hash = hash_from_seed(0x89);
        cadastral.photo_hashes = {hash_from_seed(0x8a)};
        cadastral.market_value = 100 * geocoin::COIN;
        cadastral.cadastral_value = 90 * geocoin::COIN;
        cadastral.last_valuation_date = 1700000000;
        cadastral.restrictions = {"none"};
        const double projected_area = geocoin::nft::coordinates::measure_polygon(metadata.geometry.polygons.front().exterior)->area_sqm();
        expect(projected_area > 0.0, "cadastral test polygon has projected area");
        cadastral.area_sqcm_official = static_cast<uint64_t>(std::llround(projected_area * 10000.0));
        metadata.cadastral = cadastral;
        return metadata;
    };

    geocoin::core::Transaction create_to_revert;
    expect(miner_wallet.create_nft_create(
               make_metadata("CreateUndo", chain_state->get_height() + 1),
               100,
               fee,
               chain_state,
               create_to_revert),
           "wallet creates NFT_CREATE for create undo");
    const geocoin::Hash256 reverted_nft_id = create_to_revert.nft_create->nft_id;
    const auto create_revert_block = make_next_rewarded_block(*chain_state, miner, {create_to_revert});
    expect(chain_state->apply_block(create_revert_block, true), "NFT_CREATE block applies");
    expect(chain_state->nft_exists(reverted_nft_id), "NFT_CREATE block registers NFT");
    expect(chain_state->revert_block(create_revert_block), "NFT_CREATE block reverts");
    expect(!chain_state->nft_exists(reverted_nft_id), "NFT_CREATE undo removes NFT");

    geocoin::core::Transaction create;
    expect(miner_wallet.create_nft_create(
               make_metadata("TransferBurn", chain_state->get_height() + 1),
               100,
               fee,
               chain_state,
               create),
           "wallet creates NFT_CREATE for transfer/burn lifecycle");
    const geocoin::Hash256 nft_id = create.nft_create->nft_id;
    const auto create_block = make_next_rewarded_block(*chain_state, miner, {create});
    expect(chain_state->apply_block(create_block, true), "second NFT_CREATE block applies");
    expect(chain_state->nft_exists(nft_id), "second NFT exists");
    expect(chain_state->get_nft_owner(nft_id) == miner, "second NFT owner starts as miner");

    geocoin::core::Transaction transfer;
    expect(miner_wallet.create_nft_transfer(nft_id, recipient, fee, chain_state, transfer),
           "wallet creates NFT_TRANSFER");
    const auto transfer_block = make_next_rewarded_block(*chain_state, miner, {transfer});
    expect(chain_state->apply_block(transfer_block, true), "NFT_TRANSFER block applies");
    expect(chain_state->get_nft_owner(nft_id) == recipient, "NFT_TRANSFER moves owner");
    expect(chain_state->revert_block(transfer_block), "NFT_TRANSFER block reverts");
    expect(chain_state->get_nft_owner(nft_id) == miner, "NFT_TRANSFER undo restores owner");

    geocoin::core::Transaction burn;
    expect(miner_wallet.create_nft_burn(nft_id, fee, chain_state, burn), "wallet creates NFT_BURN");
    const auto burn_block = make_next_rewarded_block(*chain_state, miner, {burn});
    expect(chain_state->apply_block(burn_block, true), "NFT_BURN block applies");
    expect(!chain_state->nft_exists(nft_id), "NFT_BURN removes NFT");
    expect(chain_state->has_nft_id_key(nft_id), "NFT_BURN retains the permanent NFT id reservation");
    expect(!chain_state->validate_transaction(create),
           "a burned NFT id cannot be created again with the same canonical metadata");
    expect(chain_state->revert_block(burn_block), "NFT_BURN block reverts");
    expect(chain_state->nft_exists(nft_id), "NFT_BURN undo restores NFT");
    expect(chain_state->get_nft_owner(nft_id) == miner, "NFT_BURN undo restores owner");

    geocoin::core::Transaction cadastral_create;
    expect(miner_wallet.create_nft_create(
               make_cadastral_metadata("77:01:0001001:1234", chain_state->get_height() + 1),
               0,
               fee,
               chain_state,
               cadastral_create),
           "wallet creates cadastral NFT_CREATE");
    const geocoin::Hash256 cadastral_nft_id = cadastral_create.nft_create->nft_id;
    const auto cadastral_create_block = make_next_rewarded_block(*chain_state, miner, {cadastral_create});
    expect(chain_state->apply_block(cadastral_create_block, true), "cadastral NFT_CREATE block applies");
    expect(chain_state->nft_exists(cadastral_nft_id), "cadastral NFT exists after create");
    const auto replacement_allowed = [&] {
        auto metadata = cadastral_create.nft_create->metadata;
        metadata.name += " replacement";
        geocoin::core::Transaction replacement;
        expect(miner_wallet.create_nft_create(metadata, 0, fee, chain_state, replacement),
               "wallet builds a fresh cadastral replacement candidate");
        return chain_state->validate_transaction(replacement);
    };
    expect(!replacement_allowed(), "created cadastral NFT reserves its number and polygon");

    geocoin::core::Transaction cadastral_transfer;
    expect(miner_wallet.create_nft_transfer(cadastral_nft_id, recipient, fee, chain_state, cadastral_transfer),
           "wallet creates cadastral NFT_TRANSFER");
    const auto cadastral_transfer_block = make_next_rewarded_block(*chain_state, miner, {cadastral_transfer});
    expect(chain_state->apply_block(cadastral_transfer_block, true),
           "cadastral NFT_TRANSFER auto-burn block applies");
    expect(!chain_state->nft_exists(cadastral_nft_id), "cadastral NFT transfer auto-burns NFT");
    expect(replacement_allowed(), "cadastral auto-burn releases its number and polygon");
    expect(chain_state->revert_block(cadastral_transfer_block),
           "cadastral NFT_TRANSFER auto-burn block reverts");
    expect(chain_state->nft_exists(cadastral_nft_id), "cadastral NFT auto-burn undo restores NFT");
    expect(chain_state->get_nft_owner(cadastral_nft_id) == miner,
           "cadastral NFT auto-burn undo restores owner");
    expect(chain_state->get_nft_metadata(cadastral_nft_id).cadastral.has_value(),
           "cadastral NFT auto-burn undo restores cadastral metadata");
    expect(!replacement_allowed(), "cadastral undo restores its number and polygon reservations");
}

void test_nft_storage_failures_leave_state_unchanged()
{
    struct Fixture {
        std::shared_ptr<geocoin::state::ChainState> state;
        SelectiveNftWriteFailureStorage* storage{nullptr};
        geocoin::core::Transaction create;
        geocoin::Address owner;
        geocoin::Address recipient;
        geocoin::bytes owner_key;
        geocoin::bytes recipient_key;
        geocoin::Height apply_height{0};
    };

    auto make_fixture = [](uint8_t seed, bool cadastral = false) {
        Fixture fixture;
        auto storage = std::make_unique<SelectiveNftWriteFailureStorage>();
        fixture.storage = storage.get();
        fixture.state = std::make_shared<geocoin::state::ChainState>(std::move(storage));
        fixture.state->set_activation_manifest(
            geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));

        fixture.owner_key = private_key_from_seed(seed);
        fixture.recipient_key = private_key_from_seed(static_cast<uint8_t>(seed + 1));
        geocoin::wallet::Wallet owner_wallet;
        geocoin::wallet::Wallet recipient_wallet;
        expect(owner_wallet.initialize(fixture.owner_key), "NFT storage owner wallet initializes");
        expect(recipient_wallet.initialize(fixture.recipient_key),
               "NFT storage recipient wallet initializes");
        fixture.owner = owner_wallet.get_address();
        fixture.recipient = recipient_wallet.get_address();

        const auto funding = geocoin::core::Transaction::create_coinbase(
            1,
            fixture.owner,
            10 * geocoin::COIN
        );
        expect(fixture.state->apply_transaction(funding, 1, true),
               "NFT storage funding applies");
        fixture.apply_height = geocoin::consensus::params::COINBASE_MATURITY + 5;
        fixture.state->set_height(fixture.apply_height - 1);
        fixture.state->set_tip_hash(hash_from_seed(static_cast<uint8_t>(seed + 2)));

        auto metadata = make_nft_metadata();
        metadata.name = "Storage NFT " + std::to_string(seed);
        metadata.content_hash.assign(64, static_cast<char>('a' + (seed % 6)));
        metadata.creator = fixture.owner.str();
        metadata.block_height = fixture.apply_height;
        if (cadastral) {
            metadata.geometry.polygons.front().exterior = {
                {3000000000LL, 3000000000LL},
                {3000000000LL, 3010000000LL},
                {3010000000LL, 3010000000LL},
                {3010000000LL, 3000000000LL}
            };
            geocoin::nft::NFTMetadata::CadastralData cadastral_data;
            cadastral_data.cadastral_number =
                "77:01:0001001:" + std::to_string(static_cast<unsigned int>(seed));
            cadastral_data.owner_name = "Storage owner";
            cadastral_data.land_category = "test";
            cadastral_data.permitted_use = "restart and recovery";
            cadastral_data.ownership_document_hash = hash_from_seed(seed);
            cadastral_data.technical_plan_hash = hash_from_seed(static_cast<uint8_t>(seed + 1));
            cadastral_data.area_sqcm_official = static_cast<uint64_t>(
                geocoin::nft::coordinates::measure_polygon(metadata.geometry.polygons.front().exterior)->area_sqm() * 10000.0);
            metadata.cadastral = cadastral_data;
        }
        expect(owner_wallet.create_nft_create(
                   metadata,
                   100,
                   geocoin::consensus::params::MIN_TX_FEE,
                   fixture.state,
                   fixture.create),
               "NFT storage create transaction builds");
        return fixture;
    };

    enum class CreateFailure {
        METADATA,
        METADATA_EXCEPTION,
        OWNER,
        INDEX,
        TRANSACTION,
        TRANSACTION_EXCEPTION,
        UTXO_INPUT_DELETE,
        UTXO_INPUT_DELETE_EXCEPTION,
        UTXO_OUTPUT_PUT,
        UTXO_OUTPUT_PUT_EXCEPTION,
        UTXO_OUTPUT_CLEANUP,
        UTXO_OUTPUT_CLEANUP_EXCEPTION,
        UTXO_INPUT_RESTORE,
        UTXO_INPUT_RESTORE_EXCEPTION,
        NFT_METADATA_CLEANUP,
        NFT_METADATA_CLEANUP_EXCEPTION,
        RAW_TRANSACTION_CLEANUP,
        RAW_TRANSACTION_CLEANUP_EXCEPTION
    };
    for (const auto failure : {
             CreateFailure::METADATA,
             CreateFailure::METADATA_EXCEPTION,
             CreateFailure::OWNER,
             CreateFailure::INDEX,
             CreateFailure::TRANSACTION,
             CreateFailure::TRANSACTION_EXCEPTION,
             CreateFailure::UTXO_INPUT_DELETE,
             CreateFailure::UTXO_INPUT_DELETE_EXCEPTION,
             CreateFailure::UTXO_OUTPUT_PUT,
             CreateFailure::UTXO_OUTPUT_PUT_EXCEPTION,
             CreateFailure::UTXO_OUTPUT_CLEANUP,
             CreateFailure::UTXO_OUTPUT_CLEANUP_EXCEPTION,
             CreateFailure::UTXO_INPUT_RESTORE,
             CreateFailure::UTXO_INPUT_RESTORE_EXCEPTION,
             CreateFailure::NFT_METADATA_CLEANUP,
             CreateFailure::NFT_METADATA_CLEANUP_EXCEPTION,
             CreateFailure::RAW_TRANSACTION_CLEANUP,
             CreateFailure::RAW_TRANSACTION_CLEANUP_EXCEPTION}) {
        auto fixture = make_fixture(static_cast<uint8_t>(0x80 + static_cast<int>(failure) * 3));
        const auto nft_id = fixture.create.nft_create->nft_id;
        const std::string id_hex = hash_to_hex(nft_id);
        geocoin::state::UTXO input_key;
        input_key.tx_hash = fixture.create.inputs.front().prev_tx_hash;
        input_key.vout = fixture.create.inputs.front().vout;
        geocoin::state::UTXO output_key;
        output_key.tx_hash = fixture.create.get_hash();
        output_key.vout = 0;

        if (failure == CreateFailure::NFT_METADATA_CLEANUP ||
            failure == CreateFailure::NFT_METADATA_CLEANUP_EXCEPTION) {
            fixture.storage->failed_put_key = "nft:owner:" + id_hex;
            fixture.storage->failed_delete_key = "nft:meta:" + id_hex;
            fixture.storage->throw_on_delete_failure =
                failure == CreateFailure::NFT_METADATA_CLEANUP_EXCEPTION;
        } else if (failure == CreateFailure::METADATA ||
            failure == CreateFailure::METADATA_EXCEPTION) {
            fixture.storage->failed_put_key = "nft:meta:" + id_hex;
            fixture.storage->throw_on_put_failure =
                failure == CreateFailure::METADATA_EXCEPTION;
        } else if (failure == CreateFailure::OWNER) {
            fixture.storage->failed_put_key = "nft:owner:" + id_hex;
        } else if (failure == CreateFailure::INDEX) {
            fixture.storage->failed_put_fragment = ":nft:" + id_hex;
        } else if (failure == CreateFailure::TRANSACTION ||
                   failure == CreateFailure::TRANSACTION_EXCEPTION) {
            fixture.storage->fail_transaction_put = true;
            fixture.storage->throw_on_transaction_failure =
                failure == CreateFailure::TRANSACTION_EXCEPTION;
        } else if (failure == CreateFailure::UTXO_INPUT_DELETE ||
                   failure == CreateFailure::UTXO_INPUT_DELETE_EXCEPTION) {
            fixture.storage->failed_delete_key = input_key.get_key();
            fixture.storage->throw_on_delete_failure =
                failure == CreateFailure::UTXO_INPUT_DELETE_EXCEPTION;
        } else {
            fixture.storage->failed_put_key = output_key.get_key();
            fixture.storage->throw_on_put_failure =
                failure == CreateFailure::UTXO_OUTPUT_PUT_EXCEPTION;
            if (failure == CreateFailure::UTXO_OUTPUT_CLEANUP ||
                failure == CreateFailure::UTXO_OUTPUT_CLEANUP_EXCEPTION) {
                fixture.storage->failed_delete_key = output_key.get_key();
                fixture.storage->throw_on_delete_failure =
                    failure == CreateFailure::UTXO_OUTPUT_CLEANUP_EXCEPTION;
            } else if (failure == CreateFailure::UTXO_INPUT_RESTORE ||
                       failure == CreateFailure::UTXO_INPUT_RESTORE_EXCEPTION) {
                fixture.storage->secondary_failed_put_key = input_key.get_key();
                fixture.storage->throw_on_secondary_put_failure =
                    failure == CreateFailure::UTXO_INPUT_RESTORE_EXCEPTION;
            } else if (failure == CreateFailure::RAW_TRANSACTION_CLEANUP ||
                       failure == CreateFailure::RAW_TRANSACTION_CLEANUP_EXCEPTION) {
                fixture.storage->fail_transaction_delete = true;
                fixture.storage->throw_on_transaction_delete_failure =
                    failure == CreateFailure::RAW_TRANSACTION_CLEANUP_EXCEPTION;
            }
        }

        const auto before = geocoin::state::ChainStateTestAccess::utxo_snapshot(*fixture.state);
        expect(!fixture.state->apply_transaction(fixture.create, fixture.apply_height, false),
               "NFT_CREATE surfaces every persistent write failure");
        expect(geocoin::state::ChainStateTestAccess::utxo_snapshot(*fixture.state) == before,
               "failed NFT_CREATE preserves the complete UTXO state");
        expect(!fixture.state->nft_exists(nft_id) && !fixture.state->has_nft_id_key(nft_id),
               "failed NFT_CREATE leaves no registry or uniqueness entry");
        const bool nft_cleanup_unconfirmed =
            failure == CreateFailure::NFT_METADATA_CLEANUP ||
            failure == CreateFailure::NFT_METADATA_CLEANUP_EXCEPTION;
        const bool transaction_cleanup_unconfirmed =
            failure == CreateFailure::RAW_TRANSACTION_CLEANUP ||
            failure == CreateFailure::RAW_TRANSACTION_CLEANUP_EXCEPTION;
        expect(fixture.storage->has_metadata("nft:meta:" + id_hex) == nft_cleanup_unconfirmed &&
                   !fixture.storage->has_metadata("nft:owner:" + id_hex) &&
                   fixture.storage->has_transaction(fixture.create.get_hash()) ==
                       transaction_cleanup_unconfirmed,
               "failed NFT_CREATE either removes partial records or exposes incomplete cleanup");
        const bool input_restore_failed =
            failure == CreateFailure::UTXO_INPUT_RESTORE ||
            failure == CreateFailure::UTXO_INPUT_RESTORE_EXCEPTION;
        expect(fixture.storage->has_metadata(input_key.get_key()) == !input_restore_failed &&
                   !fixture.storage->has_metadata(output_key.get_key()),
               "failed NFT_CREATE exposes whether the persistent UTXO pre-state was restored");
        const bool cleanup_unconfirmed =
            failure == CreateFailure::UTXO_OUTPUT_CLEANUP ||
            failure == CreateFailure::UTXO_OUTPUT_CLEANUP_EXCEPTION ||
            input_restore_failed ||
            nft_cleanup_unconfirmed ||
            transaction_cleanup_unconfirmed;
        expect(geocoin::state::ChainStateTestAccess::derived_state_valid(*fixture.state) ==
                   !cleanup_unconfirmed,
               "NFT_CREATE keeps recoverable failures usable and closes on uncertain recovery");
        if (cleanup_unconfirmed) {
            expect(!fixture.state->validate_transaction(fixture.create),
                   "uncertain NFT_CREATE recovery blocks later validation until rebuild");
            expect(!fixture.state->apply_transaction(
                       fixture.create,
                       fixture.apply_height,
                       false),
                   "uncertain NFT_CREATE recovery blocks later state transitions until rebuild");
        }
    }

    enum class TransferFailure {
        OWNER,
        TRANSACTION,
        UTXO_INPUT_DELETE,
        UTXO_OUTPUT_PUT,
        UTXO_OUTPUT_PUT_EXCEPTION
    };
    for (const auto failure : {
             TransferFailure::OWNER,
             TransferFailure::TRANSACTION,
             TransferFailure::UTXO_INPUT_DELETE,
             TransferFailure::UTXO_OUTPUT_PUT,
             TransferFailure::UTXO_OUTPUT_PUT_EXCEPTION}) {
        auto fixture = make_fixture(static_cast<uint8_t>(0xa4 + static_cast<int>(failure) * 3));
        expect(fixture.state->apply_transaction(fixture.create, fixture.apply_height, false),
               "NFT transfer failure fixture creates NFT");
        fixture.state->set_height(fixture.apply_height);

        geocoin::wallet::Wallet owner_wallet;
        expect(owner_wallet.initialize(fixture.owner_key),
               "NFT transfer failure owner wallet initializes");
        geocoin::core::Transaction transfer;
        expect(owner_wallet.create_nft_transfer(
                   fixture.create.nft_create->nft_id,
                   fixture.recipient,
                   geocoin::consensus::params::MIN_TX_FEE,
                   fixture.state,
                   transfer),
               "NFT transfer failure transaction builds");

        geocoin::state::UTXO input_key;
        input_key.tx_hash = transfer.inputs.front().prev_tx_hash;
        input_key.vout = transfer.inputs.front().vout;
        geocoin::state::UTXO output_key;
        output_key.tx_hash = transfer.get_hash();
        output_key.vout = 0;
        if (failure == TransferFailure::TRANSACTION) {
            fixture.storage->fail_transaction_put = true;
        } else if (failure == TransferFailure::OWNER) {
            fixture.storage->failed_put_key =
                "nft:owner:" + hash_to_hex(fixture.create.nft_create->nft_id);
        } else if (failure == TransferFailure::UTXO_INPUT_DELETE) {
            fixture.storage->failed_delete_key = input_key.get_key();
        } else {
            fixture.storage->failed_put_key = output_key.get_key();
            fixture.storage->throw_on_put_failure =
                failure == TransferFailure::UTXO_OUTPUT_PUT_EXCEPTION;
        }
        const auto before = geocoin::state::ChainStateTestAccess::utxo_snapshot(*fixture.state);
        expect(!fixture.state->apply_transaction(transfer, fixture.apply_height + 1, false),
               "NFT_TRANSFER surfaces owner or transaction persistence failure");
        expect(fixture.state->get_nft_owner(fixture.create.nft_create->nft_id) == fixture.owner,
               "failed NFT_TRANSFER preserves the registry owner");
        expect(geocoin::state::ChainStateTestAccess::utxo_snapshot(*fixture.state) == before,
               "failed NFT_TRANSFER preserves the complete UTXO state");
        expect(!fixture.storage->has_transaction(transfer.get_hash()),
               "failed NFT_TRANSFER leaves no transaction record");
        expect(fixture.storage->has_metadata(input_key.get_key()) &&
                   !fixture.storage->has_metadata(output_key.get_key()),
               "failed NFT_TRANSFER restores the persistent UTXO pre-state");
        expect(geocoin::state::ChainStateTestAccess::derived_state_valid(*fixture.state),
               "recoverable NFT_TRANSFER persistence failure keeps state usable");
    }

    enum class CadastralTransferFailure { TRANSACTION, OWNER_DELETE, UTXO_OUTPUT_PUT };
    for (const auto failure : {
             CadastralTransferFailure::TRANSACTION,
             CadastralTransferFailure::OWNER_DELETE,
             CadastralTransferFailure::UTXO_OUTPUT_PUT}) {
        auto fixture = make_fixture(
            static_cast<uint8_t>(0xd8 + static_cast<int>(failure) * 3),
            true
        );
        const auto nft_id = fixture.create.nft_create->nft_id;
        const std::string id_hex = hash_to_hex(nft_id);
        expect(fixture.state->apply_transaction(fixture.create, fixture.apply_height, false),
               "cadastral transfer failure fixture creates NFT");
        fixture.state->set_height(fixture.apply_height);

        geocoin::wallet::Wallet owner_wallet;
        expect(owner_wallet.initialize(fixture.owner_key),
               "cadastral transfer failure owner wallet initializes");
        geocoin::core::Transaction transfer;
        expect(owner_wallet.create_nft_transfer(
                   nft_id,
                   fixture.recipient,
                   geocoin::consensus::params::MIN_TX_FEE,
                   fixture.state,
                   transfer),
               "cadastral transfer failure transaction builds");

        geocoin::state::UTXO input_key;
        input_key.tx_hash = transfer.inputs.front().prev_tx_hash;
        input_key.vout = transfer.inputs.front().vout;
        geocoin::state::UTXO output_key;
        output_key.tx_hash = transfer.get_hash();
        output_key.vout = 0;
        if (failure == CadastralTransferFailure::TRANSACTION) {
            fixture.storage->fail_transaction_put = true;
        } else if (failure == CadastralTransferFailure::OWNER_DELETE) {
            fixture.storage->failed_delete_key = "nft:owner:" + id_hex;
        } else {
            fixture.storage->failed_put_key = output_key.get_key();
        }

        const auto before = geocoin::state::ChainStateTestAccess::utxo_snapshot(*fixture.state);
        expect(!fixture.state->apply_transaction(transfer, fixture.apply_height + 1, false),
               "cadastral NFT_TRANSFER surfaces persistence and UTXO failures");
        expect(fixture.state->nft_exists(nft_id) &&
                   fixture.state->get_nft_owner(nft_id) == fixture.owner &&
                   fixture.state->get_nft_metadata(nft_id).cadastral.has_value(),
               "failed cadastral NFT_TRANSFER restores registry metadata and owner");
        expect(geocoin::state::ChainStateTestAccess::utxo_snapshot(*fixture.state) == before,
               "failed cadastral NFT_TRANSFER preserves the complete UTXO state");
        expect(fixture.storage->has_metadata("nft:meta:" + id_hex) &&
                   fixture.storage->has_metadata("nft:owner:" + id_hex) &&
                   !fixture.storage->has_transaction(transfer.get_hash()),
               "failed cadastral NFT_TRANSFER restores persistent NFT records");
        expect(fixture.storage->has_metadata(input_key.get_key()) &&
                   !fixture.storage->has_metadata(output_key.get_key()),
               "failed cadastral NFT_TRANSFER restores persistent UTXO records");
        expect(geocoin::state::ChainStateTestAccess::derived_state_valid(*fixture.state),
               "recoverable cadastral NFT_TRANSFER failure keeps state usable");
    }

    enum class BurnFailure {
        OWNER_DELETE,
        METADATA_DELETE,
        TRANSACTION,
        UTXO_INPUT_DELETE,
        UTXO_OUTPUT_PUT,
        UTXO_OUTPUT_PUT_EXCEPTION
    };
    for (const auto failure : {
             BurnFailure::OWNER_DELETE,
             BurnFailure::METADATA_DELETE,
             BurnFailure::TRANSACTION,
             BurnFailure::UTXO_INPUT_DELETE,
             BurnFailure::UTXO_OUTPUT_PUT,
             BurnFailure::UTXO_OUTPUT_PUT_EXCEPTION}) {
        auto fixture = make_fixture(static_cast<uint8_t>(0xc0 + static_cast<int>(failure) * 3));
        const auto nft_id = fixture.create.nft_create->nft_id;
        const std::string id_hex = hash_to_hex(nft_id);
        expect(fixture.state->apply_transaction(fixture.create, fixture.apply_height, false),
               "NFT burn failure fixture creates NFT");
        fixture.state->set_height(fixture.apply_height);

        geocoin::wallet::Wallet owner_wallet;
        expect(owner_wallet.initialize(fixture.owner_key),
               "NFT burn failure owner wallet initializes");
        geocoin::core::Transaction burn;
        expect(owner_wallet.create_nft_burn(
                   nft_id,
                   geocoin::consensus::params::MIN_TX_FEE,
                   fixture.state,
                   burn),
               "NFT burn failure transaction builds");

        geocoin::state::UTXO input_key;
        input_key.tx_hash = burn.inputs.front().prev_tx_hash;
        input_key.vout = burn.inputs.front().vout;
        geocoin::state::UTXO output_key;
        output_key.tx_hash = burn.get_hash();
        output_key.vout = 0;
        if (failure == BurnFailure::OWNER_DELETE) {
            fixture.storage->failed_delete_key = "nft:owner:" + id_hex;
        } else if (failure == BurnFailure::METADATA_DELETE) {
            fixture.storage->failed_delete_key = "nft:meta:" + id_hex;
        } else if (failure == BurnFailure::TRANSACTION) {
            fixture.storage->fail_transaction_put = true;
        } else if (failure == BurnFailure::UTXO_INPUT_DELETE) {
            fixture.storage->failed_delete_key = input_key.get_key();
        } else {
            fixture.storage->failed_put_key = output_key.get_key();
            fixture.storage->throw_on_put_failure =
                failure == BurnFailure::UTXO_OUTPUT_PUT_EXCEPTION;
        }
        const auto before = geocoin::state::ChainStateTestAccess::utxo_snapshot(*fixture.state);
        expect(!fixture.state->apply_transaction(burn, fixture.apply_height + 1, false),
               "NFT_BURN surfaces every persistent deletion failure");
        expect(fixture.state->nft_exists(nft_id) &&
                   fixture.state->get_nft_owner(nft_id) == fixture.owner,
               "failed NFT_BURN preserves registry metadata and owner");
        expect(geocoin::state::ChainStateTestAccess::utxo_snapshot(*fixture.state) == before,
               "failed NFT_BURN preserves the complete UTXO state");
        expect(fixture.storage->has_metadata("nft:meta:" + id_hex) &&
                   fixture.storage->has_metadata("nft:owner:" + id_hex) &&
                   !fixture.storage->has_transaction(burn.get_hash()),
               "failed NFT_BURN restores both persistent NFT halves");
        expect(fixture.storage->has_metadata(input_key.get_key()) &&
                   !fixture.storage->has_metadata(output_key.get_key()),
               "failed NFT_BURN restores the persistent UTXO pre-state");
        expect(geocoin::state::ChainStateTestAccess::derived_state_valid(*fixture.state),
               "recoverable NFT_BURN persistence failure keeps state usable");
    }

    for (const bool throw_on_delete : {false, true}) {
        auto fixture = make_fixture(static_cast<uint8_t>(throw_on_delete ? 0xf4 : 0xf1));
        const auto block = make_next_rewarded_block(
            *fixture.state,
            fixture.owner,
            {fixture.create}
        );
        expect(fixture.state->apply_block(block, true),
               "raw transaction deletion failure fixture applies NFT block");
        fixture.storage->fail_transaction_delete = true;
        fixture.storage->throw_on_transaction_delete_failure = throw_on_delete;
        expect(!fixture.state->revert_block(block),
               "NFT revert surfaces false and exceptional raw transaction deletion failures");
        expect(!geocoin::state::ChainStateTestAccess::derived_state_valid(*fixture.state),
               "incomplete NFT revert closes state transitions until rebuild");
        expect(!fixture.state->apply_transaction(
                   geocoin::core::Transaction::create_coinbase(
                       fixture.apply_height + 2,
                       fixture.owner,
                       geocoin::COIN),
                   fixture.apply_height + 2,
                   true),
               "incomplete NFT revert rejects later state transitions");
    }
}

void test_same_block_nft_transition_matrix_and_full_rollback()
{
    const geocoin::bytes owner_key = private_key_from_seed(0xb1);
    const geocoin::bytes recipient_key = private_key_from_seed(0xb2);
    geocoin::wallet::Wallet owner_wallet;
    geocoin::wallet::Wallet recipient_wallet;
    expect(owner_wallet.initialize(owner_key), "same-block NFT owner wallet initializes");
    expect(recipient_wallet.initialize(recipient_key),
           "same-block NFT recipient wallet initializes");
    const geocoin::Address owner = owner_wallet.get_address();
    const geocoin::Address recipient = recipient_wallet.get_address();

    const auto owner_funding = geocoin::core::Transaction::create_coinbase(
        1,
        owner,
        20 * geocoin::COIN
    );
    const auto recipient_funding = geocoin::core::Transaction::create_coinbase(
        2,
        recipient,
        20 * geocoin::COIN
    );
    const geocoin::Height base_height =
        geocoin::consensus::params::COINBASE_MATURITY + 20;
    const geocoin::Hash256 base_tip = hash_from_seed(0xb3);

    auto make_seeded_state = [&]() {
        auto state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        state->set_activation_manifest(
            geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));
        expect(state->apply_transaction(owner_funding, 1, true),
               "same-block NFT owner funding applies");
        expect(state->apply_transaction(recipient_funding, 2, true),
               "same-block NFT recipient funding applies");
        state->set_height(base_height);
        state->set_tip_hash(base_tip);
        return state;
    };

    // Build a dependent create -> transfer -> burn package on an equivalent
    // state. The target state below has not seen any of these transitions.
    auto builder = make_seeded_state();
    auto metadata = make_nft_metadata();
    metadata.name = "Same block lifecycle";
    metadata.content_hash.assign(64, 'd');
    metadata.creator = owner.str();
    metadata.block_height = base_height + 1;

    geocoin::core::Transaction create;
    expect(owner_wallet.create_nft_create(
               metadata,
               100,
               geocoin::consensus::params::MIN_TX_FEE,
               builder,
               create),
           "same-block NFT_CREATE builds");
    expect(builder->apply_transaction(create, base_height + 1, false),
           "builder applies same-block NFT_CREATE");
    builder->set_height(base_height + 1);

    geocoin::core::Transaction transfer;
    expect(owner_wallet.create_nft_transfer(
               create.nft_create->nft_id,
               recipient,
               geocoin::consensus::params::MIN_TX_FEE,
               builder,
               transfer),
           "same-block NFT_TRANSFER builds from the create output");
    expect(builder->apply_transaction(transfer, base_height + 2, false),
           "builder applies same-block NFT_TRANSFER");
    builder->set_height(base_height + 2);

    geocoin::core::Transaction burn;
    expect(recipient_wallet.create_nft_burn(
               create.nft_create->nft_id,
               geocoin::consensus::params::MIN_TX_FEE,
               builder,
               burn),
           "same-block NFT_BURN builds for the new owner");
    expect(builder->apply_transaction(burn, base_height + 3, false),
           "builder applies same-block NFT_BURN");

    auto target = make_seeded_state();
    const auto before = geocoin::state::ChainStateTestAccess::utxo_snapshot(*target);
    // All three wallet operations above pay exactly MIN_TX_FEE. A lookup in
    // the untouched target cannot resolve their same-block dependent inputs.
    const auto lifecycle_fees = 3 * geocoin::consensus::params::MIN_TX_FEE;
    const auto lifecycle_block = make_next_rewarded_block(
        *target,
        owner,
        {create, transfer, burn},
        lifecycle_fees
    );
    const geocoin::Hash256 nft_id = create.nft_create->nft_id;
    expect(target->apply_block(lifecycle_block, true),
           "one block applies NFT create, transfer and burn in dependency order");
    expect(!target->nft_exists(nft_id) && target->has_nft_id_key(nft_id),
           "same-block burn removes the NFT while retaining its id reservation");

    expect(target->revert_block(lifecycle_block),
           "same-block NFT lifecycle reverts as one atomic transition");
    expect(geocoin::state::ChainStateTestAccess::utxo_snapshot(*target) == before,
           "same-block NFT revert restores the complete UTXO set byte-for-byte");
    expect(!target->nft_exists(nft_id) && !target->has_nft_id_key(nft_id),
           "same-block NFT revert removes registry and uniqueness state");
    expect(target->get_transaction(create.get_hash()) == nullptr &&
               target->get_transaction(transfer.get_hash()) == nullptr &&
               target->get_transaction(burn.get_hash()) == nullptr,
           "same-block NFT revert removes raw transaction index records");
    expect(target->get_height() == base_height && target->get_tip_hash() == base_tip,
           "same-block NFT revert restores chain position");

    expect(target->apply_block(lifecycle_block, true),
           "the same valid NFT block can be applied again after a clean revert");
    expect(target->revert_block(lifecycle_block),
           "reapplied NFT lifecycle block reverts cleanly");

    auto rejected_target = make_seeded_state();
    const auto rejected_before =
        geocoin::state::ChainStateTestAccess::utxo_snapshot(*rejected_target);
    geocoin::core::Transaction invalid_tail;
    invalid_tail.version = 2;
    invalid_tail.type = geocoin::core::TransactionType::STANDARD;
    invalid_tail.timestamp = 999999;
    invalid_tail.inputs.push_back({hash_from_seed(0xb4), 0, {}, 0xffffffff, {}, {}});
    invalid_tail.outputs.push_back({
        geocoin::consensus::params::DUST_LIMIT,
        recipient,
        {}
    });
    const auto rejected_block = make_next_rewarded_block(
        *rejected_target,
        owner,
        {create, transfer, burn, invalid_tail},
        lifecycle_fees
    );
    geocoin::consensus::ValidationState rejected_state;
    expect(!rejected_target->apply_block(rejected_block, rejected_state, true) &&
               rejected_state.get_reject_reason() == "bad-txns-inputs-missing-or-spent",
           "invalid tail rejects a block after all three NFT transitions");
    expect(geocoin::state::ChainStateTestAccess::utxo_snapshot(*rejected_target) ==
               rejected_before,
           "failed NFT lifecycle block restores the complete UTXO set byte-for-byte");
    expect(!rejected_target->nft_exists(nft_id) &&
               !rejected_target->has_nft_id_key(nft_id),
           "failed NFT lifecycle block leaves no registry or uniqueness residue");
    expect(rejected_target->get_transaction(create.get_hash()) == nullptr &&
               rejected_target->get_transaction(transfer.get_hash()) == nullptr &&
               rejected_target->get_transaction(burn.get_hash()) == nullptr,
           "failed NFT lifecycle block removes all staged transaction records");
    expect(geocoin::state::ChainStateTestAccess::derived_state_valid(*rejected_target),
           "fully restored NFT lifecycle failure keeps state usable");
}

void test_nft_rocksdb_restart_lifecycle()
{
    TempDirectoryGuard temp(make_temp_dir("geocoin-nft-rocksdb-restart"));
    const auto db_path = temp.path() / "chainstate";

    const geocoin::bytes owner_key = private_key_from_seed(0xe1);
    const geocoin::bytes recipient_key = private_key_from_seed(0xe2);
    geocoin::wallet::Wallet owner_wallet;
    geocoin::wallet::Wallet recipient_wallet;
    expect(owner_wallet.initialize(owner_key), "restart NFT owner wallet initializes");
    expect(recipient_wallet.initialize(recipient_key),
           "restart NFT recipient wallet initializes");
    const geocoin::Address owner = owner_wallet.get_address();
    const geocoin::Address recipient = recipient_wallet.get_address();

    auto open_state = [&]() {
        auto storage = std::make_unique<geocoin::state::RocksDBStorage>(db_path.string());
        expect(storage->open(), "restart NFT RocksDB opens");
        auto state = std::make_shared<geocoin::state::ChainState>(std::move(storage));
        state->set_activation_manifest(
            geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));
        return state;
    };

    const geocoin::Height base_height =
        geocoin::consensus::params::COINBASE_MATURITY + 30;
    const geocoin::Hash256 base_tip = hash_from_seed(0xe3);
    const geocoin::Hash256 create_tip = hash_from_seed(0xe4);
    const geocoin::Hash256 transfer_tip = hash_from_seed(0xe5);
    const geocoin::Hash256 burn_tip = hash_from_seed(0xe6);

    auto metadata = make_nft_metadata();
    metadata.name = "RocksDB restart NFT";
    metadata.content_hash.assign(64, 'e');
    metadata.creator = owner.str();
    metadata.block_height = base_height + 1;

    geocoin::core::Transaction create;
    geocoin::core::Transaction transfer;
    geocoin::core::Transaction burn;
    geocoin::Hash256 nft_id{};
    std::map<std::string, geocoin::bytes> create_snapshot;
    std::map<std::string, geocoin::bytes> transfer_snapshot;
    std::map<std::string, geocoin::bytes> burn_snapshot;

    {
        auto state = open_state();
        geocoin::state::ChainStateTestAccess::set_foundation_config(*state, state->get_foundation_config());
        geocoin::state::ChainStateTestAccess::set_interest_pool_balance(*state, 0);
        const auto owner_funding = geocoin::core::Transaction::create_coinbase(
            1,
            owner,
            20 * geocoin::COIN
        );
        const auto recipient_funding = geocoin::core::Transaction::create_coinbase(
            2,
            recipient,
            20 * geocoin::COIN
        );
        expect(state->apply_transaction(owner_funding, 1, true),
               "restart NFT owner funding applies");
        expect(state->apply_transaction(recipient_funding, 2, true),
               "restart NFT recipient funding applies");
        state->set_height(base_height);
        state->set_tip_hash(base_tip);
        expect(state->get_storage()->put_metadata("chainstate_utxo_count", encode_u64_le(state->get_utxo_count())),
               "persist complete NFT test snapshot count");

        expect(owner_wallet.create_nft_create(
                   metadata,
                   250,
                   geocoin::consensus::params::MIN_TX_FEE,
                   state,
                   create),
               "restart NFT_CREATE builds");
        nft_id = create.nft_create->nft_id;
        expect(state->apply_transaction(create, base_height + 1, false),
               "restart NFT_CREATE applies");
        state->set_height(base_height + 1);
        state->set_tip_hash(create_tip);
        expect(state->get_storage()->put_metadata("chainstate_utxo_count", encode_u64_le(state->get_utxo_count())),
               "persist complete NFT test snapshot count");
        create_snapshot = geocoin::state::ChainStateTestAccess::utxo_snapshot(*state);
    }

    {
        auto state = open_state();
        expect(state->get_height() == base_height + 1 && state->get_tip_hash() == create_tip,
               "restart after NFT_CREATE restores chain position");
        expect(geocoin::state::ChainStateTestAccess::utxo_snapshot(*state) == create_snapshot,
               "restart after NFT_CREATE restores the exact UTXO snapshot");
        expect(state->nft_exists(nft_id) && state->get_nft_owner(nft_id) == owner,
               "restart after NFT_CREATE restores NFT ownership");
        expect(state->get_nft_metadata(nft_id).serialize() == metadata.serialize(),
               "restart after NFT_CREATE restores canonical metadata");
        expect(state->has_nft_id_key(nft_id) && state->get_transaction(create.get_hash()) != nullptr,
               "restart after NFT_CREATE restores uniqueness and transaction records");

        expect(owner_wallet.create_nft_transfer(
                   nft_id,
                   recipient,
                   geocoin::consensus::params::MIN_TX_FEE,
                   state,
                   transfer),
               "restart NFT_TRANSFER builds");
        expect(state->apply_transaction(transfer, base_height + 2, false),
               "restart NFT_TRANSFER applies");
        state->set_height(base_height + 2);
        state->set_tip_hash(transfer_tip);
        expect(state->get_storage()->put_metadata("chainstate_utxo_count", encode_u64_le(state->get_utxo_count())),
               "persist complete NFT test snapshot count");
        transfer_snapshot = geocoin::state::ChainStateTestAccess::utxo_snapshot(*state);
    }

    {
        auto state = open_state();
        expect(state->get_height() == base_height + 2 && state->get_tip_hash() == transfer_tip,
               "restart after NFT_TRANSFER restores chain position");
        expect(geocoin::state::ChainStateTestAccess::utxo_snapshot(*state) == transfer_snapshot,
               "restart after NFT_TRANSFER restores the exact UTXO snapshot");
        expect(state->nft_exists(nft_id) && state->get_nft_owner(nft_id) == recipient,
               "restart after NFT_TRANSFER restores the new owner");
        expect(state->get_nft_metadata(nft_id).serialize() == metadata.serialize(),
               "restart after NFT_TRANSFER preserves canonical metadata");
        expect(state->get_transaction(transfer.get_hash()) != nullptr,
               "restart after NFT_TRANSFER restores its transaction record");

        expect(recipient_wallet.create_nft_burn(
                   nft_id,
                   geocoin::consensus::params::MIN_TX_FEE,
                   state,
                   burn),
               "restart NFT_BURN builds");
        expect(state->apply_transaction(burn, base_height + 3, false),
               "restart NFT_BURN applies");
        state->set_height(base_height + 3);
        state->set_tip_hash(burn_tip);
        expect(state->get_storage()->put_metadata("chainstate_utxo_count", encode_u64_le(state->get_utxo_count())),
               "persist complete NFT test snapshot count");
        burn_snapshot = geocoin::state::ChainStateTestAccess::utxo_snapshot(*state);
    }

    {
        auto state = open_state();
        expect(state->get_height() == base_height + 3 && state->get_tip_hash() == burn_tip,
               "restart after NFT_BURN restores chain position");
        expect(geocoin::state::ChainStateTestAccess::utxo_snapshot(*state) == burn_snapshot,
               "restart after NFT_BURN restores the exact UTXO snapshot");
        expect(!state->nft_exists(nft_id) && state->has_nft_id_key(nft_id),
               "restart after NFT_BURN keeps the permanent id reservation only");
        expect(state->get_transaction(create.get_hash()) != nullptr &&
                   state->get_transaction(transfer.get_hash()) != nullptr &&
                   state->get_transaction(burn.get_hash()) != nullptr,
               "restart after NFT_BURN retains the complete raw transaction history");
        expect(geocoin::state::ChainStateTestAccess::derived_state_valid(*state),
               "all NFT restart stages retain a valid derived snapshot");
    }
}

void test_apply_block_auto_undo_for_sidechain_lifecycle()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::bytes owner_key = private_key_from_seed(0x41);
    const geocoin::Address owner = address_from_seed(0x41);
    const geocoin::Address recipient = address_from_seed(0x42);
    const geocoin::Address next_committee_member = address_from_seed(0x4b);

    const auto funding = geocoin::core::Transaction::create_coinbase(
        1,
        owner,
        1000 * geocoin::COIN,
        0,
        state.get_foundation_address()
    );
    expect(state.apply_transaction(funding, 1, true), "sidechain block undo funding applies");
    state.set_height(geocoin::consensus::params::COINBASE_MATURITY + 1);
    state.set_tip_hash(hash_from_seed(0x43));

    geocoin::state::UTXO funding_utxo;
    expect(state.get_utxo(funding.get_hash(), 0, funding_utxo), "sidechain block undo funding UTXO exists");

    geocoin::core::Transaction lock;
    lock.version = 2;
    lock.type = geocoin::core::TransactionType::LOCK_DEPOSIT;
    lock.sidechain_id = hash_from_seed(0x44);
    lock.sidechain_owner = owner;
    lock.sidechain_amount = 200 * geocoin::COIN;
    lock.sidechain_unlock_height = state.get_height() + 1;
    lock.inputs.push_back({funding_utxo.tx_hash, funding_utxo.vout, {}, 0xffffffff, {}, {}});
    lock.outputs.push_back({funding_utxo.value - lock.sidechain_amount - geocoin::consensus::params::MIN_TX_FEE, owner, {}});
    lock.sidechain_signature = sign_sidechain_payload(lock, owner_key);
    lock.sign_input(0, owner_key);

    const auto lock_block = make_next_rewarded_block(state, owner, {lock});
    expect(state.apply_block(lock_block, true), "LOCK_DEPOSIT block applies");
    expect(state.has_sidechain_deposit_key(lock.sidechain_id), "LOCK_DEPOSIT block creates deposit");
    expect(state.revert_block(lock_block), "LOCK_DEPOSIT block reverts");
    expect(!state.has_sidechain_deposit_key(lock.sidechain_id), "LOCK_DEPOSIT undo removes created deposit");
    expect(state.has_utxo(funding_utxo.tx_hash, funding_utxo.vout),
           "LOCK_DEPOSIT undo restores spent funding UTXO");

    const geocoin::Hash256 spend_id = hash_from_seed(0x45);
    restore_deposit_with_revert(state, spend_id, owner, 50 * geocoin::COIN, 0);
    state.set_height(10);
    state.set_tip_hash(hash_from_seed(0x46));

    geocoin::core::Transaction spend;
    spend.version = 2;
    spend.type = geocoin::core::TransactionType::SPEND_DEPOSIT;
    spend.sidechain_id = spend_id;
    spend.sidechain_owner = owner;
    spend.sidechain_amount = 15 * geocoin::COIN;
    spend.outputs.push_back({spend.sidechain_amount, recipient, {}});
    spend.sidechain_signature = sign_sidechain_payload(spend, owner_key);
    const auto spend_block = make_next_rewarded_block(state, owner, {spend});
    expect(state.apply_block(spend_block, true), "SPEND_DEPOSIT block applies");
    geocoin::state::SidechainDeposit spend_deposit;
    expect(state.get_sidechain_deposit(spend_id, spend_deposit), "SPEND_DEPOSIT leaves deposit");
    expect(spend_deposit.amount == 35 * geocoin::COIN, "SPEND_DEPOSIT reduces deposit");
    expect(state.revert_block(spend_block), "SPEND_DEPOSIT block reverts");
    expect(state.get_sidechain_deposit(spend_id, spend_deposit), "SPEND_DEPOSIT undo restores deposit");
    expect(spend_deposit.amount == 50 * geocoin::COIN, "SPEND_DEPOSIT undo restores amount");
    expect(!state.has_utxo(spend.get_hash(), 0), "SPEND_DEPOSIT undo removes payout UTXO");

    const geocoin::Hash256 unlock_id = hash_from_seed(0x47);
    restore_deposit_with_revert(state, unlock_id, owner, 60 * geocoin::COIN, 0);
    state.set_height(20);
    state.set_tip_hash(hash_from_seed(0x48));

    geocoin::core::Transaction unlock;
    unlock.version = 2;
    unlock.type = geocoin::core::TransactionType::UNLOCK_DEPOSIT;
    unlock.sidechain_id = unlock_id;
    unlock.sidechain_owner = owner;
    unlock.outputs.push_back({60 * geocoin::COIN, recipient, {}});
    unlock.sidechain_signature = sign_sidechain_payload(unlock, owner_key);
    const auto unlock_block = make_next_rewarded_block(state, owner, {unlock});
    expect(state.apply_block(unlock_block, true), "UNLOCK_DEPOSIT block applies");
    expect(!state.has_sidechain_deposit_key(unlock_id), "UNLOCK_DEPOSIT removes deposit");
    expect(state.revert_block(unlock_block), "UNLOCK_DEPOSIT block reverts");
    expect(state.get_sidechain_deposit(unlock_id, spend_deposit), "UNLOCK_DEPOSIT undo restores deposit");
    expect(spend_deposit.amount == 60 * geocoin::COIN, "UNLOCK_DEPOSIT undo restores amount");

    const geocoin::Hash256 issue_id = hash_from_seed(0x49);
    restore_deposit_with_revert(state, issue_id, owner, 500 * geocoin::COIN, 0);
    state.set_height(30);
    state.set_tip_hash(hash_from_seed(0x4a));

    geocoin::core::Transaction issue;
    issue.version = 2;
    issue.type = geocoin::core::TransactionType::ISSUE_SIDECHAIN;
    issue.sidechain_id = issue_id;
    issue.sidechain_owner = owner;
    issue.sidechain_checkpoint = issue_id;
    issue.sidechain_token_symbol = "UND";
    issue.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_checkpoint_policy_id = 2;
    issue.sidechain_checkpoint_version = 2;
    issue.sidechain_committee_m = 1;
    issue.sidechain_committee_n = 1;
    issue.sidechain_committee_members = {owner};
    issue.sidechain_committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, issue.sidechain_committee_members);
    issue.sidechain_signature = sign_sidechain_payload(issue, owner_key);

    const auto issue_block = make_next_rewarded_block(state, owner, {issue});
    expect(state.apply_block(issue_block, true), "ISSUE_SIDECHAIN block applies");
    expect(state.has_sidechain_registry_key(issue_id), "ISSUE_SIDECHAIN creates registry");
    expect(state.revert_block(issue_block), "ISSUE_SIDECHAIN block reverts");
    expect(!state.has_sidechain_registry_key(issue_id), "ISSUE_SIDECHAIN undo removes registry");

    auto reset_chain_tip = [&](geocoin::Height height, uint8_t seed) {
        state.set_height(height);
        state.set_tip_hash(hash_from_seed(seed));
    };

    const geocoin::Hash256 mint_id = hash_from_seed(0x4c);
    restore_deposit_with_revert(state, mint_id, owner, 2000 * geocoin::COIN, 0);
    restore_sidechain_registry_with_revert(
        state,
        mint_id,
        owner,
        5000 * geocoin::COIN,
        100 * geocoin::COIN
    );
    reset_chain_tip(40, 0x4d);

    geocoin::core::Transaction mint;
    mint.version = 2;
    mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
    mint.sidechain_owner = owner;
    mint.sidechain_id = mint_id;
    mint.sidechain_amount = 20 * geocoin::COIN;
    mint.sidechain_committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner});
    mint.sidechain_committee_signatures.push_back(sign_sidechain_payload(mint, owner_key));
    const auto mint_block = make_next_rewarded_block(state, owner, {mint});
    expect(state.apply_block(mint_block, true), "MINT_SIDECHAIN block applies");
    geocoin::state::SidechainRegistryEntry registry;
    expect(state.get_sidechain_registry(mint_id, registry), "MINT_SIDECHAIN registry remains readable");
    expect(registry.total_issued == 120 * geocoin::COIN, "MINT_SIDECHAIN increments issued supply");
    expect(state.revert_block(mint_block), "MINT_SIDECHAIN block reverts");
    expect(state.get_sidechain_registry(mint_id, registry), "MINT_SIDECHAIN undo restores registry");
    expect(registry.total_issued == 100 * geocoin::COIN, "MINT_SIDECHAIN undo restores issued supply");

    const geocoin::Hash256 redeem_id = hash_from_seed(0x4e);
    restore_deposit_with_revert(state, redeem_id, owner, 2000 * geocoin::COIN, 0);
    restore_sidechain_registry_with_revert(
        state,
        redeem_id,
        owner,
        5000 * geocoin::COIN,
        100 * geocoin::COIN
    );
    reset_chain_tip(50, 0x4f);

    geocoin::core::Transaction redeem;
    redeem.version = 2;
    redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
    redeem.sidechain_owner = owner;
    redeem.sidechain_id = redeem_id;
    redeem.sidechain_amount = 25 * geocoin::COIN;
    redeem.outputs.push_back({redeem.sidechain_amount, recipient, {}});
    redeem.sidechain_committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner});
    redeem.sidechain_committee_signatures.push_back(sign_sidechain_payload(redeem, owner_key));
    const auto redeem_block = make_next_rewarded_block(state, owner, {redeem});
    expect(state.apply_block(redeem_block, true), "REDEEM_SIDECHAIN block applies");
    expect(state.get_sidechain_registry(redeem_id, registry), "REDEEM_SIDECHAIN registry remains readable");
    expect(registry.total_issued == 75 * geocoin::COIN, "REDEEM_SIDECHAIN decrements issued supply");
    expect(state.has_utxo(redeem.get_hash(), 0), "REDEEM_SIDECHAIN creates payout UTXO");
    expect(state.revert_block(redeem_block), "REDEEM_SIDECHAIN block reverts");
    expect(state.get_sidechain_registry(redeem_id, registry), "REDEEM_SIDECHAIN undo restores registry");
    expect(registry.total_issued == 100 * geocoin::COIN, "REDEEM_SIDECHAIN undo restores issued supply");
    expect(!state.has_utxo(redeem.get_hash(), 0), "REDEEM_SIDECHAIN undo removes payout UTXO");

    const geocoin::Hash256 batch_id = hash_from_seed(0x50);
    restore_deposit_with_revert(state, batch_id, owner, 2000 * geocoin::COIN, 0);
    restore_sidechain_registry_with_revert(
        state,
        batch_id,
        owner,
        5000 * geocoin::COIN,
        600 * geocoin::COIN
    );
    reset_chain_tip(100, 0x51);

    const auto checkpoint_proof =
        make_checkpoint_proof_v2(batch_id, 23, amount_to_u64(600 * geocoin::COIN));
    geocoin::core::BatchCheckpointItem item;
    item.sidechain_id = batch_id;
    item.checkpoint = checkpoint_proof.checkpoint;
    item.checkpoint_height = 23;
    item.fee = geocoin::consensus::calculate_sidechain_checkpoint_min_fee(state.get_height() + 1, 11);
    item.version = 2;
    item.proof = checkpoint_proof.proof;
    item.committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner});
    item.committee_signatures.push_back(
        sign_bytes_payload(geocoin::core::build_sidechain_batch_checkpoint_payload_for_signing(item), owner_key)
    );
    geocoin::core::Transaction batch;
    batch.version = 2;
    batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    batch.sidechain_batch_checkpoints = {item};
    const auto batch_block = make_next_rewarded_block(state, owner, {batch});
    expect(state.apply_block(batch_block, true), "BATCH_SIDECHAIN_CHECKPOINT block applies");
    geocoin::state::SidechainDeposit deposit;
    expect(state.get_sidechain_deposit(batch_id, deposit), "BATCH_SIDECHAIN_CHECKPOINT deposit remains readable");
    expect(deposit.checkpoint == item.checkpoint &&
               deposit.last_sidechain_checkpoint_height == item.checkpoint_height,
           "BATCH_SIDECHAIN_CHECKPOINT updates checkpoint metadata");
    expect(state.revert_block(batch_block), "BATCH_SIDECHAIN_CHECKPOINT block reverts");
    expect(state.get_sidechain_deposit(batch_id, deposit), "BATCH_SIDECHAIN_CHECKPOINT undo restores deposit");
    expect(deposit.last_checkpoint_height == 11 &&
               deposit.last_sidechain_checkpoint_height == 22 &&
               deposit.amount == 2000 * geocoin::COIN,
           "BATCH_SIDECHAIN_CHECKPOINT undo restores checkpoint metadata");

    const geocoin::Hash256 slash_id = hash_from_seed(0x58);
    restore_deposit_with_revert(state, slash_id, owner, 2000 * geocoin::COIN, 0);
    restore_sidechain_registry_with_revert(
        state,
        slash_id,
        owner,
        5000 * geocoin::COIN,
        1000 * geocoin::COIN
    );
    reset_chain_tip(105, 0x59);

    auto store_single_checkpoint = [&](const geocoin::Hash256& id,
                                       const geocoin::Hash256& checkpoint,
                                       geocoin::Height checkpoint_height) {
        geocoin::core::Transaction checkpoint_tx;
        checkpoint_tx.type = geocoin::core::TransactionType::SIDECHAIN_CHECKPOINT;
        checkpoint_tx.sidechain_id = id;
        checkpoint_tx.sidechain_checkpoint = checkpoint;
        checkpoint_tx.sidechain_checkpoint_height = checkpoint_height;
        const geocoin::Hash256 txid = checkpoint_tx.get_hash();
        expect(state.get_storage()->put_transaction(txid, checkpoint_tx),
               "SLASH_DEPOSIT fraud ref checkpoint stores");
        return txid;
    };

    geocoin::core::Transaction slash;
    slash.version = 2;
    slash.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
    slash.sidechain_owner = owner;
    slash.sidechain_id = slash_id;
    slash.sidechain_amount = 30 * geocoin::COIN;
    slash.outputs.push_back({10 * geocoin::COIN, recipient, {}});
    slash.sidechain_fraud_checkpoint_a =
        store_single_checkpoint(slash_id, hash_from_seed(0x5a), 17);
    slash.sidechain_fraud_checkpoint_index_a = 0xFFFF;
    slash.sidechain_fraud_checkpoint_b =
        store_single_checkpoint(slash_id, hash_from_seed(0x5b), 17);
    slash.sidechain_fraud_checkpoint_index_b = 0xFFFF;
    slash.sidechain_committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner});
    slash.sidechain_committee_signatures.push_back(sign_sidechain_payload(slash, owner_key));
    const auto slash_block = make_next_rewarded_block(state, owner, {slash});
    expect(state.apply_block(slash_block, true), "SLASH_DEPOSIT block applies");
    expect(state.get_sidechain_deposit(slash_id, deposit), "SLASH_DEPOSIT deposit remains readable");
    expect(deposit.amount == 1970 * geocoin::COIN, "SLASH_DEPOSIT block reduces deposit amount");
    geocoin::state::SidechainRegistryEntry slash_registry;
    expect(state.get_sidechain_registry(slash_id, slash_registry) &&
               slash_registry.issuance_limit ==
                   1970 * geocoin::COIN *
                       static_cast<geocoin::Amount>(geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER),
           "SLASH_DEPOSIT recomputes issuance limit from remaining deposit");
    expect(state.has_utxo(slash.get_hash(), 0), "SLASH_DEPOSIT block creates payout UTXO");
    expect(state.revert_block(slash_block), "SLASH_DEPOSIT block reverts");
    expect(state.get_sidechain_deposit(slash_id, deposit), "SLASH_DEPOSIT undo restores deposit");
    expect(deposit.amount == 2000 * geocoin::COIN, "SLASH_DEPOSIT undo restores deposit amount");
    expect(state.get_sidechain_registry(slash_id, slash_registry) &&
               slash_registry.issuance_limit == 5000 * geocoin::COIN,
           "SLASH_DEPOSIT undo restores issuance limit");
    expect(!state.has_utxo(slash.get_hash(), 0), "SLASH_DEPOSIT undo removes payout UTXO");

    const geocoin::Hash256 update_id = hash_from_seed(0x52);
    restore_sidechain_registry_with_revert(
        state,
        update_id,
        owner,
        5000 * geocoin::COIN,
        0
    );
    reset_chain_tip(110, 0x53);

    geocoin::core::Transaction update_committee;
    update_committee.version = 2;
    update_committee.type = geocoin::core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE;
    update_committee.sidechain_owner = owner;
    update_committee.sidechain_id = update_id;
    update_committee.sidechain_committee_m = 1;
    update_committee.sidechain_committee_n = 1;
    update_committee.sidechain_committee_members = {next_committee_member};
    update_committee.sidechain_committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, {next_committee_member});
    update_committee.sidechain_committee_signatures.push_back(
        sign_bytes_payload(
            geocoin::core::build_sidechain_committee_update_payload_for_signing(update_committee),
            owner_key
        )
    );
    const auto update_block = make_next_rewarded_block(state, owner, {update_committee});
    expect(state.apply_block(update_block, true), "UPDATE_SIDECHAIN_COMMITTEE block applies");
    expect(state.get_sidechain_registry(update_id, registry), "UPDATE_SIDECHAIN_COMMITTEE registry remains readable");
    expect(registry.committee_id == update_committee.sidechain_committee_id,
           "UPDATE_SIDECHAIN_COMMITTEE stores new committee");
    expect(state.revert_block(update_block), "UPDATE_SIDECHAIN_COMMITTEE block reverts");
    expect(state.get_sidechain_registry(update_id, registry), "UPDATE_SIDECHAIN_COMMITTEE undo restores registry");
    expect(registry.committee_id ==
               geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner}),
           "UPDATE_SIDECHAIN_COMMITTEE undo restores old committee");

    const geocoin::Hash256 decommission_id = hash_from_seed(0x54);
    restore_deposit_with_revert(state, decommission_id, owner, 2000 * geocoin::COIN, 0);
    restore_sidechain_registry_with_revert(
        state,
        decommission_id,
        owner,
        5000 * geocoin::COIN,
        0
    );
    reset_chain_tip(120, 0x55);

    geocoin::core::Transaction decommission;
    decommission.version = 2;
    decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
    decommission.sidechain_id = decommission_id;
    decommission.sidechain_owner = owner;
    authorize_decommission(decommission, owner_key, owner_key, owner_committee_id(owner));
    const auto decommission_block = make_next_rewarded_block(state, owner, {decommission});
    expect(state.apply_block(decommission_block, true), "DECOMMISSION_SIDECHAIN block applies");
    expect(state.get_sidechain_registry(decommission_id, registry) &&
               registry.status == geocoin::state::SidechainStatus::DECOMMISSIONING,
           "DECOMMISSION_SIDECHAIN keeps slashable registry during challenge");

    const auto challenge_checkpoint_proof = make_checkpoint_proof_v2(
        decommission_id,
        23,
        amount_to_u64(0)
    );
    geocoin::core::BatchCheckpointItem challenge_item;
    challenge_item.sidechain_id = decommission_id;
    challenge_item.checkpoint = challenge_checkpoint_proof.checkpoint;
    challenge_item.checkpoint_height = 23;
    challenge_item.fee = geocoin::consensus::calculate_sidechain_checkpoint_min_fee(
        state.get_height() + 1,
        11
    );
    challenge_item.version = 2;
    challenge_item.proof = challenge_checkpoint_proof.proof;
    challenge_item.committee_id = owner_committee_id(owner);
    challenge_item.committee_signatures.push_back(
        sign_bytes_payload(
            geocoin::core::build_sidechain_batch_checkpoint_payload_for_signing(challenge_item),
            owner_key
        )
    );
    geocoin::core::Transaction challenge_checkpoint;
    challenge_checkpoint.version = 2;
    challenge_checkpoint.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    challenge_checkpoint.sidechain_batch_checkpoints = {challenge_item};
    const auto challenge_checkpoint_block =
        make_next_rewarded_block(state, owner, {challenge_checkpoint});
    expect(state.apply_block(challenge_checkpoint_block, true),
           "BATCH_SIDECHAIN_CHECKPOINT remains valid during decommission challenge");
    expect(state.get_sidechain_deposit(decommission_id, deposit) &&
               deposit.checkpoint == challenge_item.checkpoint,
           "challenge checkpoint updates decommissioning deposit");
    expect(state.revert_block(challenge_checkpoint_block),
           "challenge BATCH_SIDECHAIN_CHECKPOINT block reverts");
    expect(state.get_sidechain_deposit(decommission_id, deposit) &&
               deposit.amount == 2000 * geocoin::COIN,
           "challenge checkpoint undo restores deposit");

    geocoin::core::Transaction challenge_slash;
    challenge_slash.version = 2;
    challenge_slash.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
    challenge_slash.sidechain_owner = owner;
    challenge_slash.sidechain_id = decommission_id;
    challenge_slash.sidechain_amount = 30 * geocoin::COIN;
    challenge_slash.outputs.push_back({10 * geocoin::COIN, recipient, {}});
    challenge_slash.sidechain_fraud_checkpoint_a =
        store_single_checkpoint(decommission_id, hash_from_seed(0x5c), 19);
    challenge_slash.sidechain_fraud_checkpoint_index_a = 0xFFFF;
    challenge_slash.sidechain_fraud_checkpoint_b =
        store_single_checkpoint(decommission_id, hash_from_seed(0x5d), 19);
    challenge_slash.sidechain_fraud_checkpoint_index_b = 0xFFFF;
    challenge_slash.sidechain_committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, {owner});
    challenge_slash.sidechain_committee_signatures.push_back(
        sign_sidechain_payload(challenge_slash, owner_key)
    );
    const auto challenge_slash_block =
        make_next_rewarded_block(state, owner, {challenge_slash});
    expect(state.apply_block(challenge_slash_block, true),
           "SLASH_DEPOSIT remains valid during decommission challenge");
    expect(state.get_sidechain_deposit(decommission_id, deposit) &&
               deposit.amount == 1970 * geocoin::COIN,
           "challenge slash reduces decommissioning deposit");
    expect(state.get_sidechain_registry(decommission_id, registry) &&
               registry.status == geocoin::state::SidechainStatus::DECOMMISSIONING,
           "challenge slash preserves decommissioning registry");
    expect(state.revert_block(challenge_slash_block),
           "challenge SLASH_DEPOSIT block reverts");
    expect(state.get_sidechain_deposit(decommission_id, deposit) &&
               deposit.amount == 2000 * geocoin::COIN,
           "challenge slash undo restores deposit");
    expect(state.revert_block(decommission_block), "DECOMMISSION_SIDECHAIN block reverts");
    expect(state.get_sidechain_registry(decommission_id, registry) &&
               registry.status == geocoin::state::SidechainStatus::ACTIVE &&
               registry.decommission_height == 0,
           "DECOMMISSION_SIDECHAIN undo restores active registry");
}

void test_batch_checkpoint_proof_v2_validation_guards()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::Address owner = address_from_seed(0x51);
    const geocoin::Hash256 sidechain_id = hash_from_seed(0x52);
    restore_deposit_with_revert(state, sidechain_id, owner, 2000 * geocoin::COIN, 0);
    restore_sidechain_registry_with_revert(
        state,
        sidechain_id,
        owner,
        5000 * geocoin::COIN,
        500 * geocoin::COIN
    );
    state.set_height(100);

    auto make_item = [&]() {
        const auto proof = make_checkpoint_proof_v2(sidechain_id, 23, amount_to_u64(500 * geocoin::COIN));
        geocoin::core::BatchCheckpointItem item;
        item.sidechain_id = sidechain_id;
        item.checkpoint = proof.checkpoint;
        item.checkpoint_height = 23;
        item.fee = geocoin::consensus::calculate_sidechain_checkpoint_min_fee(state.get_height() + 1, 11);
        item.version = 2;
        item.committee_id = geocoin::consensus::compute_sidechain_committee_id(
            1,
            1,
            std::vector<geocoin::Address>{owner}
        );
        item.proof = proof.proof;
        return item;
    };

    auto expect_batch_rejected = [&](geocoin::core::BatchCheckpointItem item, const char* message) {
        geocoin::core::Transaction batch;
        batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        batch.sidechain_batch_checkpoints = {std::move(item)};
        expect(!state.validate_transaction(batch), message);
    };

    auto item = make_item();
    item.proof = {'S', 'C'};
    expect_batch_rejected(item, "BATCH checkpoint proof rejects undersized payload");

    item = make_item();
    item.proof[0] = 'X';
    expect_batch_rejected(item, "BATCH checkpoint proof rejects bad magic");

    item = make_item();
    item.proof[4] = 2;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects unsupported version");

    item = make_item();
    item.proof[5] = 0;
    item.proof.resize(60);
    expect_batch_rejected(item, "BATCH checkpoint proof rejects empty merkle path that does not match state_root");

    item = make_item();
    item.proof[5] = 0;
    item.proof.resize(60);
    item.proof[56] = 1;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects empty merkle path with nonzero leaf_index");

    item = make_item();
    item.proof[5] = static_cast<geocoin::byte>(geocoin::consensus::params::MAX_BATCH_CHECKPOINT_MERKLE_DEPTH + 1);
    item.proof.resize(60 + (static_cast<size_t>(item.proof[5]) * 32), 0);
    expect_batch_rejected(item, "BATCH checkpoint proof rejects excessive merkle depth");

    item = make_item();
    item.proof[6] = 1;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects nonzero reserved byte");

    item = make_item();
    item.proof.push_back(0);
    expect_batch_rejected(item, "BATCH checkpoint proof rejects trailing data");

    item = make_item();
    const auto zero_height = make_checkpoint_proof_v2(sidechain_id, 0, amount_to_u64(600 * geocoin::COIN));
    item.proof = zero_height.proof;
    item.checkpoint = zero_height.checkpoint;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects zero parsed height");

    item = make_item();
    const auto wrong_height = make_checkpoint_proof_v2(sidechain_id, 24, amount_to_u64(600 * geocoin::COIN));
    item.proof = wrong_height.proof;
    item.checkpoint = wrong_height.checkpoint;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects height mismatch");

    item = make_item();
    const auto excessive_supply = make_checkpoint_proof_v2(
        sidechain_id,
        23,
        static_cast<uint64_t>(std::numeric_limits<geocoin::Amount>::max()) + 1ULL
    );
    item.proof = excessive_supply.proof;
    item.checkpoint = excessive_supply.checkpoint;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects supply above Amount max");

    item = make_item();
    const auto above_limit = make_checkpoint_proof_v2(sidechain_id, 23, amount_to_u64(5001 * geocoin::COIN));
    item.proof = above_limit.proof;
    item.checkpoint = above_limit.checkpoint;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects supply above issuance limit");

    item = make_item();
    const auto mismatched_supply = make_checkpoint_proof_v2(
        sidechain_id,
        23,
        amount_to_u64(600 * geocoin::COIN)
    );
    item.proof = mismatched_supply.proof;
    item.checkpoint = mismatched_supply.checkpoint;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects supply that does not match total_issued");

    item = make_item();
    const auto bad_index = make_checkpoint_proof_v2(sidechain_id, 23, amount_to_u64(500 * geocoin::COIN), 2);
    item.proof = bad_index.proof;
    item.checkpoint = bad_index.checkpoint;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects leaf index outside path range");

    item = make_item();
    item.proof.back() ^= 0x01;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects merkle root mismatch");

    item = make_item();
    item.checkpoint[0] ^= 0x01;
    expect_batch_rejected(item, "BATCH checkpoint proof rejects checkpoint commitment mismatch");

    expect_batch_rejected(make_item(), "BATCH checkpoint proof reaches committee signature guard");

    const auto single_leaf = make_checkpoint_proof_v2_single_leaf(
        sidechain_id,
        23,
        amount_to_u64(500 * geocoin::COIN)
    );
    geocoin::core::BatchCheckpointItem single_item;
    single_item.sidechain_id = sidechain_id;
    single_item.checkpoint = single_leaf.checkpoint;
    single_item.checkpoint_height = 23;
    single_item.fee = geocoin::consensus::calculate_sidechain_checkpoint_min_fee(state.get_height() + 1, 11);
    single_item.version = 2;
    single_item.committee_id = geocoin::consensus::compute_sidechain_committee_id(
        1,
        1,
        std::vector<geocoin::Address>{owner}
    );
    single_item.proof = single_leaf.proof;
    single_item.committee_signatures.push_back(sign_bytes_payload(
        geocoin::core::build_sidechain_batch_checkpoint_payload_for_signing(single_item),
        private_key_from_seed(0x51)
    ));
    geocoin::core::Transaction single_batch;
    single_batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    single_batch.sidechain_batch_checkpoints = {single_item};
    expect(state.validate_transaction(single_batch),
           "BATCH checkpoint proof accepts a single-leaf merkle path");
}

void test_interest_distribution_apply_and_revert()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::Address miner("GEOinterest-miner");
    const geocoin::Height activation = geocoin::consensus::params::INTEREST_POOL_ACTIVATION_HEIGHT;
    const geocoin::Height due_height =
        activation + geocoin::consensus::params::INTEREST_DISTRIBUTION_INTERVAL;

    geocoin::core::Transaction premature_distribution;
    expect(!state.create_interest_distribution_transaction(due_height, premature_distribution),
           "interest distribution is unavailable without pool balance");

    const auto activation_prev = hash_from_seed(0x90);
    state.set_height(activation - 1);
    state.set_tip_hash(activation_prev);

    const geocoin::Amount activation_reward =
        geocoin::consensus::ProofOfWork::calculate_block_reward(activation);
    const geocoin::Amount activation_cut =
        geocoin::consensus::calculate_foundation_reward_cut(activation_reward);
    const auto activation_coinbase =
        geocoin::core::Transaction::create_coinbase(activation, miner, activation_reward);
    const auto activation_block = make_block(activation, activation_prev, {activation_coinbase});
    expect(state.apply_block(activation_block, true), "activation block applies without PoW verification");
    expect(state.get_interest_pool_balance() == activation_cut,
           "activation block redirects foundation cut into interest pool");

    geocoin::core::Transaction distribution;
    expect(state.create_interest_distribution_transaction(due_height, distribution),
           "due height creates interest distribution transaction");
    expect(distribution.type == geocoin::core::TransactionType::INTEREST_DISTRIBUTION,
           "interest distribution transaction has correct type");
    expect(distribution.outputs.size() == 1, "single holder receives one interest output");
    expect(distribution.outputs[0].address == miner, "interest output goes to holder");
    expect(distribution.outputs[0].value == activation_cut, "interest output drains previous pool");

    state.set_height(due_height - 1);
    const auto due_prev = state.get_tip_hash();
    const geocoin::Amount due_reward =
        geocoin::consensus::ProofOfWork::calculate_block_reward(due_height);
    const geocoin::Amount due_cut =
        geocoin::consensus::calculate_foundation_reward_cut(due_reward);
    const auto due_coinbase =
        geocoin::core::Transaction::create_coinbase(due_height, miner, due_reward);
    const auto due_block = make_block(due_height, due_prev, {due_coinbase, distribution});

    expect(state.validate_transaction(distribution), "interest distribution validates at due height");
    expect(state.apply_block(due_block, true), "due block with interest distribution applies");
    expect(state.get_interest_pool_balance() == due_cut,
           "due block leaves only its new foundation cut in the pool");
    expect(state.has_utxo(distribution.get_hash(), 0),
           "interest distribution creates spendable UTXO");

    expect(state.revert_block(due_block), "due block reverts");
    expect(state.get_height() == due_height - 1, "revert restores previous due height");
    expect(state.get_tip_hash() == due_prev, "revert restores previous due tip");
    expect(state.get_interest_pool_balance() == activation_cut,
           "revert restores pre-distribution interest pool balance");
    expect(!state.has_utxo(distribution.get_hash(), 0),
           "revert removes interest distribution UTXO");
}

void test_interest_distribution_output_builder_guards()
{
    // This test deliberately injects an impossible negative UTXO through the
    // friend test accessor.  Keep it in-memory: persisted UTXOs now reject
    // negative values at the serialization boundary.
    geocoin::state::ChainState state;
    std::vector<geocoin::core::TxOutput> outputs;
    geocoin::Amount distributed_total = 99;

    expect(!geocoin::state::ChainStateTestAccess::build_interest_distribution_outputs(
               state,
               10,
               outputs,
               distributed_total),
           "interest output builder rejects empty pool");
    expect(outputs.empty() && distributed_total == 0, "interest output builder clears outputs on reject");

    geocoin::state::ChainStateTestAccess::set_interest_pool_balance(
        state,
        geocoin::consensus::params::DUST_LIMIT - 1
    );
    expect(!geocoin::state::ChainStateTestAccess::build_interest_distribution_outputs(
               state,
               10,
               outputs,
               distributed_total),
           "interest output builder rejects pool without eligible balances");

    auto add_test_utxo = [&](uint8_t seed,
                             uint32_t vout,
                             geocoin::Amount value,
                             geocoin::Height height,
                             const geocoin::Address& address) {
        geocoin::state::UTXO utxo;
        utxo.tx_hash = hash_from_seed(seed);
        utxo.vout = vout;
        utxo.value = value;
        utxo.height = height;
        utxo.address = address;
        utxo.is_coinbase = false;
        if (value < 0) {
            // Exercise the defensive output-builder path with a state that
            // cannot be produced through the production persistence API.
            geocoin::state::ChainStateTestAccess::inject_utxo_unchecked(state, utxo);
        } else {
            geocoin::state::ChainStateTestAccess::add_utxo(state, utxo);
        }
    };

    add_test_utxo(0x72, 0, -1, 1, geocoin::Address("GEOinterest-negative"));
    add_test_utxo(0x73, 0, 1000, 10, geocoin::Address("GEOinterest-too-new"));
    add_test_utxo(0x74, 0, 1000, 1, geocoin::Address());
    add_test_utxo(0x75, 0, 1000, 1, geocoin::Address("GEOinterest-holder"));

    expect(!geocoin::state::ChainStateTestAccess::build_interest_distribution_outputs(
               state,
               10,
               outputs,
               distributed_total),
           "interest output builder drops payouts below dust");
    expect(outputs.empty() && distributed_total == 0,
           "interest output builder emits no outputs when every payout is dust");

    geocoin::state::ChainStateTestAccess::set_interest_pool_balance(
        state,
        geocoin::consensus::params::DUST_LIMIT
    );
    expect(geocoin::state::ChainStateTestAccess::build_interest_distribution_outputs(
               state,
               10,
               outputs,
               distributed_total),
           "interest output builder accepts one eligible holder");
    expect(outputs.size() == 1 &&
               outputs[0].address == geocoin::Address("GEOinterest-holder") &&
               outputs[0].value == geocoin::consensus::params::DUST_LIMIT &&
               distributed_total == geocoin::consensus::params::DUST_LIMIT,
           "interest output builder emits deterministic payout");
}

void test_foundation_update_validate_apply_and_revert()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());

    const geocoin::bytes old_key_a = private_key_from_seed(0x61);
    const geocoin::bytes old_key_b = private_key_from_seed(0x62);
    const geocoin::bytes old_key_c = private_key_from_seed(0x63);
    const geocoin::bytes outsider_key = private_key_from_seed(0x64);

    const geocoin::Address old_a = address_from_seed(0x61);
    const geocoin::Address old_b = address_from_seed(0x62);
    const geocoin::Address old_c = address_from_seed(0x63);
    const geocoin::Address outsider = address_from_seed(0x64);
    const std::vector<geocoin::Address> old_participants{old_a, old_b, old_c};

    const geocoin::Address new_a = address_from_seed(0x65);
    const geocoin::Address new_b = address_from_seed(0x66);
    const std::vector<geocoin::Address> new_participants{new_a, new_b};

    auto make_foundation_config = [](uint8_t m, const std::vector<geocoin::Address>& participants) {
        geocoin::state::FoundationConfig config;
        config.m = m;
        config.n = static_cast<uint8_t>(participants.size());
        config.participants = participants;
        const auto multisig = geocoin::wallet::create_multisig_address(config.m, config.n, participants);
        config.address = geocoin::Address(multisig.address);
        return config;
    };

    const auto old_config = make_foundation_config(2, old_participants);
    const auto new_config = make_foundation_config(1, new_participants);
    geocoin::state::ChainStateTestAccess::set_foundation_config(state, old_config);

    auto make_update = [&]() {
        geocoin::core::Transaction tx;
        tx.version = 2;
        tx.type = geocoin::core::TransactionType::UPDATE_FOUNDATION_ADDRESS;
        tx.foundation_old_address = old_config.address;
        tx.foundation_new_address = new_config.address;
        tx.foundation_m = old_config.m;
        tx.foundation_n = old_config.n;
        tx.foundation_participants = old_config.participants;
        tx.foundation_new_m = new_config.m;
        tx.foundation_new_n = new_config.n;
        tx.foundation_new_participants = new_config.participants;
        return tx;
    };

    auto sign_update = [](geocoin::core::Transaction& tx, const geocoin::bytes& key) {
        tx.foundation_signatures.push_back(
            sign_bytes_payload(geocoin::core::build_foundation_update_payload_for_signing(tx), key)
        );
    };

    geocoin::core::Transaction invalid_old_config = make_update();
    invalid_old_config.foundation_m = 0;
    expect(!state.validate_transaction(invalid_old_config),
           "UPDATE_FOUNDATION_ADDRESS rejects invalid old multisig config");

    geocoin::core::Transaction wrong_old_address = make_update();
    wrong_old_address.foundation_old_address = geocoin::Address("GEOwrong-foundation");
    sign_update(wrong_old_address, old_key_a);
    sign_update(wrong_old_address, old_key_b);
    expect(!state.validate_transaction(wrong_old_address),
           "UPDATE_FOUNDATION_ADDRESS rejects mismatched old foundation address");

    geocoin::core::Transaction wrong_new_address = make_update();
    wrong_new_address.foundation_new_address = geocoin::Address("GEOwrong-new-foundation");
    sign_update(wrong_new_address, old_key_a);
    sign_update(wrong_new_address, old_key_b);
    expect(!state.validate_transaction(wrong_new_address),
           "UPDATE_FOUNDATION_ADDRESS rejects mismatched new foundation address");

    geocoin::core::Transaction duplicate_participant = make_update();
    duplicate_participant.foundation_participants[1] = duplicate_participant.foundation_participants[0];
    sign_update(duplicate_participant, old_key_a);
    sign_update(duplicate_participant, old_key_b);
    expect(!state.validate_transaction(duplicate_participant),
           "UPDATE_FOUNDATION_ADDRESS rejects duplicate old participants");

    geocoin::core::Transaction missing_signatures = make_update();
    sign_update(missing_signatures, old_key_a);
    expect(!state.validate_transaction(missing_signatures),
           "UPDATE_FOUNDATION_ADDRESS rejects signatures below threshold");

    geocoin::core::Transaction duplicate_signature = make_update();
    sign_update(duplicate_signature, old_key_a);
    sign_update(duplicate_signature, old_key_a);
    expect(!state.validate_transaction(duplicate_signature),
           "UPDATE_FOUNDATION_ADDRESS rejects duplicate signer quorum");

    geocoin::core::Transaction outsider_signature = make_update();
    sign_update(outsider_signature, old_key_a);
    sign_update(outsider_signature, outsider_key);
    expect(!state.validate_transaction(outsider_signature),
           "UPDATE_FOUNDATION_ADDRESS rejects signer outside old participants");

    geocoin::core::Transaction corrupted_signature = make_update();
    corrupted_signature.foundation_signatures.push_back(geocoin::Signature{0x01, 0x02});
    sign_update(corrupted_signature, old_key_b);
    expect(!state.validate_transaction(corrupted_signature),
           "UPDATE_FOUNDATION_ADDRESS rejects malformed recovered signature");

    geocoin::core::Transaction update = make_update();
    sign_update(update, old_key_a);
    sign_update(update, old_key_b);
    expect(state.validate_transaction(update), "UPDATE_FOUNDATION_ADDRESS validates with old quorum");

    state.set_height(40);
    state.set_tip_hash(hash_from_seed(0x67));
    const auto update_block = make_next_rewarded_block(state, outsider, {update});
    expect(state.apply_block(update_block, true), "UPDATE_FOUNDATION_ADDRESS block applies");
    auto current_config = state.get_foundation_config();
    expect(current_config.address == new_config.address &&
               current_config.m == new_config.m &&
               current_config.n == new_config.n &&
               current_config.participants == new_config.participants,
           "UPDATE_FOUNDATION_ADDRESS stores new foundation config");

    expect(state.revert_block(update_block), "UPDATE_FOUNDATION_ADDRESS block reverts");
    current_config = state.get_foundation_config();
    expect(current_config.address == old_config.address &&
               current_config.m == old_config.m &&
               current_config.n == old_config.n &&
               current_config.participants == old_config.participants,
           "UPDATE_FOUNDATION_ADDRESS undo restores old foundation config");
}

void test_witness_payload_cache_and_pruning()
{
    const auto path = make_temp_dir("geocoin-chainstate-witness-payload");
    std::error_code ec;
    try {
        auto block_storage = std::make_shared<geocoin::storage::BlockStorage>(path.string());
        expect(block_storage->is_open(), "witness test block storage opens");

        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        state.set_block_storage(block_storage);
        state.set_prune_depth(0);
        state.set_prune_depth(1);

        geocoin::state::ChainState no_block_storage(std::make_unique<geocoin::state::MemoryStorage>());
        geocoin::bytes missing_payload;
        geocoin::storage::BlockStorage::PruningStats missing_stats;
        geocoin::Height missing_pruned_height = 77;
        expect(!no_block_storage.get_witness_payload(hash_from_seed(0xdf), missing_payload),
               "witness payload lookup rejects missing block storage");
        expect(!no_block_storage.restore_witness_from_payload(hash_from_seed(0xdf), {0x01}),
               "witness restore rejects missing block storage");
        expect(!no_block_storage.get_pruning_stats(missing_stats),
               "pruning stats reject missing block storage");
        expect(!no_block_storage.get_witness_pruned_height(missing_pruned_height),
               "witness pruned height rejects missing block storage");
        expect(!no_block_storage.prune_witness_up_to(1),
               "witness pruning rejects missing block storage");
        geocoin::state::ChainStateTestAccess::cache_witness_payload(
            no_block_storage,
            hash_from_seed(0xde),
            {}
        );
        geocoin::state::ChainStateTestAccess::rebuild_witness_pubkey_cache_from_active_chain(
            no_block_storage
        );

        const geocoin::Address miner = address_from_seed(0xe0);
        const geocoin::Hash256 prev = hash_from_seed(0xe1);
        state.set_height(0);
        state.set_tip_hash(prev);

        auto coinbase = geocoin::core::Transaction::create_coinbase(
            1,
            miner,
            geocoin::consensus::ProofOfWork::calculate_block_reward(1),
            0,
            state.get_foundation_address()
        );
        const geocoin::bytes pubkey = make_witness_pubkey(0xe2);
        geocoin::core::TxWitness witness;
        witness.public_key = pubkey;
        witness.public_key_hash = geocoin::crypto::sha256(pubkey);
        witness.signature = geocoin::bytes{0xaa, 0xbb, 0xcc};
        coinbase.witness_version = 1;
        coinbase.witnesses.push_back(witness);

        const auto block1 = make_block(1, prev, {coinbase});
        expect(state.apply_block(block1, true), "block with coinbase witness applies");
        expect(block_storage->set_active_block_and_tip(1, block1.get_hash()),
               "standalone state fixture publishes the active block");

        geocoin::bytes payload;
        expect(state.get_witness_payload(block1.get_hash(), payload), "witness payload is cached/readable");
        expect(payload.size() > pubkey.size(), "witness payload serializes pubkey and signature");
        geocoin::bytes cached_payload;
        expect(state.get_witness_payload(block1.get_hash(), cached_payload) && cached_payload == payload,
               "witness payload cache hit returns same payload");
        expect(!state.restore_witness_from_payload(block1.get_hash(), {}),
               "chainstate witness restore rejects empty payload");

        const auto block2 = make_block(
            2,
            block1.get_hash(),
            {geocoin::core::Transaction::create_coinbase(
                2,
                miner,
                geocoin::consensus::ProofOfWork::calculate_block_reward(2),
                0,
                state.get_foundation_address()
            )}
        );
        expect(state.apply_block(block2, true), "second block applies and triggers witness pruning");
        expect(block_storage->set_active_block_and_tip(2, block2.get_hash()),
               "standalone state fixture publishes the new active tip");
        geocoin::core::Block pruned_block;
        expect(block_storage->get_block(block1.get_hash(), pruned_block, false) &&
               std::all_of(pruned_block.transactions.front().witnesses.begin(),
                           pruned_block.transactions.front().witnesses.end(),
                           [](const auto& item) { return item.public_key.empty() && item.signature.empty(); }),
               "automatic pruning actually removes witness from the active body");
        geocoin::bytes stored_payload;
        expect(block_storage->get_witness_payload(block1.get_hash(), stored_payload) && stored_payload == payload,
               "automatic pruning retains the original witness payload separately");

        geocoin::Height pruned_height = 0;
        expect(state.get_witness_pruned_height(pruned_height), "witness pruned height is readable");
        expect(pruned_height == 1, "witness pruning advances to block1");
        expect(state.prune_witness_up_to(0), "witness pruning accepts zero target as no-op");
        expect(state.prune_witness_up_to(1), "witness pruning accepts target below already-pruned range");
        expect(state.restore_witness_from_payload(block1.get_hash(), payload),
               "chainstate witness restore accepts persisted payload");
        cached_payload.clear();
        expect(state.get_witness_payload(block1.get_hash(), cached_payload) && cached_payload == payload,
               "restored witness payload is cached");

        expect(state.prune_witness_up_to(3, 1), "witness pruning honors max block batch");
        expect(state.get_witness_pruned_height(pruned_height), "witness pruned height after batch is readable");
        expect(pruned_height == 2, "witness pruning batch advances by one block");

        state.set_archive_mode(true);
        expect(!state.prune_witness_up_to(3), "archive mode disables witness pruning");
        state.set_archive_mode(false);

        geocoin::storage::BlockStorage::PruningStats stats;
        expect(state.get_pruning_stats(stats), "chainstate exposes block storage pruning stats");
        geocoin::state::ChainStateTestAccess::rebuild_witness_pubkey_cache_from_active_chain(state);

        block_storage->close();
        std::filesystem::remove_all(path, ec);
    } catch (...) {
        std::filesystem::remove_all(path, ec);
        throw;
    }
}

void test_witness_pubkey_storage_integrity_and_stats()
{
    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* storage_ptr = storage.get();

    const geocoin::bytes pubkey = make_witness_pubkey(0xa0);
    const geocoin::Hash256 pubkey_hash = geocoin::crypto::sha256(pubkey);
    const std::string pubkey_hex = hash_to_hex(pubkey_hash);
    expect(storage_ptr->put_metadata("wpk:" + pubkey_hex, pubkey),
           "seed witness pubkey");
    expect(storage_ptr->put_metadata("wpkref:" + pubkey_hex, encode_u64_le(1)), "seed exact witness refcount");
    const auto missing_ref_key = make_witness_pubkey(0xc0);
    const auto missing_ref_hex = hash_to_hex(geocoin::crypto::sha256(missing_ref_key));
    expect(storage_ptr->put_metadata("wpk:" + missing_ref_hex, missing_ref_key), "seed incomplete witness pair");

    const geocoin::bytes zero_ref_pubkey = make_witness_pubkey(0xb0);
    const geocoin::Hash256 zero_ref_hash = geocoin::crypto::sha256(zero_ref_pubkey);
    const std::string zero_ref_hex = hash_to_hex(zero_ref_hash);
    expect(storage_ptr->put_metadata("wpk:" + zero_ref_hex, zero_ref_pubkey),
           "seed zero-ref witness pubkey");
    expect(storage_ptr->put_metadata("wpkref:" + zero_ref_hex, encode_u64_le(0)),
           "seed zero refcount");

    const std::string mismatch_hex = hash_to_hex(hash_from_seed(0xb1));
    expect(storage_ptr->put_metadata("wpk:" + mismatch_hex, make_witness_pubkey(0xb2)),
           "seed mismatched witness pubkey");

    const std::string dangling_ref_hex = hash_to_hex(hash_from_seed(0xb3));
    expect(storage_ptr->put_metadata("wpkref:" + dangling_ref_hex, encode_u64_le(7)),
           "seed dangling witness refcount");

    geocoin::bytes stats_raw(24, 0);
    const auto hits = encode_u64_le(2);
    const auto misses = encode_u64_le(3);
    const auto saved = encode_u64_le(4);
    std::copy(hits.begin(), hits.end(), stats_raw.begin());
    std::copy(misses.begin(), misses.end(), stats_raw.begin() + 8);
    std::copy(saved.begin(), saved.end(), stats_raw.begin() + 16);
    expect(storage_ptr->put_metadata("wpk_stats", stats_raw), "seed witness stats");

    geocoin::state::ChainState state(std::move(storage));
    expect(state.has_witness_pubkey(pubkey_hash), "valid witness pubkey is available");

    geocoin::bytes loaded_pubkey;
    expect(state.get_witness_pubkey(pubkey_hash, loaded_pubkey), "get_witness_pubkey loads valid key");
    expect(loaded_pubkey == pubkey, "witness pubkey bytes match");
    expect(storage_ptr->has_metadata("wpkref:" + pubkey_hex), "valid witness refcount is retained");
    expect(storage_ptr->has_metadata("wpk:" + missing_ref_hex) && !storage_ptr->has_metadata("wpkref:" + missing_ref_hex),
           "opening never invents a missing witness refcount");
    expect(storage_ptr->has_metadata("wpk:" + zero_ref_hex), "invalid witness key is retained for recovery");
    expect(storage_ptr->has_metadata("wpkref:" + zero_ref_hex), "invalid witness refcount is retained for recovery");
    expect(storage_ptr->has_metadata("wpkref:" + dangling_ref_hex), "dangling witness refcount is retained for recovery");

    expect(!state.has_derived_state_snapshot(), "invalid witness pairs require recovery");
    const auto stats = state.get_witness_pubkey_stats();
    expect(stats.total_keys == 1, "only valid witness key is loaded");
    expect(stats.cache_hits >= 3, "witness stats load and cache hit are tracked");
    expect(stats.cache_misses == 3, "witness cache misses load from storage");
    expect(stats.bytes_saved >= 4 + pubkey.size(), "witness bytes saved stat is updated");
}

void test_persistence_loaders_skip_corruption_and_restore_valid_metadata()
{
    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* storage_ptr = storage.get();
    expect(storage_ptr->put_metadata("chainstate_height", encode_u64_le(13)),
           "projection height includes the latest card update");
    expect(storage_ptr->put_metadata("chainstate_utxo_count", encode_u64_le(0)), "empty projection UTXO count");
    const std::string valid_epoch = "0123456789abcdef0123456789abcdef";
    expect(storage_ptr->put_metadata(
               "idx_epoch",
               geocoin::bytes(valid_epoch.begin(), valid_epoch.end())
           ),
           "seed valid index epoch for persistence corruption test");

    const geocoin::Address owner = address_from_seed(0x21);
    const geocoin::Address member = address_from_seed(0x22);
    const geocoin::Hash256 deposit_id = hash_from_seed(0x23);
    const geocoin::Hash256 registry_id = deposit_id;
    const geocoin::Hash256 card_program_id = hash_from_seed(0x25);
    const geocoin::Hash256 card_uid = hash_from_seed(0x26);
    const geocoin::Hash256 duplicate_card_uid = hash_from_seed(0x27);
    const geocoin::Hash256 card_pubkey = hash_from_seed(0x28);
    const geocoin::Hash256 nft_missing_meta = hash_from_seed(0x29);
    const geocoin::Hash256 nft_invalid_meta = hash_from_seed(0x2a);
    const geocoin::Hash256 nft_mismatched_id = hash_from_seed(0x2b);
    geocoin::nft::NFTMetadata invalid_owner_metadata = make_nft_metadata();
    invalid_owner_metadata.name = "Invalid owner fixture";
    const geocoin::Hash256 nft_invalid_owner = invalid_owner_metadata.calculate_hash();
    geocoin::nft::NFTMetadata orphan_metadata = make_nft_metadata();
    orphan_metadata.name = "Orphan metadata fixture";
    const geocoin::Hash256 nft_orphan_metadata = orphan_metadata.calculate_hash();

    expect(storage_ptr->put_metadata("scd:short", {0x01}), "seed short sidechain deposit key");
    expect(storage_ptr->put_metadata("scd:" + std::string(64, 'z'), {0x01}),
           "seed non-hex sidechain deposit key");
    expect(storage_ptr->put_metadata("scd:" + hash_to_hex(hash_from_seed(0x2b)), {0x01, 0x02}),
           "seed truncated sidechain deposit");
    expect(storage_ptr->put_metadata(
               "scd:" + hash_to_hex(deposit_id),
               serialize_test_deposit(owner, 123 * geocoin::COIN, 77, hash_from_seed(0x2c), 9, 10)
           ),
           "seed valid sidechain deposit");

    geocoin::bytes legacy_registry_prefix;
    append_string(legacy_registry_prefix, owner.str());
    append_u64_le(legacy_registry_prefix, amount_to_u64(1000 * geocoin::COIN));
    append_u64_le(legacy_registry_prefix, amount_to_u64(10 * geocoin::COIN));
    append_u32_le(legacy_registry_prefix, geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER);
    append_u32_le(legacy_registry_prefix, 25);
    append_u32_le(legacy_registry_prefix, geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2);
    expect(storage_ptr->put_metadata("sc:reg:" + hash_to_hex(hash_from_seed(0x2d)), legacy_registry_prefix),
           "seed truncated legacy sidechain registry");
    expect(storage_ptr->put_metadata("sc:reg:" + std::string(64, 'q'), {0x01}),
           "seed non-hex sidechain registry key");
    expect(storage_ptr->put_metadata(
               "sc:reg:" + hash_to_hex(registry_id),
               serialize_test_registry(
                   owner,
                   1000 * geocoin::COIN,
                   10 * geocoin::COIN,
                   geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER,
                   25,
                   geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER * 2,
                   "TST",
                   1,
                   1,
                   {member}
               )
           ),
           "seed valid sidechain registry");

    expect(storage_ptr->put_metadata("card:prog:short", {0x01}), "seed short card program key");
    expect(storage_ptr->put_metadata("card:prog:" + hash_to_hex(hash_from_seed(0x2e)), {0x01}),
           "seed truncated card program");
    expect(storage_ptr->put_metadata(
               "card:prog:" + hash_to_hex(card_program_id),
               serialize_test_card_program(owner, hash_from_seed(0x2f), 42, hash_from_seed(0x30), 8)
           ),
           "seed valid card program");

    expect(storage_ptr->put_metadata("card:uid:short", {0x01}), "seed short card instance key");
    expect(storage_ptr->put_metadata("card:uid:" + hash_to_hex(hash_from_seed(0x31)), {0x01}),
           "seed truncated card instance");
    expect(storage_ptr->put_metadata(
               "card:uid:" + hash_to_hex(card_uid),
               serialize_test_card_instance(card_program_id, owner, card_uid, card_pubkey, 0)
           ),
           "seed valid card instance");
    expect(storage_ptr->put_metadata(
               "card:uid:" + hash_to_hex(duplicate_card_uid),
               serialize_test_card_instance(card_program_id, owner, duplicate_card_uid, card_pubkey, 1)
           ),
           "seed duplicate card pubkey instance");

    const std::vector<geocoin::Address> foundation_members{address_from_seed(0x32)};
    const geocoin::Address foundation_address = make_multisig_address(1, 1, foundation_members);
    expect(storage_ptr->put_metadata(
               "foundation_cfg",
               serialize_test_foundation_config(foundation_address, 1, 1, foundation_members)
           ),
           "seed valid foundation config");
    expect(storage_ptr->put_metadata("interest_pool_balance", encode_u64_le(12345)),
           "seed valid interest pool balance");

    expect(storage_ptr->put_metadata("nft:owner:short", {0x01}), "seed short nft owner key");
    expect(storage_ptr->put_metadata("nft:owner:" + std::string(64, 'x'), {0x01}),
           "seed non-hex nft owner key");
    expect(storage_ptr->put_metadata(
               "nft:owner:" + hash_to_hex(nft_missing_meta),
               bytes_from_string(owner.str())
           ),
           "seed nft owner without metadata");
    expect(storage_ptr->put_metadata(
               "nft:owner:" + hash_to_hex(nft_invalid_meta),
               bytes_from_string(owner.str())
           ),
           "seed nft owner with invalid metadata");
    expect(storage_ptr->put_metadata("nft:meta:" + hash_to_hex(nft_invalid_meta), {0x01, 0x02}),
           "seed invalid nft metadata");
    expect(storage_ptr->put_metadata(
               "nft:owner:" + hash_to_hex(nft_invalid_owner),
               bytes_from_string("not-a-geocoin-address")
           ),
           "seed nft with invalid owner address");
    expect(storage_ptr->put_metadata(
               "nft:meta:" + hash_to_hex(nft_invalid_owner),
               invalid_owner_metadata.serialize()
           ),
           "seed metadata paired with invalid nft owner");
    expect(storage_ptr->put_metadata(
               "nft:meta:" + hash_to_hex(nft_orphan_metadata),
               orphan_metadata.serialize()
           ),
           "seed nft metadata without an owner half");

    geocoin::nft::NFTMetadata metadata = make_nft_metadata();
    const geocoin::Hash256 nft_id = metadata.calculate_hash();
    expect(storage_ptr->put_metadata(
               "nft:owner:" + hash_to_hex(nft_mismatched_id),
               bytes_from_string(owner.str())
           ),
           "seed nft owner whose id does not match metadata");
    expect(storage_ptr->put_metadata(
               "nft:meta:" + hash_to_hex(nft_mismatched_id),
               metadata.serialize()
           ),
           "seed nft metadata under a mismatched id");
    expect(storage_ptr->put_metadata(
               "nft:owner:" + hash_to_hex(nft_id),
               bytes_from_string(owner.str())
           ),
           "seed valid nft owner");
    expect(storage_ptr->put_metadata("nft:meta:" + hash_to_hex(nft_id), metadata.serialize()),
           "seed valid nft metadata");

    geocoin::state::ChainState state(std::move(storage));

    geocoin::state::SidechainDeposit deposit;
    expect(state.get_sidechain_deposit(deposit_id, deposit), "valid sidechain deposit loads");
    expect(deposit.owner == owner && deposit.amount == 123 * geocoin::COIN &&
               deposit.unlock_height == 77 &&
               deposit.last_checkpoint_height == 9 &&
               deposit.last_sidechain_checkpoint_height == 10,
           "loaded sidechain deposit fields match");

    geocoin::state::SidechainRegistryEntry registry;
    expect(state.get_sidechain_registry(registry_id, registry), "valid sidechain registry loads");
    expect(registry.owner == owner && registry.token_symbol == "TST" &&
               registry.committee_m == 1 && registry.committee_n == 1 &&
               registry.committee_members.size() == 1 &&
               registry.committee_members.front() == member,
           "loaded sidechain registry fields match");

    expect(geocoin::state::ChainStateTestAccess::has_card_program(state, card_program_id),
           "valid card program loads");
    expect(geocoin::state::ChainStateTestAccess::has_card_instance(state, card_uid),
           "valid card instance loads");
    expect(!geocoin::state::ChainStateTestAccess::has_card_instance(state, duplicate_card_uid),
           "duplicate-pubkey card instance is discarded pending rebuild");
    expect(geocoin::state::ChainStateTestAccess::card_pubkey_maps_to(state, card_pubkey, card_uid),
           "first card pubkey mapping is retained on duplicate");

    const auto foundation = state.get_foundation_config();
    expect(foundation.address == foundation_address &&
               foundation.m == 1 &&
               foundation.n == 1 &&
               foundation.participants == foundation_members,
           "valid foundation config loads");
    expect(state.get_interest_pool_balance() == 12345, "interest pool balance loads");

    expect(state.get_nft_owner(nft_id) == owner, "valid NFT owner loads");
    expect(state.get_nft_metadata(nft_id).calculate_hash() == nft_id, "valid NFT metadata loads");
    expect(state.has_nft_id_key(nft_id), "valid NFT uniqueness key is rebuilt");
    expect(!state.nft_exists(nft_mismatched_id),
           "NFT metadata stored under a non-canonical id is discarded");
    expect(!state.nft_exists(nft_invalid_owner),
           "NFT with an invalid persisted owner is discarded");
    expect(storage_ptr->has_metadata("nft:owner:" + hash_to_hex(nft_missing_meta)),
           "incomplete NFT owner record is retained for explicit recovery");
    expect(storage_ptr->has_metadata("nft:owner:" + hash_to_hex(nft_invalid_meta)) &&
               storage_ptr->has_metadata("nft:meta:" + hash_to_hex(nft_invalid_meta)),
           "invalid NFT metadata pair is retained for explicit recovery");
    expect(storage_ptr->has_metadata("nft:owner:" + hash_to_hex(nft_invalid_owner)) &&
               storage_ptr->has_metadata("nft:meta:" + hash_to_hex(nft_invalid_owner)),
           "invalid NFT owner pair is retained for explicit recovery");
    expect(storage_ptr->has_metadata("nft:meta:" + hash_to_hex(nft_orphan_metadata)),
           "orphan NFT metadata is retained for explicit recovery");
    expect(!state.has_derived_state_snapshot(),
           "any corrupt consensus-derived metadata invalidates the complete snapshot");

    auto invalid_interest_storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* invalid_interest_ptr = invalid_interest_storage.get();
    expect(invalid_interest_ptr->put_metadata(
               "idx_epoch",
               geocoin::bytes(valid_epoch.begin(), valid_epoch.end())
           ),
           "seed valid epoch for invalid interest test");
    expect(invalid_interest_ptr->put_metadata("interest_pool_balance", {0x01}),
           "seed invalid interest pool balance");
    geocoin::state::ChainState invalid_interest(std::move(invalid_interest_storage));
    expect(invalid_interest.get_interest_pool_balance() == 0,
           "invalid interest pool balance is not used");
    expect(!invalid_interest.has_derived_state_snapshot(),
           "invalid interest pool balance requires a rebuild");

    auto invalid_foundation_storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* invalid_foundation_ptr = invalid_foundation_storage.get();
    expect(invalid_foundation_ptr->put_metadata(
               "idx_epoch",
               geocoin::bytes(valid_epoch.begin(), valid_epoch.end())
           ),
           "seed valid epoch for invalid foundation test");
    expect(invalid_foundation_ptr->put_metadata("foundation_cfg", {0xff}),
           "seed invalid foundation config");
    geocoin::state::ChainState invalid_foundation(std::move(invalid_foundation_storage));
    expect(!invalid_foundation.get_foundation_config().address.str().empty(),
           "invalid foundation config falls back to defaults");
    expect(!invalid_foundation.has_derived_state_snapshot(),
           "invalid foundation config requires a rebuild");
}

void test_activation_and_sidechain_validation_guards()
{
    {
        geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
        state.set_activation_manifest(geocoin::utils::ActivationManifest::production_defaults("degree"));

        geocoin::core::Transaction unknown;
        unknown.type = static_cast<geocoin::core::TransactionType>(0xff);
        expect(!state.validate_transaction(unknown), "unknown transaction type is rejected");

        geocoin::core::Transaction hierarchical;
        hierarchical.type = geocoin::core::TransactionType::MULTISIG_SPEND;
        hierarchical.delegation_proof_txid = hash_from_seed(0xc0);
        expect(!state.validate_transaction(hierarchical),
               "hierarchical multisig is blocked by production activation manifest");

        geocoin::core::Transaction plain_multisig;
        plain_multisig.type = geocoin::core::TransactionType::MULTISIG_SPEND;
        plain_multisig.version = 2;
        plain_multisig.m = 1;
        plain_multisig.n = 1;
        expect(!state.validate_transaction(plain_multisig),
               "standard multisig reaches active feature gate and then rejects malformed spend");

        geocoin::core::Transaction delegation;
        delegation.type = geocoin::core::TransactionType::DELEGATION_PROOF;
        expect(!state.validate_transaction(delegation),
               "delegation proof is blocked by production activation manifest");

        geocoin::core::Transaction revoke;
        revoke.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
        expect(!state.validate_transaction(revoke),
               "delegation revoke is blocked by production activation manifest");

        geocoin::core::Transaction nft;
        nft.type = geocoin::core::TransactionType::NFT_CREATE;
        geocoin::nft::NFTCreationTransaction nft_payload;
        nft_payload.metadata = make_nft_metadata();
        nft_payload.nft_id = nft_payload.metadata.calculate_hash();
        nft_payload.creator_address = address_from_seed(0xc1);
        nft_payload.royalty_percent = 0;
        nft.nft_create = nft_payload;
        expect(!state.validate_transaction(nft),
               "NFT create is blocked by production activation manifest");

        geocoin::core::Transaction cadastral;
        cadastral.type = geocoin::core::TransactionType::CADASTRAL_DOC;
        expect(!state.validate_transaction(cadastral),
               "cadastral document is blocked by production activation manifest");

        geocoin::utils::ActivationManifest sidechain_off =
            geocoin::utils::ActivationManifest::production_defaults("degree");
        sidechain_off.sidechain = geocoin::utils::FeatureActivation{false, 0};
        state.set_activation_manifest(sidechain_off);
        geocoin::core::Transaction issue;
        issue.type = geocoin::core::TransactionType::ISSUE_SIDECHAIN;
        expect(!state.validate_transaction(issue),
               "sidechain issue is blocked when the sidechain feature is disabled");
        geocoin::core::Transaction lock;
        lock.type = geocoin::core::TransactionType::LOCK_DEPOSIT;
        expect(!state.validate_transaction(lock),
               "sidechain lock is blocked when the sidechain feature is disabled");
        state.set_activation_manifest(geocoin::utils::ActivationManifest::production_defaults("degree"));
    }

    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    // Keep domain guard coverage even when an incomplete component fixture
    // is now rejected earlier by the public common validation boundary.
    const auto rejects_sidechain = [&](const geocoin::core::Transaction& tx) {
        return !state.validate_transaction(tx) && !StateAccess::validate_sidechain_state(state, tx);
    };
    const geocoin::Address owner = address_from_seed(0xd0);
    const geocoin::Address other = address_from_seed(0xd1);
    const geocoin::Hash256 sidechain_id = hash_from_seed(0xd2);
    restore_deposit_with_revert(state, sidechain_id, owner, 2000 * geocoin::COIN, 0);
    restore_sidechain_registry_with_revert(
        state,
        sidechain_id,
        owner,
        5000 * geocoin::COIN,
        1000 * geocoin::COIN
    );
    state.set_height(100);

    const geocoin::Hash256 committee_id =
        geocoin::consensus::compute_sidechain_committee_id(1, 1, std::vector<geocoin::Address>{owner});
    auto reset_height = [&]() {
        state.set_height(100);
        state.set_tip_hash(hash_from_seed(0xcf));
    };
    auto restore_deposit = [&](const geocoin::Hash256& id,
                               const geocoin::Address& deposit_owner,
                               geocoin::Amount amount,
                               geocoin::Height unlock_height) {
        restore_deposit_with_revert(state, id, deposit_owner, amount, unlock_height);
        reset_height();
    };
    auto restore_registry = [&](const geocoin::Hash256& id,
                                geocoin::Amount limit,
                                geocoin::Amount issued,
                                uint8_t policy = 2,
                                uint8_t version = 2) {
        restore_sidechain_registry_with_revert(state, id, owner, limit, issued, policy, version);
        reset_height();
    };

    geocoin::core::Transaction issue;
    issue.type = geocoin::core::TransactionType::ISSUE_SIDECHAIN;
    issue.sidechain_id = hash_from_seed(0xd3);
    issue.sidechain_owner = owner;
    issue.sidechain_token_symbol = "BAD";
    issue.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_checkpoint_policy_id = 2;
    issue.sidechain_checkpoint_version = 2;
    issue.sidechain_committee_m = 1;
    issue.sidechain_committee_n = 1;
    issue.sidechain_committee_members = {owner};
    issue.sidechain_committee_id = committee_id;
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects missing deposit");

    issue.sidechain_id = sidechain_id;
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects duplicate registry");

    issue.sidechain_id = sidechain_id;
    issue.sidechain_owner = other;
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects owner mismatch");

    issue.sidechain_owner = owner;
    issue.sidechain_token_symbol.clear();
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects empty token symbol");

    issue.sidechain_token_symbol = "BAD";
    issue.sidechain_multiplier = 0;
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects invalid multiplier");

    issue.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_checkpoint_policy_id = 9;
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects unsupported checkpoint policy");

    issue.sidechain_checkpoint_policy_id = 2;
    issue.sidechain_committee_m = 0;
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects empty committee threshold");

    issue.sidechain_committee_m = 1;
    issue.sidechain_committee_n = 2;
    issue.sidechain_committee_members = {owner, owner};
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects duplicate committee members");

    const geocoin::Hash256 issue_small_deposit_id = hash_from_seed(0xc2);
    restore_deposit(issue_small_deposit_id, owner, 1, 0);
    issue.sidechain_id = issue_small_deposit_id;
    issue.sidechain_owner = owner;
    issue.sidechain_token_symbol = "BAD";
    issue.sidechain_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
    issue.sidechain_checkpoint_policy_id = 2;
    issue.sidechain_checkpoint_version = 2;
    issue.sidechain_committee_m = 1;
    issue.sidechain_committee_n = 1;
    issue.sidechain_committee_members = {owner};
    issue.sidechain_committee_id = committee_id;
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects deposit below fee");

    const geocoin::Address invalid_owner("GEO-invalid-owner");
    const geocoin::Hash256 issue_bad_owner_id = hash_from_seed(0xc3);
    restore_deposit(issue_bad_owner_id, invalid_owner, 2000 * geocoin::COIN, 0);
    issue.sidechain_id = issue_bad_owner_id;
    issue.sidechain_owner = invalid_owner;
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects invalid owner address");

    const geocoin::Hash256 issue_bad_committee_id = hash_from_seed(0xc4);
    restore_deposit(issue_bad_committee_id, owner, 2000 * geocoin::COIN, 0);
    issue.sidechain_id = issue_bad_committee_id;
    issue.sidechain_owner = owner;
    issue.sidechain_committee_id = hash_from_seed(0xc5);
    expect(rejects_sidechain(issue), "ISSUE_SIDECHAIN rejects mismatched committee id");

    issue.sidechain_committee_id = committee_id;
    issue.sidechain_checkpoint = issue.sidechain_id;
    issue.sidechain_signature = sign_sidechain_payload(issue, private_key_from_seed(0xd0));
    expect(state.validate_transaction(issue), "ISSUE_SIDECHAIN accepts valid registry proposal");

    geocoin::core::Transaction mint;
    mint.type = geocoin::core::TransactionType::MINT_SIDECHAIN;
    mint.sidechain_id = hash_from_seed(0xc6);
    mint.sidechain_amount = geocoin::COIN;
    mint.sidechain_committee_id = committee_id;
    expect(rejects_sidechain(mint), "MINT_SIDECHAIN rejects missing registry");

    const geocoin::Hash256 mint_bad_policy_id = hash_from_seed(0xc7);
    restore_registry(mint_bad_policy_id, 5000 * geocoin::COIN, 0, 9, 2);
    mint.sidechain_id = mint_bad_policy_id;
    expect(rejects_sidechain(mint), "MINT_SIDECHAIN rejects unsupported registry policy");

    const geocoin::Hash256 mint_overflow_id = hash_from_seed(0xc8);
    restore_registry(mint_overflow_id, geocoin::COIN, geocoin::COIN);
    mint.sidechain_id = mint_overflow_id;
    expect(rejects_sidechain(mint), "MINT_SIDECHAIN rejects issuance above limit");

    const geocoin::Hash256 mint_missing_deposit_id = hash_from_seed(0xc9);
    restore_registry(mint_missing_deposit_id, 5000 * geocoin::COIN, 0);
    mint.sidechain_id = mint_missing_deposit_id;
    expect(rejects_sidechain(mint), "MINT_SIDECHAIN rejects missing deposit");

    const geocoin::Hash256 mint_low_deposit_id = hash_from_seed(0xca);
    restore_registry(mint_low_deposit_id, 5000 * geocoin::COIN, 0);
    restore_deposit(mint_low_deposit_id, owner, 1, 0);
    mint.sidechain_id = mint_low_deposit_id;
    expect(rejects_sidechain(mint), "MINT_SIDECHAIN rejects deposit below fee");

    const geocoin::Hash256 mint_bad_sig_id = hash_from_seed(0xcb);
    restore_registry(mint_bad_sig_id, 5000 * geocoin::COIN, 0);
    restore_deposit(mint_bad_sig_id, owner, 2000 * geocoin::COIN, 0);
    mint.sidechain_id = mint_bad_sig_id;
    mint.sidechain_committee_signatures = {geocoin::Signature{0x01}};
    expect(rejects_sidechain(mint), "MINT_SIDECHAIN rejects invalid committee signature");

    geocoin::core::Transaction batch;
    batch.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    expect(rejects_sidechain(batch), "BATCH_SIDECHAIN_CHECKPOINT rejects empty batch");

    geocoin::core::BatchCheckpointItem item;
    item.sidechain_id = sidechain_id;
    item.checkpoint = hash_from_seed(0xd4);
    item.checkpoint_height = 1;
    item.fee = geocoin::consensus::calculate_sidechain_checkpoint_min_fee(state.get_height() + 1, 0);
    item.version = 2;
    item.committee_id = geocoin::consensus::compute_sidechain_committee_id(1, 1, std::vector<geocoin::Address>{owner});
    batch.sidechain_batch_checkpoints = {item, item};
    expect(rejects_sidechain(batch), "BATCH_SIDECHAIN_CHECKPOINT rejects duplicate sidechain ids");

    batch.sidechain_batch_checkpoints = {item};
    batch.sidechain_batch_checkpoints[0].fee = 0;
    expect(rejects_sidechain(batch), "BATCH_SIDECHAIN_CHECKPOINT rejects zero fee");

    batch.sidechain_batch_checkpoints[0] = item;
    expect(rejects_sidechain(batch), "BATCH_SIDECHAIN_CHECKPOINT rejects missing v2 proof");

    batch.sidechain_batch_checkpoints[0] = item;
    batch.sidechain_batch_checkpoints[0].sidechain_id = hash_from_seed(0xcc);
    expect(rejects_sidechain(batch), "BATCH_SIDECHAIN_CHECKPOINT rejects missing registry");

    const geocoin::Hash256 batch_bad_policy_id = hash_from_seed(0xcd);
    restore_registry(batch_bad_policy_id, 5000 * geocoin::COIN, 1000 * geocoin::COIN, 9, 2);
    batch.sidechain_batch_checkpoints[0] = item;
    batch.sidechain_batch_checkpoints[0].sidechain_id = batch_bad_policy_id;
    expect(rejects_sidechain(batch), "BATCH_SIDECHAIN_CHECKPOINT rejects unsupported policy");

    batch.sidechain_batch_checkpoints[0] = item;
    batch.sidechain_batch_checkpoints[0].version = 1;
    expect(rejects_sidechain(batch), "BATCH_SIDECHAIN_CHECKPOINT rejects version mismatch");

    const auto valid_batch_proof = make_checkpoint_proof_v2(sidechain_id, 23, amount_to_u64(1000 * geocoin::COIN));
    batch.sidechain_batch_checkpoints[0] = item;
    batch.sidechain_batch_checkpoints[0].checkpoint = valid_batch_proof.checkpoint;
    batch.sidechain_batch_checkpoints[0].checkpoint_height = 23;
    batch.sidechain_batch_checkpoints[0].proof = valid_batch_proof.proof;
    batch.sidechain_batch_checkpoints[0].fee =
        geocoin::consensus::calculate_sidechain_checkpoint_min_fee(state.get_height() + 1, 0) - 1;
    expect(rejects_sidechain(batch), "BATCH_SIDECHAIN_CHECKPOINT rejects fee below minimum");

    batch.sidechain_batch_checkpoints[0].fee =
        geocoin::consensus::calculate_sidechain_checkpoint_min_fee(state.get_height() + 1, 0);
    batch.sidechain_batch_checkpoints[0].committee_signatures = {geocoin::Signature{0x02}};
    expect(rejects_sidechain(batch), "BATCH_SIDECHAIN_CHECKPOINT rejects invalid committee signature");

    geocoin::core::Transaction slash;
    slash.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
    slash.sidechain_id = hash_from_seed(0xd5);
    slash.sidechain_amount = 1;
    expect(rejects_sidechain(slash), "SLASH_DEPOSIT rejects missing registry");

    const geocoin::Hash256 slash_bad_policy_id = hash_from_seed(0xce);
    restore_registry(slash_bad_policy_id, 5000 * geocoin::COIN, 1000 * geocoin::COIN, 9, 2);
    slash.sidechain_id = slash_bad_policy_id;
    expect(rejects_sidechain(slash), "SLASH_DEPOSIT rejects unsupported policy");

    slash.sidechain_id = sidechain_id;
    slash.sidechain_amount = 0;
    expect(rejects_sidechain(slash), "SLASH_DEPOSIT rejects zero amount");

    slash.sidechain_amount = 3000 * geocoin::COIN;
    expect(rejects_sidechain(slash), "SLASH_DEPOSIT rejects amount above deposit");

    slash.sidechain_amount = 1;
    slash.outputs.push_back({2, owner, {}});
    expect(rejects_sidechain(slash), "SLASH_DEPOSIT rejects missing fraud proof before outputs");

    geocoin::core::Transaction redeem;
    redeem.type = geocoin::core::TransactionType::REDEEM_SIDECHAIN;
    redeem.sidechain_id = hash_from_seed(0xd6);
    redeem.sidechain_amount = 1;
    expect(rejects_sidechain(redeem), "REDEEM_SIDECHAIN rejects missing registry");

    redeem.sidechain_id = sidechain_id;
    redeem.sidechain_amount = 6000 * geocoin::COIN;
    expect(rejects_sidechain(redeem), "REDEEM_SIDECHAIN rejects amount above issued supply");

    const geocoin::Hash256 redeem_missing_deposit_id = hash_from_seed(0xb1);
    restore_registry(redeem_missing_deposit_id, 5000 * geocoin::COIN, 1000 * geocoin::COIN);
    redeem.sidechain_id = redeem_missing_deposit_id;
    redeem.sidechain_amount = geocoin::COIN;
    expect(rejects_sidechain(redeem), "REDEEM_SIDECHAIN rejects missing deposit");

    const geocoin::Hash256 redeem_low_deposit_id = hash_from_seed(0xb2);
    restore_registry(redeem_low_deposit_id, 5000 * geocoin::COIN, 1000 * geocoin::COIN);
    restore_deposit(redeem_low_deposit_id, owner, 1, 0);
    redeem.sidechain_id = redeem_low_deposit_id;
    redeem.outputs = {{geocoin::COIN, owner, {}}};
    expect(rejects_sidechain(redeem), "REDEEM_SIDECHAIN rejects deposit below payout plus fee");

    restore_deposit(sidechain_id, owner, 2000 * geocoin::COIN, 0);
    redeem.sidechain_id = sidechain_id;
    redeem.sidechain_amount = geocoin::COIN;
    redeem.outputs = {{geocoin::COIN - 1, owner, {}}};
    expect(rejects_sidechain(redeem), "REDEEM_SIDECHAIN rejects output total mismatch");

    redeem.outputs = {{geocoin::COIN, owner, {}}};
    redeem.sidechain_committee_id = committee_id;
    redeem.sidechain_committee_signatures = {geocoin::Signature{0x03}};
    expect(rejects_sidechain(redeem), "REDEEM_SIDECHAIN rejects invalid committee signature");

    geocoin::core::Transaction update_committee;
    update_committee.type = geocoin::core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE;
    update_committee.sidechain_id = hash_from_seed(0xd7);
    expect(rejects_sidechain(update_committee), "UPDATE_SIDECHAIN_COMMITTEE rejects missing registry");

    update_committee.sidechain_id = sidechain_id;
    update_committee.sidechain_committee_m = 2;
    update_committee.sidechain_committee_n = 1;
    expect(rejects_sidechain(update_committee), "UPDATE_SIDECHAIN_COMMITTEE rejects m greater than n");

    update_committee.sidechain_committee_m = 1;
    update_committee.sidechain_committee_n = 2;
    update_committee.sidechain_committee_members = {owner, owner};
    expect(rejects_sidechain(update_committee),
           "UPDATE_SIDECHAIN_COMMITTEE rejects duplicate committee members");

    update_committee.sidechain_committee_m = 1;
    update_committee.sidechain_committee_n = 1;
    update_committee.sidechain_committee_members = {owner};
    update_committee.sidechain_committee_id = hash_from_seed(0xb3);
    expect(rejects_sidechain(update_committee),
           "UPDATE_SIDECHAIN_COMMITTEE rejects mismatched committee id");

    update_committee.sidechain_committee_id = committee_id;
    update_committee.sidechain_committee_signatures = {geocoin::Signature{0x04}};
    expect(rejects_sidechain(update_committee),
           "UPDATE_SIDECHAIN_COMMITTEE rejects invalid committee signatures");

    geocoin::core::Transaction decommission;
    decommission.type = geocoin::core::TransactionType::DECOMMISSION_SIDECHAIN;
    decommission.sidechain_id = sidechain_id;
    decommission.sidechain_owner = owner;
    decommission.inputs.push_back({});
    expect(rejects_sidechain(decommission), "DECOMMISSION_SIDECHAIN rejects inputs");

    decommission.inputs.clear();
    decommission.sidechain_amount = 1;
    expect(rejects_sidechain(decommission), "DECOMMISSION_SIDECHAIN rejects nonzero amount");

    decommission.sidechain_amount = 0;
    decommission.sidechain_checkpoint = hash_from_seed(0xd8);
    expect(rejects_sidechain(decommission), "DECOMMISSION_SIDECHAIN rejects non-null checkpoint");

    decommission.sidechain_checkpoint = {};
    decommission.sidechain_owner = other;
    expect(rejects_sidechain(decommission), "DECOMMISSION_SIDECHAIN rejects owner mismatch");

    const geocoin::Hash256 decommission_missing_registry_id = hash_from_seed(0xb4);
    decommission.sidechain_id = decommission_missing_registry_id;
    decommission.sidechain_owner = owner;
    expect(rejects_sidechain(decommission), "DECOMMISSION_SIDECHAIN rejects missing registry");

    decommission.sidechain_id = sidechain_id;
    expect(rejects_sidechain(decommission), "DECOMMISSION_SIDECHAIN rejects nonzero issued supply");

    const geocoin::Hash256 decommission_missing_deposit_id = hash_from_seed(0xb5);
    restore_registry(decommission_missing_deposit_id, 5000 * geocoin::COIN, 0);
    decommission.sidechain_id = decommission_missing_deposit_id;
    expect(rejects_sidechain(decommission), "DECOMMISSION_SIDECHAIN rejects missing deposit");

    const geocoin::Hash256 decommission_valid_id = hash_from_seed(0xb6);
    restore_registry(decommission_valid_id, 5000 * geocoin::COIN, 0);
    restore_deposit(decommission_valid_id, owner, 2000 * geocoin::COIN, 0);
    decommission.sidechain_id = decommission_valid_id;
    decommission.sidechain_owner = other;
    expect(rejects_sidechain(decommission), "DECOMMISSION_SIDECHAIN rejects deposit owner mismatch");
    decommission.sidechain_owner = owner;
    authorize_decommission(decommission, private_key_from_seed(0xd0), private_key_from_seed(0xd0), committee_id);
    expect(state.validate_transaction(decommission), "DECOMMISSION_SIDECHAIN accepts empty issued registry");

    decommission.sidechain_committee_id = geocoin::Hash256{};
    expect(rejects_sidechain(decommission),
           "DECOMMISSION_SIDECHAIN rejects missing committee id");
    decommission.sidechain_committee_id = committee_id;
    decommission.sidechain_committee_signatures = {geocoin::Signature{0x09}};
    expect(rejects_sidechain(decommission),
           "DECOMMISSION_SIDECHAIN rejects invalid committee signatures");
    authorize_decommission(decommission, private_key_from_seed(0xd0), private_key_from_seed(0xd0), committee_id);
    expect(state.validate_transaction(decommission),
           "DECOMMISSION_SIDECHAIN accepts owner and committee signatures");

    geocoin::core::Transaction lock;
    lock.type = geocoin::core::TransactionType::LOCK_DEPOSIT;
    lock.sidechain_id = hash_from_seed(0xb7);
    lock.sidechain_owner = owner;
    lock.sidechain_amount = 10 * geocoin::COIN;
    lock.sidechain_unlock_height = 101;
    expect(rejects_sidechain(lock), "LOCK_DEPOSIT rejects missing funding UTXO");

    geocoin::state::UTXO lock_utxo;
    lock_utxo.tx_hash = hash_from_seed(0xb8);
    lock_utxo.vout = 0;
    lock_utxo.value = 12 * geocoin::COIN;
    lock_utxo.address = owner;
    lock_utxo.height = 100;
    lock_utxo.is_coinbase = false;
    geocoin::state::ChainStateTestAccess::add_utxo(state, lock_utxo);
    lock.inputs.push_back({lock_utxo.tx_hash, lock_utxo.vout, {}, 0xffffffff, {}, {}});
    lock.outputs.push_back({3 * geocoin::COIN, owner, {}});
    expect(rejects_sidechain(lock), "LOCK_DEPOSIT rejects overcommitted funding");

    lock.outputs[0].value = geocoin::COIN;
    lock.sidechain_signature = sign_sidechain_payload(lock, private_key_from_seed(0xd0));

    geocoin::core::Transaction forged_lock = lock;
    forged_lock.sign_input(0, private_key_from_seed(0xd1));
    expect(rejects_sidechain(forged_lock),
           "LOCK_DEPOSIT rejects a witness not owned by the funding UTXO");

    lock.sign_input(0, private_key_from_seed(0xd0));
    expect(state.validate_transaction(lock), "LOCK_DEPOSIT accepts funded deposit");

    restore_deposit(lock.sidechain_id, owner, 2000 * geocoin::COIN, 0);
    expect(rejects_sidechain(lock), "LOCK_DEPOSIT rejects duplicate deposit id");

    geocoin::core::Transaction spend;
    spend.type = geocoin::core::TransactionType::SPEND_DEPOSIT;
    spend.sidechain_id = hash_from_seed(0xb9);
    spend.sidechain_owner = owner;
    spend.sidechain_amount = geocoin::COIN;
    spend.outputs.push_back({geocoin::COIN, owner, {}});
    expect(rejects_sidechain(spend), "SPEND_DEPOSIT rejects missing deposit");

    const geocoin::Hash256 spend_locked_id = hash_from_seed(0xba);
    restore_deposit(spend_locked_id, owner, 2000 * geocoin::COIN, 200);
    spend.sidechain_id = spend_locked_id;
    expect(rejects_sidechain(spend), "SPEND_DEPOSIT rejects locked deposit");

    const geocoin::Hash256 spend_registered_id = hash_from_seed(0xbb);
    restore_deposit(spend_registered_id, owner, 2000 * geocoin::COIN, 0);
    restore_registry(spend_registered_id, 5000 * geocoin::COIN, 0);
    spend.sidechain_id = spend_registered_id;
    expect(rejects_sidechain(spend), "SPEND_DEPOSIT rejects registered sidechain");

    const geocoin::Hash256 spend_owner_mismatch_id = hash_from_seed(0xbc);
    restore_deposit(spend_owner_mismatch_id, other, 2000 * geocoin::COIN, 0);
    spend.sidechain_id = spend_owner_mismatch_id;
    spend.sidechain_owner = owner;
    expect(rejects_sidechain(spend), "SPEND_DEPOSIT rejects owner mismatch");

    const geocoin::Hash256 spend_valid_id = hash_from_seed(0xa0);
    restore_deposit(spend_valid_id, owner, 2000 * geocoin::COIN, 0);
    spend.sidechain_id = spend_valid_id;
    spend.sidechain_owner = owner;
    spend.sidechain_amount = geocoin::COIN;
    spend.outputs = {{geocoin::COIN - 1, owner, {}}};
    expect(rejects_sidechain(spend), "SPEND_DEPOSIT rejects output mismatch");
    spend.outputs = {{geocoin::COIN, owner, {}}};
    spend.sidechain_signature = sign_sidechain_payload(spend, private_key_from_seed(0xd0));
    expect(state.validate_transaction(spend), "SPEND_DEPOSIT accepts matured unregistered deposit");

    geocoin::core::Transaction unlock;
    unlock.type = geocoin::core::TransactionType::UNLOCK_DEPOSIT;
    unlock.sidechain_id = hash_from_seed(0xbd);
    unlock.sidechain_owner = owner;
    unlock.outputs.push_back({geocoin::COIN, owner, {}});
    expect(rejects_sidechain(unlock), "UNLOCK_DEPOSIT rejects missing deposit");

    const geocoin::Hash256 unlock_registered_id = hash_from_seed(0xbe);
    restore_deposit(unlock_registered_id, owner, geocoin::COIN, 0);
    restore_registry(unlock_registered_id, 5000 * geocoin::COIN, 0);
    unlock.sidechain_id = unlock_registered_id;
    expect(rejects_sidechain(unlock), "UNLOCK_DEPOSIT rejects registered sidechain");

    const geocoin::Hash256 unlock_owner_mismatch_id = hash_from_seed(0xbf);
    restore_deposit(unlock_owner_mismatch_id, other, geocoin::COIN, 0);
    unlock.sidechain_id = unlock_owner_mismatch_id;
    unlock.sidechain_owner = owner;
    expect(rejects_sidechain(unlock), "UNLOCK_DEPOSIT rejects owner mismatch");

    const geocoin::Hash256 unlock_locked_id = hash_from_seed(0xa1);
    restore_deposit(unlock_locked_id, owner, geocoin::COIN, 200);
    unlock.sidechain_id = unlock_locked_id;
    unlock.sidechain_owner = owner;
    expect(rejects_sidechain(unlock), "UNLOCK_DEPOSIT rejects locked deposit");

    const geocoin::Hash256 unlock_valid_id = hash_from_seed(0xa2);
    restore_deposit(unlock_valid_id, owner, geocoin::COIN, 0);
    unlock.sidechain_id = unlock_valid_id;
    unlock.outputs = {{geocoin::COIN - 1, owner, {}}};
    expect(rejects_sidechain(unlock), "UNLOCK_DEPOSIT rejects output total mismatch");
    unlock.outputs = {{geocoin::COIN, owner, {}}};
    unlock.sidechain_signature = sign_sidechain_payload(unlock, private_key_from_seed(0xd0));
    expect(state.validate_transaction(unlock), "UNLOCK_DEPOSIT accepts matured unregistered deposit");
}

void test_slash_deposit_fraud_proof_validation_paths()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    // Keep domain guard coverage even when an incomplete component fixture
    // is now rejected earlier by the public common validation boundary.
    const auto rejects_sidechain = [&](const geocoin::core::Transaction& tx) {
        return !state.validate_transaction(tx) && !StateAccess::validate_sidechain_state(state, tx);
    };
    const geocoin::Address owner = address_from_seed(0xf0);
    const geocoin::Hash256 sidechain_id = hash_from_seed(0xf1);
    const geocoin::Hash256 other_sidechain_id = hash_from_seed(0xf2);

    restore_deposit_with_revert(state, sidechain_id, owner, 2000 * geocoin::COIN, 0);
    restore_sidechain_registry_with_revert(
        state,
        sidechain_id,
        owner,
        5000 * geocoin::COIN,
        1000 * geocoin::COIN
    );
    state.set_height(100);

    auto store_tx = [&](geocoin::core::Transaction tx) {
        const geocoin::Hash256 txid = tx.get_hash();
        expect(state.get_storage()->put_transaction(txid, tx), "fraud ref tx stores in MemoryStorage");
        return txid;
    };

    auto single_checkpoint = [&](const geocoin::Hash256& id, const geocoin::Hash256& checkpoint, geocoin::Height h) {
        geocoin::core::Transaction tx;
        tx.type = geocoin::core::TransactionType::SIDECHAIN_CHECKPOINT;
        tx.sidechain_id = id;
        tx.sidechain_checkpoint = checkpoint;
        tx.sidechain_checkpoint_height = h;
        return store_tx(tx);
    };

    auto batch_checkpoint = [&](std::vector<geocoin::core::BatchCheckpointItem> items) {
        geocoin::core::Transaction tx;
        tx.type = geocoin::core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        tx.sidechain_batch_checkpoints = std::move(items);
        return store_tx(tx);
    };

    auto item = [&](const geocoin::Hash256& id, const geocoin::Hash256& checkpoint, geocoin::Height h) {
        geocoin::core::BatchCheckpointItem out;
        out.sidechain_id = id;
        out.checkpoint = checkpoint;
        out.checkpoint_height = h;
        out.version = 2;
        return out;
    };

    const geocoin::Hash256 single_a = single_checkpoint(sidechain_id, hash_from_seed(0xf3), 9);
    const geocoin::Hash256 single_same_checkpoint = single_checkpoint(sidechain_id, hash_from_seed(0xf3), 9);
    const geocoin::Hash256 single_height_mismatch = single_checkpoint(sidechain_id, hash_from_seed(0xf4), 10);
    const geocoin::Hash256 single_other_sidechain = single_checkpoint(other_sidechain_id, hash_from_seed(0xf5), 9);
    const geocoin::Hash256 batch_valid = batch_checkpoint({
        item(sidechain_id, hash_from_seed(0xf6), 9),
        item(other_sidechain_id, hash_from_seed(0xf7), 9),
    });

    auto slash = [&](const geocoin::Hash256& a, uint16_t a_index,
                     const geocoin::Hash256& b, uint16_t b_index) {
        geocoin::core::Transaction tx;
        tx.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
        tx.sidechain_owner = owner;
        tx.sidechain_id = sidechain_id;
        tx.sidechain_amount = 1;
        tx.sidechain_fraud_checkpoint_a = a;
        tx.sidechain_fraud_checkpoint_index_a = a_index;
        tx.sidechain_fraud_checkpoint_b = b;
        tx.sidechain_fraud_checkpoint_index_b = b_index;
        return tx;
    };

    geocoin::core::Transaction missing_refs;
    missing_refs.type = geocoin::core::TransactionType::SLASH_DEPOSIT;
    missing_refs.sidechain_id = sidechain_id;
    missing_refs.sidechain_amount = 1;
    expect(rejects_sidechain(missing_refs), "SLASH_DEPOSIT rejects missing fraud refs");

    expect(rejects_sidechain(slash(single_a, 0, batch_valid, 0)),
           "SLASH_DEPOSIT rejects non-sentinel index for single checkpoint");
    expect(rejects_sidechain(slash(batch_valid, 0xFFFF, single_a, 0xFFFF)),
           "SLASH_DEPOSIT rejects sentinel index for batch checkpoint");
    expect(rejects_sidechain(slash(batch_valid, 9, single_a, 0xFFFF)),
           "SLASH_DEPOSIT rejects out-of-range batch checkpoint index");
    expect(rejects_sidechain(slash(single_other_sidechain, 0xFFFF, batch_valid, 0)),
           "SLASH_DEPOSIT rejects fraud ref from another sidechain");
    expect(rejects_sidechain(slash(single_a, 0xFFFF, single_height_mismatch, 0xFFFF)),
           "SLASH_DEPOSIT rejects mismatched fraud checkpoint heights");
    expect(rejects_sidechain(slash(single_a, 0xFFFF, single_same_checkpoint, 0xFFFF)),
           "SLASH_DEPOSIT rejects equal fraud checkpoints");

    auto output_too_large = slash(single_a, 0xFFFF, batch_valid, 0);
    output_too_large.sidechain_amount = 100 * geocoin::COIN;
    output_too_large.outputs.push_back({output_too_large.sidechain_amount + 1, owner, {}});
    expect(rejects_sidechain(output_too_large),
           "SLASH_DEPOSIT rejects outputs above slashed amount");

    expect(rejects_sidechain(slash(single_a, 0xFFFF, batch_valid, 0)),
           "SLASH_DEPOSIT with valid fraud proof still rejects without committee signatures");

    auto valid = slash(single_a, 0xFFFF, batch_valid, 0);
    valid.sidechain_amount = 100 * geocoin::COIN;
    valid.outputs.push_back({40 * geocoin::COIN, address_from_seed(0xf8), {}});
    valid.sidechain_committee_id = geocoin::consensus::compute_sidechain_committee_id(
        1,
        1,
        std::vector<geocoin::Address>{owner}
    );
    valid.sidechain_committee_signatures.push_back(
        sign_sidechain_payload(valid, private_key_from_seed(0xf0))
    );
    expect(state.validate_transaction(valid), "SLASH_DEPOSIT validates with fraud proof and committee signature");
    expect(state.apply_transaction(valid, state.get_height() + 1, false), "SLASH_DEPOSIT applies");

    geocoin::state::SidechainDeposit after_slash;
    expect(state.get_sidechain_deposit(sidechain_id, after_slash), "SLASH_DEPOSIT leaves sidechain deposit");
    expect(after_slash.amount == 1900 * geocoin::COIN, "SLASH_DEPOSIT reduces deposit by slash amount");
    geocoin::state::UTXO slash_utxo;
    expect(state.get_utxo(valid.get_hash(), 0, slash_utxo), "SLASH_DEPOSIT creates payout UTXO");
    expect(slash_utxo.value == 40 * geocoin::COIN, "SLASH_DEPOSIT payout UTXO value matches output");
}

void test_standard_transaction_witness_validation_guards()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    const geocoin::bytes sender_key = private_key_from_seed(0x31);
    const geocoin::bytes other_key = private_key_from_seed(0x32);
    const geocoin::Address sender = address_from_seed(0x31);
    const geocoin::Address recipient = address_from_seed(0x33);
    const geocoin::Height mature_height = geocoin::consensus::params::COINBASE_MATURITY + 5;

    auto make_funding = [&](geocoin::Height height, geocoin::Amount amount) {
        geocoin::core::Transaction funding = geocoin::core::Transaction::create_coinbase(
            height,
            sender,
            amount
        );
        expect(state.apply_transaction(funding, height, true), "standard validation funding applies");
        return funding;
    };

    auto make_spend = [&](const geocoin::core::Transaction& funding,
                          const geocoin::bytes& signing_key,
                          geocoin::Amount amount) {
        geocoin::core::Transaction tx;
        tx.version = 2;
        tx.type = geocoin::core::TransactionType::STANDARD;
        tx.timestamp = 1234567;

        geocoin::core::TxInput input;
        input.prev_tx_hash = funding.get_hash();
        input.vout = 0;
        tx.inputs.push_back(input);

        geocoin::core::TxOutput output;
        output.value = amount;
        output.address = recipient;
        tx.outputs.push_back(output);
        tx.sign_input(0, signing_key);
        return tx;
    };

    const auto funding = make_funding(1, 5 * geocoin::COIN);
    geocoin::core::Transaction valid = make_spend(funding, sender_key, 1 * geocoin::COIN);
    expect(state.validate_transaction(valid), "valid standard transaction validates");

    geocoin::core::Transaction missing_utxo = valid;
    missing_utxo.inputs[0].prev_tx_hash = hash_from_seed(0x91);
    expect(!state.validate_transaction(missing_utxo), "standard validation rejects missing UTXO");

    geocoin::core::Transaction missing_witness = valid;
    missing_witness.witnesses.clear();
    expect(!state.validate_transaction(missing_witness), "standard validation rejects missing witness");

    geocoin::core::Transaction missing_signature = valid;
    missing_signature.witnesses[0].signature.clear();
    expect(!state.validate_transaction(missing_signature), "standard validation rejects missing witness signature");

    geocoin::core::Transaction missing_pubkey_hash = valid;
    missing_pubkey_hash.witnesses[0].public_key_hash = {};
    expect(!state.validate_transaction(missing_pubkey_hash),
           "standard validation rejects missing witness public key hash");

    geocoin::core::Transaction pubkey_hash_mismatch = valid;
    pubkey_hash_mismatch.witnesses[0].public_key_hash[0] ^= 0x01;
    expect(!state.validate_transaction(pubkey_hash_mismatch),
           "standard validation rejects public key hash mismatch");

    geocoin::core::Transaction unknown_cached_pubkey = valid;
    unknown_cached_pubkey.witnesses[0].public_key.clear();
    expect(!state.validate_transaction(unknown_cached_pubkey),
           "standard validation rejects unknown cached witness public key");

    geocoin::core::Transaction signer_mismatch = make_spend(funding, other_key, 1 * geocoin::COIN);
    expect(!state.validate_transaction(signer_mismatch),
           "standard validation rejects signer address mismatch");

    geocoin::core::Transaction bad_signature_hash = valid;
    bad_signature_hash.inputs[0].signature_hash[0] ^= 0x01;
    expect(!state.validate_transaction(bad_signature_hash),
           "standard validation rejects invalid detached signature hash");

    expect(state.apply_transaction(valid, mature_height, false),
           "valid standard transaction applies and registers witness public key");
    geocoin::state::ChainStateTestAccess::register_witness_pubkeys_from_tx(state, valid);

    const auto cached_funding = make_funding(2, 4 * geocoin::COIN);
    geocoin::core::Transaction cached_pubkey_spend = make_spend(
        cached_funding,
        sender_key,
        1 * geocoin::COIN
    );
    cached_pubkey_spend.witnesses[0].public_key.clear();
    expect(state.validate_transaction(cached_pubkey_spend),
           "standard validation accepts cached witness public key");
}

void test_nft_standard_utxo_effect_guard_paths()
{
    struct Fixture {
        std::shared_ptr<geocoin::state::ChainState> state;
        geocoin::core::Transaction tx;
        geocoin::Address sender;
        geocoin::Height mature_height = geocoin::consensus::params::COINBASE_MATURITY + 5;
    };

    auto make_fixture = [&](uint8_t seed) {
        Fixture fixture;
        fixture.state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        fixture.state->set_activation_manifest(
            geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));

        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(private_key_from_seed(seed)), "NFT guard wallet initializes");
        fixture.sender = wallet.get_address();

        const auto funding = geocoin::core::Transaction::create_coinbase(
            1,
            fixture.sender,
            10 * geocoin::COIN
        );
        expect(fixture.state->apply_transaction(funding, 1, true), "NFT guard funding applies");
        fixture.state->set_height(fixture.mature_height - 1);
        fixture.state->set_tip_hash(hash_from_seed(static_cast<uint8_t>(seed + 1)));

        geocoin::nft::NFTMetadata metadata = make_nft_metadata();
        metadata.name = "Guard NFT " + std::to_string(seed);
        metadata.content_hash = std::string(64, static_cast<char>('a' + (seed % 6)));
        metadata.block_height = fixture.mature_height;
        metadata.block_hash = hash_from_seed(static_cast<uint8_t>(seed + 2));
        metadata.creator = fixture.sender.str();

        expect(wallet.create_nft_create(
                   metadata,
                   100,
                   geocoin::consensus::params::MIN_TX_FEE,
                   fixture.state,
                   fixture.tx),
               "NFT guard base transaction builds");
        expect(!fixture.tx.inputs.empty() && !fixture.tx.outputs.empty() && !fixture.tx.witnesses.empty(),
               "NFT guard transaction has standard spend material");
        return fixture;
    };

    auto add_test_utxo = [](
        geocoin::state::ChainState& state,
        const geocoin::Hash256& txid,
        geocoin::Amount value,
        const geocoin::Address& address,
        geocoin::Height height,
        bool coinbase
    ) {
        geocoin::state::UTXO utxo;
        utxo.tx_hash = txid;
        utxo.vout = 0;
        utxo.value = value;
        utxo.address = address;
        utxo.height = height;
        utxo.is_coinbase = coinbase;
        if (value < 0) {
            geocoin::state::ChainStateTestAccess::inject_utxo_unchecked(state, utxo);
        } else {
            geocoin::state::ChainStateTestAccess::add_utxo(state, utxo);
        }
    };

    {
        auto fixture = make_fixture(0x41);
        fixture.tx.inputs[0].prev_tx_hash = hash_from_seed(0x42);
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects missing input");
    }
    {
        auto fixture = make_fixture(0x43);
        const auto txid = hash_from_seed(0x44);
        fixture.tx.inputs[0].prev_tx_hash = txid;
        add_test_utxo(*fixture.state, txid, -1, fixture.sender, fixture.mature_height, false);
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects negative UTXO value");
    }
    {
        auto fixture = make_fixture(0x45);
        const auto txid = hash_from_seed(0x46);
        fixture.tx.inputs[0].prev_tx_hash = txid;
        add_test_utxo(*fixture.state, txid, 2 * geocoin::COIN, fixture.sender, fixture.mature_height + 1, false);
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects future-height UTXO");
    }
    {
        auto fixture = make_fixture(0x47);
        const auto txid = hash_from_seed(0x48);
        fixture.tx.inputs[0].prev_tx_hash = txid;
        add_test_utxo(*fixture.state, txid, 2 * geocoin::COIN, fixture.sender, fixture.mature_height, true);
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects immature coinbase UTXO");
    }
    {
        auto fixture = make_fixture(0x49);
        fixture.tx.witnesses.clear();
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects missing witness");
    }
    {
        auto fixture = make_fixture(0x4a);
        fixture.tx.witnesses[0].signature.clear();
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects empty witness signature");
    }
    {
        auto fixture = make_fixture(0x4b);
        fixture.tx.witnesses[0].public_key_hash = {};
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects null witness public-key hash");
    }
    {
        auto fixture = make_fixture(0x4c);
        fixture.tx.witnesses[0].public_key_hash[0] ^= 0x01;
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects public-key hash mismatch");
    }
    {
        auto fixture = make_fixture(0x4d);
        fixture.tx.witnesses[0].public_key.clear();
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects missing uncached witness public key");
    }
    {
        auto fixture = make_fixture(0x4e);
        fixture.tx.witnesses[0].public_key.assign(10, 0x11);
        fixture.tx.witnesses[0].public_key_hash = geocoin::crypto::sha256(fixture.tx.witnesses[0].public_key);
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects invalid witness public key bytes");
    }
    {
        auto fixture = make_fixture(0x4f);
        const auto txid = hash_from_seed(0x50);
        fixture.tx.inputs[0].prev_tx_hash = txid;
        add_test_utxo(*fixture.state, txid, 2 * geocoin::COIN, address_from_seed(0x51), 1, false);
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects signer/address mismatch");
    }
    {
        auto fixture = make_fixture(0x52);
        fixture.tx.outputs[0].value = -1;
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects negative output value");
    }
    {
        auto fixture = make_fixture(0x53);
        fixture.tx.outputs.clear();
        fixture.tx.outputs.push_back({
            (std::numeric_limits<geocoin::Amount>::max)(),
            address_from_seed(0x54),
            {}
        });
        fixture.tx.outputs.push_back({1, address_from_seed(0x55), {}});
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects output total overflow");
    }
    {
        auto fixture = make_fixture(0x56);
        fixture.tx.outputs[0].value = 20 * geocoin::COIN;
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects outputs above inputs");
    }
    {
        auto fixture = make_fixture(0x58);
        fixture.tx.inputs.push_back(fixture.tx.inputs.front());
        fixture.tx.witnesses.push_back(fixture.tx.witnesses.front());
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects a repeated input");
    }
    {
        auto fixture = make_fixture(0x5a);
        const auto first_txid = hash_from_seed(0x5b);
        const auto second_txid = hash_from_seed(0x5c);
        fixture.tx.inputs[0].prev_tx_hash = first_txid;
        fixture.tx.inputs.push_back(fixture.tx.inputs.front());
        fixture.tx.inputs.back().prev_tx_hash = second_txid;
        fixture.tx.witnesses.push_back(fixture.tx.witnesses.front());
        add_test_utxo(
            *fixture.state,
            first_txid,
            (std::numeric_limits<geocoin::Amount>::max)(),
            fixture.sender,
            1,
            false
        );
        add_test_utxo(*fixture.state, second_txid, 1, fixture.sender, 1, false);
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects an overflowing input total");
    }
    {
        auto fixture = make_fixture(0x5d);
        add_test_utxo(
            *fixture.state,
            fixture.tx.get_hash(),
            fixture.tx.outputs.front().value,
            fixture.tx.outputs.front().address,
            fixture.mature_height,
            false
        );
        expect(!StateAccess::apply_nft_effects(*fixture.state, fixture.tx, fixture.mature_height),
               "NFT standard UTXO path rejects an output-key collision");
    }
    {
        auto fixture = make_fixture(0x57);
        fixture.state->set_activation_manifest(
            geocoin::utils::ActivationManifest::production_defaults("degree"));
        expect(!fixture.state->validate_transaction(fixture.tx),
               "local activation policy keeps disabled NFT out of admission paths");
        expect(fixture.state->apply_transaction(fixture.tx, fixture.mature_height, false),
               "local activation policy cannot change consensus transaction validity");
        geocoin::state::UTXO created;
        expect(fixture.state->get_utxo(fixture.tx.get_hash(), 0, created),
               "valid NFT standard UTXO path creates spend output");
    }
}

void test_nft_delegation_and_multisig_validation_guards()
{
    geocoin::state::ChainState state(std::make_unique<geocoin::state::MemoryStorage>());
    state.set_activation_manifest(geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));

    geocoin::core::Transaction nft_create_missing;
    nft_create_missing.type = geocoin::core::TransactionType::NFT_CREATE;
    nft_create_missing.inputs.push_back({hash_from_seed(0x6a), 0, {}, 0xffffffff, {}, {}});
    expect(!state.validate_transaction(nft_create_missing), "NFT_CREATE rejects missing payload");

    geocoin::core::Transaction nft_transfer_missing;
    nft_transfer_missing.type = geocoin::core::TransactionType::NFT_TRANSFER;
    nft_transfer_missing.inputs.push_back({hash_from_seed(0x6b), 0, {}, 0xffffffff, {}, {}});
    expect(!state.validate_transaction(nft_transfer_missing), "NFT_TRANSFER rejects missing payload");

    geocoin::core::Transaction nft_burn_missing;
    nft_burn_missing.type = geocoin::core::TransactionType::NFT_BURN;
    nft_burn_missing.inputs.push_back({hash_from_seed(0x6c), 0, {}, 0xffffffff, {}, {}});
    expect(!state.validate_transaction(nft_burn_missing), "NFT_BURN rejects missing payload");

    const geocoin::Address primary = address_from_seed(0x70);
    const geocoin::Address delegate = address_from_seed(0x71);
    const geocoin::Address recipient = address_from_seed(0x72);
    const geocoin::Address other = address_from_seed(0x73);
    const geocoin::Timestamp now = static_cast<geocoin::Timestamp>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    geocoin::core::Transaction stored_delegation;
    stored_delegation.type = geocoin::core::TransactionType::DELEGATION_PROOF;
    stored_delegation.primary_multisig_address = primary;
    stored_delegation.delegated_multisig_address = delegate;
    stored_delegation.nonce = 7;
    expect(StateAccess::apply_delegation_effects(state, stored_delegation, 10), "delegation proof stores nonce");

    geocoin::core::Transaction duplicate_nonce = stored_delegation;
    duplicate_nonce.delegated_multisig_address = address_from_seed(0x74);
    expect(!state.validate_transaction(duplicate_nonce), "DELEGATION_PROOF rejects reused nonce");

    geocoin::core::Transaction revoke_missing;
    revoke_missing.type = geocoin::core::TransactionType::REVOKE_DELEGATION;
    revoke_missing.revoked_delegation_proof_txid = hash_from_seed(0x75);
    expect(!state.validate_transaction(revoke_missing), "REVOKE_DELEGATION rejects missing proof");

    geocoin::core::Transaction stored_standard;
    stored_standard.type = geocoin::core::TransactionType::STANDARD;
    stored_standard.outputs.push_back({1, recipient, {}});
    const geocoin::Hash256 stored_standard_txid = stored_standard.get_hash();
    expect(state.get_storage()->put_transaction(stored_standard_txid, stored_standard),
           "non-delegation reference tx stores");
    geocoin::core::Transaction revoke_non_delegation = revoke_missing;
    revoke_non_delegation.revoked_delegation_proof_txid = stored_standard_txid;
    expect(!state.validate_transaction(revoke_non_delegation),
           "REVOKE_DELEGATION rejects non-delegation proof tx");

    auto store_delegation = [&](uint64_t nonce,
                                geocoin::Timestamp valid_from,
                                geocoin::Timestamp valid_until,
                                geocoin::Amount max_per,
                                geocoin::Amount max_total,
                                uint8_t max_percent) {
        geocoin::core::Transaction tx;
        tx.type = geocoin::core::TransactionType::DELEGATION_PROOF;
        tx.primary_multisig_address = primary;
        tx.delegated_multisig_address = delegate;
        tx.nonce = nonce;
        tx.valid_from = valid_from;
        tx.valid_until = valid_until;
        tx.max_per_transaction = max_per;
        tx.max_total_delegated = max_total;
        tx.max_percent_of_balance = max_percent;
        expect(StateAccess::apply_delegation_effects(state, tx, static_cast<geocoin::Height>(10 + nonce)),
               "delegation proof stores for hierarchical guard");
        return tx;
    };

    auto make_hierarchical_spend = [&](const geocoin::Hash256& proof_txid) {
        geocoin::core::Transaction tx;
        tx.version = 2;
        tx.type = geocoin::core::TransactionType::MULTISIG_SPEND;
        tx.m = 1;
        tx.n = 1;
        tx.delegation_proof_txid = proof_txid;
        tx.timestamp = now;
        return tx;
    };

    geocoin::core::Transaction missing_proof_spend = make_hierarchical_spend(hash_from_seed(0x76));
    expect(!state.validate_transaction(missing_proof_spend),
           "HIERARCHICAL_SPEND rejects missing delegation proof");

    const auto future_proof = store_delegation(8, now + 100000, 0, 0, 0, 0);
    expect(!state.validate_transaction(make_hierarchical_spend(future_proof.get_hash())),
           "HIERARCHICAL_SPEND rejects proof before valid_from");

    const auto expired_proof = store_delegation(9, 0, now - 1, 0, 0, 0);
    expect(!state.validate_transaction(make_hierarchical_spend(expired_proof.get_hash())),
           "HIERARCHICAL_SPEND rejects expired proof");

    const auto active_proof = store_delegation(10, 0, now + 100000, 100 * geocoin::COIN, 0, 0);
    geocoin::core::Transaction missing_utxo_spend = make_hierarchical_spend(active_proof.get_hash());
    missing_utxo_spend.inputs.push_back({hash_from_seed(0x77), 0, {}, 0xffffffff, {}, {}});
    expect(!state.validate_transaction(missing_utxo_spend),
           "HIERARCHICAL_SPEND rejects missing input UTXO");

    geocoin::state::UTXO wrong_owner_utxo;
    wrong_owner_utxo.tx_hash = hash_from_seed(0x78);
    wrong_owner_utxo.vout = 0;
    wrong_owner_utxo.value = 50 * geocoin::COIN;
    wrong_owner_utxo.address = other;
    wrong_owner_utxo.height = 1;
    wrong_owner_utxo.is_coinbase = false;
    geocoin::state::ChainStateTestAccess::add_utxo(state, wrong_owner_utxo);
    geocoin::core::Transaction wrong_owner_spend = make_hierarchical_spend(active_proof.get_hash());
    wrong_owner_spend.inputs.push_back({wrong_owner_utxo.tx_hash, wrong_owner_utxo.vout, {}, 0xffffffff, {}, {}});
    expect(!state.validate_transaction(wrong_owner_spend),
           "HIERARCHICAL_SPEND rejects input outside primary multisig");

    geocoin::state::UTXO primary_utxo;
    primary_utxo.tx_hash = hash_from_seed(0x79);
    primary_utxo.vout = 0;
    primary_utxo.value = 100 * geocoin::COIN;
    primary_utxo.address = primary;
    primary_utxo.height = 1;
    primary_utxo.is_coinbase = false;
    geocoin::state::ChainStateTestAccess::add_utxo(state, primary_utxo);

    const auto limited_proof = store_delegation(11, 0, now + 100000, geocoin::COIN, 0, 0);
    geocoin::core::Transaction max_per_spend = make_hierarchical_spend(limited_proof.get_hash());
    max_per_spend.inputs.push_back({primary_utxo.tx_hash, primary_utxo.vout, {}, 0xffffffff, {}, {}});
    max_per_spend.outputs.push_back({2 * geocoin::COIN, recipient, {}});
    expect(!state.validate_transaction(max_per_spend),
           "HIERARCHICAL_SPEND rejects max_per_transaction breach");

    const auto percent_proof = store_delegation(12, 0, now + 100000, 0, 0, 10);
    geocoin::core::Transaction percent_spend = make_hierarchical_spend(percent_proof.get_hash());
    percent_spend.inputs.push_back({primary_utxo.tx_hash, primary_utxo.vout, {}, 0xffffffff, {}, {}});
    percent_spend.outputs.push_back({20 * geocoin::COIN, recipient, {}});
    expect(!state.validate_transaction(percent_spend),
           "HIERARCHICAL_SPEND rejects max_percent_of_balance breach");

    geocoin::core::Transaction plain_multisig_bad_version;
    plain_multisig_bad_version.type = geocoin::core::TransactionType::MULTISIG_SPEND;
    plain_multisig_bad_version.version = 1;
    expect(!state.validate_transaction(plain_multisig_bad_version),
           "MULTISIG_SPEND rejects legacy version");

    geocoin::core::Transaction plain_multisig_bad_participants;
    plain_multisig_bad_participants.type = geocoin::core::TransactionType::MULTISIG_SPEND;
    plain_multisig_bad_participants.version = 2;
    plain_multisig_bad_participants.m = 1;
    plain_multisig_bad_participants.n = 2;
    plain_multisig_bad_participants.spend_participants = {primary};
    expect(!state.validate_transaction(plain_multisig_bad_participants),
           "MULTISIG_SPEND rejects missing spend_participants");

    geocoin::core::Transaction plain_multisig_missing_commitment;
    plain_multisig_missing_commitment.type = geocoin::core::TransactionType::MULTISIG_SPEND;
    plain_multisig_missing_commitment.version = 2;
    plain_multisig_missing_commitment.m = 1;
    plain_multisig_missing_commitment.n = 1;
    plain_multisig_missing_commitment.spend_participants = {primary};
    plain_multisig_missing_commitment.angle_commitment_hashes.push_back(hash_from_seed(0x7a));
    expect(!state.validate_transaction(plain_multisig_missing_commitment),
           "MULTISIG_SPEND rejects missing first angle commitment");
}

} // namespace

int main(int argc, char** argv)
{
#ifndef _WIN32
    if (argc == 3 && std::string(argv[1]) == "--chainstate-crash-ibd-child") {
        run_chainstate_crash_child(argv[2], true);
    }
    if (argc == 3 && std::string(argv[1]) == "--chainstate-crash-durable-child") {
        run_chainstate_crash_child(argv[2], false);
    }
#endif

    try {
        test_custom_revert_restores_consensus_state();
        test_sidechain_spend_and_unlock_apply_paths();
        test_sidechain_deposit_apply_guard_paths();
        test_sidechain_deposit_top_up_lifecycle();
        test_sidechain_apply_edge_guard_paths();
        test_sidechain_registry_apply_paths();
        test_accounting_sidechain_lifecycle();
        test_special_apply_and_revert_index_paths();
        test_card_apply_lifecycle_and_validation_guards();
        test_card_corporate_policy_authorities_and_recovery();
        test_state_accessor_fee_and_undo_paths();
        test_utxo_delete_failure_is_fail_closed();
        test_persistence_loads_sidechain_and_interest_metadata();
        test_journal_recovery_and_query_indexes();
        test_deferred_durability_retains_recovery_journal();
#ifndef _WIN32
        test_split_chainstate_survives_abrupt_exit(
            std::filesystem::absolute(argv[0]).string()
        );
#endif
        test_apply_block_guard_paths();
        test_apply_block_dependency_rollback_restores_full_utxo_state();
        test_revert_block_guard_paths_and_snapshot_probe();
        test_apply_block_auto_undo_for_nft_lifecycle();
        test_nft_storage_failures_leave_state_unchanged();
        test_same_block_nft_transition_matrix_and_full_rollback();
        test_nft_rocksdb_restart_lifecycle();
        test_apply_block_auto_undo_for_sidechain_lifecycle();
        test_batch_checkpoint_proof_v2_validation_guards();
        test_interest_distribution_apply_and_revert();
        test_interest_distribution_output_builder_guards();
        test_foundation_update_validate_apply_and_revert();
        test_witness_payload_cache_and_pruning();
        test_witness_pubkey_storage_integrity_and_stats();
        test_persistence_loaders_skip_corruption_and_restore_valid_metadata();
        test_activation_and_sidechain_validation_guards();
        test_slash_deposit_fraud_proof_validation_paths();
        test_standard_transaction_witness_validation_guards();
        test_nft_standard_utxo_effect_guard_paths();
        test_nft_delegation_and_multisig_validation_guards();
        std::cout << "ChainState revert unit tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ChainState revert unit tests failed: " << ex.what() << "\n";
        return 1;
    }
}
