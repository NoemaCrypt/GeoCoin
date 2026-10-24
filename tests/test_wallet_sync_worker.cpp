#include "chain_test_fixture.hpp"
#include "walletsyncworker.h"

#include "geocoin/wallet/wallet.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

using namespace geocoin;
using test::expect;

namespace {

struct ScanObserver {
    void record(const wallet::WalletScanResult& result) {
        std::lock_guard<std::mutex> lock(mutex);
        results.push_back(result);
        changed.notify_all();
    }

    bool wait_for(bool complete, std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&] {
            return std::any_of(results.begin(), results.end(), [&](const auto& result) {
                return result.complete() == complete;
            });
        });
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<wallet::WalletScanResult> results;
};

bool has_confirmed_transaction(
    const std::shared_ptr<wallet::Wallet>& wallet,
    const Hash256& txid,
    Height height
) {
    const auto transactions = wallet->get_transactions();
    return std::any_of(transactions.begin(), transactions.end(), [&](const auto& transaction) {
        return transaction.tx_hash == txid && transaction.block_height == height;
    });
}

void check_missing_transition_body_schedules_recovery() {
    test::TestChain fixture;
    const auto block = fixture.make_block();
    fixture.append(block);

    auto wallet = std::make_shared<wallet::Wallet>();
    expect(wallet->initialize(test::make_key(42)), "initialize transition recovery wallet");
    ScanObserver observer;
    auto connection = wallet->NotifyScanFinished.connect(
        [&](auto*, const wallet::WalletScanResult& result) { observer.record(result); }
    );

    WalletSyncWorker worker(wallet, fixture.state);
    services::ChainMutationEvent event;
    event.sequence = 1;
    event.kind = services::ChainMutationEventKind::ACTIVE_CHAIN_CHANGED;
    event.approximate_bytes = 1;
    event.transition.sequence = 1;
    event.transition.fork_height = 0;
    event.transition.new_height = 1;
    event.transition.new_tip_hash = block.get_hash();
    services::ChainBlockChange unavailable;
    unavailable.height = 1;
    unavailable.hash = block.get_hash();
    unavailable.body_available = false;
    event.transition.connected.push_back(unavailable);

    expect(worker.enqueue(event), "queue transition whose payload body is unavailable");
    expect(observer.wait_for(true, std::chrono::seconds(5)),
           "missing transition body triggers a storage-backed wallet reconciliation");
    worker.stopAndDrain();
    connection.disconnect();

    expect(has_confirmed_transaction(wallet, block.transactions.front().get_hash(), 1),
           "reconciliation recovers the transaction omitted from the event payload");
}

void check_incomplete_recovery_retries() {
    test::TestChain fixture;
    const auto block = fixture.make_block();
    fixture.append(block);
    expect(fixture.blocks->prune_block_body_at_height(1),
           "temporarily remove the active block body");

    auto wallet = std::make_shared<wallet::Wallet>();
    expect(wallet->initialize(test::make_key(42)), "initialize retry recovery wallet");
    ScanObserver observer;
    auto connection = wallet->NotifyScanFinished.connect(
        [&](auto*, const wallet::WalletScanResult& result) { observer.record(result); }
    );

    WalletSyncWorker worker(wallet, fixture.state);
    services::ChainMutationEvent event;
    event.sequence = 2;
    event.kind = services::ChainMutationEventKind::RESCAN_REQUIRED;
    event.gap.first_missing_sequence = 1;
    event.gap.last_missing_sequence = 2;
    event.gap.chain_transition_lost = true;
    event.gap.rescan_from_height = 0;

    expect(worker.enqueue(event), "queue recovery while active history is incomplete");
    expect(observer.wait_for(false, std::chrono::seconds(5)),
           "worker reports the incomplete history instead of advancing the cursor");
    expect(!has_confirmed_transaction(wallet, block.transactions.front().get_hash(), 1),
           "missing history is not presented as a confirmed wallet transaction");

    expect(fixture.blocks->save_block(block, 1, false),
           "restore the active block body before the scheduled retry");
    expect(observer.wait_for(true, std::chrono::seconds(8)),
           "worker retries the incomplete reconciliation without another event");
    worker.stopAndDrain();
    connection.disconnect();

    expect(has_confirmed_transaction(wallet, block.transactions.front().get_hash(), 1),
           "scheduled retry discovers the restored wallet transaction");
    expect(wallet->get_last_scanned_height() == 1 &&
               wallet->get_last_scanned_block_hash() == block.get_hash(),
           "scheduled retry persists the recovered wallet scan anchor");
}

} // namespace

int main() {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        check_missing_transition_body_schedules_recovery();
        check_incomplete_recovery_retries();
        std::cout << "Wallet event body recovery and retry scheduling passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
