#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/sidechain_fee.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/state/storage.hpp"
#include "geocoin/storage/block_storage.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace geocoin;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bytes make_seed(uint8_t seed) {
    bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<byte>(seed + i);
    }
    return key;
}

Address address_from_seed(const bytes& seed) {
    Address out;
    if (!crypto::derive_identity_address_from_private_key(seed, out)) {
        return Address();
    }
    return out;
}

Hash256 hash_from_string(const std::string& s) {
    return crypto::sha256(bytes(s.begin(), s.end()));
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

Hash256 build_checkpoint_supply_leaf_v2(
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

Hash256 build_checkpoint_commitment_v2(
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

CheckpointProofV2Fixture make_checkpoint_fixture(
    const Hash256& sidechain_id,
    Height checkpoint_height,
    Amount total_supply
) {
    CheckpointProofV2Fixture out{};

    const Hash256 supply_leaf = build_checkpoint_supply_leaf_v2(sidechain_id, checkpoint_height, total_supply);
    std::vector<Hash256> leaves{
        hash_from_string("restart:leaf0"),
        hash_from_string("restart:leaf1"),
        supply_leaf,
        hash_from_string("restart:leaf3"),
    };
    const size_t leaf_index = 2;
    const Hash256 state_root = core::compute_merkle_root(leaves);
    const std::vector<Hash256> path = core::compute_merkle_path(leaves, leaf_index);

    out.checkpoint = build_checkpoint_commitment_v2(
        sidechain_id, state_root, checkpoint_height, total_supply
    );

    bytes proof;
    proof.push_back('S');
    proof.push_back('C');
    proof.push_back('P');
    proof.push_back('2');
    proof.push_back(1); // format version
    proof.push_back(static_cast<byte>(path.size()));
    proof.push_back(0);
    proof.push_back(0);
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

void set_single_input(core::Transaction& tx, const Hash256& prev_hash, uint32_t vout = 0) {
    tx.inputs.clear();
    core::TxInput in;
    in.prev_tx_hash = prev_hash;
    in.vout = vout;
    tx.inputs.push_back(in);
}

void set_single_output(core::Transaction& tx, Amount value, const Address& addr) {
    tx.outputs.clear();
    core::TxOutput out;
    out.value = value;
    out.address = addr;
    tx.outputs.push_back(out);
}

Signature sign_sidechain_payload(const core::Transaction& tx, const bytes& private_key) {
    const bytes payload = core::build_sidechain_payload_for_signing(tx);
    return crypto::sign_recoverable_message(crypto::sha256(payload), private_key);
}

Signature sign_batch_item_payload(const core::BatchCheckpointItem& item, const bytes& private_key) {
    const bytes payload = core::build_sidechain_batch_checkpoint_payload_for_signing(item);
    return crypto::sign_recoverable_message(crypto::sha256(payload), private_key);
}

Signature sign_committee_update_payload(const core::Transaction& tx, const bytes& private_key) {
    const bytes payload = core::build_sidechain_committee_update_payload_for_signing(tx);
    return crypto::sign_recoverable_message(crypto::sha256(payload), private_key);
}
} // namespace

int main() {
    using namespace geocoin;
    namespace fs = std::filesystem;

    const fs::path root = fs::temp_directory_path() / "geocoin_test_sidechain_restart_persistence";
    const fs::path chainstate_path = root / "chainstate";
    fs::remove_all(root);
    fs::create_directories(root);

    try {
        const bytes owner_key = make_seed(11);
        const Address owner_addr = address_from_seed(owner_key);
        expect(!owner_addr.empty(), "owner address generation failed");

        struct CommitteeMember {
            Address address;
            bytes key;
        };
        std::vector<CommitteeMember> committee{
            {address_from_seed(make_seed(41)), make_seed(41)},
            {address_from_seed(make_seed(42)), make_seed(42)},
            {address_from_seed(make_seed(43)), make_seed(43)},
        };
        std::sort(committee.begin(), committee.end(), [](const CommitteeMember& a, const CommitteeMember& b) {
            return a.address.str() < b.address.str();
        });
        std::vector<Address> committee_members;
        for (const auto& member : committee) {
            committee_members.push_back(member.address);
        }
        const Hash256 committee_id = consensus::compute_sidechain_committee_id(2, 3, committee_members);

        const Hash256 sidechain_id = hash_from_string("restart-sidechain-id");

        {
            auto storage = std::make_unique<state::RocksDBStorage>(chainstate_path.string());
            expect(storage->open(), "failed to open chainstate db");

            state::ChainState st(std::move(storage));

            core::Transaction coinbase = core::Transaction::create_coinbase(1, owner_addr, 10000 * COIN);
            expect(st.apply_transaction(coinbase, 1, true), "apply coinbase failed");
            expect(!coinbase.outputs.empty(), "coinbase has no outputs");
            const Amount miner_spendable = coinbase.outputs[0].value;
            expect(miner_spendable > 0, "coinbase miner output is zero");

            const Height lock_height = consensus::params::COINBASE_MATURITY + 2;
            const Amount lock_sidechain_amount = (miner_spendable * 2) / 3;
            const Amount lock_change_amount = miner_spendable / 6;
            expect(lock_sidechain_amount > 0, "lock sidechain amount is zero");
            expect(lock_change_amount > 0, "lock change amount is zero");

            core::Transaction lock;
            lock.version = 2;
            lock.type = core::TransactionType::LOCK_DEPOSIT;
            lock.timestamp = 2;
            set_single_input(lock, coinbase.get_hash(), 0);
            set_single_output(lock, lock_change_amount, owner_addr);
            lock.sidechain_id = sidechain_id;
            lock.sidechain_owner = owner_addr;
            lock.sidechain_amount = lock_sidechain_amount;
            lock.sidechain_unlock_height = lock_height + 100;
            lock.sidechain_signature = sign_sidechain_payload(lock, owner_key);
            lock.sign_input(0, owner_key);
            expect(st.validate_transaction(lock), "lock validate failed");
            expect(st.apply_transaction(lock, lock_height, false), "lock apply failed");

            core::Transaction issue;
            issue.version = 2;
            issue.type = core::TransactionType::ISSUE_SIDECHAIN;
            issue.timestamp = 3;
            issue.sidechain_id = sidechain_id;
            issue.sidechain_owner = owner_addr;
            issue.sidechain_checkpoint = sidechain_id;
            issue.sidechain_token_symbol = "RST";
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
            expect(st.validate_transaction(issue), "issue validate failed");
            expect(st.apply_transaction(issue, lock_height + 1, false), "issue apply failed");

            const CheckpointProofV2Fixture fixture = make_checkpoint_fixture(sidechain_id, 1, 0);
            core::Transaction batch;
            batch.version = 2;
            batch.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
            batch.timestamp = 4;
            core::BatchCheckpointItem item;
            item.sidechain_id = sidechain_id;
            item.checkpoint = fixture.checkpoint;
            item.checkpoint_height = 1;
            item.fee = consensus::calculate_sidechain_checkpoint_min_fee(lock_height + 2, 0);
            item.version = 2;
            item.committee_id = committee_id;
            item.proof = fixture.proof;
            item.committee_signatures.push_back(sign_batch_item_payload(item, committee[0].key));
            item.committee_signatures.push_back(sign_batch_item_payload(item, committee[1].key));
            batch.sidechain_batch_checkpoints.push_back(item);
            expect(st.validate_transaction(batch), "batch validate failed");
            expect(st.apply_transaction(batch, lock_height + 2, false), "batch apply failed");

            st.set_height(lock_height + 2);

            state::SidechainDeposit dep{};
            state::SidechainRegistryEntry reg{};
            expect(st.get_sidechain_deposit(sidechain_id, dep), "deposit missing before restart");
            expect(st.get_sidechain_registry(sidechain_id, reg), "registry missing before restart");

            const Amount issue_fee = consensus::calc_sidechain_fee(0, issue.sidechain_fee_bps);
            const Amount expected_after_issue = lock.sidechain_amount - issue_fee;
            const Amount expected_after_batch = expected_after_issue - item.fee;
            expect(dep.amount == expected_after_batch, "unexpected deposit amount before restart");
            expect(dep.last_checkpoint_height == lock_height + 2, "unexpected checkpoint mainchain height before restart");
            expect(dep.last_sidechain_checkpoint_height == 1, "unexpected checkpoint sidechain height before restart");
            expect(dep.checkpoint == fixture.checkpoint, "unexpected checkpoint hash before restart");
            expect(reg.checkpoint_policy_id == 2 && reg.checkpoint_version == 2, "unexpected checkpoint policy/version");
        }

        {
            auto storage = std::make_unique<state::RocksDBStorage>(chainstate_path.string());
            expect(storage->open(), "failed to reopen chainstate db");

            state::ChainState st(std::move(storage));

            state::SidechainDeposit dep{};
            state::SidechainRegistryEntry reg{};
            expect(st.get_sidechain_deposit(sidechain_id, dep), "deposit missing after restart");
            expect(st.get_sidechain_registry(sidechain_id, reg), "registry missing after restart");
            expect(dep.owner.str() == owner_addr.str(), "deposit owner mismatch after restart");
            expect(reg.owner.str() == owner_addr.str(), "registry owner mismatch after restart");
            expect(reg.checkpoint_policy_id == 2 && reg.checkpoint_version == 2, "policy/version mismatch after restart");
            expect(reg.committee_id == committee_id, "committee id mismatch after restart");

            const Amount minted_amount = 5 * COIN;
            core::Transaction mint;
            mint.version = 2;
            mint.type = core::TransactionType::MINT_SIDECHAIN;
            mint.timestamp = 5;
            mint.sidechain_id = sidechain_id;
            mint.sidechain_owner = owner_addr;
            mint.sidechain_amount = minted_amount;
            mint.sidechain_committee_id = committee_id;
            mint.sidechain_committee_signatures.push_back(sign_sidechain_payload(mint, committee[0].key));
            mint.sidechain_committee_signatures.push_back(sign_sidechain_payload(mint, committee[1].key));
            expect(st.validate_transaction(mint), "mint validate failed");
            expect(st.apply_transaction(mint, st.get_height() + 1, false), "mint apply failed");

            state::SidechainRegistryEntry minted_reg{};
            state::SidechainDeposit minted_dep{};
            expect(st.get_sidechain_registry(sidechain_id, minted_reg), "registry missing after mint");
            expect(st.get_sidechain_deposit(sidechain_id, minted_dep), "deposit missing after mint");
            expect(minted_reg.total_issued == minted_amount, "mint did not increase issued supply");
            expect(minted_dep.amount < dep.amount, "mint did not charge fee");

            core::Transaction redeem;
            redeem.version = 2;
            redeem.type = core::TransactionType::REDEEM_SIDECHAIN;
            redeem.timestamp = 6;
            redeem.sidechain_id = sidechain_id;
            redeem.sidechain_owner = owner_addr;
            redeem.sidechain_amount = minted_amount;
            redeem.sidechain_committee_id = committee_id;
            set_single_output(redeem, minted_amount, owner_addr);
            redeem.sidechain_committee_signatures.push_back(sign_sidechain_payload(redeem, committee[0].key));
            redeem.sidechain_committee_signatures.push_back(sign_sidechain_payload(redeem, committee[1].key));
            expect(st.validate_transaction(redeem), "redeem validate failed");
            expect(st.apply_transaction(redeem, st.get_height() + 2, false), "redeem apply failed");

            state::UTXO redeem_utxo{};
            state::SidechainRegistryEntry redeemed_reg{};
            expect(st.get_utxo(redeem.get_hash(), 0, redeem_utxo), "redeem output UTXO missing");
            expect(redeem_utxo.value == minted_amount, "redeem UTXO value mismatch");
            expect(st.get_sidechain_registry(sidechain_id, redeemed_reg), "registry missing after redeem");
            expect(redeemed_reg.total_issued == 0, "redeem did not burn issued supply");

            std::vector<CommitteeMember> next_committee{
                {address_from_seed(make_seed(61)), make_seed(61)},
                {address_from_seed(make_seed(62)), make_seed(62)},
                {address_from_seed(make_seed(63)), make_seed(63)},
            };
            std::sort(next_committee.begin(), next_committee.end(), [](const CommitteeMember& a, const CommitteeMember& b) {
                return a.address.str() < b.address.str();
            });
            std::vector<Address> next_committee_members;
            for (const auto& member : next_committee) {
                next_committee_members.push_back(member.address);
            }
            const Hash256 next_committee_id =
                consensus::compute_sidechain_committee_id(2, 3, next_committee_members);

            core::Transaction committee_update;
            committee_update.version = 2;
            committee_update.type = core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE;
            committee_update.timestamp = 7;
            committee_update.sidechain_id = sidechain_id;
            committee_update.sidechain_owner = owner_addr;
            committee_update.sidechain_committee_m = 2;
            committee_update.sidechain_committee_n = 3;
            committee_update.sidechain_committee_members = next_committee_members;
            committee_update.sidechain_committee_id = next_committee_id;
            committee_update.sidechain_committee_signatures.push_back(
                sign_committee_update_payload(committee_update, committee[0].key)
            );
            committee_update.sidechain_committee_signatures.push_back(
                sign_committee_update_payload(committee_update, committee[1].key)
            );
            expect(st.validate_transaction(committee_update), "committee update validate failed");
            expect(st.apply_transaction(committee_update, st.get_height() + 3, false), "committee update apply failed");

            state::SidechainRegistryEntry updated_reg{};
            expect(st.get_sidechain_registry(sidechain_id, updated_reg), "registry missing after committee update");
            expect(updated_reg.committee_id == next_committee_id, "committee id was not updated");
            expect(updated_reg.committee_members.size() == next_committee_members.size(), "committee member count mismatch");

            core::Transaction decommission;
            decommission.version = 2;
            decommission.type = core::TransactionType::DECOMMISSION_SIDECHAIN;
            decommission.timestamp = 8;
            decommission.sidechain_id = sidechain_id;
            decommission.sidechain_owner = owner_addr;
            decommission.sidechain_committee_id = next_committee_id;
            decommission.sidechain_signature = sign_sidechain_payload(decommission, owner_key);
            decommission.sidechain_committee_signatures.push_back(
                sign_sidechain_payload(decommission, next_committee[0].key)
            );
            decommission.sidechain_committee_signatures.push_back(
                sign_sidechain_payload(decommission, next_committee[1].key)
            );
            expect(st.validate_transaction(decommission), "decommission validate failed");
            expect(st.apply_transaction(decommission, st.get_height() + 4, false), "decommission apply failed");
            state::SidechainRegistryEntry decommissioning_reg{};
            expect(st.get_sidechain_registry(sidechain_id, decommissioning_reg),
                   "registry missing during decommission challenge");
            expect(decommissioning_reg.status == state::SidechainStatus::DECOMMISSIONING,
                   "registry status was not changed to decommissioning");
            expect(st.has_sidechain_deposit_key(sidechain_id), "deposit should remain after decommission");
            st.set_height(st.get_height() + 4);
        }

        {
            auto storage = std::make_unique<state::RocksDBStorage>(chainstate_path.string());
            expect(storage->open(), "failed to reopen chainstate db after decommission");

            state::ChainState st(std::move(storage));

            state::SidechainDeposit dep{};
            state::SidechainRegistryEntry reg{};
            expect(st.get_sidechain_deposit(sidechain_id, dep), "deposit missing after decommission restart");
            expect(st.get_sidechain_registry(sidechain_id, reg),
                   "decommissioning registry missing after restart");
            expect(reg.status == state::SidechainStatus::DECOMMISSIONING &&
                       reg.decommission_height != 0,
                   "decommission challenge metadata was not persisted");
        }

        fs::remove_all(root);
        std::cout << "test_sidechain_restart_persistence: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_sidechain_restart_persistence failed: " << e.what() << "\n";
        fs::remove_all(root);
        return 1;
    }
}
