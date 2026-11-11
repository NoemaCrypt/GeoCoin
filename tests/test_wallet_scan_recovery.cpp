#include "chain_test_fixture.hpp"
#include "geocoin/wallet/wallet.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace geocoin;
using test::expect;

namespace {
void check_unavailable_history() {
    test::TestChain fixture;
    for (int i = 0; i < 3; ++i) fixture.append(fixture.make_block());
    wallet::Wallet wallet;
    expect(wallet.initialize(test::make_key(42)), "initialize wallet");
    unsigned incomplete_notifications = 0;
    auto connection = wallet.NotifyScanFinished.connect([&](auto*, const wallet::WalletScanResult& result) {
        if (!result.complete()) ++incomplete_notifications;
    });
    fixture.blocks->close();
    const auto missing = wallet.scan_blockchain(fixture.state);
    expect(!missing.complete() && missing.next_height == 0 && wallet.get_last_scanned_height() == 0,
           "unavailable storage cannot advance scan progress");
    expect(incomplete_notifications == 1, "incomplete history is announced to callers and observers");
    expect(fixture.blocks->open((fixture.directory / "blocks").string()), "reopen storage");
    const auto recovered = wallet.scan_blockchain(fixture.state, missing.next_height);
    expect(recovered.complete() && wallet.get_transactions().size() == 3, "retry discovers all missed payments");

    core::Block block2;
    expect(fixture.blocks->get_block_by_height(2, block2), "retain block for history restoration");
    expect(fixture.blocks->prune_block_body_at_height(2), "prune one historical body");
    const auto partial = wallet.scan_blockchain(fixture.state);
    expect(partial.status == wallet::WalletScanStatus::HISTORY_PRUNED && partial.next_height == 2 &&
               wallet.get_last_scanned_height() == 1, "pruned history stops at first gap even after a prior full scan");
    core::Block latest;
    expect(fixture.blocks->get_block_by_height(3, latest), "read live block");
    wallet.sync_transaction(latest.transactions.front(), 3, latest.header.timestamp, fixture.state);
    expect(wallet.get_last_scanned_height() == 1, "live transaction does not hide the history gap");
    const auto path = (fixture.directory / "wallet.dat").string();
    expect(wallet.save_to_file(path, "test-password"), "persist partial wallet progress");
    wallet::Wallet reopened;
    expect(reopened.load_from_file(path, "test-password") && reopened.get_last_scanned_height() == 1,
           "partial progress survives wallet reopen");
    expect(fixture.blocks->save_block(block2, 2, false), "restore missing body");
    expect(reopened.scan_blockchain(fixture.state, reopened.get_last_scanned_height() + 1).complete(),
           "saved cursor resumes at the missing block");
    expect(reopened.get_transactions().size() == 3 && reopened.get_last_scanned_height() == 3,
           "resume is complete and does not duplicate history");
}

void check_bounded_cache_and_cross_batch_spends() {
    test::TestChain fixture;
    wallet::Wallet wallet;
    expect(wallet.initialize(test::make_key(42)), "initialize archive scanner");
    const auto own = wallet.get_address();
    const auto other = test::address_from_seed(70);
    Hash256 previous = fixture.chain->get_tip()->get_block_hash();
    const auto funding = core::Transaction::create_coinbase(1, own, 10 * COIN);
    core::Transaction spend;
    core::TxInput input;
    input.prev_tx_hash = funding.get_hash();
    input.vout = 0;
    spend.inputs.push_back(input);
    core::TxOutput output;
    output.address = own;
    output.value = funding.outputs.front().value - COIN;
    spend.outputs.push_back(output);
    core::Transaction second_spend;
    input.prev_tx_hash = spend.get_hash();
    second_spend.inputs.push_back(input);
    output.address = other;
    second_spend.outputs.push_back(output);
    core::Block gap;
    // Archived blocks isolate scanning/accounting from unrelated PoW validation.
    // The spend crosses the 1000-block merge boundary, then spends its change in the same block.
    for (Height h = 1; h <= 1205; ++h) {
        core::Block block;
        block.header.prev_block_hash = previous;
        block.header.timestamp = h + 1000;
        block.transactions.push_back(core::Transaction::create_coinbase(h, other, COIN));
        if (h == 1) block.transactions.push_back(funding);
        if (h == 1002) { block.transactions.push_back(spend); block.transactions.push_back(second_spend); }
        block.header.merkle_root = block.calculate_merkle_root();
        block.header.witness_commitment = block.calculate_witness_commitment();
        previous = block.get_hash();
        expect(fixture.blocks->save_block(block, h, false), "archive block saves");
        expect(fixture.blocks->set_active_block_and_tip(h, previous), "archive active height saves");
        if (h == 1001) gap = block;
    }
    expect(fixture.state->set_height(1205), "archive scan target saves");
    // This scanner fixture writes bodies directly without applying ChainState
    // transitions, so it deliberately has no transaction membership index.
    fixture.blocks->set_transaction_archiver({});
    expect(fixture.blocks->prune_block_body_at_height(1001), "introduce gap after first scan batch");
    const auto partial = wallet.scan_blockchain(fixture.state);
    expect(!partial.complete() && partial.next_height == 1001 && wallet.get_last_scanned_height() == 1000,
           "first complete batch is retained without skipping the next block");
    expect(fixture.blocks->save_block(gap, 1001, false), "restore archive gap");
    const auto resumed = wallet.scan_blockchain(fixture.state, partial.next_height);
    expect(resumed.complete() && resumed.peak_owned_outputs <= 1 && partial.peak_owned_outputs <= 1,
           "scan cache depends only on owned unspent outputs, not 1205 unrelated transactions");
    const auto history = wallet.get_transactions();
    expect(history.size() == 3, "cross-batch and same-block spends are both discovered");
    for (const auto& tx : history) {
        if (tx.tx_hash == spend.get_hash()) expect(tx.amount == -COIN, "cross-batch debit includes existing funding");
        if (tx.tx_hash == second_spend.get_hash()) expect(tx.amount == -spend.outputs.front().value,
                                                       "same-block debit includes the preceding change");
    }
    const auto full = wallet.scan_blockchain(fixture.state);
    expect(full.complete() && full.peak_owned_outputs <= 1 && wallet.get_transactions().size() == 3,
           "full rescan has the same bounded cache and stable history");

    core::Block alternate;
    expect(fixture.blocks->get_block_by_height(1205, alternate), "read alternate tip template");
    ++alternate.header.nonce;
    expect(fixture.blocks->save_block(alternate, 1205, false), "save alternate tip");
    wallet::WalletScanResult interrupted;
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool batch_ready = false;
    bool resume = false;
    bool finished = false;
    std::exception_ptr scan_error;
    auto progress = wallet.NotifyScanProgress.connect([&](auto*, Height height) {
        if (height != 999) return;
        std::unique_lock<std::mutex> lock(gate_mutex);
        batch_ready = true;
        gate.notify_all();
        gate.wait(lock, [&] { return resume; });
    });
    std::thread scanner([&] {
        try { interrupted = wallet.scan_blockchain(fixture.state); }
        catch (...) { scan_error = std::current_exception(); }
        std::lock_guard<std::mutex> lock(gate_mutex);
        finished = true;
        gate.notify_all();
    });
    // Hold the scan between batches until the other thread has published the reorg.
    // This also covers slow durable writes, without depending on scheduler timing.
    bool changed_during_scan = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate.wait(lock, [&] { return batch_ready || finished; });
        changed_during_scan = batch_ready && !finished;
    }
    const bool changed = fixture.blocks->set_active_block_and_tip(1205, alternate.get_hash());
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        resume = true;
        gate.notify_all();
    }
    scanner.join();
    progress.disconnect();
    if (scan_error) std::rethrow_exception(scan_error);
    expect(changed_during_scan && changed, "change active branch between scan batches");
    expect(interrupted.status == wallet::WalletScanStatus::CHAIN_CHANGED &&
               interrupted.next_height == 0 && wallet.get_last_scanned_height() == 0,
           "branch change invalidates partial progress and requests a consistent rescan");
    expect(wallet.scan_blockchain(fixture.state, interrupted.next_height).complete(),
           "retry after branch change succeeds");
}

void check_genesis_funding_on_incremental_scan() {
    test::TestChain fixture;
    wallet::Wallet wallet;
    expect(wallet.initialize(test::make_key(42)), "initialize genesis history wallet");
    core::Block genesis;
    genesis.transactions.push_back(core::Transaction::create_coinbase(
        0, wallet.get_address(), 10 * COIN, 0, wallet.get_address()));
    genesis.header.merkle_root = genesis.calculate_merkle_root();
    genesis.header.witness_commitment = genesis.calculate_witness_commitment();
    expect(fixture.blocks->save_block(genesis, 0, false) &&
               fixture.blocks->set_active_block_and_tip(0, genesis.get_hash()), "save genesis history fixture");
    expect(wallet.scan_blockchain(fixture.state).complete(), "discover genesis funding");
    expect(wallet.get_transactions().size() == 1, "genesis fixture funds the scanned wallet");
    core::Block next;
    next.header.prev_block_hash = genesis.get_hash();
    core::Transaction spend;
    core::TxInput input;
    input.prev_tx_hash = genesis.transactions.front().get_hash();
    input.vout = 0;
    spend.inputs.push_back(input);
    core::TxOutput output;
    output.address = test::address_from_seed(70);
    output.value = genesis.transactions.front().outputs.front().value - COIN;
    spend.outputs.push_back(output);
    next.transactions.push_back(spend);
    next.header.merkle_root = next.calculate_merkle_root();
    next.header.witness_commitment = next.calculate_witness_commitment();
    expect(fixture.blocks->save_block(next, 1, false) &&
               fixture.blocks->set_active_block_and_tip(1, next.get_hash()) && fixture.state->set_height(1),
           "save incremental genesis spend");
    expect(wallet.scan_blockchain(fixture.state, 1).complete(), "scan incrementally after genesis");
    const auto history = wallet.get_transactions();
    expect(history.size() == 2, "incremental scan discovers genesis debit");
    expect(history.back().amount == -genesis.transactions.front().outputs.front().value,
           "height-zero funding contributes to debit");
}

void check_same_height_offline_reorg_anchor() {
    test::TestChain fixture;
    wallet::Wallet original;
    expect(original.initialize(test::make_key(42)), "initialize offline reorg wallet");
    const Address alternate_owned = original.create_new_key("alternate-branch");

    const auto old_block = fixture.make_block();
    fixture.append(old_block);
    const Hash256 old_txid = old_block.transactions.front().get_hash();
    expect(original.scan_blockchain(fixture.state).complete(), "scan original one-block branch");
    expect(original.get_last_scanned_block_hash() == old_block.get_hash(),
           "wallet scan cursor is anchored to the original tip hash");

    const auto wallet_path = fixture.directory / "offline-reorg-wallet.dat";
    expect(original.save_to_file(wallet_path.string(), "anchor-test-password"),
           "persist wallet scan anchor");
    wallet::Wallet reopened;
    expect(reopened.load_from_file(wallet_path.string(), "anchor-test-password"),
           "reopen wallet while its original branch is offline");
    expect(reopened.get_last_scanned_height() == 1 &&
               reopened.get_last_scanned_block_hash() == old_block.get_hash(),
           "wallet v12 roundtrip preserves the height and hash anchor");

    auto* genesis = fixture.chain->get_tip()->prev;
    auto alternate = fixture.make_block(genesis);
    alternate.transactions.front().outputs.front().address = alternate_owned;
    ++alternate.transactions.front().timestamp;
    test::TestChain::seal(alternate, genesis);
    auto* alternate_index = fixture.accept(alternate);
    consensus::ValidationState validation;
    expect(fixture.chain->switch_to_tip(alternate_index->get_block_hash(), validation),
           "activate alternate branch at the same height");
    expect(reopened.get_last_scanned_block_hash() != fixture.state->get_tip_hash(),
           "persisted wallet anchor detects an offline same-height reorg");

    const auto reconciled = reopened.reconcile_blockchain(fixture.state, 0);
    expect(reconciled.complete(), "same-height reorg reconciliation completes");
    const auto history = reopened.get_transactions();
    const auto old_entry = std::find_if(history.begin(), history.end(), [&](const auto& tx) {
        return tx.tx_hash == old_txid;
    });
    const Hash256 alternate_txid = alternate.transactions.front().get_hash();
    const auto new_entry = std::find_if(history.begin(), history.end(), [&](const auto& tx) {
        return tx.tx_hash == alternate_txid;
    });
    expect(old_entry != history.end() && old_entry->block_height == 0,
           "transaction absent from the replacement branch is demoted");
    expect(new_entry != history.end() && new_entry->block_height == 1,
           "replacement branch transaction is discovered");
    expect(reopened.get_last_scanned_height() == 1 &&
               reopened.get_last_scanned_block_hash() == alternate.get_hash(),
           "wallet cursor is re-anchored to the replacement tip");
}
}

int main() {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        check_genesis_funding_on_incremental_scan();
        check_same_height_offline_reorg_anchor();
        check_unavailable_history();
        check_bounded_cache_and_cross_batch_spends();
        std::cout << "Wallet partial scan recovery, persistence, notifications and bounded accounting passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
