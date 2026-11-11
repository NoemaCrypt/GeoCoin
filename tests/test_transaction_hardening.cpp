// Transaction hardening soft fork: every rule must leave blocks below the
// activation height valid under the original rules, reject the targeted
// encodings from the activation height on, and be enforced for relay at once.

#include "sidechain_registry_fixture.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/sidechain_checkpoint.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/network/mempool_policy.hpp"
#include "geocoin/storage/block_storage.hpp"

#include <filesystem>
#include <iostream>

namespace {
using namespace geocoin;
using test::expect;

constexpr size_t kPaddedFalconSignatureBytes = 666;

Height activation()
{
    return consensus::Params().transaction_hardening_activation_height();
}

core::Transaction signed_payment(uint8_t key_seed)
{
    core::Transaction tx;
    tx.version = 2;
    tx.type = core::TransactionType::STANDARD;
    tx.timestamp = 1700000000;
    core::TxInput input;
    input.prev_tx_hash = test::hash_from_seed(key_seed);
    input.vout = 1;
    tx.inputs.push_back(input);
    tx.outputs.push_back({COIN, test::address_from_seed(static_cast<uint8_t>(key_seed + 1)), {}});
    tx.sign(test::make_key(key_seed));
    return tx;
}

// "F512" | pub_len (u16 BE) | sig_len (u16 BE) | public key | Falcon signature
Signature pad_packed_signature(const Signature& packed)
{
    expect(packed.size() > 8, "packed signature has a header");
    const size_t pub_len = (static_cast<size_t>(packed[4]) << 8) | packed[5];
    const size_t sig_len = (static_cast<size_t>(packed[6]) << 8) | packed[7];
    expect(packed.size() == 8 + pub_len + sig_len && sig_len < kPaddedFalconSignatureBytes,
           "packed signature carries a compressed Falcon signature");
    Signature padded = packed;
    padded.resize(8 + pub_len + kPaddedFalconSignatureBytes, 0);
    padded[6] = static_cast<byte>(kPaddedFalconSignatureBytes >> 8);
    padded[7] = static_cast<byte>(kPaddedFalconSignatureBytes & 0xFF);
    return padded;
}

core::Block block_with(const core::Transaction& tx)
{
    core::Block block;
    block.transactions.push_back(core::Transaction::create_coinbase(1, test::address_from_seed(9), COIN));
    block.transactions.push_back(tx);
    return block;
}

void test_activation_schedule()
{
    consensus::SelectParams(consensus::Network::MAIN);
    const Height mainnet = activation();
    expect(mainnet > consensus::Params().geopow_activation_height(),
           "mainnet hardening activates after the established GeoPoW history");
    expect(!consensus::is_transaction_hardening_active(mainnet - 1) &&
               consensus::is_transaction_hardening_active(mainnet),
           "mainnet activation boundary");
    consensus::SelectParams(consensus::Network::TESTNET);
    expect(activation() > 1, "testnet keeps its existing history under the original rules");
    consensus::SelectParams(consensus::Network::LOADNET);
    expect(activation() == 1, "the isolated load network enforces the rules from its first block");
    consensus::SelectParams(consensus::Network::MAIN);
}

void test_coinbase_maturity_boundary()
{
    const Height h = activation();
    const Height maturity = consensus::Params().coinbase_maturity();
    expect(consensus::is_coinbase_spend_mature(h - 1, h - maturity),
           "below activation a spend counts its own block, as established history did");
    expect(!consensus::is_coinbase_spend_mature(h, h - maturity + 1),
           "from activation a spend one block short of maturity is premature");
    expect(consensus::is_coinbase_spend_mature(h, h - maturity),
           "from activation the full maturity window is required, like the wallet and mempool");
    expect(!consensus::is_coinbase_spend_mature(10, 11), "a coinbase cannot be spent before it exists");
}

void test_canonical_signature_encodings()
{
    const Height h = activation();
    const auto tx = signed_payment(41);
    expect(tx.verify() && consensus::has_canonical_signature_encodings(tx),
           "a wallet signature uses the minimal encoding");

    auto padded = tx;
    padded.witnesses[0].signature.resize(kPaddedFalconSignatureBytes, 0);
    padded.inputs[0].signature_hash = crypto::sha256(padded.witnesses[0].signature);
    expect(padded.verify(), "a zero-padded witness signature still verifies");
    expect(padded.get_hash() != tx.get_hash(), "padding changes the txid");
    expect(!consensus::has_canonical_signature_encodings(padded), "padding is detected");

    consensus::ValidationState below;
    expect(consensus::check_block_transaction_hardening(block_with(padded), h - 1, below),
           "a block below activation keeps accepting the original encodings");
    consensus::ValidationState at;
    expect(!consensus::check_block_transaction_hardening(block_with(padded), h, at) &&
               at.get_reject_reason() == "bad-txns-noncanonical-signature",
           "a block at activation rejects a re-encoded signature");
    consensus::ValidationState honest;
    expect(consensus::check_block_transaction_hardening(block_with(tx), h, honest),
           "a block at activation accepts wallet-encoded signatures");

    // Packed signatures (NFT, sidechain, committee, foundation fields).
    const Hash256 message = test::hash_from_seed(42);
    const Signature packed = crypto::sign_recoverable_message(message, test::make_key(43));
    const Signature padded_packed = pad_packed_signature(packed);
    expect(crypto::verify_signed_message(message, padded_packed),
           "a zero-padded packed signature still verifies");
    core::Transaction foundation;
    foundation.type = core::TransactionType::UPDATE_FOUNDATION_ADDRESS;
    foundation.foundation_signatures = {packed};
    expect(consensus::has_canonical_signature_encodings(foundation), "packed signature is minimal");
    foundation.foundation_signatures.push_back(padded_packed);
    expect(!consensus::has_canonical_signature_encodings(foundation),
           "a padded packed signature in a signature list is detected");

    // Coinbase witness bytes are not verified signatures; leave them alone.
    auto coinbase = core::Transaction::create_coinbase(1, test::address_from_seed(9), COIN);
    coinbase.witnesses.resize(1);
    coinbase.witnesses[0].signature = {1, 2, 3};
    expect(consensus::has_canonical_signature_encodings(coinbase), "coinbase witness bytes are exempt");

    // Relay enforces the rule regardless of height.
    network::MempoolPolicyConfig config;
    std::string reason;
    auto state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    expect(!network::MempoolPolicy::check(padded, *state, 1'000'000, config, &reason) &&
               reason == "non-canonical-signature",
           "relay policy rejects a re-encoded signature before activation");
}

void test_revoke_delegation_change()
{
    const Height h = activation();
    core::Transaction revoke;
    revoke.type = core::TransactionType::REVOKE_DELEGATION;
    expect(consensus::is_revoke_delegation_change_standard(revoke), "a revoke without change is standard");
    revoke.outputs.push_back({static_cast<Amount>(consensus::params::DUST_LIMIT), test::address_from_seed(44), {}});
    expect(consensus::is_revoke_delegation_change_standard(revoke), "change at the dust limit is standard");

    auto dust = revoke;
    dust.outputs[0].value -= 1;
    expect(!consensus::is_revoke_delegation_change_standard(dust), "dust change is detected");
    auto scripted = revoke;
    scripted.outputs[0].script_pubkey = bytes(64, 0xAB);
    expect(!consensus::is_revoke_delegation_change_standard(scripted), "a change script is detected");

    consensus::ValidationState below;
    expect(consensus::check_block_transaction_hardening(block_with(dust), h - 1, below),
           "a block below activation keeps accepting the original revoke change");
    consensus::ValidationState at;
    expect(!consensus::check_block_transaction_hardening(block_with(dust), h, at) &&
               at.get_reject_reason() == "bad-revoke-delegation",
           "a block at activation rejects dust revoke change");

    core::Transaction payment;
    payment.type = core::TransactionType::STANDARD;
    payment.outputs.push_back({1, test::address_from_seed(45), {}});
    expect(consensus::is_revoke_delegation_change_standard(payment), "the rule only concerns REVOKE_DELEGATION");
}

struct SidechainState {
    test::SidechainRegistryFixture fixture;
    std::unique_ptr<state::ChainState> state;

    explicit SidechainState(Height height) {
        auto db = std::make_unique<state::MemoryStorage>();
        expect(db->put_metadata("chainstate_height", utils::encode_u64_le(3)), "seed projection height");
        expect(db->put_metadata("chainstate_utxo_count", utils::encode_u64_le(0)), "seed empty UTXO count");
        expect(db->put_metadata("idx_epoch", bytes(32, '1')), "seed index epoch");
        for (const auto& [key, value] : fixture.records("valid")) {
            expect(db->put_metadata(key, value), "seed sidechain projection");
        }
        state = std::make_unique<state::ChainState>(std::move(db));
        expect(state->set_height(height), "advance sidechain fixture height");
    }

    // A committee-signed checkpoint for height `spend_height`.
    core::Transaction checkpoint(uint8_t seed, Height spend_height) const {
        core::BatchCheckpointItem item;
        item.sidechain_id = fixture.id;
        item.checkpoint_height = 121;
        item.version = 2;
        item.committee_id = fixture.registry.committee_id;
        item.fee = consensus::calculate_sidechain_checkpoint_min_fee(
            spend_height, fixture.deposit.last_checkpoint_height);
        const auto supply = static_cast<uint64_t>(fixture.registry.total_issued);
        const auto leaf = consensus::sidechain_checkpoint::build_financial_supply_leaf_v2(
            fixture.id, item.checkpoint_height, supply);
        const std::vector<Hash256> leaves = {leaf, test::hash_from_seed(seed)};
        const auto root = core::compute_merkle_root(leaves);
        item.checkpoint = consensus::sidechain_checkpoint::build_financial_commitment_v2(
            fixture.id, root, item.checkpoint_height, supply);
        item.proof = consensus::sidechain_checkpoint::build_financial_proof_v2(
            root, item.checkpoint_height, supply, 0, {leaves[1]});
        item.committee_signatures.push_back(crypto::sign_recoverable_message(
            crypto::sha256(core::build_sidechain_batch_checkpoint_payload_for_signing(item)),
            test::make_key(111)));
        core::Transaction tx;
        tx.type = core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        tx.sidechain_batch_checkpoints = {item};
        return tx;
    }

    core::Transaction slash(const Hash256& evidence_a, const Hash256& evidence_b) const {
        core::Transaction tx;
        tx.version = 2;
        tx.type = core::TransactionType::SLASH_DEPOSIT;
        tx.sidechain_id = fixture.id;
        tx.sidechain_owner = fixture.deposit.owner;
        tx.sidechain_amount = fixture.deposit.amount;
        tx.sidechain_committee_id = fixture.registry.committee_id;
        tx.sidechain_fraud_checkpoint_a = evidence_a;
        tx.sidechain_fraud_checkpoint_b = evidence_b;
        tx.sidechain_fraud_checkpoint_index_a = tx.sidechain_fraud_checkpoint_index_b = 0;
        tx.sidechain_committee_signatures.push_back(crypto::sign_recoverable_message(
            crypto::sha256(core::build_sidechain_payload_for_signing(tx)), test::make_key(111)));
        return tx;
    }
};

void test_strict_committee_signature_lists()
{
    const Height h = activation();
    const auto with_repeated_signature = [](core::Transaction tx) {
        auto& signatures = tx.sidechain_batch_checkpoints[0].committee_signatures;
        signatures.push_back(signatures.front());
        return tx;
    };

    {
        SidechainState before(8);
        const auto honest = before.checkpoint(130, 9);
        expect(before.state->validate_transaction(honest), "an honest committee checkpoint is valid");
        const auto repeated = with_repeated_signature(honest);
        expect(repeated.get_hash() != honest.get_hash(), "a repeated signature changes the txid");
        expect(!before.state->validate_transaction(repeated),
               "relay rejects a committee list with a repeated signature");
        expect(before.state->apply_transaction(repeated, 9, false),
               "a block below activation keeps the original committee rule");
    }
    {
        SidechainState at(h - 1);
        const auto repeated = with_repeated_signature(at.checkpoint(131, h));
        expect(!at.state->apply_transaction(repeated, h, false),
               "a block at activation rejects a repeated committee signature");
        auto foreign = at.checkpoint(131, h);
        auto& item = foreign.sidechain_batch_checkpoints[0];
        item.committee_signatures.push_back(crypto::sign_recoverable_message(
            crypto::sha256(core::build_sidechain_batch_checkpoint_payload_for_signing(item)),
            test::make_key(112)));
        expect(!at.state->apply_transaction(foreign, h, false),
               "a block at activation rejects a non-member committee signature");
        expect(at.state->apply_transaction(at.checkpoint(131, h), h, false),
               "a block at activation accepts an honest committee checkpoint");
    }
}

void test_slash_evidence_must_be_on_active_chain()
{
    const Height h = activation();
    const auto archive = [](SidechainState& fixture, uint8_t seed, Height height) {
        const auto tx = fixture.checkpoint(seed, height);
        expect(fixture.state->get_storage()->put_transaction(tx.get_hash(), tx),
               "archive checkpoint evidence outside the active chain");
        return tx.get_hash();
    };
    const auto attach_empty_block_storage = [](SidechainState& fixture, const std::string& name) {
        const auto path = test::make_temp_dir(name);
        fixture.state->set_block_storage(std::make_shared<storage::BlockStorage>(path.string()));
        return path;
    };

    {
        // Production nodes keep an active-chain index. Evidence that only this
        // node archived (an orphaned checkpoint) is not part of it.
        SidechainState before(8);
        const auto a = archive(before, 140, 9);
        const auto b = archive(before, 141, 9);
        const auto slash = before.slash(a, b);
        const auto path = attach_empty_block_storage(before, "geocoin-hardening-slash-before");
        expect(!before.state->validate_transaction(slash),
               "relay rejects slashing with evidence outside the active chain");
        expect(before.state->apply_transaction(slash, 9, false),
               "a block below activation keeps the original evidence rule");
        before.state.reset();
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    {
        SidechainState at(h - 1);
        const auto a = archive(at, 142, h);
        const auto b = archive(at, 143, h);
        const auto slash = at.slash(a, b);
        const auto path = attach_empty_block_storage(at, "geocoin-hardening-slash-at");
        expect(!at.state->apply_transaction(slash, h, false),
               "a block at activation rejects evidence outside the active chain");
        at.state.reset();
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    {
        // Without a block index every stored transaction counts as active, so
        // the same evidence passes: only active-chain membership changed.
        SidechainState standalone(h - 1);
        const auto slash = standalone.slash(archive(standalone, 144, h), archive(standalone, 145, h));
        expect(standalone.state->apply_transaction(slash, h, false),
               "a block at activation accepts evidence that is on the active chain");
    }
}

} // namespace

int main()
{
    try {
        consensus::SelectParams(consensus::Network::MAIN);
        test_activation_schedule();
        test_coinbase_maturity_boundary();
        test_canonical_signature_encodings();
        test_revoke_delegation_change();
        test_strict_committee_signature_lists();
        test_slash_evidence_must_be_on_active_chain();
        std::cout << "test_transaction_hardening: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_transaction_hardening failed: " << e.what() << "\n";
        return 1;
    }
}
