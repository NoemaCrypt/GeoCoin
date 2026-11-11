#include "test_utils.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/state/storage.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/wallet/wallet.hpp"

#include <atomic>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

int main()
{
    using namespace geocoin;
    using test::expect;
    try {
        auto state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
        state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));
        wallet::Wallet wallet;
        const bytes funding_key = test::make_key(1);
        expect(wallet.initialize(funding_key), "initialize wallet");
        const Address source = wallet.get_address();
        const Address recipient = test::address_from_seed(200);
        const auto coinbase = core::Transaction::create_coinbase(1, source, 50 * COIN);
        expect(state->apply_transaction(coinbase, 1, true), "fund wallet");
        state->set_height(120);

        wallet::WalletTransaction funding;
        funding.tx = coinbase;
        funding.tx_hash = coinbase.get_hash();
        funding.is_coinbase = true;
        wallet.add_transaction(funding);
        const Amount funded = coinbase.outputs.front().value;
        const Amount fee = consensus::params::MIN_TX_FEE;

        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        std::mutex failure_mutex;
        std::exception_ptr failure;
        std::vector<std::thread> workers;
        auto run = [&](auto task) {
            workers.emplace_back([&, task] {
                ++ready;
                while (!start.load()) std::this_thread::yield();
                try {
                    task();
                } catch (...) {
                    std::lock_guard<std::mutex> lock(failure_mutex);
                    if (!failure) failure = std::current_exception();
                }
            });
        };

        // Import grows the map; initialize replaces the very key used to sign.
        // Readers must not retain map iterators or key buffers across either.
        run([&] {
            for (int i = 0; i < 96; ++i) {
                expect(wallet.import_private_key(test::make_key(static_cast<uint8_t>(i + 10)), "imported"),
                       "concurrent import");
                expect(wallet.initialize(funding_key), "replace funding key");
                wallet.add_transaction(funding);
            }
        });
        run([&] {
            for (int i = 0; i < 64; ++i) {
                core::Transaction payment;
                expect(wallet.create_transaction(recipient, COIN, fee, state, payment), "concurrent payment");
                expect(payment.verify(), "payment signature corrupted during key replacement");
                expect(state->validate_transaction(payment), "concurrent payment is invalid");
            }
        });
        run([&] {
            for (int i = 0; i < 64; ++i) {
                core::Transaction sweep;
                expect(wallet.move_funds(source, recipient, fee, state, sweep), "concurrent sweep");
                expect(sweep.verify(), "sweep signature corrupted during key replacement");
            }
        });
        run([&] {
            for (int i = 0; i < 192; ++i) {
                expect(wallet.estimate_required_fee(COIN, state) >= fee, "concurrent fee estimate");
                auto credit = funding;
                expect(wallet.get_credit(credit) == funded, "concurrent credit");
                wallet::WalletTransaction debit;
                core::TxInput input;
                input.prev_tx_hash = coinbase.get_hash();
                input.vout = 0;
                debit.tx.inputs.push_back(input);
                expect(wallet.get_debit(debit, state) == funded, "concurrent debit");
            }
        });
        while (ready.load() != 4) std::this_thread::yield();
        start.store(true);
        for (auto& worker : workers) worker.join();
        if (failure) std::rethrow_exception(failure);
        std::cout << "Wallet concurrent import, signing, fees and history passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
